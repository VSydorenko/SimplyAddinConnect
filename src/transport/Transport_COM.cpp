#include "../core/pch.h"

#include "Transport_COM.h"
#include "../helpers/ServiceTools.h"

TransportCOM::TransportCOM(
    const std::string& portName,
    int baudRate,
    int dataBits,
    char parity,
    float stopBits)
    : m_portName(portName),
      m_baudRate(baudRate),
      m_dataBits(dataBits),
      m_parity(parity),
      m_stopBits(stopBits),
      m_portHandle(INVALID_HANDLE_VALUE),
      m_isOpen(false),
      m_threadRunning(false),
      m_stateDownEmitted(false),
      m_writeFn([](HANDLE h, const void* buf, DWORD n, DWORD* written)
                { return WriteFile(h, buf, n, written, NULL); })
{
    NEUTRAL_REPORT_DEBUG("TransportCOM", "Создан объект COM-порта: " + m_portName);
}

TransportCOM::~TransportCOM()
{
    NEUTRAL_REPORT_DEBUG("TransportCOM", "Уничтожение объекта COM-порта: " + m_portName);
    Close();
}

bool TransportCOM::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("TransportCOM", "Попытка открыть уже открытый порт: " + m_portName);
        return true;
    }

    // Префикс \\.\ + имя порта. Имя COM-порта всегда ASCII, поэтому расширяем
    // побайтово в широкую строку для CreateFileW (без кодировочных проблем).
    std::string fullPortName = "\\\\.\\" + m_portName;
    std::wstring widePortName(fullPortName.begin(), fullPortName.end());
    NEUTRAL_REPORT_INFO("TransportCOM", "Открытие порта: " + m_portName);

    HANDLE handle = CreateFileW(
        widePortName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,                // Не разделять
        NULL,             // Атрибуты безопасности по умолчанию
        OPEN_EXISTING,    // Открыть существующий порт
        FILE_ATTRIBUTE_NORMAL, // Нормальные атрибуты файла
        NULL              // Шаблон не используется
    );
    m_portHandle = handle;

    if (handle == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        std::string errorMsg = "Не удалось открыть порт: " + m_portName + 
                               ". Ошибка: " + std::to_string(error);
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, error);
        }
        return false;
    }

    // Настраиваем параметры порта
    if (!ConfigurePort(m_baudRate, m_dataBits, m_parity, m_stopBits))
    {
        Close();
        return false;
    }

    // Устанавливаем таймауты
    if (!SetTimeouts(100, 0, 100, 0, 100))
    {
        Close();
        return false;
    }

    // Устанавливаем флаг "открыт"
    m_isOpen = true;
    m_stateDownEmitted = false;   // новое соединение — разрешаем следующий state(false)

    // Запускаем поток чтения
    if (!StartReadThread())
    {
        NEUTRAL_REPORT_ERROR("TransportCOM", "Не удалось запустить поток чтения для порта: " + m_portName);
        Close();
        return false;
    }

    NEUTRAL_REPORT_INFO("TransportCOM", "Порт успешно открыт: " + m_portName);
    
    // Уведомляем о изменении состояния соединения
    if (m_connectionStateCallback)
    {
        m_connectionStateCallback(true);
    }
    
    return true;
}

