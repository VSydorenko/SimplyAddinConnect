#pragma once

#include "Transport.h"
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

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

    // Тип функции записи в порт (тестовый шов для all-or-error / partial-write).
    using WriteFn = std::function<BOOL(HANDLE, const void*, DWORD, DWORD*)>;

    /**
     * @brief Тестовый шов: подменить функцию записи в порт (partial-write / провал).
     * @details По умолчанию — WriteFile. Задаётся ДО активной отправки; используется
     *          только тестами (смоук all-or-error без реального оборудования).
     */
    void SetWriteFunctionForTest(WriteFn fn);

    /**
     * @brief Тестовый хук: привязать готовый хендл и пометить порт открытым.
     * @details Без реального COM-порта (com0com недоступен в CI) — чтобы проверить
     *          all-or-error Send над шовом записи. Reader НЕ запускается. Хендл
     *          закроет Close()/деструктор. Только для тестов.
     */
    void AttachHandleForTest(HANDLE h);

    /**
     * @brief Тестовый хук: текущее значение хендла порта (для проверки cleanup).
     */
    HANDLE GetHandleForTest() const { return m_portHandle.load(); }

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

    // state(false) ровно один раз на разрыв (контракт §4.1 п.5).
    void EmitStateDown();

    // Приватные переменные
    std::string m_portName;
    std::atomic<HANDLE> m_portHandle;   // атомарный: Close сбрасывает, reader читает (§4.1 п.3)
    int m_baudRate;
    int m_dataBits;
    char m_parity;
    float m_stopBits;

    // Флаги состояния
    std::atomic<bool> m_isOpen;
    std::atomic<bool> m_threadRunning;
    std::atomic<bool> m_stateDownEmitted;

    // Потоки и синхронизация
    std::thread m_readThread;
    std::mutex m_writeMutex;   // сериализует Send и защищает Close-порядок (§4.1 п.2)

    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;

    // Функция записи в порт (шов; по умолчанию WriteFile).
    WriteFn m_writeFn;

    // Буфер для чтения
    static constexpr size_t READ_BUFFER_SIZE = 4096;
};