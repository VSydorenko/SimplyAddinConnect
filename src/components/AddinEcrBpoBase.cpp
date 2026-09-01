#include "../core/pch.h"
#include "AddinEcrBpoBase.h"
#include "../helpers/ServiceTools.h"
#include "../platform/MoneyFormat.h"
#include <cstdlib>

namespace {

// Таймаут короткого службового запиту (перевірка зв'язку при ТестУстройства).
constexpr int kProbeTimeoutMs = 5000;

} // namespace

AddinEcrBpoBase::AddinEcrBpoBase() = default;

AddinEcrBpoBase::~AddinEcrBpoBase() {
    driver_.Disconnect();
}

// ============================ помилки ============================

ResultEnvelope AddinEcrBpoBase::Unsupported(const std::string& method) {
    // Формулювання за вимогою ІТС §1.3: у описі помилки має бути прямо сказано,
    // що функція обладнанням не підтримується.
    return ResultEnvelope::Fail("UNSUPPORTED",
        "Операція \"" + method + "\" не підтримується обладнанням");
}

// ============================ хелпери ============================

bool AddinEcrBpoBase::VoidAsRefundEnabled() const {
    return ParamBool("VoidAsRefund", true);
}

std::string AddinEcrBpoBase::BuildConnectionString() const {
    const std::string kind = ToUpperAscii(Param("TransportKind", "tcp"));
    if (kind == "COM") {
        const std::string port = Param("ComPort");
        const std::string baud = Param("Baud", "115200");
        return port.empty() ? std::string() : (port + ":" + baud);
    }
    const std::string host = Param("Host");
    const std::string port = Param("Port", "2000");
    return host.empty() ? std::string() : ("tcp://" + host + ":" + port);
}

// ============================ операції ============================

