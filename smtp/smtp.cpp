#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

constexpr int SMTP_PORT = 587;
const std::string SMTP_SERVER = "smtp.mail.ru";
const std::string CLIENT_HOSTNAME = "localhost";
const std::string SENDER_EMAIL = "sanya.apakaev@mail.ru";
const std::string SMTP_PASSWORD = "jZKyn8mGn5rCM8AwEtj3";
const std::string RECIPIENT_EMAIL = "pokemonivan32@gmail.com";

std::string Base64Encode(const std::string& input)
{
    static const char base64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    int val = 0, valb = -6;
    for (unsigned char c : input)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            output.push_back(base64Chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
    {
        output.push_back(base64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (output.size() % 4) output.push_back('=');
    return output;
}

bool SendCommand(const int socketFd, const std::string& command)
{
    const std::string fullCommand = command + "\r\n";
    const ssize_t bytesSent = write(socketFd, fullCommand.c_str(), fullCommand.size());
    return bytesSent == static_cast<ssize_t>(fullCommand.size());
}

std::string ReadResponse(const int socketFd)
{
    std::string response;
    char buffer[1024];
    ssize_t bytesRead;
    while ((bytesRead = read(socketFd, buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[bytesRead] = '\0';
        response += buffer;
        if (bytesRead >= 2 && buffer[bytesRead - 2] == '\r' && buffer[bytesRead - 1] == '\n')
        {
            break;
        }
    }
    return response;
}

bool ExpectCode(const std::string& response, const std::string& expectedCode)
{
    if (response.length() < 3) return false;
    return response.substr(0, 3) == expectedCode;
}

bool ConnectToSmtpServer(int& socketFd)
{
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* serverInfo = nullptr;
    if (const int status = getaddrinfo(SMTP_SERVER.c_str(), nullptr, &hints, &serverInfo); status != 0)
    {
        std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
        return false;
    }

    socketFd = socket(serverInfo->ai_family, serverInfo->ai_socktype, serverInfo->ai_protocol);
    if (socketFd == -1)
    {
        std::cerr << "Failed to create socket" << std::endl;
        freeaddrinfo(serverInfo);
        return false;
    }

    if (serverInfo->ai_family == AF_INET)
    {
        reinterpret_cast<sockaddr_in*>(serverInfo->ai_addr)->sin_port = htons(SMTP_PORT);
    }
    else if (serverInfo->ai_family == AF_INET6)
    {
        reinterpret_cast<sockaddr_in6*>(serverInfo->ai_addr)->sin6_port = htons(SMTP_PORT);
    }

    if (connect(socketFd, serverInfo->ai_addr, serverInfo->ai_addrlen) == -1)
    {
        std::cerr << "Failed to connect to SMTP server" << std::endl;
        close(socketFd);
        freeaddrinfo(serverInfo);
        return false;
    }

    freeaddrinfo(serverInfo);
    return true;
}

bool PerformSmtpSession(const int socketFd)
{
    std::string response = ReadResponse(socketFd);
    std::cout << "Server: " << response;
    if (!ExpectCode(response, "220"))
    {
        std::cerr << "Unexpected greeting from server" << std::endl;
        return false;
    }

    if (!SendCommand(socketFd, "EHLO " + CLIENT_HOSTNAME))
    {
        std::cerr << "Failed to send EHLO" << std::endl;
        return false;
    }
    response = ReadResponse(socketFd);
    std::cout << "Server: " << response;
    if (!ExpectCode(response, "250"))
    {
        std::cerr << "EHLO not accepted" << std::endl;
        return false;
    }

    if (response.find("STARTTLS") == std::string::npos)
    {
        std::cerr << "Server does not support STARTTLS" << std::endl;
        return false;
    }

    if (!SendCommand(socketFd, "STARTTLS"))
    {
        std::cerr << "Failed to send STARTTLS" << std::endl;
        return false;
    }
    response = ReadResponse(socketFd);
    std::cout << "Server: " << response;
    if (!ExpectCode(response, "220"))
    {
        std::cerr << "STARTTLS not accepted" << std::endl;
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
    {
        std::cerr << "Unable to create SSL context" << std::endl;
        return false;
    }

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, socketFd);

    if (SSL_connect(ssl) <= 0)
    {
        std::cerr << "SSL handshake failed" << std::endl;
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    auto SslSendCommand = [ssl](const std::string& cmd) -> bool {
        const std::string full = cmd + "\r\n";
        const int written = SSL_write(ssl, full.c_str(), static_cast<int>(full.size()));
        return written == static_cast<int>(full.size());
    };

    auto SslReadResponse = [ssl]() -> std::string {
        std::string resp;
        char buf[1024];
        int bytes;
        while ((bytes = SSL_read(ssl, buf, sizeof(buf) - 1)) > 0)
        {
            buf[bytes] = '\0';
            resp += buf;
            if (bytes >= 2 && buf[bytes - 2] == '\r' && buf[bytes - 1] == '\n') break;
        }
        return resp;
    };

    if (!SslSendCommand("EHLO " + CLIENT_HOSTNAME))
    {
        std::cerr << "Failed to send EHLO over TLS" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "250"))
    {
        std::cerr << "EHLO over TLS not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (!SslSendCommand("AUTH LOGIN"))
    {
        std::cerr << "Failed to send AUTH LOGIN" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "334"))
    {
        std::cerr << "AUTH LOGIN not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (const std::string encodedUser = Base64Encode(SENDER_EMAIL); !SslSendCommand(encodedUser))
    {
        std::cerr << "Failed to send username" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "334"))
    {
        std::cerr << "Username rejected" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (const std::string encodedPass = Base64Encode(SMTP_PASSWORD); !SslSendCommand(encodedPass))
    {
        std::cerr << "Failed to send password" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "235"))
    {
        std::cerr << "Authentication failed" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (!SslSendCommand("MAIL FROM:<" + SENDER_EMAIL + ">"))
    {
        std::cerr << "Failed to send MAIL FROM" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "250"))
    {
        std::cerr << "MAIL FROM not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (!SslSendCommand("RCPT TO:<" + RECIPIENT_EMAIL + ">"))
    {
        std::cerr << "Failed to send RCPT TO" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "250"))
    {
        std::cerr << "RCPT TO not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (!SslSendCommand("DATA"))
    {
        std::cerr << "Failed to send DATA" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "354"))
    {
        std::cerr << "DATA not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    const std::string messageBody = "From: " + SENDER_EMAIL + "\r\n" "To: " + RECIPIENT_EMAIL + "\r\n"
                                    "Subject: Test\r\n" "\r\n" "Hello! This is test message!\r\n" ".\r\n";
    if (const int written = SSL_write(ssl, messageBody.c_str(), static_cast<int>(messageBody.size()));
        written != static_cast<int>(messageBody.size()))
    {
        std::cerr << "Failed to send message body" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;
    if (!ExpectCode(response, "250"))
    {
        std::cerr << "Message not accepted" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    if (!SslSendCommand("QUIT"))
    {
        std::cerr << "Failed to send QUIT" << std::endl;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }
    response = SslReadResponse();
    std::cout << "Server (TLS): " << response;

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return true;
}

int main()
{
    SSL_load_error_strings();
    ERR_load_crypto_strings();

    int socketFd = -1;
    if (!ConnectToSmtpServer(socketFd))
    {
        return 1;
    }

    const bool success = PerformSmtpSession(socketFd);

    close(socketFd);

    if (!success)
    {
        std::cerr << "SMTP session failed" << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Email sent successfully!" << std::endl;
    return EXIT_SUCCESS;
}