bool TransportCOM::Close()
{
    // Контракт §4.1: идемпотентный; освобождает ресурсы ПО ВАЛИДНОСТИ хендла,
    // а НЕ по флагу m_isOpen — чтобы дочистить порт и после ошибки в reader.

    // 1) Синхронизация с Send (§4.1 п.2): под тем же m_writeMutex сбрасываем хендл,
    //    чтобы не закрыть его посреди активной записи. Порядок §4.1 п.3:
    //    сбросить handle → прервать read-цикл → join → закрыть хендл.
    HANDLE handle;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        handle = m_portHandle.exchange(INVALID_HANDLE_VALUE);
        m_isOpen = false;
    }

    if (handle != INVALID_HANDLE_VALUE)
    {
        NEUTRAL_REPORT_INFO("TransportCOM", "Закрытие порта: " + m_portName);
        // CancelIoEx разблокирует блокирующий ReadFile в reader ДО join.
        CancelIoEx(handle, NULL);
    }

    // 2) Останавливаем reader (ReadFile прерван) и join — до CloseHandle,
    //    чтобы ни один поток не использовал хендл в момент его закрытия.
    StopReadThread();

    if (handle != INVALID_HANDLE_VALUE)
    {
        if (!CloseHandle(handle))
        {
            DWORD error = GetLastError();
            std::string errorMsg = "Ошибка при закрытии порта: " + m_portName +
                                   ". Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);

            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
        }
    }

    // 3) state(false) ровно один раз (§4.1 п.5): если reader уже сообщил разрыв —
    //    здесь no-op; после возврата Close колбеков больше нет (§4.1 п.4).
    EmitStateDown();

    NEUTRAL_REPORT_INFO("TransportCOM", "Порт закрыт: " + m_portName);
    return true;
}

void TransportCOM::EmitStateDown()
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

bool TransportCOM::IsOpen() const
{
    return m_isOpen;
}

void TransportCOM::SetDataReceivedCallback(DataReceivedCallback callback)
{
    m_dataReceivedCallback = callback;
}

void TransportCOM::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = callback;
}

void TransportCOM::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = callback;
}

bool TransportCOM::ConfigurePort(int baudRate, int dataBits, char parity, float stopBits)
{
    if (m_portHandle == INVALID_HANDLE_VALUE)
    {
        NEUTRAL_REPORT_ERROR("TransportCOM", "Попытка настроить недействительный порт");
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);

    // Получаем текущие настройки
    if (!GetCommState(m_portHandle, &dcb))
    {
        DWORD error = GetLastError();
        std::string errorMsg = "Не удалось получить настройки порта: " + m_portName + 
                              ". Ошибка: " + std::to_string(error);
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, error);
        }
        return false;
    }

    // Устанавливаем скорость
    dcb.BaudRate = baudRate;
    
    // Устанавливаем биты данных
    dcb.ByteSize = dataBits;

    // Устанавливаем четность
    switch (parity)
    {
    case 'N':
    case 'n':
        dcb.Parity = NOPARITY;
        break;
    case 'E':
    case 'e':
        dcb.Parity = EVENPARITY;
        break;
    case 'O':
    case 'o':
        dcb.Parity = ODDPARITY;
        break;
    case 'M':
    case 'm':
        dcb.Parity = MARKPARITY;
        break;
    case 'S':
    case 's':
        dcb.Parity = SPACEPARITY;
        break;
    default:
        dcb.Parity = NOPARITY;
        break;
    }

    // Устанавливаем стоповые биты
    if (stopBits == 1.0f)
        dcb.StopBits = ONESTOPBIT;
    else if (stopBits == 1.5f)
        dcb.StopBits = ONE5STOPBITS;
    else if (stopBits == 2.0f)
        dcb.StopBits = TWOSTOPBITS;
    else
        dcb.StopBits = ONESTOPBIT;

    // Прочие настройки
    dcb.fBinary = TRUE;                // Двоичный режим
    dcb.fOutxCtsFlow = FALSE;          // Не использовать управление потоком CTS
    dcb.fOutxDsrFlow = FALSE;          // Не использовать управление потоком DSR
    dcb.fDtrControl = DTR_CONTROL_ENABLE; // Включить DTR
    dcb.fDsrSensitivity = FALSE;       // Игнорировать состояние DSR
    dcb.fTXContinueOnXoff = TRUE;      // Продолжать передачу при буферизации XOff
    dcb.fOutX = FALSE;                 // Не использовать XON/XOFF для отправки
    dcb.fInX = FALSE;                  // Не использовать XON/XOFF для приема
    dcb.fErrorChar = FALSE;            // Не заменять байты с ошибками четности
    dcb.fNull = FALSE;                 // Не отбрасывать пустые байты
    dcb.fRtsControl = RTS_CONTROL_ENABLE; // Включить RTS
    dcb.fAbortOnError = FALSE;         // Не прерывать чтение/запись при ошибке

    // Применяем настройки
    if (!SetCommState(m_portHandle, &dcb))
    {
        DWORD error = GetLastError();
        std::string errorMsg = "Не удалось применить настройки порта: " + m_portName + 
                              ". Ошибка: " + std::to_string(error);
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, error);
        }
        return false;
    }

    // Сохраняем настройки
    m_baudRate = baudRate;
    m_dataBits = dataBits;
    m_parity = parity;
    m_stopBits = stopBits;

    NEUTRAL_REPORT_INFO("TransportCOM", "Настройки порта применены: " + m_portName + 
                       ", BaudRate=" + std::to_string(baudRate) + 
                       ", DataBits=" + std::to_string(dataBits) + 
                       ", Parity=" + parity + 
                       ", StopBits=" + std::to_string(stopBits));

    return true;
}

