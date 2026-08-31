#include "../core/pch.h"
#include "AddinEcrBpoBase.h"
#include "../helpers/ServiceTools.h"
#include <cmath>
#include <cstdlib>

namespace {

// Таймаут короткого службового запиту (перевірка зв'язку при ТестУстройства).
constexpr int kProbeTimeoutMs = 5000;

std::string XmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;
        }
    }
    return out;
}

std::string ToUpperAscii(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

} // namespace

AddinEcrBpoBase::AddinEcrBpoBase() = default;

AddinEcrBpoBase::~AddinEcrBpoBase() {
    driver_.Disconnect();
    ServiceTools::DisableComponentLogging(this);
}

// ============================ помилки ============================

void AddinEcrBpoBase::SetError(int code, const std::string& description) {
    lastErrorCode_ = code;
    lastErrorDesc_ = description;
}

void AddinEcrBpoBase::ClearError() {
    lastErrorCode_ = 0;
    lastErrorDesc_.clear();
}

// Числова таксономія для ПолучитьОшибку (LONG). Протокольний код термінала —
// числовий, проходить як є; рядкові коди драйвера мапляться в стабільні числа
// (та сама схема, що в AddinLabelPrinter::CodeToInt).
int AddinEcrBpoBase::CodeToInt(const std::string& code) {
    if (code.empty()) return -1;
    bool numeric = true;
    for (char c : code) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) { try { return std::stoi(code); } catch (...) { return -1; } }
    if (code == "OK")            return 0;
    if (code == "NOT_CONNECTED") return 1;
    if (code == "DEVICE_BUSY")   return 2;
    if (code == "UNSUPPORTED")   return 3;
    if (code == "TIMEOUT")       return 4;
    if (code == "DISCONNECTED")  return 5;
    if (code == "SEND_FAILED")   return 6;
    if (code == "STOPPED")       return 7;
    if (code == "CONCURRENT")    return 8;
    if (code == "DESYNC")        return 9;
    if (code == "BAD_RESPONSE")  return 10;
    if (code == "EXCEPTION")     return 11;
    return -1;
}

bool AddinEcrBpoBase::MapEnvToBool(const ResultEnvelope& env) {
    if (env.ok) { ClearError(); return true; }
    SetError(CodeToInt(env.code), env.description.empty() ? env.code : env.description);
    return false;
}

ResultEnvelope AddinEcrBpoBase::Unsupported(const std::string& method) {
    // Формулювання за вимогою ІТС §1.3: у описі помилки має бути прямо сказано,
    // що функція обладнанням не підтримується.
    return ResultEnvelope::Fail("UNSUPPORTED",
        "Операція \"" + method + "\" не підтримується обладнанням");
}

// ============================ хелпери ============================

std::string AddinEcrBpoBase::PayloadStr(const ResultEnvelope& env, const char* field) {
    try {
        if (!env.payload.is_object()) return {};
        auto it = env.payload.find(field);
        if (it == env.payload.end()) return {};
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
        if (it->is_number()) return AmountToString(it->get<double>());
        return {};
    } catch (...) { return {}; }
}

std::string AddinEcrBpoBase::AmountToString(double amount) {
    // Через цілі копійки: printf-форматування залежить від локалі й дало б кому.
    const bool negative = amount < 0;
    const long long cents = std::llround(std::fabs(amount) * 100.0);
    std::string s = (negative ? "-" : "") + std::to_string(cents / 100) + ".";
    const long long frac = cents % 100;
    if (frac < 10) s += '0';
    s += std::to_string(frac);
    return s;
}

std::string AddinEcrBpoBase::VariantToString(VH value) {
    try {
        switch (value.type()) {
        case VTYPE_PWSTR:
            return static_cast<std::string>(value);
        case VTYPE_EMPTY:
        case VTYPE_NULL:
            return {};
        case VTYPE_BOOL:
            return static_cast<bool>(value) ? "true" : "false";
        case VTYPE_R4:
        case VTYPE_R8: {
            const double d = value;
            // Ціле значення віддаємо без дробової частини: Port=2000, а не 2000.00.
            if (d == static_cast<double>(static_cast<long long>(d)))
                return std::to_string(static_cast<long long>(d));
            return AmountToString(d);
        }
        default:
            return std::to_string(static_cast<int64_t>(value));
        }
    } catch (...) {
        return {};
    }
}

double AddinEcrBpoBase::VariantToDouble(VH value) {
    try { return static_cast<double>(value); }
    catch (...) { return 0.0; }
}

