#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <chrono>
#include <random>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <optional>

constexpr size_t MAX_DATA_SIZE = 1024;
constexpr uint16_t HEADER_SIZE = 4;
constexpr uint16_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_DATA_SIZE;
constexpr int WINDOW_SIZE = 4;
constexpr int TIMEOUT_MS = 1000;
constexpr double LOSS_PROBABILITY = 0.2;
constexpr double DELAY_PROBABILITY = 0.2;

static bool g_debug = false;

void DebugPrint(const std::string& message)
{
    if (g_debug)
    {
        std::cerr << "[SENDER] " << message << std::endl;
    }
}

bool ShouldDropPacket()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return dis(gen) < LOSS_PROBABILITY;
}

void MaybeDelayPacket()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    if (dis(gen) < DELAY_PROBABILITY)
    {
        std::uniform_int_distribution<> delayMs(50, 300);
        usleep(delayMs(gen) * 1000);
    }
}

std::vector<uint8_t> BuildPacket(uint32_t seqNum, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> packet(HEADER_SIZE + data.size());
    std::memcpy(packet.data(), &seqNum, sizeof(seqNum));
    if (!data.empty())
    {
        std::memcpy(packet.data() + HEADER_SIZE, data.data(), data.size());
    }
    return packet;
}

std::optional<uint32_t> ParseAck(const uint8_t* buffer, size_t length)
{
    if (length < sizeof(uint32_t))
    {
        return std::nullopt;
    }
    uint32_t ackNum;
    std::memcpy(&ackNum, buffer, sizeof(ackNum));
    return ackNum;
}

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <receiver_host> <receiver_port> <file.txt> [-d]" << std::endl;
        return EXIT_FAILURE;
    }

    std::string receiverHost = argv[1];
    uint16_t receiverPort = static_cast<uint16_t>(std::stoi(argv[2]));
    std::string fileName = argv[3];
    g_debug = (argc > 4 && std::string(argv[4]) == "-d");

    std::ifstream file(fileName, std::ios::binary);
    if (!file)
    {
        std::cerr << "Error: Cannot open file " << fileName << std::endl;
        return EXIT_FAILURE;
    }
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    int sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    sockaddr_in receiverAddr = {};
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port = htons(receiverPort);
    if (inet_pton(AF_INET, receiverHost.c_str(), &receiverAddr.sin_addr) <= 0)
    {
        std::cerr << "Error: Invalid IP address " << receiverHost << std::endl;
        close(sockFd);
        return EXIT_FAILURE;
    }

    std::vector<std::vector<uint8_t>> packets;
    size_t totalBytes = fileData.size();
    for (size_t i = 0; i < totalBytes; i += MAX_DATA_SIZE)
    {
        size_t chunkSize = std::min(MAX_DATA_SIZE, totalBytes - i);
        packets.emplace_back(fileData.begin() + i, fileData.begin() + i + chunkSize);
    }
    if (packets.empty())
    {
        packets.emplace_back();
    }
    uint32_t totalPackets = static_cast<uint32_t>(packets.size());

    uint32_t sendBase = 0;
    uint32_t nextSeqNum = 0;
    std::vector<std::vector<uint8_t>> sentPackets(totalPackets);
    std::vector<std::chrono::steady_clock::time_point> sentTime(totalPackets);
    size_t retransmissions = 0;
    auto startTime = std::chrono::steady_clock::now();

    for (uint32_t i = 0; i < totalPackets; ++i)
    {
        sentPackets[i] = BuildPacket(i, packets[i]);
    }

    while (sendBase < totalPackets)
    {
        while (nextSeqNum < sendBase + WINDOW_SIZE && nextSeqNum < totalPackets)
        {
            if (!ShouldDropPacket())
            {
                MaybeDelayPacket();
                ssize_t sentBytes = sendto(sockFd, sentPackets[nextSeqNum].data(), sentPackets[nextSeqNum].size(), 0,
                                           reinterpret_cast<sockaddr*>(&receiverAddr), sizeof(receiverAddr));
                if (sentBytes > 0)
                {
                    sentTime[nextSeqNum] = std::chrono::steady_clock::now();
                    DebugPrint("Sent packet #" + std::to_string(nextSeqNum));
                }
            }
            else
            {
                DebugPrint("Dropped packet #" + std::to_string(nextSeqNum) + " (simulated loss)");
            }
            nextSeqNum++;
        }

        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(sockFd, &readFds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // 10 ms

        if (int activity = select(sockFd + 1, &readFds, nullptr, nullptr, &tv); activity > 0)
        {
            uint8_t ackBuffer[sizeof(uint32_t)];
            ssize_t recvBytes = recvfrom(sockFd, ackBuffer, sizeof(ackBuffer), 0, nullptr, nullptr);
            auto ackNum = ParseAck(ackBuffer, recvBytes);
            if (ackNum && *ackNum >= sendBase)
            {
                sendBase = *ackNum + 1;
                DebugPrint("Received ACK #" + std::to_string(*ackNum) + ", new base = " + std::to_string(sendBase));
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime[sendBase]).count();
            elapsed > TIMEOUT_MS)
        {
            DebugPrint("Timeout on base packet #" + std::to_string(sendBase) + ", retransmitting window");
            nextSeqNum = sendBase;
            retransmissions += (std::min(sendBase + WINDOW_SIZE, totalPackets) - sendBase);
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double durationSec = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count() / 1000.0;
    double lossRate = (totalPackets > 0) ? (static_cast<double>(retransmissions) / totalPackets) * 100.0 : 0.0;
    double throughput = (totalBytes > 0 && durationSec > 0) ? (totalBytes / durationSec / 1024.0) : 0.0; // KB/s

    std::cout << "Transfer completed successfully." << std::endl;
    std::cout << "Total packets: " << totalPackets << std::endl;
    std::cout << "Retransmissions: " << retransmissions << std::endl;
    std::cout << "Loss rate: " << lossRate << "%" << std::endl;
    std::cout << "Throughput: " << throughput << " KB/s" << std::endl;

    close(sockFd);
    return EXIT_SUCCESS;
}
