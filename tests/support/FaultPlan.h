#pragma once
// FaultPlan — керовані збої імітації ДПС (спека §8.2-8.3): одноразовий збій на
// наступний запит своєї цілі + постійні налаштування до наступного reset.
// Чиста логіка; НЕ потокобезпечний — викликач тримає м'ютекс.
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace prrofs {

enum class FaultMode { Delay, Status, DropBeforeRegister, DropAfterRegister };
const char* FaultModeName(FaultMode m);                 // "delay" | "status" | "dropBeforeRegister" | "dropAfterRegister"
bool        ParseFaultMode(const std::string& s, FaultMode& out);

struct Fault {
    FaultMode   mode = FaultMode::Delay;
    int         seconds = 0;       // delay
    int         code = 0;          // status
    std::string body;              // status (необов'язково)
    std::string location;          // status 302 (необов'язково)
    int         holdSeconds = 0;   // drop*: 0 — розрив одразу; >0 — мовчати, потім розрив
};

enum class RejectFormat { Text, Ticket };

class FaultPlan {
public:
    enum class ArmResult { Ok, Conflict, BadTarget };

    // target: "doc" | "cmd" | "cmd:<Команда>". Для тієї самої цілі вже взведено -> Conflict (409).
    ArmResult Arm(const std::string& target, const Fault& f);
    // Витратити збій для запиту kind ("doc" | "cmd"). commandName викликається ЛИШЕ коли
    // взведено хоч один "cmd:<Команда>" — інакше тіло команди до збою не розбирається.
    // "cmd:<Команда>" має перевагу над "cmd"; витрачається рівно один збій.
    bool Take(const std::string& kind, const std::function<std::string()>& commandName, Fault& out);
    void Clear();                                          // reset: збої + постійні налаштування
    std::vector<std::pair<std::string, Fault>> Armed() const { return armed_; }

    int          dateSkewSeconds = 0;
    RejectFormat rejectFormat    = RejectFormat::Text;

private:
    bool TakeTarget(const std::string& target, Fault& out);
    std::vector<std::pair<std::string, Fault>> armed_;
};

}  // namespace prrofs
