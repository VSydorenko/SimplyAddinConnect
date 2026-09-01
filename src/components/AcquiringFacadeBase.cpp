#include "../core/pch.h"
#include "AcquiringFacadeBase.h"
#include "../helpers/ServiceTools.h"

AcquiringFacadeBase::AcquiringFacadeBase() = default;

AcquiringFacadeBase::~AcquiringFacadeBase() {
    if (driver_) driver_->Close();
}

IAcquiringDriver& AcquiringFacadeBase::Driver() const {
    if (!driver_) driver_ = MakeDriver();
    return *driver_;
}

ResultEnvelope AcquiringFacadeBase::Unsupported(const std::string& method) {
    return AcquiringUnsupported(method);
}

// ---------------------------- операції ----------------------------

ResultEnvelope AcquiringFacadeBase::RunPurchase(double amount) {
    try {
        ResultEnvelope env = Driver().Purchase(amount);
        // Адаптер ловить власні винятки в EXCEPTION-конверт (щоб не впасти повз
        // ResultEnvelope), але сам не реєструє помилку для 1С — це робимо тут,
        // так само, як робив REPORT_ERROR до рефакторингу на IAcquiringDriver.
        if (env.code == "EXCEPTION") REPORT_ERROR(env.description);
        return env;
    }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ОплатитьПлатежнойКартой: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunRefund(double amount, const std::string& rrn) {
    try {
        ResultEnvelope env = Driver().Refund(amount, rrn);
        if (env.code == "EXCEPTION") REPORT_ERROR(env.description);
        return env;
    }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ВернутьПлатежПоПлатежнойКарте: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunVoid(double amount, const std::string& rrn) {
    // Є власна операція скасування — вона й правильна.
    ResultEnvelope env = Driver().Void(amount, rrn);
    // ⚠️⚠️ ВІДКАТ НА ПОВЕРНЕННЯ — ТІЛЬКИ при UNSUPPORTED. Це про гроші.
    // TIMEOUT/DISCONNECTED/DESYNC/SEND_FAILED означають «НЕВІДОМО, чи виконалось»:
    // термінал МІГ скасувати операцію, а ми просто не отримали відповіді. Відкат на
    // Refund після такого зрушив би гроші ДВІЧІ. UNSUPPORTED — єдина відповідь, що
    // гарантує: команда термінала не досягла й нічого не сталося.
    // Під тестом: TestVoidFallbackOnlyOnUnsupported у ecr_privatjson_selftest.
    if (env.code != "UNSUPPORTED") return env;

    if (!VoidAsRefundEnabled())
        return Unsupported("ОтменитьПлатежПоПлатежнойКарте");
    if (rrn.empty())
        return ResultEnvelope::Fail("BAD_INPUT",
            "Скасування виконується поверненням за RRN, але СсылочныйНомер порожній");
    REPORT_INFO("Скасування виконується поверненням за RRN " + rrn + " (VoidAsRefund)");
    return RunRefund(amount, rrn);
}

ResultEnvelope AcquiringFacadeBase::RunEmergencyVoid() {
    try {
        ResultEnvelope env = Driver().EmergencyVoid();
        // Дефолт IAcquiringDriver::EmergencyVoid() не знає імен методів 1С —
        // підміняємо опис на те ім'я, яке справді викликав користувач, щоб
        // повідомлення лишилось таким самим інформативним, як до рефакторингу.
        if (env.code == "UNSUPPORTED") return Unsupported("АварийнаяОтменаОперации");
        if (env.code == "EXCEPTION") REPORT_ERROR(env.description);
        return env;
    }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка АварийнаяОтменаОперации: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunDayTotals() {
    try {
        ResultEnvelope env = Driver().DayTotals();
        if (env.code == "EXCEPTION") REPORT_ERROR(env.description);
        return env;
    }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ИтогиДняПоКартам: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

// ---------------------------- гачки бази ----------------------------

BpoFacadeBase::DriverInfo AcquiringFacadeBase::BuildDriverInfo() const {
    // Name/Description бере драйвер: «…ПриватБанк» і «…BPOS1» — різні рядки.
    // Лог для POSTerminal вмикається за замовчуванням — вимога ІТС для цього типу.
    return DriverInfo{ Driver().DriverName(), Driver().DriverDescription(),
                       "POSTerminal", /*logEnabled*/ true };
}

std::string AcquiringFacadeBase::BuildSettingsXml() const { return Driver().SettingsXml(); }

std::string AcquiringFacadeBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Actions>"
           "<Action Name=\"XReport\" Caption=\"X-звіт (без вилучення)\"/>"
           "</Actions>";
}

bool AcquiringFacadeBase::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал), а не англійський
    // рядок ІТС. Приймаємо обидва написання — страховка від наступної редакції (§4.1).
    return value == "ЭквайринговыйТерминал" || ToUpperAscii(value) == "POSTERMINAL";
}

bool AcquiringFacadeBase::OpenDevice(std::string& deviceIdOut) {
    const ResultEnvelope env = Driver().Open(Params());
    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        return false;
    }
    deviceIdOut = "ECR-1";   // поняття власного id протокол не має — синтезуємо стабільний
    return true;
}

