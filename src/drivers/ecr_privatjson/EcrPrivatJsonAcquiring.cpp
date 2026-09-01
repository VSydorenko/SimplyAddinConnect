#include "../../core/pch.h"
#include "EcrPrivatJsonAcquiring.h"
#include "../../platform/MoneyFormat.h"
#include "../../helpers/ServiceTools.h"

namespace {
constexpr const char* kTag = "EcrPrivatJsonAcquiring";
/// Таймаут короткого службового запиту (перевірка зв'язку при ТестУстройства).
constexpr int kProbeTimeoutMs = 5000;

std::string ToUpperAscii(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string Get(const std::map<std::string, std::string>& p, const char* name,
                const std::string& fallback = "") {
    auto it = p.find(name);
    return (it == p.end() || it->second.empty()) ? fallback : it->second;
}
} // namespace

std::string EcrPrivatJsonAcquiring::BuildConnectionString(
        const std::map<std::string, std::string>& params) {
    const std::string kind = ToUpperAscii(Get(params, "TransportKind", "tcp"));
    if (kind == "COM") {
        const std::string port = Get(params, "ComPort");
        const std::string baud = Get(params, "Baud", "115200");
        return port.empty() ? std::string() : (port + ":" + baud);
    }
    const std::string host = Get(params, "Host");
    const std::string port = Get(params, "Port", "2000");
    return host.empty() ? std::string() : ("tcp://" + host + ":" + port);
}

ResultEnvelope EcrPrivatJsonAcquiring::Open(const std::map<std::string, std::string>& params) {
    const std::string conn = BuildConnectionString(params);
    if (conn.empty())
        return ResultEnvelope::Fail("BAD_INPUT", "Не задано параметри підключення до термінала");
    if (!drv_.Connect(conn))
        return ResultEnvelope::Fail("NOT_CONNECTED", "Не вдалося підключитися до термінала: " + conn);
    return ResultEnvelope::Ok({ { "connection", conn } });
}

void EcrPrivatJsonAcquiring::Close()            { drv_.Disconnect(); }
bool EcrPrivatJsonAcquiring::IsConnected() const { return drv_.IsConnected(); }
std::string EcrPrivatJsonAcquiring::Vendor() const { return drv_.Vendor(); }
std::string EcrPrivatJsonAcquiring::Model() const  { return drv_.Model(); }

std::string EcrPrivatJsonAcquiring::DriverName() const {
    return "Драйвер еквайрингового термінала (SimplyAddinConnect)";
}
std::string EcrPrivatJsonAcquiring::DriverDescription() const {
    return "Платіжний термінал ПриватБанк, JSON-протокол, TCP/COM";
}

// Опис форми налаштувань. Формат СУВОРИЙ: корінь Settings, тип у TypeValue —
// інакше форма БПО мовчки лишається без жодного поля (§3.1 контракту). Літерал
// перенесено ДОСЛІВНО з AddinEcrBpoBase::BuildSettingsXml() (Задача 1) — у BPOS1
// набір параметрів підключення буде свій, і фасад не має про це знати.
std::string EcrPrivatJsonAcquiring::SettingsXml() const {
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

AcquiringCapabilities EcrPrivatJsonAcquiring::Capabilities() const {
    AcquiringCapabilities c;
    // Справжнього TID протокол не віддає — модель як найближче стабільне значення.
    c.terminalId = drv_.Model();
    c.printSlipOnTerminal = true;   // N950 друкує квитанції сам; 1С не проситиме друк на ЧПУ
    // Решта лишається false: часткове скасування, видача готівки, Consumer-Presented QR,
    // електронні сертифікати й список операцій протокол/термінал не вміють
    // (docs/architecture/ecrprivatjson.md §8). Конфігурація відсіє їх ДО виклику драйвера.
    return c;
}

ResultEnvelope EcrPrivatJsonAcquiring::Probe() {
    return drv_.Execute("GetTerminalInfo", nlohmann::json::object(), kProbeTimeoutMs);
}

ResultEnvelope EcrPrivatJsonAcquiring::Purchase(double amount) {
    try { return drv_.Purchase(MoneyToString(amount)); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Purchase: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::Refund(double amount, const std::string& rrn) {
    try { return drv_.Refund(MoneyToString(amount), rrn); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Refund: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::DayTotals() {
    // ИтогиДняПоКартам = підсумки на хост для звірки = Verify драйвера (спека §5.18).
    try { return drv_.Verify("0"); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка DayTotals: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::Audit() {
    try { return drv_.Audit("0"); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Audit: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

bool EcrPrivatJsonAcquiring::StartPurchase(double amount) {
    return drv_.StartPurchase(MoneyToString(amount));
}
bool EcrPrivatJsonAcquiring::StartRefund(double amount, const std::string& rrn) {
    return drv_.StartRefund(MoneyToString(amount), rrn);
}
int  EcrPrivatJsonAcquiring::OperationState() const { return static_cast<int>(drv_.OperationState()); }
bool EcrPrivatJsonAcquiring::TryGetOperationResult(ResultEnvelope& out) const {
    return drv_.TryGetOperationResult(out);
}
int  EcrPrivatJsonAcquiring::LastStatus() const { return drv_.LastStatus(); }
void EcrPrivatJsonAcquiring::CancelOperation()  { drv_.CancelOperation(); }
