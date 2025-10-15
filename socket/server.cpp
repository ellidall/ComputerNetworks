#include <iostream>
#include <string>
#include <stdexcept>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr int SERVER_PORT = 5001;
constexpr int SERVER_NUMBER = 500;
const std::string SERVER_NAME = "Alexander's server";

class InvalidMessageException final: public std::runtime_error
{
public:
    explicit InvalidMessageException(const std::string& msg) : std::runtime_error(msg) {}
};

int CreateAndBindSocket(const int port)
{
    const int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock == -1)
    {
        throw std::runtime_error("Не удалось создать сокет");
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == -1)
    {
        close(serverSock);
        throw std::runtime_error("Не удалось привязать сокет к порту " + std::to_string(port));
    }

    if (listen(serverSock, 5) == -1)
    {
        close(serverSock);
        throw std::runtime_error("Не удалось начать прослушивание");
    }

    return serverSock;
}

std::pair<std::string, int> CheckValidClientMessage(const std::string& message)
{
    const size_t newlinePos = message.find('\n');
    if (newlinePos == std::string::npos)
    {
        throw InvalidMessageException("Сообщение не содержит символа новой строки");
    }

    std::string clientName = message.substr(0, newlinePos);
    const std::string numberStr = message.substr(newlinePos + 1);

    if (clientName.empty() || numberStr.empty())
    {
        throw InvalidMessageException("Имя клиента или число пусты");
    }

    int clientNumber = 0;
    try
    {
        clientNumber = std::stoi(numberStr);
    }
    catch (...)
    {
        throw InvalidMessageException("Число клиента не является целым");
    }

    if (clientNumber < 1 || clientNumber > 100)
    {
        throw InvalidMessageException("Число клиента вне диапазона [1,100]: " + std::to_string(clientNumber));
    }

    return {clientName, clientNumber};
}

void ProcessClientMessage(const std::string& clientName, const int clientNumber, const int serverNumber)
{
    std::cout << "\n--- Обработка запроса ---" << std::endl;
    std::cout << "Имя клиента: " << clientName << std::endl;
    std::cout << "Имя сервера: " << SERVER_NAME << std::endl;
    std::cout << "Число клиента: " << clientNumber << std::endl;
    std::cout << "Число сервера: " << serverNumber << std::endl;
    std::cout << "Сумма: " << clientNumber + serverNumber << std::endl;
}

void RunServer()
{
    const int serverSock = CreateAndBindSocket(SERVER_PORT);
    std::cout << "Сервер запущен на порту " << SERVER_PORT << ". Ожидание клиентов..." << std::endl;

    while (true)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        const int clientSock = accept(serverSock, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

        if (clientSock == -1)
        {
            std::cerr << "Предупреждение: не удалось принять подключение" << std::endl;
            continue;
        }

        char buffer[1024];
        const ssize_t bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0)
        {
            std::cerr << "Ошибка: не удалось получить данные от клиента" << std::endl;
            close(clientSock);
            continue;
        }
        buffer[bytesReceived] = '\0';
        std::string message(buffer);

        try
        {
            auto [clientName, clientNumber] = CheckValidClientMessage(message);
            std::string response = SERVER_NAME + "\n" + std::to_string(SERVER_NUMBER);
            send(clientSock, response.c_str(), response.size(), 0);
            ProcessClientMessage(clientName, clientNumber, SERVER_NUMBER);
        }
        catch (const InvalidMessageException& e)
        {
            std::cout << "\nПолучено некорректное сообщение: " << e.what() << std::endl;
            std::cout << "Сервер завершает работу по правилу остановки." << std::endl;

            const std::string errorMessage = "Ошибка: число вне диапазона [1,100]. Сервер завершает работу.";
            send(clientSock, errorMessage.c_str(), errorMessage.size(), 0);

            close(clientSock);
            close(serverSock);
            std::cout << "Сервер остановлен." << std::endl;
            return;
        } catch (const std::exception& e)
        {
            std::cerr << "Ошибка обработки клиента: " << e.what() << std::endl;
        }

        close(clientSock);
        std::cout << "Соединение с клиентом закрыто." << std::endl;
    }
}

int main()
{
    try
    {
        RunServer();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Критическая ошибка сервера: " << e.what() << std::endl;
        return 1;
    }
}
