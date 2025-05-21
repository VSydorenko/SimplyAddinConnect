#pragma once

#include "transport/Transport.h"
#include "extern/ixwebsocket/ixwebsocket/IXWebSocketServer.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <set>

/**
 * @class TransportWSServer
 * @brief Реализация транспортного слоя для WebSocket-сервера
 */
class TransportWSServer : public ITransport {
public:
    /**
     * @brief Конструктор для создания WebSocket-сервера
     * @param port Порт для прослушивания подключений
     * @param host Хост для привязки (по умолчанию "127.0.0.1")
     * @param maxConnections Максимальное количество одновременных подключений (по умолчанию 32)
     */
    TransportWSServer(int port, const std::string& host = "127.0.0.1", int maxConnections = 32);
    
    /**
     * @brief Деструктор
     */
    ~TransportWSServer() override;
    
    // Реализация методов интерфейса ITransport
    bool Open() override;
    bool Close() override;
    bool IsOpen() const override;
    int Send(const std::vector<uint8_t>& data) override;
    void SetDataReceivedCallback(DataReceivedCallback callback) override;
    void SetErrorCallback(ErrorCallback callback) override;
    void SetConnectionStateCallback(ConnectionStateCallback callback) override;
    
    /**
     * @brief Отправить данные конкретному клиенту
     * @param clientId Идентификатор клиента
     * @param data Данные для отправки
     * @return Количество отправленных байт или -1 в случае ошибки
     */
    int SendToClient(const std::string& clientId, const std::vector<uint8_t>& data);
    
    /**
     * @brief Установить интервал отправки пинг-сообщений
     * @param pingIntervalSecs Интервал в секундах
     */
    void SetPingInterval(int pingIntervalSecs);
    
    /**
     * @brief Включить или выключить поддержку сжатия сообщений
     * @param enable true для включения, false для выключения
     */
    void EnableCompression(bool enable);

private:
    /**
     * @brief Обработчик новых подключений
     * @param webSocket Указатель на WebSocket соединение
     * @param connectionState Состояние соединения
     */
    void OnConnectionCallback(std::weak_ptr<ix::WebSocket> webSocket, 
                             std::shared_ptr<ix::ConnectionState> connectionState);
    
    /**
     * @brief Обработчик сообщений от клиентов
     * @param connectionState Состояние соединения
     * @param webSocket Ссылка на WebSocket соединение
     * @param msgPtr Указатель на полученное сообщение
     */
    void OnClientMessageCallback(std::shared_ptr<ix::ConnectionState> connectionState, 
                                ix::WebSocket& webSocket, 
                                const ix::WebSocketMessagePtr& msgPtr);
    
    /**
     * @brief Находит идентификатор клиента по объекту WebSocket
     * @param webSocket Ссылка на WebSocket объект
     * @return Идентификатор клиента или пустую строку, если клиент не найден
     */
    std::string FindClientId(const ix::WebSocket& webSocket);
    
    // Приватные переменные
    std::string m_host;
    int m_port;
    int m_maxConnections;
    std::unique_ptr<ix::WebSocketServer> m_server;
    
    // Флаги состояния
    std::atomic<bool> m_isOpen;
    
    // Потоки и синхронизация
    std::mutex m_clientsMutex;
    std::mutex m_sendMutex;
    
    // Хранение клиентских соединений (WebSocket, clientId)
    std::map<std::shared_ptr<ix::WebSocket>, std::string> m_clients;
    
    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;
};