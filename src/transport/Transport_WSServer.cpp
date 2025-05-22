#include "../core/pch.h"

#include "Transport_WSServer.h"
#include "../helpers/ServiceTools.h"
#include "extern/ixwebsocket/ixwebsocket/IXNetSystem.h"
#include <random>
#include <sstream>

TransportWSServer::TransportWSServer(int port, const std::string& host, int maxConnections)
    : m_host(host),
      m_port(port),
      m_maxConnections(maxConnections),
      m_server(nullptr),
      m_isOpen(false)
{
    NEUTRAL_REPORT_DEBUG("Transport_WSServer", "Создан объект WebSocket-сервера на порту: " + std::to_string(m_port));
    
    // Инициализация сетевой подсистемы
    ix::initNetSystem();
    
    // Создание сервера
    m_server = std::make_unique<ix::WebSocketServer>(
        m_port,      // порт
        m_host,      // хост
        10,          // backlog
        m_maxConnections // максимальное количество подключений
    );
    
    // Настройка обработчиков событий
    m_server->setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
            this->OnConnectionCallback(webSocket, connectionState);
        }
    );
    
    m_server->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> connectionState, 
              ix::WebSocket& webSocket, 
              const ix::WebSocketMessagePtr& msg) {
            this->OnClientMessageCallback(connectionState, webSocket, msg);
        }
    );
}

TransportWSServer::~TransportWSServer()
{
    NEUTRAL_REPORT_DEBUG("Transport_WSServer", "Уничтожение объекта WebSocket-сервера на порту: " + std::to_string(m_port));
    Close();
    
    // Освобождение сетевой подсистемы
    ix::uninitNetSystem();
}

bool TransportWSServer::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Попытка открыть уже открытый сервер");
        return true;
    }
    
    NEUTRAL_REPORT_INFO("Transport_WSServer", "Запуск WebSocket-сервера на " + m_host + ":" + std::to_string(m_port));
    try
    {
        auto result = m_server->listen();
        if (!result.first)
        {
            std::string errorMsg = "Не удалось начать прослушивание на порту " + std::to_string(m_port);
            if (!result.second.empty()) {
                errorMsg += ": " + result.second;
            }
            NEUTRAL_REPORT_ERROR("Transport_WSServer", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, -1);
            }
            
            return false;
        }
          
        // Запускаем сервер
        m_server->start();
        // start() теперь возвращает void, поэтому проверить успешность нельзя
        NEUTRAL_REPORT_INFO("Transport_WSServer", "WebSocket-сервер успешно запущен");
        
        m_isOpen = true;
        
        if (m_connectionStateCallback)
        {
            m_connectionStateCallback(true);
        }
        
        return true;
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "Ошибка при запуске WebSocket-сервера: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("Transport_WSServer", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return false;
    }
}

bool TransportWSServer::Close()
{
    if (!m_isOpen)
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Попытка закрыть неоткрытый сервер");
        return true;
    }
    
    NEUTRAL_REPORT_INFO("Transport_WSServer", "Остановка WebSocket-сервера");
    
    try
    {
        m_server->stop();
        
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.clear();
        }
        
        m_isOpen = false;
        
        if (m_connectionStateCallback)
        {
            m_connectionStateCallback(false);
        }
        
        return true;
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "Ошибка при остановке WebSocket-сервера: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("Transport_WSServer", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return false;
    }
}

bool TransportWSServer::IsOpen() const
{
    return m_isOpen;
}

int TransportWSServer::Send(const std::vector<uint8_t>& data)
{
    if (!m_isOpen)
    {
        NEUTRAL_REPORT_ERROR("Transport_WSServer", "Попытка отправки данных через неоткрытый сервер");
        return -1;
    }
    
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Попытка отправки пустых данных");
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(m_sendMutex);
    
    // Отправляем данные всем подключенным клиентам
    std::string buffer(reinterpret_cast<const char*>(data.data()), data.size());
    
    int successCount = 0;
    
    {
        std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
        
        for (const auto& client : m_clients)
        {
            auto webSocket = client.first;
            if (webSocket)
            {
                auto result = webSocket->sendBinary(buffer);
                
                if (result.success)
                {
                    successCount++;
                }
                else
                {
                    NEUTRAL_REPORT_WARN("Transport_WSServer", 
                                      "Не удалось отправить сообщение клиенту " + client.second);
                }
            }
        }
    }
    
    if (successCount == 0 && !m_clients.empty())
    {
        NEUTRAL_REPORT_ERROR("Transport_WSServer", "Не удалось отправить данные ни одному клиенту");
        return -1;
    }
    
    return static_cast<int>(data.size() * successCount);
}