bool TransportCOM::SetTimeouts(
    DWORD readIntervalTimeout,
    DWORD readTotalTimeoutMultiplier,
    DWORD readTotalTimeoutConstant,
    DWORD writeTotalTimeoutMultiplier,
    DWORD writeTotalTimeoutConstant)
{
    if (m_portHandle == INVALID_HANDLE_VALUE)
    {
        NEUTRAL_REPORT_ERROR("TransportCOM", "Попытка настроить таймауты для недействительного порта");
        return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = readIntervalTimeout;
    timeouts.ReadTotalTimeoutMultiplier = readTotalTimeoutMultiplier;
    timeouts.ReadTotalTimeoutConstant = readTotalTimeoutConstant;
    timeouts.WriteTotalTimeoutMultiplier = writeTotalTimeoutMultiplier;
    timeouts.WriteTotalTimeoutConstant = writeTotalTimeoutConstant;

    if (!SetCommTimeouts(m_portHandle, &timeouts))
    {
        DWORD error = GetLastError();
        std::string errorMsg = "Не удалось установить таймауты для порта: " + m_portName + 
                              ". Ошибка: " + std::to_string(error);
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, error);
        }
        return false;
    }

    NEUTRAL_REPORT_DEBUG("TransportCOM", "Таймауты порта установлены: " + m_portName);
    return true;
}

int TransportCOM::Send(const std::vector<uint8_t>& data)
{
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("TransportCOM", "Попытка отправить пустые данные");
        return 0;
    }

    // Блокируем mutex: сериализация Send и синхронизация с Close (§4.1 п.2) —
    // хендл проверяем и используем под тем же локом, что сбрасывает его Close.
    std::lock_guard<std::mutex> lock(m_writeMutex);

    HANDLE handle = m_portHandle.load();
    if (!m_isOpen || handle == INVALID_HANDLE_VALUE)
    {
        std::string errorMsg = "Попытка отправить данные в закрытый порт: " + m_portName;
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);

        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return -1;
    }

    // ALL-OR-ERROR (§4.1, §14): дописываем остаток в цикле; partial → продолжаем,
    // любая ошибка/нулевая запись → -1 (никогда не «успех» на частичной записи).
    const uint8_t* buf = data.data();
    const size_t total = data.size();
    size_t written = 0;
    while (written < total)
    {
        DWORD chunk = 0;
        if (!m_writeFn(handle, buf + written, static_cast<DWORD>(total - written), &chunk))
        {
            DWORD error = GetLastError();
            std::string errorMsg = "Ошибка при отправке данных в порт: " + m_portName +
                                  ". Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);

            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
            return -1;
        }

        if (chunk == 0)
        {
            // Ноль записанных байт при непустом остатке — обрыв/ошибка порта.
            std::string errorMsg = "Запись 0 байт в порт: " + m_portName + " (обрыв?)";
            NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);

            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, -1);
            }
            return -1;
        }

        written += chunk;
    }

    NEUTRAL_REPORT_DEBUG("TransportCOM", "Отправлено " + std::to_string(written) + " байт");
    return static_cast<int>(written);   // == data.size()
}

