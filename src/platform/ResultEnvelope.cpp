#include "../core/pch.h"
#include "ResultEnvelope.h"

nlohmann::json ResultEnvelope::ToJson() const {
    nlohmann::json j;
    j["ok"] = ok;
    j["code"] = code;
    j["description"] = description;
    j["payload"] = payload;
    return j;
}
