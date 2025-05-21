#pragma once

#include "Transport.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

/**
 * @class TransportTCP
 * @brief Реализация транспортного слоя для TCP-соединения
 */
class TransportTCP : public ITransport {
public:
    /**
     * @brief Конструктор клиентского соединения
     * @param host Имя хоста или IP-адрес
     * @param port Номер порта
     */
    TransportTCP(const std::string& host, int port);
    
    /**
     * @brief Конструктор серверного соединения
     * @param port Номер порта для прослушивания
     * @param maxConnections Максимальное число соединений
     */
    explicit TransportTCP(int port, int maxConnections = 5);
    
    /**
     * @brief Деструктор
     */
    ~TransportTCP() override;
    
    // Реализация методов интерфейса ITransport
    bool Open() override;
    bool Close() override;
    bool IsOpen() const override;
    int Send(const std::vector<uint8_t>& data) override;
    void SetDataReceivedCallback(DataReceivedCallback callback) override;
    void SetErrorCallback(ErrorCallback callback) override;
    void SetConnectionStateCallback(ConnectionStateCallback callback) override;
    
    /**
     * @brief Установить тайм-аут соединения
     * @param timeoutMs Тайм-аут в миллисекундах
     * @return true если тайм-аут успешно установлен, false в противном случае
     */
    bool SetTimeout(int timeoutMs);

private:
    // Методы для режимов клиент/сервер
    bool ConnectAsClient();
    bool StartServer();
    
    // Методы для чтения/записи
    void ReadThreadFunction();
    bool StartReadThread();
    void StopReadThread();
    
    // Серверные методы
    void AcceptThreadFunction();
    bool StartAcceptThread();
    void StopAcceptThread();
    
    // Инициализация WSA
    bool InitializeWinsock();
    
    // Приватные переменные
    std::string m_host;
    int m_port;
    int m_maxConnections;
    SOCKET m_socket;
    SOCKET m_serverSocket;
    bool m_isServer;
    
    // Флаги состояния
    std::atomic<bool> m_isOpen;
    std::atomic<bool> m_readThreadRunning;
    std::atomic<bool> m_acceptThreadRunning;
    
    // Потоки и синхронизация
    std::thread m_readThread;
    std::thread m_acceptThread;
    std::mutex m_sendMutex;
    
    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;
    
    // Буфер для чтения
    static constexpr size_t READ_BUFFER_SIZE = 4096;
};