#pragma once

#include <vector>
#include <cstdint>

/**
 * @file IFramer.h
 * @brief Інтерфейс кадрувальника байтового потоку (§4.2 дизайну).
 *
 * Feed — stateful накопичення + нарізка вхідного потоку на кадри.
 * Wrap — обгортання payload у кадр для відправки.
 * Reset — скидання внутрішнього буфера (при реконекті).
 */

/// Опції обгортання кадру (per-request, лише для Wrap).
struct FrameOptions {
    bool leadingDelimiter = false;   ///< додати провідний роздільник (PingDevice-хендшейк)
};

class IFramer {
public:
    virtual ~IFramer() = default;

    /// Згодувати вхідний чанк; готові кадри дописуються в out.
    virtual void Feed(const std::vector<uint8_t>& chunk,
                      std::vector<std::vector<uint8_t>>& out) = 0;

    /// Обгорнути payload у кадр для відправки.
    virtual std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload,
                                      FrameOptions opts = {}) = 0;

    /// Скинути внутрішній буфер.
    virtual void Reset() = 0;
};
