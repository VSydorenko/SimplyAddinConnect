#include "../core/pch.h"

#include "Transport_TCP.h"
#include "../helpers/ServiceTools.h"
#include <cstring>

TransportTCP::TransportTCP(const std::string& host, int port)
    : m_host(host),
      m_port(port),
      m_socket(INVALID_SOCKET),
      m_isOpen(false),
      m_readThreadRunning(false),
      m_stateDownEmitted(false),
      m_sendFn([](SOCKET s, const char* buf, int len) { return ::send(s, buf, len, 0); })
{
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Создан объект TCP-клиента: " + m_host + ":" + std::to_string(m_port));
    InitializeWinsock();
}

TransportTCP::~TransportTCP()
{
    NEUTRAL_REPORT_DEBUG("TransportTCP",
                       "Уничтожение объекта TCP-клиента: " + m_host + ":" + std::to_string(m_port));
    Close();
    WSACleanup();
}

bool TransportTCP::InitializeWinsock()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::string errorMsg = "Ошибка инициализации Winsock. Ошибка: " + std::to_string(result);
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, result);
        }
        return false;
    }
    
    return true;
}

bool TransportTCP::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("TransportTCP", "Попытка открыть уже открытое соединение");
        return true;
    }

    return ConnectAsClient();
}

bool TransportTCP::ResolveWithDeadline(std::vector<ResolvedAddr>& out)
{
    out.clear();
    std::string portStr = std::to_string(m_port);

    // 1) Быстрый numeric-путь (без DNS, мгновенно) — не блокирует поток.
    {
        struct addrinfo hints = {0};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_NUMERICHOST;   // только числовой адрес, без резолва имён

        struct addrinfo* result = nullptr;
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result) == 0)
        {
            for (struct addrinfo* p = result; p != nullptr; p = p->ai_next)
            {
                if (p->ai_addrlen == 0 || p->ai_addrlen > sizeof(sockaddr_storage)) continue;
                ResolvedAddr ra{};
                std::memcpy(&ra.addr, p->ai_addr, p->ai_addrlen);
                ra.addrlen = static_cast<int>(p->ai_addrlen);
                ra.family = p->ai_family;
                out.push_back(ra);
            }
            freeaddrinfo(result);
            if (!out.empty()) return true;
        }
    }

    // 2) Имя хоста → DNS с дедлайном через cancellable GetAddrInfoExW.
    std::wstring wHost(m_host.begin(), m_host.end());
    std::wstring wPort(portStr.begin(), portStr.end());

    ADDRINFOEXW hintsEx = {0};
    hintsEx.ai_family = AF_UNSPEC;
    hintsEx.ai_socktype = SOCK_STREAM;
    hintsEx.ai_protocol = IPPROTO_TCP;

    PADDRINFOEXW resultEx = nullptr;
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr)
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Не удалось создать событие для DNS-резолва");
        return false;
    }

    HANDLE cancelHandle = nullptr;
    int rc = GetAddrInfoExW(wHost.c_str(), wPort.c_str(), NS_ALL, nullptr,
                            &hintsEx, &resultEx, nullptr, &overlapped,
                            nullptr, &cancelHandle);

    bool ok = false;
    if (rc == 0)
    {
        ok = true;   // синхронно завершился
    }
    else if (rc == WSA_IO_PENDING)
    {
        DWORD wr = WaitForSingleObject(overlapped.hEvent, DNS_TIMEOUT_MS);
        if (wr == WAIT_OBJECT_0)
        {
            int err = GetAddrInfoExOverlappedResult(&overlapped);
            ok = (err == 0);
        }
        else
        {
            // Дедлайн истёк — отменяем и не «висим».
            if (cancelHandle) GetAddrInfoExCancel(&cancelHandle);
            WaitForSingleObject(overlapped.hEvent, INFINITE);   // дождаться завершения отмены
            GetAddrInfoExOverlappedResult(&overlapped);
            NEUTRAL_REPORT_ERROR("TransportTCP",
                                 "DNS-резолв превысил дедлайн для: " + m_host);
        }
    }

    if (ok && resultEx)
    {
        for (PADDRINFOEXW p = resultEx; p != nullptr; p = p->ai_next)
        {
            if (p->ai_addrlen == 0 || p->ai_addrlen > sizeof(sockaddr_storage)) continue;
            ResolvedAddr ra{};
            std::memcpy(&ra.addr, p->ai_addr, p->ai_addrlen);
            ra.addrlen = static_cast<int>(p->ai_addrlen);
            ra.family = p->ai_family;
            out.push_back(ra);
        }
    }
    if (resultEx) FreeAddrInfoExW(resultEx);
    CloseHandle(overlapped.hEvent);

    if (out.empty())
    {
        std::string errorMsg = "Не удалось получить адрес для: " + m_host + ":" + portStr;
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        if (m_errorCallback) m_errorCallback(errorMsg, WSAGetLastError());
        return false;
    }
    return true;
}