void TransportCOM::SetWriteFunctionForTest(WriteFn fn)
{
    m_writeFn = std::move(fn);
}

void TransportCOM::AttachHandleForTest(HANDLE h)
{
    m_portHandle = h;
    m_isOpen = true;
    m_stateDownEmitted = false;
}

bool TransportCOM::StartReadThread()
{
    if (m_threadRunning)
    {
        return true;
    }

    m_threadRunning = true;
    
    try
    {
        m_readThread = std::thread(&TransportCOM::ReadThreadFunction, this);
        NEUTRAL_REPORT_DEBUG("TransportCOM", "Поток чтения запущен для порта: " + m_portName);
        return true;
    }
    catch (const std::exception& e)
    {
        m_threadRunning = false;
        std::string errorMsg = "Не удалось создать поток чтения: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);
        
        if (m_errorCallback)
        {
            m_errorCallback(errorMsg, -1);
        }
        return false;
    }
}

void TransportCOM::StopReadThread()
{
    if (!m_threadRunning)
    {
        return;
    }

    m_threadRunning = false;
    
    if (m_readThread.joinable())
    {
        NEUTRAL_REPORT_DEBUG("TransportCOM", "Ожидание завершения потока чтения");
        m_readThread.join();
    }
    
    NEUTRAL_REPORT_DEBUG("TransportCOM", "Поток чтения остановлен для порта: " + m_portName);
}

void TransportCOM::ReadThreadFunction()
{
    std::vector<uint8_t> buffer(READ_BUFFER_SIZE);

    while (m_threadRunning && m_isOpen)
    {
        HANDLE handle = m_portHandle.load();
        if (handle == INVALID_HANDLE_VALUE)
        {
            break;   // Close сбросил хендл — тихий выход
        }

        DWORD bytesRead = 0;

        // Чтение из порта. Таймаут (SetTimeouts) → ReadFile возвращает TRUE с
        // bytesRead==0, поэтому цикл не крутит CPU и реагирует на m_threadRunning.
        BOOL readResult = ReadFile(
            handle,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            NULL
        );

        if (!readResult)
        {
            DWORD error = GetLastError();

            // Остановлен извне (Close сбросил хендл/флаг) — тихий выход, без колбеков.
            if (!m_threadRunning || m_portHandle.load() == INVALID_HANDLE_VALUE)
            {
                break;
            }

            // Неустранимая ошибка/обрыв порта: сообщаем и завершаем reader (§9.1).
            // Выход из цикла → state(false) ниже разбудит супервизор ровно один раз.
            std::string errorMsg = "Ошибка чтения из порта: " + m_portName +
                                  ". Ошибка: " + std::to_string(error);
            NEUTRAL_REPORT_ERROR("TransportCOM", errorMsg);

            if (m_errorCallback)
            {
                m_errorCallback(errorMsg, error);
            }
            break;
        }

        // Если данные получены, вызываем callback
        if (bytesRead > 0)
        {
            NEUTRAL_REPORT_DEBUG("TransportCOM", "Получено " + std::to_string(bytesRead) + " байт");

            if (m_dataReceivedCallback)
            {
                std::vector<uint8_t> receivedData(buffer.begin(), buffer.begin() + bytesRead);
                m_dataReceivedCallback(receivedData);
            }
        }
        // bytesRead==0 → таймаут чтения (нет данных); цикл продолжается.
    }

    // Выход = разрыв/ошибка/локальный Close. Помечаем закрытым и сообщаем
    // state(false) РОВНО один раз (§4.1 п.5): при локальном Close его уже сделает
    // Close (EmitStateDown идемпотентен), при обрыве — здесь (будит супервизор).
    m_isOpen = false;
    EmitStateDown();

    NEUTRAL_REPORT_DEBUG("TransportCOM", "Поток чтения завершен для порта: " + m_portName);
}