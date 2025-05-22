#include "../core/pch.h"

#include "Transport_WSClient.h"
#include "../helpers/ServiceTools.h"
#include "extern/ixwebsocket/ixwebsocket/IXNetSystem.h"

TransportWSClient::TransportWSClient(const std::string& url, const std::vector<std::string>& protocols)
    : m_url(url),
      m_protocols(protocols),
      m_webSocket(std::make_unique<ix::WebSocket>()),
      m_isOpen(false),
      m_isConnecting(false),
      m_timeoutSecs(60)
{
    NEUTRAL_REPORT_DEBUG("Transport_WSClient", "Создан объект WebSocket-клиента: " + m_url);
    
    // Инициализация сетевой подсистемы
    ix::initNetSystem();
    
    // Настройка WebSocket
    m_webSocket->setUrl(m_url);
    
    // Добавление протоколов, если они указаны
    for (const auto& protocol : m_protocols)
    {
        m_webSocket->addSubProtocol(protocol);
    }
    
    // Устанавливаем обработчик сообщений
    m_webSocket->setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg) {
            this->OnMessageCallback(msg);
        }
    );
}

TransportWSClient::~TransportWSClient()
{
    NEUTRAL_REPORT_DEBUG("Transport_WSClient", "Уничтожение объекта WebSocket-клиента: " + m_url);
    Close();
    
    // Освобождение сетевой подсистемы
    ix::uninitNetSystem();
}

bool TransportWSClient::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Попытка открыть уже открытое соединение");
        return true;
    }
    
    if (m_isConnecting)
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Соединение уже в процессе установки");
        return false;
    }
    
    NEUTRAL_REPORT_INFO("Transport_WSClient", "Подключение к " + m_url);
    
    m_isConnecting = true;
    
    try
    {
        // Установка таймаута подключения
        m_webSocket->setHandshakeTimeout(m_timeoutSecs);
        
        // Запуск соединения асинхронно
        m_webSocket->start();
        
        // Соединение будет установлено асинхронно, статус изменится в OnMessageCallback
        return true;
    }
    catch (const std::exception& e)
    {
        m_isConnecting = false;
        std::string errorMsg = "Ошибка при открытии WebSocket-соединения: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return false;
    }
}

bool TransportWSClient::Close()
{
    if (!m_isOpen && !m_isConnecting)
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Попытка закрыть неоткрытое соединение");
        return true;
    }
    
    NEUTRAL_REPORT_INFO("Transport_WSClient", "Закрытие соединения WebSocket");
    
    try
    {
        // Остановка WebSocket
        m_webSocket->stop();
        
        m_isOpen = false;
        m_isConnecting = false;
        
        return true;
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "Ошибка при закрытии WebSocket-соединения: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return false;
    }
}

bool TransportWSClient::IsOpen() const
{
    return m_isOpen;
}

int TransportWSClient::Send(const std::vector<uint8_t>& data)
{
    if (!m_isOpen)
    {
        NEUTRAL_REPORT_ERROR("Transport_WSClient", "Попытка отправки данных через неоткрытое соединение");
        return -1;
    }
    
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Попытка отправки пустых данных");
        return 0;
    }
    
    try
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        
        // Создаем копию данных для отправки
        std::string buffer(reinterpret_cast<const char*>(data.data()), data.size());
        
        // Отправляем данные как бинарные
        auto result = m_webSocket->sendBinary(buffer);
        
        if (!result.success)
        {
            std::string errorMsg = "Ошибка при отправке данных: " + result.errorStr;
            NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, -1);
            }
            
            return -1;
        }
        
        return static_cast<int>(data.size());
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "Исключение при отправке данных: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        
        return -1;
    }
}

void TransportWSClient::SetDataReceivedCallback(DataReceivedCallback callback)
{
    m_dataReceivedCallback = callback;
}

void TransportWSClient::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = callback;
}

void TransportWSClient::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = callback;
}

bool TransportWSClient::SetTimeout(int timeoutSecs)
{
    if (timeoutSecs <= 0)
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Некорректное значение таймаута: " + std::to_string(timeoutSecs));
        return false;
    }
    
    m_timeoutSecs = timeoutSecs;
    m_webSocket->setHandshakeTimeout(m_timeoutSecs);
    return true;
}

void TransportWSClient::SetPingInterval(int pingIntervalSecs)
{
    if (pingIntervalSecs <= 0)
    {
        NEUTRAL_REPORT_WARN("Transport_WSClient", "Некорректное значение интервала пинга: " + std::to_string(pingIntervalSecs));
        return;
    }
    
    m_webSocket->setPingInterval(pingIntervalSecs);
}

void TransportWSClient::SetExtraHeaders(const std::map<std::string, std::string>& headers)
{
    // Преобразуем в формат ixwebsocket
    ix::WebSocketHttpHeaders ixHeaders;
    for (const auto& pair : headers)
    {
        ixHeaders[pair.first] = pair.second;
    }
    
    m_extraHeaders = ixHeaders;
    m_webSocket->setExtraHeaders(m_extraHeaders);
}

void TransportWSClient::OnMessageCallback(const ix::WebSocketMessagePtr& msgPtr)
{
    switch (msgPtr->type)
    {
        case ix::WebSocketMessageType::Open:
        {
            m_isOpen = true;
            m_isConnecting = false;
            
            NEUTRAL_REPORT_INFO("Transport_WSClient", "WebSocket-соединение открыто");
            
            if (m_connectionStateCallback)
            {
                m_connectionStateCallback(true);
            }
            break;
        }
        
        case ix::WebSocketMessageType::Close:
        {
            m_isOpen = false;
            m_isConnecting = false;
            
            std::string closeMsg = "WebSocket-соединение закрыто. Код: " + 
                                  std::to_string(msgPtr->closeInfo.code) + 
                                  ", причина: " + msgPtr->closeInfo.reason;
            NEUTRAL_REPORT_INFO("Transport_WSClient", closeMsg);
            
            if (m_connectionStateCallback)
            {
                m_connectionStateCallback(false);
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
            m_isOpen = false;
            m_isConnecting = false;
            
            std::string errorMsg = "Ошибка WebSocket: " + msgPtr->errorInfo.reason;
            NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, msgPtr->errorInfo.retries);
            }
            
            if (m_connectionStateCallback)
            {
                m_connectionStateCallback(false);
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