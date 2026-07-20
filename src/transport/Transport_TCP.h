#pragma once

#include "Transport.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>

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

    // Тип функции записи в сокет (тестовый шов для all-or-error / partial-write).
    using SendFn = std::function<int(SOCKET, const char*, int)>;

    /**
     * @brief Тестовый шов: подменить функцию записи в сокет (partial-write / провал).
     * @details По умолчанию — ::send. Задаётся ДО активной отправки; используется
     *          только тестами (смоук all-or-error без реального обрыва сети).
     */
    void SetSendFunctionForTest(SendFn fn);

private:
    // Разрешённый адрес (унифицирует numeric-путь и DNS-с-дедлайном).
    struct ResolvedAddr {
        sockaddr_storage addr;
        int addrlen;
        int family;
    };

    // Клиентское подключение (неблокирующий connect + select + SO_ERROR).
    bool ConnectAsClient();
    // Резолв адреса без бесконечной блокировки: numeric (мгновенно) либо
    // GetAddrInfoExW с дедлайном (cancellable). Заполняет out.
    bool ResolveWithDeadline(std::vector<ResolvedAddr>& out);

    // Методы для чтения
    void ReadThreadFunction();
    bool StartReadThread();
    void StopReadThread();

    // Инициализация WSA
    bool InitializeWinsock();

    // state(false) ровно один раз на разрыв (контракт §4.1 п.5).
    void EmitStateDown();

    // Приватные переменные
    std::string m_host;
    int m_port;
    std::atomic<SOCKET> m_socket;

    // Флаги состояния
    std::atomic<bool> m_isOpen;
    std::atomic<bool> m_readThreadRunning;
    std::atomic<bool> m_stateDownEmitted;

    // Потоки и синхронизация
    std::thread m_readThread;
    std::mutex m_sendMutex;   // сериализует Send и защищает Close-порядок (§4.1 п.2)

    // Обратные вызовы
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;

    // Функция записи в сокет (шов; по умолчанию ::send).
    SendFn m_sendFn;

    // Буфер для чтения
    static constexpr size_t READ_BUFFER_SIZE = 4096;
    // Дедлайны (мс): неблокирующий connect и DNS-резолв.
    static constexpr int CONNECT_TIMEOUT_MS = 10000;
    static constexpr int DNS_TIMEOUT_MS = 5000;
};