bool AddinEcrBpoBase::VoidAsRefundEnabled() const {
    const std::string v = ToUpperAscii(Param("VoidAsRefund", "true"));
    return !(v == "FALSE" || v == "0" || v == "НЕТ" || v == "НІ");
}

std::string AddinEcrBpoBase::Param(const char* name, const std::string& fallback) const {
    auto it = params_.find(name);
    return (it == params_.end() || it->second.empty()) ? fallback : it->second;
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

bool AddinEcrBpoBase::CheckDeviceId(const std::string& deviceId) {
    if (deviceId_.empty()) {
        SetError(1, "Обладнання не підключено");
        return false;
    }
    if (deviceId != deviceId_) {
        SetError(1, "Невідомий ідентифікатор пристрою: " + deviceId);
        return false;
    }
    return true;
}

// ============================ операції ============================

ResultEnvelope AddinEcrBpoBase::RunPurchase(double amount) {
    try { return driver_.Purchase(AmountToString(amount)); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ОплатитьПлатежнойКартой: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AddinEcrBpoBase::RunRefund(double amount, const std::string& rrn) {
    try { return driver_.Refund(AmountToString(amount), rrn); }
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

// ============================ системні методи ============================

void AddinEcrBpoBase::RegisterSystemMethods() {

    AddFunction(u"GetInterfaceRevision", u"ПолучитьРевизиюИнтерфейса",
        Ret([this]() -> int { return InterfaceRevision(); }));

    AddFunction(u"GetVersionNumber", u"ПолучитьНомерВерсии",
        Ret([]() -> std::string { return AddInNative::version(); }));

    // Паспорт драйвера. EquipmentType тут ІНФОРМАЦІЙНИЙ (конфігурація його з переліком
    // не звіряє) — на відміну від EquipmentType, що ПРИХОДИТЬ в УстановитьПараметр.
    // Булеві читаються як ВРег(...) = "TRUE", тож "1" не спрацював би. Лог для POSTerminal
    // вмикається за замовчуванням — вимога ІТС для цього типу обладнання.
    AddFunction(u"GetDescription", u"ПолучитьОписание",
        Ret([this](VH out) -> bool {
            try {
                const std::string ver = AddInNative::version();
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<DriverDescription "
                    "Name=\"Драйвер еквайрингового термінала (SimplyAddinConnect)\" "
                    "Description=\"Платіжний термінал ПриватБанк, JSON-протокол, TCP/COM\" "
                    "EquipmentType=\"POSTerminal\" "
                    "IntegrationComponent=\"false\" "
                    "MainDriverInstalled=\"true\" "
                    "DriverVersion=\"" + ver + "\" "
                    "IntegrationComponentVersion=\"" + ver + "\" "
                    "IsEmulator=\"false\" "
                    "LocalizationSupported=\"false\" "
                    "LogIsEnabled=\"true\" "
                    "LogPath=\"" + XmlEscape(Param("LogPath")) + "\"/>");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ПолучитьОписание: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverDescription", u"ОписаниеДрайвера", false, {} } });

    // Опис форми налаштувань. Формат СУВОРИЙ: корінь Settings, тип у TypeValue —
    // інакше форма БПО мовчки лишається без жодного поля (§3.1 доку).
    AddFunction(u"GetParameters", u"ПолучитьПараметры",
        Ret([this](VH out) -> bool {
            try {
                out = std::string(
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
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverParameters", u"ПараметрыДрайвера", false, {} } });

    // Параметри приходять ПООДИНЦІ до Подключить, і першим — EquipmentType.
    AddFunction(u"SetParameter", u"УстановитьПараметр",
        Ret([this](VH name, VH value) -> bool {
            try {
                // ⚠️ Тільки через VariantToString: Port/Baud приходять ЧИСЛАМИ, а
                // VoidAsRefund — БУЛЕВИМ, і пряме приведення до рядка кинуло б.
                const std::string n = VariantToString(name);
                const std::string v = VariantToString(value);
                if (n == "EquipmentType") {
                    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал),
                    // а не англійський рядок ІТС. Порівнюємо толерантно й приймаємо
                    // обидва написання — страховка від наступної редакції (§4.1 доку).
                    const std::string up = ToUpperAscii(v);
                    if (v != "ЭквайринговыйТерминал" && up != "POSTERMINAL") {
                        SetError(2, "Непідтримуваний тип обладнання: " + v);
                        return false;
                    }
                }
                params_[n] = v;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Name", u"ИмяПараметра", true, {} },
                                ParamSpec{ u"Value", u"ЗначениеПараметра", false, DefaultHelper(u"") } });

    // Підключення: параметрів не приймає (вони вже накопичені), ИДУстройства — OUT.
    AddFunction(u"Connect", u"Подключить",
        Ret([this](VH deviceIdOut) -> bool {
            try {
                // Журнал для POSTerminal вмикаємо самі — вимога ІТС; рівень і шлях
                // беремо з параметрів підключення.
                const std::string logPath = Param("LogPath");
                if (!logPath.empty())
                    ServiceTools::EnableComponentLogging(this, Param("LogLevel", "Info"), logPath);

                const std::string conn = BuildConnectionString();
                if (conn.empty()) {
                    SetError(2, "Не задано параметри підключення до термінала");
                    return false;
                }
                if (!driver_.Connect(conn)) {
                    SetError(1, "Не вдалося підключитися до термінала: " + conn);
                    return false;
                }
                deviceId_ = "ECR-1";
                deviceIdOut = deviceId_;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка Подключить: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    AddFunction(u"Disconnect", u"Отключить",
        Ret([this](VH deviceId) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!deviceId_.empty() && id != deviceId_) {
                    SetError(1, "Невідомий ідентифікатор пристрою: " + id);
                    return false;
                }
                driver_.Disconnect();
                deviceId_.clear();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    // Перевірка параметрів підключення з форми налаштувань: коротка сесія
    // Connect -> ПроверитьСвязь -> Disconnect. Демо-режиму драйвер не має.
    AddFunction(u"EquipmentTest", u"ТестУстройства",
        Ret([this](VH resultOut, VH demoOut) -> bool {
            demoOut = false;
            try {
                const std::string conn = BuildConnectionString();
                if (conn.empty()) {
                    SetError(2, "Не задано параметри підключення до термінала");
                    resultOut = std::string("Не задано параметри підключення");
                    return false;
                }
                if (!driver_.Connect(conn)) {
                    SetError(1, "Термінал не відповідає: " + conn);
                    resultOut = std::string("Термінал не відповідає (" + conn + ")");
                    return false;
                }
                const ResultEnvelope env = driver_.Execute("GetTerminalInfo",
                                                           nlohmann::json::object(), kProbeTimeoutMs);
                const std::string vendor = driver_.Vendor();
                const std::string model = driver_.Model();
                driver_.Disconnect();

                if (!env.ok) {
                    SetError(CodeToInt(env.code), env.description);
                    resultOut = std::string("Підключення є, але термінал відповів помилкою: " + env.code);
                    return false;
                }
                resultOut = std::string("Термінал на зв'язку: " + vendor + " " + model + " (" + conn + ")");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                resultOut = std::string("Помилка тесту: ") + e.what();
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"TestResult", u"РезультатТеста", false, {} },
                                ParamSpec{ u"DemoMode", u"АктивированДемоРежим", false, {} } });

    AddFunction(u"GetLastError", u"ПолучитьОшибку",
        Ret([this](VH descriptionOut) -> int {
            descriptionOut = lastErrorDesc_;
            return lastErrorCode_;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ErrorDescription", u"ОписаниеОшибки", false, {} } });

    // Додаткові дії — пункти меню «Функції» форми налаштувань обладнання
    // (форма АДМІНІСТРАТОРА, не робоче місце касира).
    AddFunction(u"GetAdditionalActions", u"ПолучитьДополнительныеДействия",
        Ret([this](VH out) -> bool {
            out = std::string(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<Actions>"
                "<Action Name=\"XReport\" Caption=\"X-звіт (без вилучення)\"/>"
                "</Actions>");
            ClearError();
            return true;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Actions", u"ДополнительныеДействия", false, {} } });

    AddFunction(u"DoAdditionalAction", u"ВыполнитьДополнительноеДействие",
        Ret([this](VH actionName) -> bool {
            try {
                const std::string action = VariantToString(actionName);
                if (action != "XReport") {
                    SetError(2, "Невідома додаткова дія: " + action);
                    return false;
                }
                return MapEnvToBool(driver_.Audit("0"));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ActionName", u"ИмяДействия", true, {} } });

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

    REPORT_INFO("Реєстрація системних методів БПО-фасаду еквайрингу завершена");
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
                if (deviceId_.empty()) { SetError(1, "Обладнання не підключено"); return false; }
                ClearError();
                return driver_.StartPurchase(AmountToString(static_cast<double>(amount)));
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
                if (deviceId_.empty()) { SetError(1, "Обладнання не підключено"); return false; }
                ClearError();
                return driver_.StartRefund(AmountToString(static_cast<double>(amount)),
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
