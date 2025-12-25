#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <cerrno>
#include <string>
#include <iomanip>

constexpr int MAX_BUFFER_SIZE = 1024;
constexpr int TIMEOUT_SECONDS = 1;
constexpr int PING_COUNT = 10;

double GetTimeSeconds()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1'000'000.0;
}

long long GetCurrentTimestampMs()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<long long>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

int CreateUdpSocket()
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        std::cerr << "Failed to set socket timeout: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }

    return sock;
}

bool SendPing(const int sock, const sockaddr_in& serverAddr, const int seqNum, const long long timestamp)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Ping %d %lld", seqNum, timestamp);
    const ssize_t sentBytes = sendto(sock, buffer, strlen(buffer), 0,
                                     reinterpret_cast<const struct sockaddr*>(&serverAddr), sizeof(serverAddr));
    return sentBytes > 0;
}

bool ReceivePong(const int sock, char* buffer, const size_t bufferSize, long long* rttMs, const long long sendTime)
{
    const ssize_t recvBytes = recvfrom(sock, buffer, bufferSize - 1, 0, nullptr, nullptr);
    const auto recvTime = static_cast<long long>(GetTimeSeconds());

    if (recvBytes > 0)
    {
        buffer[recvBytes] = '\0';
        *rttMs = recvTime - sendTime;
        return true;
    }
    return false;
}

void ProcessPing(const int sock, const sockaddr_in& serverAddr, const int seqNum)
{
    const auto sendTime = GetTimeSeconds();

    if (!SendPing(sock, serverAddr, seqNum, static_cast<long long>(sendTime)))
    {
        std::cout << "Request timed out" << std::endl;
        return;
    }

    char buffer[MAX_BUFFER_SIZE];
    long long rttMs;
    if (ReceivePong(sock, buffer, sizeof(buffer), &rttMs, static_cast<long long>(sendTime)))
    {
        const double rttSec = static_cast<double>(rttMs) / 1000.0;
        std::cout << "Ответ от сервера: " << buffer << ", RTT = " << rttSec << " сек" << std::endl;
    }
    else
    {
        std::cout << "Request timed out" << std::endl;
    }
}

int ParsePort(const char* arg)
{
    if (!arg) return -1;

    try
    {
        size_t pos = 0;
        int port = std::stoi(std::string(arg), &pos);

        if (pos != std::strlen(arg))
        {
            return -1;
        }
        if (port <= 0 || port > 65535)
        {
            return -1;
        }
        return port;
    }
    catch (const std::invalid_argument&)
    {
        return -1;
    } catch (const std::out_of_range&)
    {
        return -1;
    }
}

int main(const int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <server_port>" << std::endl;
        return 1;
    }
    const char* serverIp = argv[1];
    const int serverPort = ParsePort(argv[1]);
    if (serverPort == -1)
    {
        std::cerr << "Error: invalid port number. Must be an integer in range 1–65535." << std::endl;
        return EXIT_FAILURE;
    }
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return 1;
    }

    timeval timeout{};
    timeout.tv_sec = TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        std::cerr << "Failed to set socket timeout" << std::endl;
        close(sock);
        return 1;
    }

    sockaddr_in serverAddr{};
    std::memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(serverPort));
    if (inet_pton(AF_INET, serverIp, &serverAddr.sin_addr) <= 0)
    {
        std::cerr << "Invalid server IP address" << std::endl;
        close(sock);
        return 1;
    }

    for (int i = 1; i <= PING_COUNT; ++i)
    {
        const double sendTimeSec = GetTimeSeconds();
        const long long timestampMs = GetCurrentTimestampMs();
        char sendBuffer[64];
        snprintf(sendBuffer, sizeof(sendBuffer), "Ping %d %lld", i, timestampMs);

        const ssize_t sent = sendto(sock, sendBuffer, strlen(sendBuffer), 0,
                                    reinterpret_cast<const struct sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (sent <= 0)
        {
            std::cout << "Request timed out" << std::endl;
            continue;
        }

        char recvBuffer[MAX_BUFFER_SIZE];

        if (const ssize_t recvBytes = recvfrom(sock, recvBuffer, sizeof(recvBuffer) - 1, 0, nullptr, nullptr);
            recvBytes > 0)
        {
            recvBuffer[recvBytes] = '\0';
            const double recvTimeSec = GetTimeSeconds();
            const double rtt = recvTimeSec - sendTimeSec;

            std::cout << "Ответ от сервера: " << recvBuffer << ", RTT = " << std::fixed << std::setprecision(6) << rtt
                    << " сек" << std::endl;
        }
        else
        {
            std::cout << "Request timed out" << std::endl;
        }
    }

    close(sock);
    return 0;
}