void AcquiringFacadeBase::CloseDevice() {
    if (driver_ && driver_->IsConnected()) driver_->Close();
}

bool AcquiringFacadeBase::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має

    // ⚠️ Уже підключений термінал перевіряємо НА ЖИВОМУ З'ЄДНАННІ. Безумовний
    // Open() ПЕРЕПІДКЛЮЧИВ би драйвер, а фінальний Close() лишив би його
    // роз'єднаним — при заповненому ИДУстройства на фасаді. Наступна
    // ОплатитьПлатежнойКартой пройшла б CheckDeviceId, дійшла до драйвера й
    // упала з NOT_CONNECTED, доки обладнання не перепідключать вручну. Каса при
    // цьому вважає термінал підключеним, бо тест щойно сказав «на зв'язку».
    // Окрема короткоживуча сесія лишається шляхом для випадку, коли підключення
    // ще немає (форма налаштувань кличе ТестУстройства одразу після
    // УстановитьПараметр) — тоді закривати за собою правильно.
    // Умова та сама, що на сусідньому фасаді AddinLabelPrinter::ProbeDevice.
    const bool reuseActive = !DeviceId().empty();

    std::string conn;
    if (!reuseActive) {
        const ResultEnvelope opened = Driver().Open(Params());
        if (!opened.ok) {
            SetError(CodeToInt(opened.code), opened.description);
            resultOut = opened.description;
            return false;
        }
        // Адреса, за якою відповів термінал, — головне, що адміністратор перевіряє
        // в результаті ТестУстройства. Open() кладе рядок підключення в payload.
        conn = PayloadStr(opened, "connection");
    }

    const ResultEnvelope env = Driver().Probe();
    const std::string vendor = Driver().Vendor();
    const std::string model = Driver().Model();
    if (!reuseActive) Driver().Close();   // закриваємо ЛИШЕ власну тимчасову сесію

    // Адреса відома тільки коли підключалися самі; при перевикористанні живого
    // з'єднання її взяти нізвідки — тоді порожніх дужок не лишаємо.
    const std::string where = conn.empty() ? std::string() : (" (" + conn + ")");
    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        resultOut = "Підключення є, але термінал відповів помилкою: " + env.code + where;
        return false;
    }
    resultOut = "Термінал на зв'язку: " + vendor + " " + model + where;
    ClearError();
    return true;
}

bool AcquiringFacadeBase::RunAction(const std::string& name) {
    if (name != "XReport") return BpoFacadeBase::RunAction(name);
    return MapEnvToBool(Driver().Audit());
}

// ======================= еквайрингові методи =======================

