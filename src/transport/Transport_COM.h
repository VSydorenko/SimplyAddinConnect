#pragma once

#include "Transport.h"
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

/**
 * @class TransportCOM
 * @brief Реализация транспортного слоя для COM-порта
 */
class TransportCOM : public ITransport {
public:
    /**
     * @brief Конструктор
     * @param portName Имя COM-порта (например, "COM1")
     * @param baudRate Скорость передачи данных
     * @param dataBits Количество бит данных
     * @param parity Четность
     * @param stopBits Количество стоповых бит
     */
    TransportCOM(
        const std::string& portName,
        int baudRate = 9600,
        int dataBits = 8,
        char parity = 'N',
        float stopBits = 1.0f
    );
    
    /**
     * @brief Деструктор
     */
    ~TransportCOM() override;
    
    // Реализация методов интерфейса ITransport
    bool Open() override;
    bool Close() override;
    bool IsOpen() const override;
    int Send(const std::vector<uint8_t>& data) override;
    void SetDataReceivedCallback(DataReceivedCallback callback) override;
    void SetErrorCallback(ErrorCallback callback) override;
    void SetConnectionStateCallback(ConnectionStateCallback callback) override;

    /**
     * @brief Возвращает имя текущего COM-порта
     * @return Имя COM-порта (например, "COM1")
     */
    std::string GetPortName() const { return m_portName; }
    
    /**
     * @brief Настроить параметры COM-порта
     * @param baudRate Скорость передачи данных
     * @param dataBits Количество бит данных
     * @param parity Четность
     * @param stopBits Количество стоповых бит
     * @return true если параметры успешно настроены, false в противном случае
     */
    bool ConfigurePort(int baudRate, int dataBits, char parity, float stopBits);
    
    /**
     * @brief Установить таймауты операций чтения/записи
     * @param readIntervalTimeout Интервал чтения
     * @param readTotalTimeoutMultiplier Множитель общего таймаута чтения
     * @param readTotalTimeoutConstant Константа общего таймаута чтения
     * @param writeTotalTimeoutMultiplier Множитель общего таймаута записи
     * @param writeTotalTimeoutConstant Константа общего таймаута записи
     * @return true если таймауты успешно установлены, false в противном случае
     */
    bool SetTimeouts(
        DWORD readIntervalTimeout,
        DWORD readTotalTimeoutMultiplier,
        DWORD readTotalTimeoutConstant,
        DWORD writeTotalTimeoutMultiplier,
        DWORD writeTotalTimeoutConstant
    );

private:
    // Методы для чтения/записи
    void ReadThreadFunction();
    bool StartReadThread();
    void StopReadThread();
    
    // Приватные переменные
    std::string m_portName;
    HANDLE m_portHandle;
    int m_baudRate;
    int m_dataBits;
    char m_parity;
    float m_stopBits;
    
    // Флаги состояния
    std::atomic<bool> m_isOpen;
    std::atomic<bool> m_threadRunning;
    
    // Потоки и синхронизация
    std::thread m_readThread;
    std::mutex m_writeMutex;
    
    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;
    
    // Буфер для чтения
    static constexpr size_t READ_BUFFER_SIZE = 4096;
};