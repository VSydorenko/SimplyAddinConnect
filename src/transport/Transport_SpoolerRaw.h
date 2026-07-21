#pragma once

#include "Transport.h"
#include <string>
#include <functional>
#include <vector>
#include <cstdint>

// Windows spooler-handle тип (HANDLE) без затягування windows.h у цей заголовок:
// зберігаємо як void* і кастуємо в .cpp. У .h — ніякого PCH/WinAPI.

/**
 * @class TransportSpoolerRaw
 * @brief ITransport поверх Winspool: односпрямований RAW-друк у чергу принтера.
 *
 * @details Write-only канал для драйвера принтера етикеток: пакет ZPL віддається
 *          в чергу друку послідовністю OpenPrinter → StartDocPrinter(RAW) →
 *          StartPagePrinter → WritePrinter → EndPagePrinter → EndDocPrinter.
 *          Прийому даних немає — колбеки-сеттери no-op. Успіх Send() = «прийнято
 *          в чергу» (усі байти записані), а НЕ фізичний друк.
 */
class TransportSpoolerRaw : public ITransport {
public:
    /**
     * @brief Конструктор.
     * @param printerName Ім'я принтера у системі (як у панелі «Принтери»).
     */
    explicit TransportSpoolerRaw(std::string printerName);

    ~TransportSpoolerRaw() override;

    // Реалізація ITransport (рівно 7 методів).
    bool Open() override;
    bool Close() override;
    bool IsOpen() const override;
    int Send(const std::vector<uint8_t>& data) override;
    void SetDataReceivedCallback(DataReceivedCallback callback) override;
    void SetErrorCallback(ErrorCallback callback) override;
    void SetConnectionStateCallback(ConnectionStateCallback callback) override;

    // Тип тест-функції: перехоплює етап запису байтів (замість WritePrinter-послідовності).
    using SendFn = std::function<int(const std::vector<uint8_t>&)>;

    /**
     * @brief Тест-шов: підмінити етап відправки байтів у чергу.
     * @details Якщо задано, Send() викликає fn замість реального Winspool-циклу
     *          (перевірка кодека/оркестрації без реального принтера). Задавати ДО Send().
     */
    void SetSendFunctionForTest(SendFn fn);

private:
    std::string m_printerName;
    void* m_hPrinter;          // HANDLE черги друку (nullptr = закрито)
    bool m_isOpen;
    SendFn m_sendFn;           // тест-шов; порожній → реальний Winspool-шлях

    // Колбеки зберігаються, але не викликаються (write-only канал).
    DataReceivedCallback m_dataReceivedCallback;
    ErrorCallback m_errorCallback;
    ConnectionStateCallback m_connectionStateCallback;
};
