#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

/// Розібрана відповідь термінала (кадр без делімітера).
struct ParsedResponse {
    std::string method;
    int step = 0;
    nlohmann::json params = nlohmann::json::object();
    bool error = false;
    std::string errorDescription;
    std::string msgType;   ///< params.msgType, якщо method=="ServiceMessage"; інакше порожній
    bool valid = false;    ///< false → JSON не розібрано
};

/// Кодек прикладного рівня ECR ПриватБанк: JSON <-> байти. Делімітером 0x00 НЕ оперує
/// (це робить NullTerminatedFramer); працює з payload без термінатора.
class EcrJsonCodec {
public:
    /// {method, step[, params]} → UTF-8 JSON-байти без делімітера. params==nullptr → без поля params.
    static std::vector<uint8_t> BuildRequest(const std::string& method, int step,
                                             const nlohmann::json& params);
    /// Повний розбір кадру. Невалідний JSON → {valid=false} (без винятку).
    static ParsedResponse Parse(const std::vector<uint8_t>& frameNoDelimiter);
    /// Легкий парс лише method (+ params.msgType для ServiceMessage). false → не розібрано.
    static bool PeekMethod(const std::vector<uint8_t>& frameNoDelimiter,
                           std::string& method, std::string& msgType);
};
