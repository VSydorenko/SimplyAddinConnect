#include "../../core/pch.h"
#include "EcrJsonCodec.h"

std::vector<uint8_t> EcrJsonCodec::BuildRequest(const std::string& method, int step,
                                                const nlohmann::json& params) {
    nlohmann::json j;
    j["method"] = method;
    j["step"] = step;
    if (!params.is_null()) j["params"] = params;
    // error_handler=replace: не кидати type_error(316) на невалідний UTF-8 у рядках
    // (params можуть прийти з 1С у Частині 2) — виняток не перетинає межу 1С.
    std::string s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return std::vector<uint8_t>(s.begin(), s.end());
}

ParsedResponse EcrJsonCodec::Parse(const std::vector<uint8_t>& frame) {
    ParsedResponse r;
    // try/catch: allow_exceptions=false гасить лише синтаксис parse(); .value<T>() усе одно
    // кидає type_error(302) при типовій невідповідності поля ("step":"1" тощо). Ненадійний
    // кадр термінала не має пробивати виняток за межу 1С — тихо повертаємо valid=false
    // (контракт заголовка; класифікатор трактує !valid як Unsolicited, без лог-спаму).
    try {
        auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return r;   // valid лишається false
        r.method = j.value("method", std::string{});
        r.step = j.value("step", 0);
        r.params = j.value("params", nlohmann::json::object());
        r.error = j.value("error", false);
        r.errorDescription = j.value("errorDescription", std::string{});
        if (r.method == "ServiceMessage" && r.params.is_object())
            r.msgType = r.params.value("msgType", std::string{});
        r.valid = true;
    } catch (const nlohmann::json::exception&) {
        return ParsedResponse{};   // типово-неправильний JSON → valid=false, без кидка
    }
    return r;
}

bool EcrJsonCodec::PeekMethod(const std::vector<uint8_t>& frame,
                              std::string& method, std::string& msgType) {
    try {
        auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        method = j.value("method", std::string{});
        msgType.clear();
        if (method == "ServiceMessage") {
            auto it = j.find("params");
            if (it != j.end() && it->is_object()) msgType = it->value("msgType", std::string{});
        }
        return !method.empty();
    } catch (const nlohmann::json::exception&) {
        method.clear();
        msgType.clear();
        return false;   // типово-неправильний JSON → не розібрано, без кидка (див. Parse)
    }
}
