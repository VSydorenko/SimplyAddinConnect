// FaultPlan — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#include "FaultPlan.h"

namespace prrofs {
namespace {

bool ValidTarget(const std::string& t) {
    return t == "doc" || t == "cmd" || (t.size() > 4 && t.compare(0, 4, "cmd:") == 0);
}

}  // namespace

const char* FaultModeName(FaultMode m) {
    switch (m) {
        case FaultMode::Delay:              return "delay";
        case FaultMode::Status:             return "status";
        case FaultMode::DropBeforeRegister: return "dropBeforeRegister";
        case FaultMode::DropAfterRegister:  return "dropAfterRegister";
    }
    return "?";
}

bool ParseFaultMode(const std::string& s, FaultMode& out) {
    for (FaultMode m : { FaultMode::Delay, FaultMode::Status,
                         FaultMode::DropBeforeRegister, FaultMode::DropAfterRegister }) {
        if (s == FaultModeName(m)) { out = m; return true; }
    }
    return false;
}

FaultPlan::ArmResult FaultPlan::Arm(const std::string& target, const Fault& f) {
    if (!ValidTarget(target)) return ArmResult::BadTarget;
    for (const auto& a : armed_) if (a.first == target) return ArmResult::Conflict;
    armed_.push_back({ target, f });
    return ArmResult::Ok;
}

bool FaultPlan::TakeTarget(const std::string& target, Fault& out) {
    for (auto it = armed_.begin(); it != armed_.end(); ++it) {
        if (it->first == target) { out = it->second; armed_.erase(it); return true; }
    }
    return false;
}

bool FaultPlan::Take(const std::string& kind, const std::function<std::string()>& commandName, Fault& out) {
    if (kind == "doc") return TakeTarget("doc", out);
    if (kind != "cmd") return false;
    bool anySpecific = false;
    for (const auto& a : armed_) if (a.first.compare(0, 4, "cmd:") == 0) { anySpecific = true; break; }
    if (anySpecific && commandName) {
        const std::string name = commandName();
        if (!name.empty() && TakeTarget("cmd:" + name, out)) return true;
    }
    return TakeTarget("cmd", out);
}

void FaultPlan::Clear() {
    armed_.clear();
    dateSkewSeconds = 0;
    rejectFormat    = RejectFormat::Text;
}

}  // namespace prrofs
