#include "../core/pch.h"
#include "NullTerminatedFramer.h"
#include "../helpers/ServiceTools.h"

/**
 * @file NullTerminatedFramer.cpp
 * @brief Реалізація кадрувальника з термінатором 0x00 (§4.2 дизайну).
 */

NullTerminatedFramer::NullTerminatedFramer(std::size_t maxBufferedBytes)
    : max_(maxBufferedBytes) {
}

void NullTerminatedFramer::Feed(const std::vector<uint8_t>& chunk,
                                std::vector<std::vector<uint8_t>>& out) {
    std::lock_guard<std::mutex> lock(framerMutex_);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());

    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
        if (buffer_[i] == 0x00) {
            // Кадр — байти [start, i); термінатор відкидається.
            if (i > start) {
                out.emplace_back(buffer_.begin() + start,
                                 buffer_.begin() + i);
            }
            // Порожні кадри (провідний/подвійний 0x00) пропускаємо.
            start = i + 1;
        }
    }

    // Прибрати спожите; лишок (незавершений кадр) переноситься на наступний Feed.
    if (start > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + start);
    }

    // Переповнення: буфер без термінатора понад ліміт → скидання (не «отруєний»).
    if (buffer_.size() > max_) {
        NEUTRAL_REPORT_WARN("NullTerminatedFramer",
            "Переповнення буфера кадрувальника без термінатора: " +
            std::to_string(buffer_.size()) + " > " + std::to_string(max_) +
            ", буфер скинуто");
        buffer_.clear();
    }
}

std::vector<uint8_t> NullTerminatedFramer::Wrap(const std::vector<uint8_t>& payload,
                                                FrameOptions opts) {
    std::lock_guard<std::mutex> lock(framerMutex_);
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + (opts.leadingDelimiter ? 2 : 1));
    if (opts.leadingDelimiter) {
        frame.push_back(0x00);
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0x00);
    return frame;
}

void NullTerminatedFramer::Reset() {
    std::lock_guard<std::mutex> lock(framerMutex_);
    buffer_.clear();
}

void NullTerminatedFramer::SetMaxBufferedBytes(std::size_t maxBufferedBytes) {
    std::lock_guard<std::mutex> lock(framerMutex_);
    max_ = maxBufferedBytes;
}