bool TransportTCP::ConnectAsClient()
{
    std::string portStr = std::to_string(m_port);
    NEUTRAL_REPORT_INFO("TransportTCP", "Подключение к " + m_host + ":" + portStr);

    std::vector<ResolvedAddr> addrs;
    if (!ResolveWithDeadline(addrs))
    {
        return false;   // сообщение об ошибке уже выдано
    }

    SOCKET sock = INVALID_SOCKET;
    for (const ResolvedAddr& ra : addrs)
    {
        sock = socket(ra.family, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            NEUTRAL_REPORT_WARN("TransportTCP", "Ошибка создания сокета. Ошибка: " +
                                std::to_string(WSAGetLastError()));
            continue;
        }

        // Неблокирующий режим для connect с дедлайном.
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);

        int cr = connect(sock, reinterpret_cast<const sockaddr*>(&ra.addr), ra.addrlen);
        bool connected = false;
        if (cr == 0)
        {
            connected = true;   // мгновенное подключение (loopback)
        }
        else if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            // Ждём готовности к записи в пределах дедлайна.
            fd_set writeFds, exceptFds;
            FD_ZERO(&writeFds); FD_SET(sock, &writeFds);
            FD_ZERO(&exceptFds); FD_SET(sock, &exceptFds);
            timeval tv;
            tv.tv_sec = CONNECT_TIMEOUT_MS / 1000;
            tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;

            int sel = select(0, nullptr, &writeFds, &exceptFds, &tv);
            if (sel > 0 && FD_ISSET(sock, &writeFds))
            {
                int soErr = 0;
                int soLen = sizeof(soErr);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&soErr), &soLen) == 0 && soErr == 0)
                {
                    connected = true;
                }
            }
            // sel==0 → таймаут; FD в exceptFds или SO_ERROR!=0 → отказ.
        }

        // Возврат в блокирующий режим (reader/Send работают синхронно).
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);

        if (connected)
        {
            break;
        }

        NEUTRAL_REPORT_WARN("TransportTCP", "Не удалось подключиться к адресу. Пробуем следующий.");
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (sock == INVALID_SOCKET)
    {
        std::string errorMsg = "Не удалось подключиться к: " + m_host + ":" + portStr;
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        if (m_errorCallback) m_errorCallback(errorMsg, WSAGetLastError());
        return false;
    }

    m_socket = sock;
    m_stateDownEmitted = false;   // новое соединение — разрешаем следующий state(false)
    m_isOpen = true;

    // Запускаем поток чтения
    if (!StartReadThread())
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Не удалось запустить поток чтения");
        Close();
        return false;
    }

    NEUTRAL_REPORT_INFO("TransportTCP", "Успешно подключено к: " + m_host + ":" + portStr);

    // Уведомляем о изменении состояния соединения
    if (m_connectionStateCallback)
    {
        m_connectionStateCallback(true);
    }

    return true;
}

