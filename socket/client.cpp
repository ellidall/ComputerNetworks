#include <iostream>
#include <string>
#include <stdexcept>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr int SERVER_PORT = 5001;
constexpr auto SERVER_IP = "127.0.0.1";
const std::string CLIENT_NAME = "Client Alexander";

class ClientException final: public std::runtime_error
{
public:
    explicit ClientException(const std::string& msg) : std::runtime_error(msg) {}
};

int CreateClientSocket()
{
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        throw ClientException("Не удалось создать сокет");
    }
    return sock;
}

void ConnectToServer(const int sock, const char* ip, int port)
{
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0)
    {
        close(sock);
        throw ClientException("Неверный IP-адрес сервера");
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == -1)
    {
        close(sock);
        throw ClientException("Не удалось подключиться к серверу");
    }
}

int InteractWithUser()
{
    int userNumber;
    std::cout << "Введите целое число от 1 до 100: ";
    std::cin >> userNumber;
    return userNumber;
}

void SendClientMessage(const int sock, const std::string& clientName, const int number)
{
    const std::string message = clientName + "\n" + std::to_string(number);
    if (send(sock, message.c_str(), message.size(), 0) == -1)
    {
        throw ClientException("Не удалось отправить сообщение серверу");
    }
}

std::pair<std::string, int> ParseServerResponse(const int sock)
{
    char buffer[1024];
    const ssize_t bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0)
    {
        throw ClientException("Не удалось получить ответ от сервера");
    }
    buffer[bytesReceived] = '\0';

    std::string response(buffer);
    const size_t newlinePos = response.find('\n');
    if (newlinePos == std::string::npos)
    {
        throw ClientException("Некорректный формат ответа от сервера");
    }

    std::string serverName = response.substr(0, newlinePos);
    const std::string numberStr = response.substr(newlinePos + 1);

    try
    {
        int serverNumber = std::stoi(numberStr);
        return {serverName, serverNumber};
    }
    catch (const std::exception&)
    {
        throw ClientException("Сервер прислал некорректное число");
    }
}

void PrintResult(const std::string& clientName, const std::string& serverName, int clientNumber, int serverNumber)
{
    std::cout << "\n--- Результат ---" << std::endl;
    std::cout << "Имя клиента: " << clientName << std::endl;
    std::cout << "Имя сервера: " << serverName << std::endl;
    std::cout << "Число клиента: " << clientNumber << std::endl;
    std::cout << "Число сервера: " << serverNumber << std::endl;
    std::cout << "Сумма: " << clientNumber + serverNumber << std::endl;
}

void ProcessServerResponse(const std::string& serverResponse, const int clientNumber) {
    const size_t newlinePos = serverResponse.find('\n');
    if (newlinePos == std::string::npos) {
        std::cout << "\n--- Сервер сообщил об ошибке ---" << std::endl;
        std::cout << serverResponse << std::endl;
        return;
    }

    const std::string serverName = serverResponse.substr(0, newlinePos);
    const std::string numberStr = serverResponse.substr(newlinePos + 1);

    int serverNumber = 0;
    try {
        serverNumber = std::stoi(numberStr);
    } catch (...) {
        std::cerr << "Ошибка: сервер прислал некорректное число" << std::endl;
    }

    PrintResult(CLIENT_NAME, serverName, clientNumber, serverNumber);
}

int RunClient()
{
    try
    {
        const int userNumber = InteractWithUser();
        const int sock = CreateClientSocket();
        std::cout << "Сокет создан." << std::endl;

        ConnectToServer(sock, SERVER_IP, SERVER_PORT);
        std::cout << "Подключено к серверу." << std::endl;

        SendClientMessage(sock, CLIENT_NAME, userNumber);
        std::cout << "Сообщение отправлено." << std::endl;

        char buffer[1024];
        const ssize_t bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            std::cerr << "Ошибка: сервер разорвал соединение без ответа." << std::endl;
            close(sock);
            return 1;
        }
        buffer[bytesReceived] = '\0';
        const std::string serverResponse(buffer);

        ProcessServerResponse(serverResponse, userNumber);

        close(sock);
        std::cout << "Сокет закрыт. Работа завершена." << std::endl;
        return 0;

    }
    catch (const ClientException& e)
    {
        std::cerr << "Ошибка клиента: " << e.what() << std::endl;
        return 1;
    }
}

int main()
{
    try
    {
        return RunClient();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Неизвестная ошибка: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
