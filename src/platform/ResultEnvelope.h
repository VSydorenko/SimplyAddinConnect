#pragma once
// ResultEnvelope — уніфікований конверт результату операції драйвера (спека §3.1/§7).
#include <string>
#include <nlohmann/json.hpp>

struct ResultEnvelope {
    bool ok = false;
    std::string code;          ///< машинний код ("OK", "TIMEOUT", "DEVICE_BUSY", "1004", ...)
    std::string description;   ///< людиночитний опис
    nlohmann::json payload = nlohmann::json::object();  ///< корисне навантаження операції

    /// Серіалізація для 1С (ПолучитьРезультатJSON): {ok, code, description, payload}.
    nlohmann::json ToJson() const;

    static ResultEnvelope Ok(nlohmann::json payload = nlohmann::json::object()) {
        return ResultEnvelope{ true, "OK", "", std::move(payload) };
    }
    static ResultEnvelope Fail(std::string code, std::string description) {
        return ResultEnvelope{ false, std::move(code), std::move(description), nlohmann::json::object() };
    }
};
