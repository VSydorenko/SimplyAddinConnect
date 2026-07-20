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
      m_started(false),
      m_timeoutSecs(60)
{
    NEUTRAL_REPORT_DEBUG("Transport_WSClient", "Создан объект WebSocket-клиента: " + m_url);

    // Инициализация сетевой подсистемы
    ix::initNetSystem();

    // Настройка WebSocket
    m_webSocket->setUrl(m_url);

    // Реконектом управляет супервизор DeviceSession (Close+Open), а не сам ix:
    // встроенный авто-реконект ix (по умолчанию ВКЛ) конфликтовал бы с нашим
    // жизненным циклом и ломал exactly-once state (§9.1, §4.1).
    m_webSocket->disableAutomaticReconnection();

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

    // Сброс флагов цикла соединения ДО start() (иначе доставленный воркером Open
    // мог бы прийти раньше сброса и «потеряться»).
    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        m_upDelivered = false;
        m_downDelivered = false;
        m_openFailed = false;
    }

    m_isConnecting = true;

    try
    {
        // Установка таймаута подключения
        m_webSocket->setHandshakeTimeout(m_timeoutSecs);

        // #W1 (§4.1): Open для WS — НЕблокирующий. Возвращаем true СРАЗУ после start()
        // (конект ИНИЦИИРОВАН), не ожидая хендшейка. Иначе поток 1С мёрзнул бы до
        // m_timeoutSecs (~60с) на m_connCv против недостижимого/black-hole хоста, а
        // Stop() (джойнит супервизор ДО Close()) вис бы, и connectDeadlineMs
        // становился неэффективным. Фактический конект подтверждается АСИНХРОННО через
        // Open-сообщение воркера → OnMessageCallback → ConnectionState(true); гейтит его
        // уже DeviceSession по connectDeadlineMs (Start/ReconnectLoop). m_started
        // фиксирует, что ресурс (воркер) существует — именно по нему Close() решает
        // вызывать stop() (а не по m_isOpen).
        m_webSocket->start();
        m_started = true;
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
    // §4.1 п.1: идемпотентность по ВАЛИДНОСТИ ресурса (ix-воркер = m_started), а НЕ по
    // m_isOpen. После remote-close m_isOpen==false, но воркер ещё запущен — stop() всё
    // равно нужен, иначе последующий Open (reopen) не перезапустит соединение (§9.1).
    if (!m_started.exchange(false))
    {
        return true;  // start() не вызывался (или уже остановлен) — нечего закрывать
    }

    NEUTRAL_REPORT_INFO("Transport_WSClient", "Закрытие соединения WebSocket");

    try
    {
        // §4.1 п.2: синхронизация с Send через m_sendMutex — не остановить воркер
        // посреди активной отправки. stop() джойнит ix-воркер: после его возврата ни
        // один OnMessageCallback больше не стартует (§4.1 п.4).
        {
            std::lock_guard<std::mutex> lk(m_sendMutex);
            StopWebSocket();
        }

        m_isOpen = false;
        m_isConnecting = false;

        // §4.1 п.5: exactly-once state(false). Если stop() уже прогнал OnMessageCallback
        // с Close/Error, тот доставил state(false) — здесь будет no-op (гейт m_downDelivered).
        EmitStateDownIfNeeded();

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

void TransportWSClient::StopWebSocket()
{
    if (m_stopHookForTest)
    {
        m_stopHookForTest();  // тестовый шов: считаем вызов, реальный stop не трогаем
        return;
    }
    m_webSocket->stop();
}

void TransportWSClient::EmitStateDownIfNeeded()
{
    bool emit = false;
    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        if (m_upDelivered && !m_downDelivered)
        {
            m_downDelivered = true;
            emit = true;
        }
        cb = m_connectionStateCallback;
    }
    m_connCv.notify_all();  // разбудить ожидающего в Open (если разрыв на этапе конекта)
    if (emit && cb)
    {
        cb(false);  // ВНЕ лока (§4.1: колбеки не из-под внутреннего лока)
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
            std::string errorMsg = "Ошибка при отправке данных";
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
            // state(true) РОВНО один раз на цикл (§4.1 п.5). Флаги — под m_connMutex,
            // колбек — вне лока. Разбудить ожидающего в Open (state-based успех).
            bool emit = false;
            ConnectionStateCallback cb;
            {
                std::lock_guard<std::mutex> lk(m_connMutex);
                m_isOpen = true;
                m_isConnecting = false;
                if (!m_upDelivered)
                {
                    m_upDelivered = true;
                    emit = true;
                }
                cb = m_connectionStateCallback;
            }
            m_connCv.notify_all();

            NEUTRAL_REPORT_INFO("Transport_WSClient", "WebSocket-соединение открыто");

            if (emit && cb)
            {
                cb(true);
            }
            break;
        }

        case ix::WebSocketMessageType::Close:
        {
            std::string closeMsg = "WebSocket-соединение закрыто. Код: " +
                                  std::to_string(msgPtr->closeInfo.code) +
                                  ", причина: " + msgPtr->closeInfo.reason;
            NEUTRAL_REPORT_INFO("Transport_WSClient", closeMsg);

            // m_openFailed=true будит Open, если разрыв случился на этапе конекта.
            {
                std::lock_guard<std::mutex> lk(m_connMutex);
                m_isOpen = false;
                m_isConnecting = false;
                m_openFailed = true;
            }
            // exactly-once state(false): только если было state(true) в этом цикле.
            EmitStateDownIfNeeded();
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
            std::string errorMsg = "Ошибка WebSocket: " + msgPtr->errorInfo.reason;
            NEUTRAL_REPORT_ERROR("Transport_WSClient", errorMsg);

            ErrorCallback ecb;
            {
                std::lock_guard<std::mutex> lk(m_connMutex);
                m_isOpen = false;
                m_isConnecting = false;
                m_openFailed = true;  // разбудить Open — конект не удался
                ecb = m_errorCallback;
            }
            m_connCv.notify_all();

            if (ecb)
            {
                ecb(errorMsg, msgPtr->errorInfo.retries);
            }
            // state(false) — только если ранее было state(true) (§4.1 п.5).
            EmitStateDownIfNeeded();
            break;
        }

        case ix::WebSocketMessageType::Ping:
        case ix::WebSocketMessageType::Pong:
        case ix::WebSocketMessageType::Fragment:
            // Эти сообщения обрабатываются автоматически библиотекой
            break;
    }
}

// --- Тестовые швы ------------------------------------------------------------

bool TransportWSClient::IsAutomaticReconnectionEnabledForTest() const
{
    return m_webSocket->isAutomaticReconnectionEnabled();
}

void TransportWSClient::ForceStartedForTest(bool started)
{
    m_started = started;
}

void TransportWSClient::SetStopHookForTest(std::function<void()> hook)
{
    m_stopHookForTest = std::move(hook);
}