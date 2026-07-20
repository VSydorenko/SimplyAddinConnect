#pragma once

#include "IFramer.h"
#include <mutex>
#include <cstddef>

/**
 * @file NullTerminatedFramer.h
 * @brief Кадрувальник із термінатором 0x00 (§4.2 дизайну).
 *
 * Ріже потік по байту 0x00 (термінатор відкидається), порожні кадри
 * (провідний/подвійний 0x00) ігноруються. Wrap додає кінцевий 0x00,
 * за leadingDelimiter — ще й провідний. Переповнення буфера без термінатора
 * понад maxBufferedBytes → скидання буфера (постумова §14: не «отруєний»).
 */
class NullTerminatedFramer : public IFramer {
public:
    explicit NullTerminatedFramer(std::size_t maxBufferedBytes = (1u << 20));

    void Feed(const std::vector<uint8_t>& chunk,
              std::vector<std::vector<uint8_t>>& out) override;
    std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload,
                              FrameOptions opts = {}) override;
    void Reset() override;
    void SetMaxBufferedBytes(std::size_t maxBufferedBytes) override;

private:
    std::mutex           framerMutex_;
    std::vector<uint8_t> buffer_;
    std::size_t          max_;
};
