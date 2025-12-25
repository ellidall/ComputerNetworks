#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <sys/select.h>

constexpr int MAX_BUFFER_SIZE = 1024;
constexpr int INACTIVITY_TIMEOUT_SEC = 5;

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
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return EXIT_FAILURE;
    }
    const int port = ParsePort(argv[1]);
    if (port == -1)
    {
        std::cerr << "Error: invalid port number. Must be an integer in range 1–65535." << std::endl;
        return EXIT_FAILURE;
    }
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
    {
        std::cerr << "Bind failed: " << strerror(errno) << std::endl;
        close(sock);
        return EXIT_FAILURE;
    }

    std::cout << "UDP Pinger server listening on port " << port << "..." << std::endl;
    std::cout << "Server will exit automatically after " << INACTIVITY_TIMEOUT_SEC << " seconds of inactivity." <<
            std::endl;

    sockaddr_in clientAddr{};
    socklen_t clientLen;
    char buffer[MAX_BUFFER_SIZE];

    while (true)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout{};
        timeout.tv_sec = INACTIVITY_TIMEOUT_SEC;
        timeout.tv_usec = 0;

        const int activity = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (activity < 0)
        {
            std::cerr << "select() error: " << strerror(errno) << std::endl;
            break;
        }
        if (activity == 0)
        {
            std::cout << "No activity for " << INACTIVITY_TIMEOUT_SEC << " seconds. Shutting down server." << std::endl;
            break;
        }

        if (FD_ISSET(sock, &readfds))
        {
            clientLen = sizeof(clientAddr);
            const ssize_t recvBytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                               reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
            if (recvBytes > 0)
            {
                buffer[recvBytes] = '\0';
                sendto(sock, buffer, recvBytes, 0, reinterpret_cast<const sockaddr*>(&clientAddr), clientLen);
            }
        }
    }

    close(sock);
    return 0;
}
