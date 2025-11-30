#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <filesystem>

constexpr int SERVER_PORT = 8888;
constexpr int BACKLOG_SIZE = 10;
constexpr size_t BUFFER_SIZE = 4096;
constexpr auto DEFAULT_DOCUMENT_ROOT = "./www";

int CreateTcpSocket();

void BindSocket(int socketFd, int port);

void ListenForConnections(int socketFd);

std::string ExtractRequestedFile(const std::string& request);

std::string DetermineContentType(const std::string& filename);

std::string BuildHttpResponse(int statusCode, const std::string& contentType, const std::string& body);

void SendFileResponse(int clientFd, const std::string& filePath);

void HandleClientConnection(int clientFd);

[[noreturn]] int main()
{
    std::cout << "Starting web server on port " << SERVER_PORT << "...\n";
    std::cout << "Document root: " << DEFAULT_DOCUMENT_ROOT << "\n";

    const int serverSocket = CreateTcpSocket();
    BindSocket(serverSocket, SERVER_PORT);
    ListenForConnections(serverSocket);

    std::cout << "Server is listening...\n";

    while (true)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        const int clientSocket = accept(serverSocket, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

        if (clientSocket < 0)
        {
            std::cerr << "Accept failed: " << strerror(errno) << "\n";
            continue;
        }

        std::cout << "Connection from " << inet_ntoa(clientAddr.sin_addr) << "\n";
        HandleClientConnection(clientSocket);
        close(clientSocket);
    }
}

int CreateTcpSocket()
{
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
    {
        std::cerr << "Socket creation failed: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    }

    int enable = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
    {
        std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
    }

    return socketFd;
}

void BindSocket(const int socketFd, const int port)
{
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(socketFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
    {
        std::cerr << "Bind failed on port " << port << ": " << strerror(errno) << "\n";
        close(socketFd);
        exit(EXIT_FAILURE);
    }
}

void ListenForConnections(const int socketFd)
{
    if (listen(socketFd, BACKLOG_SIZE) < 0)
    {
        std::cerr << "Listen failed: " << strerror(errno) << "\n";
        close(socketFd);
        exit(EXIT_FAILURE);
    }
}

std::string ExtractRequestedFile(const std::string& request)
{
    std::istringstream requestStream(request);
    std::string method, path, version;
    requestStream >> method >> path >> version;

    if (method != "GET")
    {
        return "";
    }

    if (path.empty() || path[0] != '/')
    {
        return "index.html";
    }

    std::string filename = path.substr(1);
    if (filename.empty())
    {
        filename = "index.html";
    }

    if (filename.find("..") != std::string::npos)
    {
        return "";
    }

    return filename;
}

std::string DetermineContentType(const std::string& filename)
{
    if (filename.ends_with(".html") || filename.ends_with(".htm"))
    {
        return "text/html";
    }
    if (filename.ends_with(".css"))
    {
        return "text/css";
    }
    if (filename.ends_with(".js"))
    {
        return "application/javascript";
    }
    if (filename.ends_with(".png"))
    {
        return "image/png";
    }
    if (filename.ends_with(".jpg") || filename.ends_with(".jpeg"))
    {
        return "image/jpeg";
    }
    if (filename.ends_with(".gif"))
    {
        return "image/gif";
    }
    return "application/octet-stream";
}

std::string BuildHttpResponse(const int statusCode, const std::string& contentType, const std::string& body)
{
    std::ostringstream response;
    if (statusCode == 200)
    {
        response << "HTTP/1.1 200 OK\r\n";
    }
    else if (statusCode == 404)
    {
        response << "HTTP/1.1 404 Not Found\r\n";
    }
    else
    {
        response << "HTTP/1.1 500 Internal Server Error\r\n";
    }
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    return response.str();
}

void SendFileResponse(int clientFd, const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        std::string notFoundBody = "File Not Found";
        std::string response = BuildHttpResponse(404, "text/plain", notFoundBody);
        write(clientFd, response.c_str(), response.length());
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    std::string contentType = DetermineContentType(filePath);
    std::string httpResponse = BuildHttpResponse(200, contentType, content);
    write(clientFd, httpResponse.c_str(), httpResponse.length());
    file.close();
}

void HandleClientConnection(const int clientFd)
{
    char buffer[BUFFER_SIZE];
    const ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
    if (bytesRead == 0)
    {
        std::cout << "Client closed connection (no data)\n";
        return;
    }
    if (bytesRead < 0)
    {
        std::cerr << "Read error: " << strerror(errno) << "\n";
        return;
    }
    buffer[bytesRead] = '\0';

    const std::string request(buffer);
    const std::string filename = ExtractRequestedFile(request);

    if (filename.empty())
    {
        const std::string response = BuildHttpResponse(404, "text/plain", "File Not Found");
        write(clientFd, response.c_str(), response.length());
        return;
    }

    const std::string fullPath = std::string(DEFAULT_DOCUMENT_ROOT) + "/" + filename;
    if (!std::filesystem::exists(fullPath) || std::filesystem::is_directory(fullPath))
    {
        const std::string response = BuildHttpResponse(404, "text/plain", "File Not Found");
        write(clientFd, response.c_str(), response.length());
        return;
    }

    SendFileResponse(clientFd, fullPath);
}