ResultEnvelope AddinEcrBpoBase::RunPurchase(double amount) {
    try { return driver_.Purchase(MoneyToString(amount)); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ОплатитьПлатежнойКартой: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AddinEcrBpoBase::RunRefund(double amount, const std::string& rrn) {
    try { return driver_.Refund(MoneyToString(amount), rrn); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ВернутьПлатежПоПлатежнойКарте: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AddinEcrBpoBase::RunVoid(double amount, const std::string& rrn) {
    // Власної операції void протокол у нашій реалізації не має. Штатний шлях для
    // таких терміналів — скасування ПОВЕРНЕННЯМ за RRN (так само робить наявний
    // модуль Ingenico у розширенні). Керується параметром підключення, бо це
    // рішення бізнесу: void скасовує до звірки й сліду не лишає, refund створює
    // зворотну транзакцію.
    if (!VoidAsRefundEnabled())
        return Unsupported("ОтменитьПлатежПоПлатежнойКарте");

    if (rrn.empty()) {
        return ResultEnvelope::Fail("BAD_INPUT",
            "Скасування виконується поверненням за RRN, але СсылочныйНомер порожній");
    }
    REPORT_INFO("Скасування виконується поверненням за RRN " + rrn + " (VoidAsRefund)");
    return RunRefund(amount, rrn);
}

ResultEnvelope AddinEcrBpoBase::RunEmergencyVoid() {
    return Unsupported("АварийнаяОтменаОперации");
}

ResultEnvelope AddinEcrBpoBase::RunDayTotals() {
    // ИтогиДняПоКартам = підсумки на хост для звірки = Verify драйвера (спека §5.18).
    try { return driver_.Verify("0"); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ИтогиДняПоКартам: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

// ======================= гачки BpoFacadeBase =======================

BpoFacadeBase::DriverInfo AddinEcrBpoBase::BuildDriverInfo() const {
    // Лог для POSTerminal вмикається за замовчуванням — вимога ІТС для цього типу.
    return DriverInfo{ "Драйвер еквайрингового термінала (SimplyAddinConnect)",
                       "Платіжний термінал ПриватБанк, JSON-протокол, TCP/COM",
                       "POSTerminal",
                       /*logEnabled*/ true };
}

// Опис форми налаштувань. Формат СУВОРИЙ: корінь Settings, тип у TypeValue —
// інакше форма БПО мовчки лишається без жодного поля (§3.1 доку).
std::string AddinEcrBpoBase::BuildSettingsXml() const {
    return std::string(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Settings>"
        "<Page Caption=\"Параметри\">"
        "<Group Caption=\"Підключення\">"
        "<Parameter Name=\"TransportKind\" Caption=\"Транспорт\" TypeValue=\"String\" DefaultValue=\"tcp\">"
        "<ChoiceList>"
        "<Item Value=\"tcp\">Мережа (TCP/Wi-Fi)</Item>"
        "<Item Value=\"com\">COM/USB</Item>"
        "</ChoiceList>"
        "</Parameter>"
        "<Parameter Name=\"Host\" Caption=\"IP-адреса термінала\" TypeValue=\"String\" DefaultValue=\"\""
        " Description=\"Лише для транспорту tcp\"/>"
        "<Parameter Name=\"Port\" Caption=\"Порт\" TypeValue=\"Number\" DefaultValue=\"2000\""
        " Description=\"Лише для транспорту tcp; типовий порт термінала — 2000\"/>"
        "<Parameter Name=\"ComPort\" Caption=\"COM-порт\" TypeValue=\"String\" DefaultValue=\"\""
        " Description=\"Лише для транспорту com, напр. COM4\"/>"
        "<Parameter Name=\"Baud\" Caption=\"Швидкість COM\" TypeValue=\"Number\" DefaultValue=\"115200\"/>"
        "</Group>"
        "<Group Caption=\"Поведінка операцій\">"
        "<Parameter Name=\"VoidAsRefund\" Caption=\"Скасування виконувати поверненням\""
        " TypeValue=\"Boolean\" DefaultValue=\"true\""
        " Description=\"Термінал не має власної операції скасування. Увімкнено —"
        " скасування виконується поверненням за RRN (зворотна транзакція)."
        " Вимкнено — операція відхиляється як непідтримувана\"/>"
        "</Group>"
        "<Group Caption=\"Журналювання\">"
        "<Parameter Name=\"LogLevel\" Caption=\"Рівень деталізації\" TypeValue=\"String\" DefaultValue=\"Info\">"
        "<ChoiceList>"
        "<Item Value=\"Error\">Лише помилки</Item>"
        "<Item Value=\"Warn\">Попередження</Item>"
        "<Item Value=\"Info\">Звичайний</Item>"
        "<Item Value=\"Debug\">Детальний</Item>"
        "<Item Value=\"Trace\">Повний, з обміном</Item>"
        "</ChoiceList>"
        "</Parameter>"
        "<Parameter Name=\"LogPath\" Caption=\"Файл журналу\" TypeValue=\"String\" DefaultValue=\"\""
        " Description=\"Порожньо — журнал не пишеться у файл\"/>"
        "</Group>"
        "</Page>"
        "</Settings>");
}

std::string AddinEcrBpoBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Actions>"
           "<Action Name=\"XReport\" Caption=\"X-звіт (без вилучення)\"/>"
           "</Actions>";
}

bool AddinEcrBpoBase::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал), а не англійський
    // рядок ІТС. Приймаємо обидва написання — страховка від наступної редакції (§4.1).
    return value == "ЭквайринговыйТерминал" || ToUpperAscii(value) == "POSTERMINAL";
}

bool AddinEcrBpoBase::OpenDevice(std::string& deviceIdOut) {
    const std::string conn = BuildConnectionString();
    if (conn.empty()) {
        SetError(2, "Не задано параметри підключення до термінала");
        return false;
    }
    if (!driver_.Connect(conn)) {
        SetError(1, "Не вдалося підключитися до термінала: " + conn);
        return false;
    }
    deviceIdOut = "ECR-1";   // драйвер поняття id не має — синтезуємо стабільний
    return true;
}

void AddinEcrBpoBase::CloseDevice() { driver_.Disconnect(); }

bool AddinEcrBpoBase::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має
    const std::string conn = BuildConnectionString();
    if (conn.empty()) {
        SetError(2, "Не задано параметри підключення до термінала");
        resultOut = "Не задано параметри підключення";
        return false;
    }
    if (!driver_.Connect(conn)) {
        SetError(1, "Термінал не відповідає: " + conn);
        resultOut = "Термінал не відповідає (" + conn + ")";
        return false;
    }
    const ResultEnvelope env = driver_.Execute("GetTerminalInfo",
                                               nlohmann::json::object(), kProbeTimeoutMs);
    const std::string vendor = driver_.Vendor();
    const std::string model = driver_.Model();
    driver_.Disconnect();

    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        resultOut = "Підключення є, але термінал відповів помилкою: " + env.code;
        return false;
    }
    resultOut = "Термінал на зв'язку: " + vendor + " " + model + " (" + conn + ")";
    ClearError();
    return true;
}

bool AddinEcrBpoBase::RunAction(const std::string& name) {
    if (name != "XReport") return BpoFacadeBase::RunAction(name);
    return MapEnvToBool(driver_.Audit("0"));
}

// ======================= еквайрингові методи =======================

void AddinEcrBpoBase::RegisterAcquiringMethods() {

    // ---- Методи можливостей: різні в ru і ua, потрібні ОБИДВА ----

    // ru (ревізія >= 3004): XML із прапорцями. Прапорці невміючих операцій НЕ
    // виставляємо — конфігурація тоді відсіє їх ДО виклику драйвера (§2.3.1 доку).
    AddFunction(u"TerminalParameters", u"ПараметрыТерминала",
        Ret([this](VH deviceId, VH out) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!CheckDeviceId(id)) return false;
                const std::string model = driver_.Model();
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<TerminalParameters "
                    "TerminalID=\"" + XmlEscape(model) + "\" "
                    "PrintSlipOnTerminal=\"true\" "
                    "ShortSlip=\"false\" "
                    "CashWithdrawal=\"false\" "
                    "ElectronicCertificates=\"false\" "
                    "PartialCancellation=\"false\" "
                    "ConsumerPresentedQR=\"false\" "
                    "ListCardTransactions=\"false\"/>");
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
    // квитанції сам». N950 друкує, тож Истина: 1С не проситиме друк сліпа на ЧПУ.
    AddFunction(u"PrintSlipOnTerminal", u"ПечатьКвитанцийНаТерминале",
        Ret([this]() -> bool { ClearError(); return true; }));

    // ---- Еквайрингові методи БЕЗ гілок за ревізією (звірено: 3004 і 4000 однакові) ----

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
// Чому саме тут, а не в прямому класі `ECRPrivatJSON`: доступ до термінала
// МОНОПОЛЬНИЙ. Обладнання вже підключене цим об'єктом, тож другий об'єкт до того
// самого термінала не під'єднається. Один об'єкт — обидва режими.
//
// Навіщо взагалі: під час синхронного виклику драйвера клієнтський потік 1С мертвий
// (зміряно зондом — див. bpo-contract.md §4), тож живий статус можливий лише коли
// операцію веде розширення: старт → полінг стану й статусу → результат.
void AddinEcrBpoBase::RegisterAsyncExtensions() {

    AddFunction(u"StartPurchase", u"НачатьОплату",
        Ret([this](VH amount) -> bool {
            try {
                if (DeviceId().empty()) { SetError(1, "Обладнання не підключено"); return false; }
                ClearError();
                return driver_.StartPurchase(MoneyToString(static_cast<double>(amount)));
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
                return driver_.StartRefund(MoneyToString(static_cast<double>(amount)),
                                           VariantToString(rrn));
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
        Ret([this]() -> int { return static_cast<int>(driver_.OperationState()); }));

    AddFunction(u"OperationResult", u"РезультатОперацииJSON",
        Ret([this]() -> std::string {
            ResultEnvelope out;
            if (driver_.TryGetOperationResult(out))
                return out.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return std::string("{}");
        }));

    // Онлайн-статус термінала, -1..11. Живий ЛИШЕ під час операції; читання дешеве
    // (атомік), термінала не турбує — тож полінг розширенням безпечний.
    AddFunction(u"LastStatus", u"СтатусТерминала",
        Ret([this]() -> int { return driver_.LastStatus(); }));

    AddProcedure(u"CancelOperation", u"ПрерватьОперацию",
        MethFunction(std::function<void()>([this]() {
            try { driver_.CancelOperation(); }
            catch (const std::exception& e) {
                REPORT_ERROR(std::string("Помилка ПрерватьОперацию: ") + e.what());
            }
        })));
}