bool TransportTCP::Close()
{
    // Контракт §4.1: идемпотентный; освобождает ресурсы ПО ВАЛИДНОСТИ (сокет/поток),
    // а НЕ по флагу m_isOpen — чтобы дочистить сокет и после remote-close.

    // 1) Синхронизация с Send (§4.1 п.2): под тем же m_sendMutex обнуляем дескриптор,
    //    чтобы не закрыть сокет посреди активной отправки. Порядок §4.1 п.3:
    //    exchange(socket→INVALID) → shutdown(SD_BOTH) → closesocket → (потом join).
    SOCKET sock;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        sock = m_socket.exchange(INVALID_SOCKET);
        m_isOpen = false;
    }
    if (sock != INVALID_SOCKET)
    {
        NEUTRAL_REPORT_INFO("TransportTCP", "Закрытие TCP-соединения: " + m_host + ":" + std::to_string(m_port));
        shutdown(sock, SD_BOTH);      // разблокировать recv в reader ДО join
        closesocket(sock);
    }

    // 2) Останавливаем reader (recv уже разблокирован закрытием сокета) и join.
    StopReadThread();

    // 3) state(false) ровно один раз (§4.1 п.5): если reader уже сообщил разрыв —
    //    здесь no-op; после возврата Close колбеков больше нет (§4.1 п.4).
    EmitStateDown();

    NEUTRAL_REPORT_INFO("TransportTCP", "TCP-соединение закрыто");
    return true;
}

void TransportTCP::EmitStateDown()
{
    bool expected = false;
    if (m_stateDownEmitted.compare_exchange_strong(expected, true))
    {
        if (m_connectionStateCallback)
        {
            m_connectionStateCallback(false);
        }
    }
}

bool TransportTCP::IsOpen() const
{
    return m_isOpen;
}

int TransportTCP::Send(const std::vector<uint8_t>& data)
{
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("TransportTCP", "Попытка отправить пустые данные");
        return 0;
    }

    bool needClose = false;   // фатальный обрыв: Close() ПОСЛЕ снятия m_sendMutex
    int result = -1;
    {
        // Блокируем mutex: сериализация Send и синхронизация с Close (§4.1 п.2).
        std::lock_guard<std::mutex> lock(m_sendMutex);

        SOCKET sock = m_socket.load();
        if (!m_isOpen || sock == INVALID_SOCKET)
        {
            std::string errorMsg = "Попытка отправить данные через закрытое соединение";
            NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
            if (m_errorCallback) m_errorCallback(errorMsg, -1);
            return -1;
        }

        // ALL-OR-ERROR (§4.1, §14): дописываем остаток в цикле; partial → продолжаем,
        // любая ошибка → -1 (никогда не «успех» на частичной записи).
        const char* buf = reinterpret_cast<const char*>(data.data());
        const size_t total = data.size();
        size_t written = 0;
        bool failed = false;
        while (written < total)
        {
            int n = m_sendFn(sock, buf + written, static_cast<int>(total - written));
            if (n == SOCKET_ERROR || n <= 0)
            {
                int error = (n == SOCKET_ERROR) ? WSAGetLastError() : -1;
                std::string errorMsg = "Ошибка при отправке данных. Ошибка: " + std::to_string(error);
                NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
                if (m_errorCallback) m_errorCallback(errorMsg, error);

                if (error == WSAECONNRESET || error == WSAECONNABORTED)
                {
                    NEUTRAL_REPORT_WARN("TransportTCP", "Соединение разорвано удаленной стороной");
                    needClose = true;   // синхронный обрыв — детект в вызове (§5.3 precedence)
                }
                failed = true;
                break;
            }
            written += static_cast<size_t>(n);
        }

        if (!failed)
        {
            NEUTRAL_REPORT_DEBUG("TransportTCP", "Отправлено " + std::to_string(written) + " байт");
            result = static_cast<int>(written);   // == data.size()
        }
    }

    // Close вызывается вне m_sendMutex — иначе self-deadlock (Close тоже берёт мьютекс).
    if (needClose)
    {
        Close();
    }
    return result;
}

void TransportTCP::SetSendFunctionForTest(SendFn fn)
{
    m_sendFn = std::move(fn);
}

