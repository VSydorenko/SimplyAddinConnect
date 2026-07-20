#pragma once

#include <vector>
#include <cstdint>

/**
 * @file RequestTypes.h
 * @brief Спільні типи результату запиту device-сесії (§4.0 дизайну).
 *
 * Статус-enum винесено в окремий заголовок, щоб класифікатор і транспорти
 * не залежали від DeviceSession.
 */

/// Статус завершення запиту (RequestPrimary/RequestService).
enum class RequestStatus {
    Response,        ///< frame валідний — успішна відповідь
    Busy,            ///< deviceBusy
    Unsupported,     ///< methodNotImplemented
    Timeout,         ///< вичерпано час очікування відповіді
    Disconnected,    ///< обрив під час in-flight
    SendFailed,      ///< помилка відправки (Send<0)
    Stopped,         ///< Stop()/деструктор перервав запит
    Concurrent,      ///< друга операція тієї ж доріжки
    Desynchronized   ///< сесія в desync (§7), новий primary заборонено
};

/// Результат запиту: статус + кадр відповіді (порожній, якщо статус != Response).
struct RequestResult {
    RequestStatus status;
    std::vector<uint8_t> frame;
};
