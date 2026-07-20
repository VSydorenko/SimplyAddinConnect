#pragma once

#include "Transport.h"
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

    // --- Тестові шви (лише для wire_selftest; у продакшн-шляху не задіяні) --------
    // Чи ввімкнено авто-реконект у нижнього ix::WebSocket. Конструктор ОБОВ'ЯЗКОВО
    // вимикає його (наш супервізор керує реконектом Close+Open) — юніт це перевіряє.
    bool IsAutomaticReconnectionEnabledForTest() const;
    // Примусово виставити m_started (модель стану «start() викликано, але з'єднання
    // впало remote-close»): дозволяє детерміновано перевірити, що Close все одно
    // кличе stop() (реальний reopen після remote-close залежить від цього).
    void ForceStartedForTest(bool started);
    // Підмінити виклик m_webSocket->stop() лічильником (як SetSendFunctionForTest у
    // TCP/COM): Close має проходити через нього незалежно від m_isOpen/m_isConnecting.
    void SetStopHookForTest(std::function<void()> hook);

private:
    /**
     * @brief Обработчик сообщений от WebSocket
     * @param msgPtr Указатель на полученное сообщение
     */
    void OnMessageCallback(const ix::WebSocketMessagePtr& msgPtr);

    // Остановить нижний ix::WebSocket (или тестовый шов). Вызывается из Close по
    // валидности m_started, а НЕ по m_isOpen (контракт §4.1 п.1).
    void StopWebSocket();
    // Доставить state(false) РОВНО один раз на разрыв (§4.1 п.5): только если было
    // доставлено state(true) в текущем цикле и state(false) ещё не доставлялся.
    // Колбек вызывается ВНЕ внутреннего лока.
    void EmitStateDownIfNeeded();

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
    // Отдельный флаг «start() был вызван» — ресурс (ix-воркер) существует независимо
    // от m_isOpen/m_isConnecting. Close закрывает по нему, чтобы reopen после
    // remote-close реально перезапускал соединение (§9.1).
    std::atomic<bool> m_started;

    // Синхронизация
    std::mutex m_sendMutex;

    // Гейт установления соединения / exactly-once state (§4.1). m_connCv будит
    // ожидающего в Open по факту state(true) или ошибки; m_upDelivered/m_downDelivered
    // гарантируют единичность state(true)/state(false) на цикл соединения.
    std::mutex m_connMutex;
    std::condition_variable m_connCv;
    bool m_upDelivered = false;    // state(true) доставлен в текущем цикле
    bool m_downDelivered = false;  // state(false) доставлен в текущем цикле
    bool m_openFailed = false;     // соединение оборвалось/ошибка — разбудить Open

    // Тестовый шов подмены stop() (см. SetStopHookForTest).
    std::function<void()> m_stopHookForTest;

    // Таймаут соединения
    int m_timeoutSecs;

    // Дополнительные HTTP-заголовки
    ix::WebSocketHttpHeaders m_extraHeaders;
};