void TransportTCP::SetDataReceivedCallback(DataReceivedCallback callback)
{
    m_dataReceivedCallback = callback;
}

void TransportTCP::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = callback;
}

void TransportTCP::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = callback;
}

bool TransportTCP::SetTimeout(int timeoutMs)
{
    if (m_socket == INVALID_SOCKET)
    {
        NEUTRAL_REPORT_ERROR("TransportTCP", "Попытка установить тайм-аут для недействительного сокета");
        return false;
    }

    // Устанавливаем тайм-аут для приема
    DWORD timeout = timeoutMs;
    int result = setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (result == SOCKET_ERROR)
    {
        std::string errorMsg = "Не удалось установить тайм-аут приема. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    // Устанавливаем тайм-аут для отправки
    result = setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    if (result == SOCKET_ERROR)
    {
        std::string errorMsg = "Не удалось установить тайм-аут отправки. Ошибка: " + std::to_string(WSAGetLastError());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, WSAGetLastError());
        }
        return false;
    }

    NEUTRAL_REPORT_DEBUG("TransportTCP", "Установлен тайм-аут: " + std::to_string(timeoutMs) + " мс");
    return true;
}

bool TransportTCP::StartReadThread()
{
    if (m_readThreadRunning)
    {
        return true;
    }

    m_readThreadRunning = true;
    
    try
    {
        m_readThread = std::thread(&TransportTCP::ReadThreadFunction, this);
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения запущен");
        return true;
    }
    catch (const std::exception& e)
    {
        m_readThreadRunning = false;
        std::string errorMsg = "Не удалось создать поток чтения: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return false;
    }
}

void TransportTCP::StopReadThread()
{
    if (!m_readThreadRunning)
    {
        return;
    }

    m_readThreadRunning = false;
    
    if (m_readThread.joinable())
    {
        NEUTRAL_REPORT_DEBUG("TransportTCP", "Ожидание завершения потока чтения");
        m_readThread.join();
    }
    
    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения остановлен");
}

void TransportTCP::ReadThreadFunction()
{
    std::vector<char> buffer(READ_BUFFER_SIZE);
    
    while (m_readThreadRunning && m_isOpen && m_socket != INVALID_SOCKET)
    {
        int bytesReceived = recv(m_socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        
        if (bytesReceived > 0)
        {
            NEUTRAL_REPORT_DEBUG("TransportTCP", "Получено " + std::to_string(bytesReceived) + " байт");
            
            if (m_dataReceivedCallback)
            {
                std::vector<uint8_t> receivedData(buffer.begin(), buffer.begin() + bytesReceived);
                m_dataReceivedCallback(receivedData);
            }
        }
        else if (bytesReceived == 0)
        {
            // Соединение закрыто корректно
            NEUTRAL_REPORT_INFO("TransportTCP", "Соединение закрыто удаленной стороной");
            break;
        }
        else
        {
            // Ошибка приема данных
            int error = WSAGetLastError();
            
            // Игнорируем ошибку, если поток был остановлен извне
            if (!m_readThreadRunning)
            {
                break;
            }
            
            // Игнорируем ошибки таймаута
            if (error == WSAETIMEDOUT)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            std::string errorMsg = "Ошибка приема данных. Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportTCP", errorMsg);
            
            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
            
            // Если ошибка связана с отключением, выходим из цикла
            if (error == WSAECONNRESET || error == WSAECONNABORTED)
            {
                NEUTRAL_REPORT_WARN("TransportTCP", "Соединение разорвано удаленной стороной");
                break;
            }
            
            // Небольшая пауза, чтобы избежать 100% загрузки CPU при повторяющихся ошибках
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // Выход из цикла = разрыв/ошибка/локальный Close. Помечаем соединение закрытым
    // и сообщаем state(false) РОВНО один раз (§4.1 п.5): при локальном Close это уже
    // сделал Close (EmitStateDown идемпотентен), при remote-close — здесь.
    m_isOpen = false;
    EmitStateDown();

    NEUTRAL_REPORT_DEBUG("TransportTCP", "Поток чтения завершен");
}