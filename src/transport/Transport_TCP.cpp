#include "../core/pch.h"

#include "Transport_TCP.h"
#include "../helpers/ServiceTools.h"
#include <sstream>

TransportTCP::TransportTCP(const std::string& host, int port)
    : m_host(host),
      m_port(port),
      m_maxConnections(0),
      m_socket(INVALID_SOCKET),
      m_serverSocket(INVALID_SOCKET),
      m_isServer(false),
      m_isOpen(false),
      m_readThreadRunning(false),
      m_acceptThreadRunning(false)
{
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Создан объект TCP-клиента: " + m_host + ":" + std::to_string(m_port));
    InitializeWinsock();
}

TransportTCP::TransportTCP(int port, int maxConnections)
    : m_host(""),
      m_port(port),
      m_maxConnections(maxConnections),
      m_socket(INVALID_SOCKET),
      m_serverSocket(INVALID_SOCKET),
      m_isServer(true),
      m_isOpen(false),
      m_readThreadRunning(false),
      m_acceptThreadRunning(false)
{
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Создан объект TCP-сервера на порту: " + std::to_string(m_port));
    InitializeWinsock();
}

TransportTCP::~TransportTCP()
{
    NEUTRAL_REPORT_DEBUG("TransportTCP", 
                       m_isServer 
                       ? "Уничтожение объекта TCP-сервера на порту: " + std::to_string(m_port)
                       : "Уничтожение объекта TCP-клиента: " + m_host + ":" + std::to_string(m_port));
    Close();
    WSACleanup();
}

bool TransportTCP::InitializeWinsock()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::string errorMsg = "Ошибка инициализации Winsock. Ошибка: " + std::to_string(result);
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, result);
        }
        return false;
    }
    
    return true;
}

bool TransportTCP::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("TransportTCP", "Попытка открыть уже открытое соединение");
        return true;
    }

    if (m_isServer)
    {
        return StartServer();
    }
    else
    {
        return ConnectAsClient();
    }
}

bool TransportTCP::ConnectAsClient()
{
    std::stringstream logMsg;
    logMsg << "Подключение к " << m_host << ":" << m_port;
    NEUTRAL_REPORT_INFO("TransportTCP", logMsg.str());

    struct addrinfo hints = {0};
    struct addrinfo* result = nullptr;
    struct addrinfo* ptr = nullptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Преобразуем номер порта в строку
    std::string portStr = std::to_string(m_port);

    // Получаем адрес
    int iResult = getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result);
    if (iResult != 0)
    {
        std::string errorMsg = "Не удалось получить адрес для: " + m_host + ":" + portStr +
                               ". Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    // Пытаемся подключиться к одному из адресов
    for (ptr = result; ptr != nullptr; ptr = ptr->ai_next)
    {
        // Создаем сокет
        m_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (m_socket == INVALID_SOCKET)
        {
            std::string errorMsg = "Ошибка создания сокета. Ошибка: " + std::to_string(WSAGetLastError());
            NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, WSAGetLastError());
            }
            freeaddrinfo(result);
            return false;
        }

        // Пытаемся подключиться
        iResult = connect(m_socket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (iResult == SOCKET_ERROR)
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            NEUTRAL_REPORT_WARN("TransportTCP", "Не удалось подключиться к адресу. Пробуем следующий.");
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (m_socket == INVALID_SOCKET)
    {
        std::string errorMsg = "Не удалось подключиться к: " + m_host + ":" + portStr;
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    // Устанавливаем флаг "открыт"
    m_isOpen = true;

    // Запускаем поток чтения
    if (!StartReadThread())
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Не удалось запустить поток чтения");
        Close();
        return false;
    }

    NEUTRAL_REPORT_INFO("TransportTCP", "Успешно подключено к: " + m_host + ":" + portStr);
    
    // Уведомляем о изменении состояния соединения
    if (m_connectionStateCallback)
    {
        m_connectionStateCallback(true);
    }
    
    return true;
}

