#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <random>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <optional>

constexpr uint16_t MAX_DATA_SIZE = 1024;
constexpr uint16_t HEADER_SIZE = 4;
constexpr uint16_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_DATA_SIZE;
constexpr double LOSS_PROBABILITY = 0.2;
constexpr double DELAY_PROBABILITY = 0.2;

static bool g_debug = false;

void DebugPrint(const std::string& message)
{
    if (g_debug) {
        std::cerr << "[RECEIVER] " << message << std::endl;
    }
}

bool ShouldDropAck()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return dis(gen) < LOSS_PROBABILITY;
}

void MaybeDelayAck()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    if (dis(gen) < DELAY_PROBABILITY) {
        std::uniform_int_distribution<> delayMs(50, 200);
        usleep(delayMs(gen) * 1000);
    }
}

std::vector<uint8_t> BuildAck(uint32_t ackNum)
{
    std::vector<uint8_t> ack(HEADER_SIZE);
    std::memcpy(ack.data(), &ackNum, sizeof(ackNum));
    return ack;
}

std::optional<std::pair<uint32_t, std::vector<uint8_t>>> ParsePacket(const uint8_t* buffer, size_t length)
{
    if (length < HEADER_SIZE) {
        return std::nullopt;
    }
    uint32_t seqNum;
    std::memcpy(&seqNum, buffer, sizeof(seqNum));
    std::vector<uint8_t> payload;
    if (length > HEADER_SIZE) {
        payload = std::vector(buffer + HEADER_SIZE, buffer + length);
    }
    return std::make_pair(seqNum, payload);
}

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <receiver_port> <output_file> [-d]" << std::endl;
        return EXIT_FAILURE;
    }

    uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
    std::string outputFile = argv[2];
    g_debug = (argc > 3 && std::string(argv[3]) == "-d");

    int sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(sockFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        perror("bind");
        close(sockFd);
        return EXIT_FAILURE;
    }

    std::ofstream outFile(outputFile, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Cannot create output file " << outputFile << std::endl;
        close(sockFd);
        return EXIT_FAILURE;
    }

    uint32_t expectedSeqNum = 0;
    sockaddr_in senderAddr = {};
    socklen_t senderAddrLen = sizeof(senderAddr);
    bool receivedAny = false;
    static uint32_t lastAck = 0;

    while (true) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(sockFd, &readFds);
        timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int activity = select(sockFd + 1, &readFds, nullptr, nullptr, &tv);
        if (activity <= 0) {
            if (receivedAny) {
                DebugPrint("Timeout: Assuming end of transmission");
                break;
            } else {
                std::cerr << "Error: No data received within timeout" << std::endl;
                break;
            }
        }

        uint8_t buffer[MAX_PACKET_SIZE];
        ssize_t recvBytes = recvfrom(sockFd, buffer, MAX_PACKET_SIZE, 0,
                                     reinterpret_cast<sockaddr*>(&senderAddr), &senderAddrLen);
        if (recvBytes <= 0) continue;

        auto packet = ParsePacket(buffer, recvBytes);
        if (!packet) continue;

        uint32_t seqNum = packet->first;
        const auto& payload = packet->second;
        receivedAny = true;

        if (seqNum == expectedSeqNum) {
            outFile.write(reinterpret_cast<const char*>(payload.data()), payload.size());
            outFile.flush();
            expectedSeqNum++;
            lastAck = seqNum; // обновляем последний подтверждённый
            DebugPrint("Delivered in-order packet #" + std::to_string(seqNum));
        }

        std::vector<uint8_t> ackPacket = BuildAck(lastAck);
        if (!ShouldDropAck()) {
            MaybeDelayAck();
            sendto(sockFd, ackPacket.data(), ackPacket.size(), 0,
                   reinterpret_cast<sockaddr*>(&senderAddr), senderAddrLen);
            DebugPrint("Sent ACK #" + std::to_string(lastAck));
        } else {
            DebugPrint("Dropped ACK #" + std::to_string(lastAck) + " (simulated loss)");
        }
    }

    outFile.close();
    close(sockFd);
    std::cout << "File received and saved to " << outputFile << std::endl;
    return EXIT_SUCCESS;
}