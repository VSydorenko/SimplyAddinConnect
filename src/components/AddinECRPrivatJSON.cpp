#include "../core/pch.h"
#include "AddinECRPrivatJSON.h"
#include "../helpers/ServiceTools.h"

// Реєстрація компоненти через статичний член класу + анти-стрип reference.
REGISTER_COMPONENT(u"ECRPrivatJSON", AddinECRPrivatJSON)

namespace {
// Таймаут короткого запиту версії ПЗ термінала (як хендшейк — з запасом).
constexpr int kInfoTimeoutMs = 5000;
}

AddinECRPrivatJSON::AddinECRPrivatJSON() {
    REPORT_INFO("Ініціалізація компоненти ECRPrivatJSON");
    RegisterMethods();
}

AddinECRPrivatJSON::~AddinECRPrivatJSON() {
    REPORT_INFO("Завершення роботи компоненти ECRPrivatJSON");
    driver_.Disconnect();
    ServiceTools::DisableComponentLogging(this);
}

void AddinECRPrivatJSON::RegisterMethods() {
    // --- Підключення --------------------------------------------------------
    AddFunction(u"Connect", u"Подключить",
        Ret([this](VH connString) -> bool {
            try { return driver_.Connect(static_cast<std::string>(connString)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Connect: ") + e.what()); return false; }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"connString", u"СтрокаПодключения", /*required*/true, {} } });

    AddProcedure(u"Disconnect", u"Отключить",
        MethFunction(std::function<void()>([this]() {
            try { driver_.Disconnect(); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Disconnect: ") + e.what()); }
        })));

    AddFunction(u"IsConnected", u"Подключен",
        Ret([this]() -> bool { return driver_.IsConnected(); }));

    // --- Синхронні операції -------------------------------------------------
    // runSync ставить this->result рядком JSON (ResultEnvelope) і повертає env.ok.
    // Хендлери, що кличуть runSync, — «голі» void-лямбди (НЕ через Ret): runSync
    // сам присвоює this->result, тож Ret перезаписав би корисний результат (AGENTS.md).
    auto runSync = [this](ResultEnvelope env) -> bool {
        lastResultJson_ = env.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        this->result = lastResultJson_;
        return env.ok;
    };

    // 0-параметрові методи: MethFunction0 (без VH). Раніше були (VH) без ParamSpec → 1С
    // вважала їх 1-параметровими й давала "Недостаточно фактических параметров" при виклику без аргументів.
    AddFunction(u"CheckConnection", u"ПроверитьСвязь",
        MethFunction(std::function<void()>([this, runSync]() {
            try { runSync(driver_.CheckConnection()); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка ПроверитьСвязь: ") + e.what()); runSync(ResultEnvelope::Fail("EXCEPTION", e.what())); }
        })));

    AddFunction(u"GetTerminalInfo", u"ВерсияПО",
        MethFunction(std::function<void()>([this, runSync]() {
            try { runSync(driver_.Execute("GetTerminalInfo", nlohmann::json::object(), kInfoTimeoutMs)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка ВерсияПО: ") + e.what()); runSync(ResultEnvelope::Fail("EXCEPTION", e.what())); }
        })));

    AddFunction(u"Purchase", u"Оплата",
        [this, runSync](VH amount) {
            try { runSync(driver_.Purchase(static_cast<std::string>(amount))); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Оплата: ") + e.what()); runSync(ResultEnvelope::Fail("EXCEPTION", e.what())); }
        },
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} } });

    AddFunction(u"Refund", u"Возврат",
        [this, runSync](VH amount, VH rrn) {
            try { runSync(driver_.Refund(static_cast<std::string>(amount), static_cast<std::string>(rrn))); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Возврат: ") + e.what()); runSync(ResultEnvelope::Fail("EXCEPTION", e.what())); }
        },
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} }, ParamSpec{ u"rrn", u"RRN", true, {} } });

    AddFunction(u"GetReceiptInfo", u"ПолучитьЧек",
        [this, runSync](VH invoice) {
            try { runSync(driver_.GetReceiptInfo(static_cast<std::string>(invoice))); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка ПолучитьЧек: ") + e.what()); runSync(ResultEnvelope::Fail("EXCEPTION", e.what())); }
        },
        std::vector<ParamSpec>{ ParamSpec{ u"invoiceNumber", u"НомерЧека", true, {} } });

    // --- Асинхронні операції ------------------------------------------------
    AddFunction(u"StartPurchase", u"НачатьОплату",
        Ret([this](VH amount) -> bool {
            try { return driver_.StartPurchase(static_cast<std::string>(amount)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка НачатьОплату: ") + e.what()); return false; }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} } });

    AddFunction(u"StartRefund", u"НачатьВозврат",
        Ret([this](VH amount, VH rrn) -> bool {
            try { return driver_.StartRefund(static_cast<std::string>(amount), static_cast<std::string>(rrn)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка НачатьВозврат: ") + e.what()); return false; }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} }, ParamSpec{ u"rrn", u"RRN", true, {} } });

    AddFunction(u"OperationState", u"СостояниеОперации",
        Ret([this]() -> int { return static_cast<int>(driver_.OperationState()); }));

    AddFunction(u"OperationResult", u"РезультатОперацииJSON",
        Ret([this]() -> std::string {
            ResultEnvelope out;
            if (driver_.TryGetOperationResult(out))
                return out.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return lastResultJson_;
        }));

    AddProcedure(u"CancelOperation", u"ПрерватьОперацию",
        MethFunction(std::function<void()>([this]() {
            try { driver_.CancelOperation(); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка ПрерватьОперацию: ") + e.what()); }
        })));

    AddFunction(u"LastStatus", u"СтатусТерминала",
        Ret([this]() -> int { return driver_.LastStatus(); }));

    // --- Ідентичність / трасування ------------------------------------------
    AddFunction(u"Vendor", u"Вендор", Ret([this]() -> std::string { return driver_.Vendor(); }));
    AddFunction(u"Model", u"Модель", Ret([this]() -> std::string { return driver_.Model(); }));

    AddFunction(u"EnableTrace", u"ВключитьТрассировку",
        Ret([this](VH enable) -> bool {
            bool on = static_cast<bool>(enable);
            driver_.SetTrace(on);
            REPORT_INFO(std::string("Трасування ") + (on ? "увімкнено" : "вимкнено") + " (діє з наступного Connect)");
            return on;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"enable", u"Включить", false, DefaultHelper(true) } });

    // Опційний пуш подій термінала в 1С через ВнешнееСобытие. За замовчуванням вимкнено;
    // якщо у 1С немає обробника ВнешнееСобытие — PostExternalEvent просто нічого не робить (не падає).
    AddFunction(u"EnableEvents", u"ВключитьСобытия",
        Ret([this](VH enable) -> bool {
            bool on = static_cast<bool>(enable);
            if (on) {
                // Обробник кличеться з poller/worker-потоку → PostExternalEvent потокобезпечний.
                driver_.SetEventHandler([this](const std::string& ev, const std::string& data) {
                    this->PostExternalEvent(AddInNative::MB2WCHAR(ev), AddInNative::MB2WCHAR(data));
                });
            } else {
                driver_.SetEventHandler(nullptr);
            }
            REPORT_INFO(std::string("Події термінала ") + (on ? "увімкнено" : "вимкнено"));
            return on;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"enable", u"Включить", false, DefaultHelper(true) } });

    REPORT_INFO("Реєстрація методів ECRPrivatJSON завершена");
}