bool TransportTCP::StartServer()
{
    std::string portStr = std::to_string(m_port);
    NEUTRAL_REPORT_INFO("TransportTCP", "Запуск TCP-сервера на порту: " + portStr);

    struct addrinfo hints = {0};
    struct addrinfo* result = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // Получаем адрес для привязки
    int iResult = getaddrinfo(NULL, portStr.c_str(), &hints, &result);
    if (iResult != 0)
    {
        std::string errorMsg = "Не удалось получить адрес для порта: " + portStr +
                               ". Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    // Создаем сокет
    m_serverSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (m_serverSocket == INVALID_SOCKET)
    {
        std::string errorMsg = "Ошибка создания серверного сокета. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        freeaddrinfo(result);
        return false;
    }

    // Привязываем сокет к адресу
    iResult = bind(m_serverSocket, result->ai_addr, (int)result->ai_addrlen);
    freeaddrinfo(result);

    if (iResult == SOCKET_ERROR)
    {
        std::string errorMsg = "Ошибка привязки серверного сокета. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return false;
    }

    // Слушаем входящие подключения
    iResult = listen(m_serverSocket, m_maxConnections);
    if (iResult == SOCKET_ERROR)
    {
        std::string errorMsg = "Ошибка при переходе в режим прослушивания. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return false;
    }

    NEUTRAL_REPORT_INFO("TransportTCP", "Сервер запущен и слушает порт: " + portStr);

    // Запускаем поток принятия соединений
    if (!StartAcceptThread())
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Не удалось запустить поток принятия соединений");
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return false;
    }

    m_isOpen = true;
    
    // Уведомляем о изменении состояния соединения
    if (m_connectionStateCallback)
    {
        m_connectionStateCallback(true);
    }
    
    return true;
}

bool TransportTCP::Close()
{
    if (!m_isOpen)
    {
        return true;
    }

    NEUTRAL_REPORT_INFO("TransportTCP", 
                      m_isServer 
                      ? "Закрытие TCP-сервера на порту: " + std::to_string(m_port)
                      : "Закрытие TCP-соединения: " + m_host + ":" + std::to_string(m_port));

    // Останавливаем потоки
    StopReadThread();
    
    if (m_isServer)
    {
        StopAcceptThread();
    }

    // Закрываем сокеты
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_serverSocket != INVALID_SOCKET)
    {
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
    }

    m_isOpen = false;

    NEUTRAL_REPORT_INFO("TransportTCP", "TCP-соединение закрыто");
    
    // Уведомляем о изменении состояния соединения
    if (m_connectionStateCallback)
    {
        m_connectionStateCallback(false);
    }
    
    return true;
}

bool TransportTCP::IsOpen() const
{
    return m_isOpen;
}

int TransportTCP::Send(const std::vector<uint8_t>& data)
{
    if (!m_isOpen || m_socket == INVALID_SOCKET)
    {
        std::string errorMsg = "Попытка отправить данные через закрытое соединение";
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return -1;
    }

    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("TransportTCP", "Попытка отправить пустые данные");
        return 0;
    }

    // Блокируем mutex для безопасной записи
    std::lock_guard<std::mutex> lock(m_sendMutex);

    int iResult = send(m_socket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
    if (iResult == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        std::string errorMsg = "Ошибка при отправке данных. Ошибка: " + std::to_string(error);
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, error);
        }
        
        // Если ошибка связана с отключением, закрываем соединение
        if (error == WSAECONNRESET || error == WSAECONNABORTED)
        {
            NEUTRAL_REPORT_WARN("TransportTCP", "Соединение разорвано удаленной стороной");
            Close();
        }
        
        return -1;
    }

    NEUTRAL_REPORT_DEBUG("TransportTCP", "Отправлено " + std::to_string(iResult) + " байт");
    return iResult;
}

void TransportTCP::SetDataReceivedCallback(DataReceivedCallback callback)
{
    m_dataReceivedCallback = callback;
}

void TransportTCP::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = callback;
}

void TransportTCP::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = callback;
}