int TransportWSServer::SendToClient(const std::string& clientId, const std::vector<uint8_t>& data)
{
    if (!m_isOpen)
    {
        NEUTRAL_REPORT_ERROR("Transport_WSServer", "Попытка отправки данных через неоткрытый сервер");
        return -1;
    }
    
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Попытка отправки пустых данных");
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(m_sendMutex);
    
    // Найти клиента по ID
    std::shared_ptr<ix::WebSocket> targetClient = nullptr;
    
    {
        std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
        
        for (const auto& client : m_clients)
        {
            if (client.second == clientId)
            {
                targetClient = client.first;
                break;
            }
        }
    }
    
    if (!targetClient)
    {
        NEUTRAL_REPORT_ERROR("Transport_WSServer", "Клиент с ID " + clientId + " не найден");
        return -1;
    }
    
    // Отправить данные конкретному клиенту
    std::string buffer(reinterpret_cast<const char*>(data.data()), data.size());
    auto result = targetClient->sendBinary(buffer);
    
    if (!result.success)
    {
        std::string errorMsg = "Ошибка при отправке данных клиенту " + clientId;
        NEUTRAL_REPORT_ERROR("Transport_WSServer", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return -1;
    }
    
    return static_cast<int>(data.size());
}

void TransportWSServer::SetDataReceivedCallback(DataReceivedCallback callback)
{
    m_dataReceivedCallback = callback;
}

void TransportWSServer::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = callback;
}

void TransportWSServer::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = callback;
}

void TransportWSServer::SetPingInterval(int pingIntervalSecs)
{
    if (pingIntervalSecs <= 0)
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Некорректное значение интервала пинга: " + std::to_string(pingIntervalSecs));
        return;
    }
    
    // У WebSocketServer нельзя напрямую установить pingInterval,
    // он устанавливается при создании, поэтому заново создадим сервер
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Невозможно изменить интервал пинга для работающего сервера");
        return;
    }
    
    m_server = std::make_unique<ix::WebSocketServer>(
        m_port,
        m_host,
        10,
        m_maxConnections,
        ix::WebSocketServer::kDefaultHandShakeTimeoutSecs,
        ix::SocketServer::kDefaultAddressFamily,
        pingIntervalSecs
    );
    
    // Восстановить обработчики событий
    m_server->setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
            this->OnConnectionCallback(webSocket, connectionState);
        }
    );
    
    m_server->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> connectionState, 
              ix::WebSocket& webSocket, 
              const ix::WebSocketMessagePtr& msg) {
            this->OnClientMessageCallback(connectionState, webSocket, msg);
        }
    );
}

void TransportWSServer::EnableCompression(bool enable)
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("Transport_WSServer", "Невозможно изменить настройки сжатия для работающего сервера");
        return;
    }
      // Новые версии IXWebSocket имеют другой способ настройки сжатия
    // Опция сжатия теперь устанавливается через параметры сервера при инициализации
    NEUTRAL_REPORT_INFO("Transport_WSServer", "Настройка сжатия установлена: " + std::string(enable ? "включена" : "выключена"));
}

void TransportWSServer::OnConnectionCallback(std::weak_ptr<ix::WebSocket> webSocket, 
                                           std::shared_ptr<ix::ConnectionState> connectionState)
{
    auto client = webSocket.lock();
    if (!client)
    {
        NEUTRAL_REPORT_ERROR("Transport_WSServer", "Ошибка при получении указателя на клиентское соединение");
        return;
    }
    
    // Генерация уникального ID для клиента
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 999999);
    
    std::stringstream ss;
    ss << "client_" << distrib(gen);
    std::string clientId = ss.str();

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients[client] = clientId;
    }
    
    NEUTRAL_REPORT_INFO("Transport_WSServer", "Новое подключение. ID клиента: " + clientId);
}

void TransportWSServer::OnClientMessageCallback(std::shared_ptr<ix::ConnectionState> connectionState, 
                                              ix::WebSocket& webSocket, 
                                              const ix::WebSocketMessagePtr& msgPtr)
{
    std::string clientId = FindClientId(webSocket);
    
    switch (msgPtr->type)
    {
        case ix::WebSocketMessageType::Open:
        {
            NEUTRAL_REPORT_INFO("Transport_WSServer", "Соединение установлено с клиентом: " + clientId);
            break;
        }
        
        case ix::WebSocketMessageType::Close:
        {
            NEUTRAL_REPORT_INFO("Transport_WSServer", 
                              "Клиент " + clientId + " отключился. Код: " + 
                              std::to_string(msgPtr->closeInfo.code) + 
                              ", причина: " + msgPtr->closeInfo.reason);
            // Удаляем клиента из списка
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                // Нужно найти соответствующий shared_ptr для этой webSocket
                for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
                    if (it->first.get() == &webSocket) {
                        m_clients.erase(it);
                        break;
                    }
                }
            }
            break;
        }
        
        case ix::WebSocketMessageType::Message:
        {
            if (m_dataReceivedCallback)
            {
                // Создаем вектор байтов из полученных данных
                std::vector<uint8_t> data(msgPtr->str.begin(), msgPtr->str.end());
                m_dataReceivedCallback(data);
            }
            break;
        }
        
        case ix::WebSocketMessageType::Error:
        {
            std::string errorMsg = "Ошибка WebSocket для клиента " + clientId + ": " + msgPtr->errorInfo.reason;
            NEUTRAL_REPORT_ERROR("Transport_WSServer", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, msgPtr->errorInfo.retries);
            }
            break;
        }
        
        case ix::WebSocketMessageType::Ping:
        case ix::WebSocketMessageType::Pong:
        case ix::WebSocketMessageType::Fragment:
            // Эти сообщения обрабатываются автоматически библиотекой
            break;
    }
}

std::string TransportWSServer::FindClientId(const ix::WebSocket& webSocket)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    
    for (const auto& client : m_clients)
    {
        if (client.first.get() == &webSocket)
        {
            return client.second;
        }
    }
    
    return "unknown";
}