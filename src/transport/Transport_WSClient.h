#pragma once

#include "transport/Transport.h"
#include "extern/ixwebsocket/ixwebsocket/IXWebSocket.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>

/**
 * @class TransportWSClient
 * @brief Реализация транспортного слоя для WebSocket-клиента
 */
class TransportWSClient : public ITransport {
public:
    /**
     * @brief Конструктор клиентского WebSocket-соединения
     * @param url URL для подключения
     * @param protocols Список поддерживаемых протоколов (необязательно)
     */
    TransportWSClient(const std::string& url, const std::vector<std::string>& protocols = {});
    
    /**
     * @brief Деструктор
     */
    ~TransportWSClient() override;
    
    // Реализация методов интерфейса ITransport
    bool Open() override;
    bool Close() override;
    bool IsOpen() const override;
    int Send(const std::vector<uint8_t>& data) override;
    void SetDataReceivedCallback(DataReceivedCallback callback) override;
    void SetErrorCallback(ErrorCallback callback) override;
    void SetConnectionStateCallback(ConnectionStateCallback callback) override;
    
    /**
     * @brief Установить таймаут соединения
     * @param timeoutSecs Таймаут в секундах
     * @return true если таймаут успешно установлен, false в противном случае
     */
    bool SetTimeout(int timeoutSecs);
    
    /**
     * @brief Установить интервал отправки пинг-сообщений
     * @param pingIntervalSecs Интервал в секундах
     */
    void SetPingInterval(int pingIntervalSecs);
    
    /**
     * @brief Установить дополнительные HTTP-заголовки для подключения
     * @param headers Карта HTTP-заголовков
     */
    void SetExtraHeaders(const std::map<std::string, std::string>& headers);

private:
    /**
     * @brief Обработчик сообщений от WebSocket
     * @param msgPtr Указатель на полученное сообщение
     */
    void OnMessageCallback(const ix::WebSocketMessagePtr& msgPtr);
    
    // Приватные переменные
    std::string m_url;
    std::vector<std::string> m_protocols;
    std::unique_ptr<ix::WebSocket> m_webSocket;
    
    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;
    
    // Состояние соединения
    std::atomic<bool> m_isOpen;
    std::atomic<bool> m_isConnecting;
    
    // Синхронизация
    std::mutex m_sendMutex;
    
    // Таймаут соединения
    int m_timeoutSecs;
    
    // Дополнительные HTTP-заголовки
    ix::WebSocketHttpHeaders m_extraHeaders;
};