bool TransportTCP::SetTimeout(int timeoutMs)
{
    if (m_socket == INVALID_SOCKET)
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Попытка установить тайм-аут для недействительного сокета");
        return false;
    }

    // Устанавливаем тайм-аут для приема
    DWORD timeout = timeoutMs;
    int result = setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (result == SOCKET_ERROR)
    {
        std::string errorMsg = "Не удалось установить тайм-аут приема. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    // Устанавливаем тайм-аут для отправки
    result = setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    if (result == SOCKET_ERROR)
    {
        std::string errorMsg = "Не удалось установить тайм-аут отправки. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    NEUTRAL_REPORT_DEBUG("TransportTCP", "Установлен тайм-аут: " + std::to_string(timeoutMs) + " мс");
    return true;
}

bool TransportTCP::StartReadThread()
{
    if (m_readThreadRunning)
    {
        return true;
    }

    m_readThreadRunning = true;
    
    try
    {
        m_readThread = std::thread(&TransportTCP::ReadThreadFunction, this);
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения запущен");
        return true;
    }
    catch (const std::exception& e)
    {
        m_readThreadRunning = false;
        std::string errorMsg = "Не удалось создать поток чтения: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return false;
    }
}

void TransportTCP::StopReadThread()
{
    if (!m_readThreadRunning)
    {
        return;
    }

    m_readThreadRunning = false;
    
    if (m_readThread.joinable())
    {
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Ожидание завершения потока чтения");
        m_readThread.join();
    }
    
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения остановлен");
}

void TransportTCP::ReadThreadFunction()
{
    std::vector<char> buffer(READ_BUFFER_SIZE);
    
    while (m_readThreadRunning && m_isOpen && m_socket != INVALID_SOCKET)
    {
        int bytesReceived = recv(m_socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        
        if (bytesReceived > 0)
        {
            NEUTRAL_REPORT_DEBUG("TransportTCP", "Получено " + std::to_string(bytesReceived) + " байт");
            
            if (m_dataReceivedCallback)
            {
                std::vector<uint8_t> receivedData(buffer.begin(), buffer.begin() + bytesReceived);
                m_dataReceivedCallback(receivedData);
            }
        }
        else if (bytesReceived == 0)
        {
            // Соединение закрыто корректно
            NEUTRAL_REPORT_INFO("TransportTCP", "Соединение закрыто удаленной стороной");
            break;
        }
        else
        {
            // Ошибка приема данных
            int error = WSAGetLastError();
            
            // Игнорируем ошибку, если поток был остановлен извне
            if (!m_readThreadRunning)
            {
                break;
            }
            
            // Игнорируем ошибки таймаута
            if (error == WSAETIMEDOUT)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            std::string errorMsg = "Ошибка приема данных. Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
            
            // Если ошибка связана с отключением, выходим из цикла
            if (error == WSAECONNRESET || error == WSAECONNABORTED)
            {
                NEUTRAL_REPORT_WARN("TransportTCP", "Соединение разорвано удаленной стороной");
                break;
            }
            
            // Небольшая пауза, чтобы избежать 100% загрузки CPU при повторяющихся ошибках
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // Закрываем соединение, если оно еще открыто
    if (m_isOpen)
    {
        NEUTRAL_REPORT_INFO("TransportTCP", "Закрытие соединения из потока чтения");
        m_isOpen = false;
        
        // Уведомляем о изменении состояния соединения
        if (m_connectionStateCallback)
        {
            m_connectionStateCallback(false);
        }
    }
    
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения завершен");
}

bool TransportTCP::StartAcceptThread()
{
    if (m_acceptThreadRunning)
    {
        return true;
    }

    m_acceptThreadRunning = true;
    
    try
    {
        m_acceptThread = std::thread(&TransportTCP::AcceptThreadFunction, this);
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток принятия соединений запущен");
        return true;
    }
    catch (const std::exception& e)
    {
        m_acceptThreadRunning = false;
        std::string errorMsg = "Не удалось создать поток принятия соединений: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return false;
    }
}

void TransportTCP::StopAcceptThread()
{
    if (!m_acceptThreadRunning)
    {
        return;
    }

    m_acceptThreadRunning = false;
    
    if (m_acceptThread.joinable())
    {
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Ожидание завершения потока принятия соединений");
        m_acceptThread.join();
    }
    
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток принятия соединений остановлен");
}

void TransportTCP::AcceptThreadFunction()
{
    while (m_acceptThreadRunning && m_isOpen && m_serverSocket != INVALID_SOCKET)
    {
        // Принимаем входящее соединение
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        
        SOCKET clientSocket = accept(m_serverSocket, (sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket == INVALID_SOCKET)
        {
            int error = WSAGetLastError();
            
            // Игнорируем ошибку, если поток был остановлен извне
            if (!m_acceptThreadRunning)
            {
                break;
            }
            
            std::string errorMsg = "Ошибка при принятии соединения. Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
            
            // Небольшая пауза, чтобы избежать 100% загрузки CPU при повторяющихся ошибках
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Получаем информацию о клиенте
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIP, INET_ADDRSTRLEN);
        
        NEUTRAL_REPORT_INFO("TransportTCP", "Принято новое соединение от: " + 
                          std::string(clientIP) + ":" + std::to_string(ntohs(clientAddr.sin_port)));
        
        // Если у нас уже есть клиентский сокет, закрываем его
        if (m_socket != INVALID_SOCKET)
        {
            StopReadThread();
            closesocket(m_socket);
        }
        
        // Сохраняем новый сокет
        m_socket = clientSocket;
        
        // Запускаем поток чтения для нового клиента
        if (!StartReadThread())
        {
            NEUTRAL_REPORT_ERROR("TransportTCP", "Не удалось запустить поток чтения для клиента");
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
    }
    
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток принятия соединений завершен");
}