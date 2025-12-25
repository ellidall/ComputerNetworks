#include <iostream>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>
#include <sys/stat.h>
#include <cctype>

constexpr int PROXY_PORT = 8888;
constexpr int BUFFER_SIZE = 8192;
constexpr auto CACHE_DIR = "cache";

void InitCacheDir()
{
    struct stat st;
    if (stat(CACHE_DIR, &st) != 0)
    {
        mkdir(CACHE_DIR, 0755);
    }
}

std::string MakeCacheKey(const std::string& host, const std::string& path)
{
    const std::string key = host + (path.empty() || path == "/" ? "/index.html" : path);
    std::string safe;
    for (const char c : key)
    {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_')
        {
            safe += c;
        }
        else
        {
            safe += '_';
        }
    }
    return std::string(CACHE_DIR) + "/" + safe;
}

bool ParseProxyStyle(const std::string& line, std::string& host, std::string& path, int& port)
{
    if (line.substr(0, 4) != "GET ") return false;
    const size_t urlStart = 4;
    const size_t urlEnd = line.find(' ', urlStart);
    if (urlEnd == std::string::npos) return false;
    std::string url = line.substr(urlStart, urlEnd - urlStart);
    if (url.substr(0, 7) != "http://") return false;
    url = url.substr(7);
    const size_t slash = url.find('/');
    if (const size_t colon = url.find(':'); colon != std::string::npos && (slash == std::string::npos || colon < slash))
    {
        host = url.substr(0, colon);
        const size_t portEnd = (slash == std::string::npos) ? std::string::npos : slash;
        const std::string portStr = url.substr(colon + 1, portEnd == std::string::npos
                                                        ? std::string::npos
                                                        : portEnd - colon - 1);
        try
        {
            port = std::stoi(portStr);
        }
        catch (...)
        {
            return false;
        }
        path = (slash == std::string::npos) ? "/" : url.substr(slash);
    }
    else
    {
        host = (slash == std::string::npos) ? url : url.substr(0, slash);
        port = 80;
        path = (slash == std::string::npos) ? "/" : url.substr(slash);
    }
    return true;
}

bool ParseGatewayStyle(const std::string& line, std::string& host, std::string& path, int& port)
{
    if (line.substr(0, 4) != "GET ") return false;
    const size_t pathStart = 4;
    const size_t pathEnd = line.find(' ', pathStart);
    if (pathEnd == std::string::npos) return false;
    const std::string fullPath = line.substr(pathStart, pathEnd - pathStart);
    if (fullPath == "/") return false;
    if (fullPath[0] != '/') return false;
    std::string rest = fullPath.substr(1);
    if (const size_t firstSlash = rest.find('/'); firstSlash == std::string::npos)
    {
        host = rest;
        path = "/";
    }
    else
    {
        host = rest.substr(0, firstSlash);
        path = rest.substr(firstSlash);
    }
    port = 80;
    return !host.empty();
}

void SendError(const int sock, const int code, const std::string& msg)
{
    const std::string resp = "HTTP/1.0 " + std::to_string(code) + " " + msg + "\r\n\r\n";
    send(sock, resp.c_str(), resp.size(), 0);
}

int ConnectTo(const std::string& host, const int port)
{
    addrinfo hints{},* res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    const int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0 || connect(sock, res->ai_addr, res->ai_addrlen) < 0)
    {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

void Handle(int clientSock)
{
    char buf[BUFFER_SIZE];
    ssize_t n = recv(clientSock, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        close(clientSock);
        return;
    }
    buf[n] = '\0';
    std::string req(buf);

    std::string host, path;
    int port = 80;
    if (bool ok = ParseProxyStyle(req, host, path, port) || ParseGatewayStyle(req, host, path, port); !ok)
    {
        SendError(clientSock, 400, "Bad Request");
        close(clientSock);
        return;
    }

    std::cout << "[REQ] " << host << ":" << port << path << std::endl;

    std::string cacheFile = MakeCacheKey(host, path);
    if (std::ifstream cache(cacheFile, std::ios::binary); cache.is_open())
    {
        std::cout << "[HIT] " << cacheFile << std::endl;
        std::string content((std::istreambuf_iterator<char>(cache)), {});
        send(clientSock, content.data(), content.size(), 0);
        cache.close();
        close(clientSock);
        return;
    }

    std::cout << "[MISS] Fetching..." << std::endl;
    int targetSock = ConnectTo(host, port);
    if (targetSock < 0)
    {
        SendError(clientSock, 502, "Bad Gateway");
        close(clientSock);
        return;
    }

    std::string forward = "GET " + path + " HTTP/1.0\r\n" "Host: " + host + "\r\n" "Connection: close\r\n\r\n";
    send(targetSock, forward.c_str(), forward.size(), 0);

    std::ofstream out(cacheFile, std::ios::binary);
    while ((n = recv(targetSock, buf, sizeof(buf), 0)) > 0)
    {
        send(clientSock, buf, n, 0);
        out.write(buf, n);
    }
    out.close();
    close(targetSock);
    close(clientSock);
    std::cout << "[SAVED] " << cacheFile << std::endl;
}

int main()
{
    InitCacheDir();
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    const int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PROXY_PORT);

    bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(sock, 10);

    std::cout << "Proxy запущен на порту " << PROXY_PORT << std::endl;
    std::cout << "Кэш: ./" << CACHE_DIR << "/" << std::endl;
    std::cout << "\n--- СПОСОБЫ ТЕСТИРОВАНИЯ ---" << std::endl;
    std::cout << "Откройте в браузере: http://localhost:8888/example.com" << std::endl;

    while (true)
    {
        sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        if (const int client = accept(sock, reinterpret_cast<struct sockaddr*>(&clientAddr), &len); client >= 0)
        {
            Handle(client);
        }
    }
}