void AcquiringFacadeBase::RegisterAcquiringMethods() {

    // ---- Методи можливостей: різні в ru і ua, потрібні ОБИДВА ----

    // ru (ревізія >= 3004): XML із прапорцями. Прапорці невміючих операцій НЕ
    // виставляємо — конфігурація тоді відсіє їх ДО виклику драйвера (§2.3.1).
    AddFunction(u"TerminalParameters", u"ПараметрыТерминала",
        Ret([this](VH deviceId, VH out) -> bool {
            try {
                if (!CheckDeviceId(VariantToString(deviceId))) return false;
                const AcquiringCapabilities c = Capabilities();
                auto b = [](bool v) { return v ? "true" : "false"; };
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<TerminalParameters "
                    "TerminalID=\"" + XmlEscape(c.terminalId) + "\" "
                    "PrintSlipOnTerminal=\"" + b(c.printSlipOnTerminal) + "\" "
                    "ShortSlip=\"" + b(c.shortSlip) + "\" "
                    "CashWithdrawal=\"" + b(c.cashWithdrawal) + "\" "
                    "ElectronicCertificates=\"" + b(c.electronicCertificates) + "\" "
                    "PartialCancellation=\"" + b(c.partialCancellation) + "\" "
                    "ConsumerPresentedQR=\"" + b(c.consumerPresentedQR) + "\" "
                    "ListCardTransactions=\"" + b(c.listCardTransactions) + "\"/>");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} },
                                ParamSpec{ u"Parameters", u"ПараметрыТерминала", false, {} } });

    // ua: на місці ПараметрыТерминала — безпараметровий прапорець «термінал друкує
    // квитанції сам». Береться з тих самих можливостей, а не константою.
    AddFunction(u"PrintSlipOnTerminal", u"ПечатьКвитанцийНаТерминале",
        Ret([this]() -> bool { ClearError(); return Capabilities().printSlipOnTerminal; }));

    // ---- Еквайрингові методи БЕЗ гілок за ревізією (3004 і 4000 однакові) ----

    AddFunction(u"CardDayTotals", u"ИтогиДняПоКартам",
        Ret([this](VH deviceId, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            const ResultEnvelope env = RunDayTotals();
            if (!MapEnvToBool(env)) return false;
            slip = PayloadStr(env, "receipt");
            return true;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства",  false, {} },
                                ParamSpec{ u"SlipText", u"ТекстСлипЧека", false, {} } });

    AddFunction(u"EmergencyCancelOperation", u"АварийнаяОтменаОперации",
        Ret([this](VH deviceId) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            return MapEnvToBool(RunEmergencyVoid());
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    RegisterAsyncExtensions();

    REPORT_INFO("Реєстрація еквайрингових методів БПО-фасаду завершена");
}

// ==================== асинхронне розширення поверх БПО ====================
//
// Штатний обробник БПО цих методів не знає й ніколи не покличе — він працює лише
// синхронним контрактом. Їх бере РОЗШИРЕННЯ 1С із точки розширення РМК
// (`МенеджерОборудованияРМККлиентПереопределяемый.НачатьВыполнениеОперацииНа-
// ЭквайринговомТерминале`), взявши ТОЙ САМИЙ екземпляр через
// `ПодключенноеУстройство.ОбъектДрайвера`.
//
// Чому саме тут, а не в адаптері протоколу: доступ до термінала МОНОПОЛЬНИЙ.
// Обладнання вже підключене цим об'єктом, тож другий об'єкт до того самого
// термінала не під'єднається. Один об'єкт — обидва режими.
//
// Навіщо взагалі: під час синхронного виклику драйвера клієнтський потік 1С мертвий
// (зміряно зондом — див. bpo-contract.md §4), тож живий статус можливий лише коли
// операцію веде розширення: старт → полінг стану й статусу → результат.
void AcquiringFacadeBase::RegisterAsyncExtensions() {

    AddFunction(u"StartPurchase", u"НачатьОплату",
        Ret([this](VH amount) -> bool {
            try {
                if (DeviceId().empty()) { SetError(1, "Обладнання не підключено"); return false; }
                ClearError();
                return Driver().StartPurchase(VariantToDouble(amount));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка НачатьОплату: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Amount", u"Сумма", true, {} } });

    AddFunction(u"StartRefund", u"НачатьВозврат",
        Ret([this](VH amount, VH rrn) -> bool {
            try {
                if (DeviceId().empty()) { SetError(1, "Обладнання не підключено"); return false; }
                ClearError();
                return Driver().StartRefund(VariantToDouble(amount), VariantToString(rrn));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка НачатьВозврат: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Amount", u"Сумма", true, {} },
                                ParamSpec{ u"RRN", u"СсылочныйНомер", true, {} } });

    // 0 Idle, 1 Running, 2 Interrupting, 3 Done, 4 Error
    AddFunction(u"OperationState", u"СостояниеОперации",
        Ret([this]() -> int { return Driver().OperationState(); }));

    AddFunction(u"OperationResult", u"РезультатОперацииJSON",
        Ret([this]() -> std::string {
            ResultEnvelope out;
            if (Driver().TryGetOperationResult(out))
                return out.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return std::string("{}");
        }));

    // Онлайн-статус термінала, -1..11. Живий ЛИШЕ під час операції; читання дешеве
    // (атомік), термінала не турбує — тож полінг розширенням безпечний.
    AddFunction(u"LastStatus", u"СтатусТерминала",
        Ret([this]() -> int { return Driver().LastStatus(); }));

    AddProcedure(u"CancelOperation", u"ПрерватьОперацию",
        MethFunction(std::function<void()>([this]() {
            try { Driver().CancelOperation(); }
            catch (const std::exception& e) {
                REPORT_ERROR(std::string("Помилка ПрерватьОперацию: ") + e.what());
            }
        })));
}
