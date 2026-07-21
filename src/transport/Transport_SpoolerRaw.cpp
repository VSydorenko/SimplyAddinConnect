#include "../core/pch.h"

#include "Transport_SpoolerRaw.h"
#include "../helpers/ServiceTools.h"
#include <windows.h>
#include <winspool.h>

#pragma comment(lib, "winspool.lib")

TransportSpoolerRaw::TransportSpoolerRaw(std::string printerName)
    : m_printerName(std::move(printerName)),
      m_hPrinter(nullptr),
      m_isOpen(false)
{
    NEUTRAL_REPORT_DEBUG("TransportSpoolerRaw", "Створено об'єкт spooler-каналу для принтера: " + m_printerName);
}

TransportSpoolerRaw::~TransportSpoolerRaw()
{
    NEUTRAL_REPORT_DEBUG("TransportSpoolerRaw", "Знищення об'єкта spooler-каналу для принтера: " + m_printerName);
    Close();
}

bool TransportSpoolerRaw::Open()
{
    if (m_isOpen)
    {
        NEUTRAL_REPORT_WARN("TransportSpoolerRaw", "Спроба відкрити вже відкритий канал");
        return true;
    }

    // Тест-шов: без реального принтера — вважаємо канал відкритим.
    if (m_sendFn)
    {
        m_isOpen = true;
        NEUTRAL_REPORT_DEBUG("TransportSpoolerRaw", "Канал відкрито через тест-шов");
        return true;
    }

    std::wstring wName = ServiceTools::U16StringToWString(ServiceTools::SafeMB2WCHAR(m_printerName.c_str()));

    HANDLE hPrinter = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wName.c_str()), &hPrinter, nullptr) || hPrinter == nullptr)
    {
        const DWORD err = GetLastError();
        NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                             "Не вдалося відкрити принтер: " + m_printerName + ", код помилки: " + std::to_string(err));
        return false;
    }

    m_hPrinter = hPrinter;
    m_isOpen = true;
    NEUTRAL_REPORT_INFO("TransportSpoolerRaw", "Принтер відкрито: " + m_printerName);
    return true;
}

bool TransportSpoolerRaw::Close()
{
    if (m_hPrinter != nullptr)
    {
        ClosePrinter(static_cast<HANDLE>(m_hPrinter));
        m_hPrinter = nullptr;
        NEUTRAL_REPORT_INFO("TransportSpoolerRaw", "Принтер закрито: " + m_printerName);
    }
    m_isOpen = false;
    return true;
}

bool TransportSpoolerRaw::IsOpen() const
{
    return m_isOpen;
}

int TransportSpoolerRaw::Send(const std::vector<uint8_t>& data)
{
    if (data.empty())
    {
        NEUTRAL_REPORT_WARN("TransportSpoolerRaw", "Спроба відправити порожні дані");
        return 0;
    }

    // Тест-шов: перехоплюємо етап запису байтів у чергу.
    if (m_sendFn)
    {
        return m_sendFn(data);
    }

    if (!m_isOpen || m_hPrinter == nullptr)
    {
        NEUTRAL_REPORT_ERROR("TransportSpoolerRaw", "Спроба відправити дані через закритий канал");
        return -1;
    }

    HANDLE hPrinter = static_cast<HANDLE>(m_hPrinter);

    // Ім'я документа в UTF-16 (мусить пережити виклик StartDocPrinterW).
    std::wstring wDocName = ServiceTools::U16StringToWString(
        ServiceTools::SafeMB2WCHAR("ZPL Label Job"));

    DOC_INFO_1W docInfo;
    docInfo.pDocName = const_cast<LPWSTR>(wDocName.c_str());
    docInfo.pOutputFile = nullptr;
    docInfo.pDatatype = const_cast<LPWSTR>(L"RAW");

    // Прапорці досягнутого стану — для гарантованого teardown у зворотному порядку.
    bool docStarted = false;
    bool pageStarted = false;
    DWORD savedErr = 0;
    int result = -1;

    // 1) StartDocPrinter
    if (StartDocPrinterW(hPrinter, 1, reinterpret_cast<LPBYTE>(&docInfo)) == 0)
    {
        savedErr = GetLastError();
        NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                             "StartDocPrinter не вдався для принтера: " + m_printerName + ", код помилки: " + std::to_string(savedErr));
        return -1;
    }
    docStarted = true;

    do
    {
        // 2) StartPagePrinter
        if (StartPagePrinter(hPrinter) == 0)
        {
            savedErr = GetLastError();
            NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                                 "StartPagePrinter не вдався, код помилки: " + std::to_string(savedErr));
            break;
        }
        pageStarted = true;

        // 3) WritePrinter — all-or-error: усі байти або помилка.
        DWORD written = 0;
        const BOOL wrote = WritePrinter(hPrinter,
                                        const_cast<uint8_t*>(data.data()),
                                        static_cast<DWORD>(data.size()),
                                        &written);
        if (wrote == 0 || written != static_cast<DWORD>(data.size()))
        {
            savedErr = GetLastError();
            NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                                 "WritePrinter записав не всі байти: " + std::to_string(written) + " з " +
                                 std::to_string(data.size()) + ", код помилки: " + std::to_string(savedErr));
            break;
        }

        result = static_cast<int>(written);
        NEUTRAL_REPORT_DEBUG("TransportSpoolerRaw", "Записано в чергу " + std::to_string(written) + " байт");
    } while (false);

    // Teardown у зворотному порядку залежно від досягнутого стану; помилки завершення
    // при вже успішному записі роблять результат невдалим (документ міг не потрапити в чергу).
    if (pageStarted)
    {
        if (EndPagePrinter(hPrinter) == 0)
        {
            const DWORD err = GetLastError();
            NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                                 "EndPagePrinter не вдався, код помилки: " + std::to_string(err));
            result = -1;
        }
    }
    if (docStarted)
    {
        if (EndDocPrinter(hPrinter) == 0)
        {
            const DWORD err = GetLastError();
            NEUTRAL_REPORT_ERROR("TransportSpoolerRaw",
                                 "EndDocPrinter не вдався, код помилки: " + std::to_string(err));
            result = -1;
        }
    }

    return result;
}

void TransportSpoolerRaw::SetSendFunctionForTest(SendFn fn)
{
    m_sendFn = std::move(fn);
}

void TransportSpoolerRaw::SetDataReceivedCallback(DataReceivedCallback callback)
{
    // Write-only канал: зберігаємо, але не викликаємо.
    m_dataReceivedCallback = std::move(callback);
}

void TransportSpoolerRaw::SetErrorCallback(ErrorCallback callback)
{
    m_errorCallback = std::move(callback);
}

void TransportSpoolerRaw::SetConnectionStateCallback(ConnectionStateCallback callback)
{
    m_connectionStateCallback = std::move(callback);
}
