// PrroFsService — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "PrroFsService.h"      // тягне winsock2.h — до windows.h

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "UapkiOracle.h"

#include <windows.h>

using nlohmann::json;

namespace prrofs {
namespace {

const size_t kMinBody = 10;        // [споживач] вимір на бойовому ДПС (§5)
const size_t kMaxBody = 512000;
const char*  kSubjectKeyId = "0000000000000000000000000000000000000000000000000000000000000000";

std::string StripJsonPrefix(const std::string& s) {
    size_t i = 0;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) i = 3;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

std::string StrField(const json& q, const char* k) {
    if (!q.contains(k) || q[k].is_null()) return std::string();
    if (q[k].is_string()) return q[k].get<std::string>();
    if (q[k].is_number_integer()) return std::to_string(q[k].get<long long>());
    return std::string();
}

bool IntField(const json& q, const char* k, long long& out) {
    if (!q.contains(k) || q[k].is_null()) return false;
    if (q[k].is_number_integer()) { out = q[k].get<long long>(); return true; }
    if (q[k].is_string()) {
        const std::string s = q[k].get<std::string>();
        char* e = nullptr;
        out = std::strtoll(s.c_str(), &e, 10);
        return !s.empty() && *e == '\0';
    }
    return false;
}

json Money(int64_t v) { return json(static_cast<double>(v) / 100.0); }
json NullIfEmpty(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

minihttp::Response Text(int code, const std::string& text) {
    minihttp::Response r;
    r.code = code;
    r.contentType = "text/plain; charset=utf-8";
    r.body = text;
    return r;
}

minihttp::Response Json(int code, const json& j) {
    minihttp::Response r;
    r.code = code;
    r.contentType = "application/json; charset=utf-8";
    r.body = j.dump();
    return r;
}

minihttp::Response Binary(const std::string& der) {
    minihttp::Response r;
    r.contentType = "application/octet-stream";
    r.body = der;
    return r;
}

minihttp::Response NoContent() {
    minihttp::Response r;
    r.code = 204;
    return r;
}

minihttp::Response ErrorResponse(int code, const std::string& text) {
    return Text(400, ErrorBody(code, text));
}

json TaxObjectJson(const std::vector<const RegistrarState*>& regs) {
    json trs = json::array();
    for (const RegistrarState* r : regs) {
        trs.push_back({ { "NumFiscal", std::stoll(r->numFiscal) }, { "NumLocal", r->numLocal },
                        { "Name", "Тестовий ПРРО " + std::to_string(r->numLocal) }, { "Closed", false } });
    }
    return { { "Entity", 1 }, { "TaxObjGuid", "00000000-0000-0000-0000-000000000001" }, { "TaxObjId", 1 },
             { "SingleTax", false }, { "Name", "Тестова господарська одиниця" },
             { "Address", "м. Київ, вул. Тестова, 1" }, { "Tin", "99900000" }, { "Ipn", "999000000009" },
             { "OrgName", "ТОВ \"ТЕСТ\"" }, { "ChiefCashier", true }, { "TransactionsRegistrars", trs } };
}

json OrderJson(const OrderTypeTotals& o) {
    json pf = json::array();
    for (const PayFormTotal& p : o.payForms)
        pf.push_back({ { "PayFormCode", p.code }, { "PayFormName", p.name }, { "Sum", Money(p.sum) } });
    json tx = json::array();
    for (const TaxTotal& t : o.taxes)
        tx.push_back({ { "Type", t.type }, { "Name", t.name }, { "Letter", t.letter }, { "Prc", Money(t.prc) },
                       { "Sign", t.sign }, { "Turnover", Money(t.turnover) },
                       { "TurnoverDiscount", Money(t.turnoverDiscount) }, { "SourceSum", Money(t.sourceSum) },
                       { "Sum", Money(t.sum) } });
    return { { "Sum", Money(o.sum) }, { "PwnSumIssued", 0 }, { "PwnSumReceived", 0 },
             { "RndSum", Money(o.rndSum) }, { "NoRndSum", Money(o.noRndSum) },
             { "TotalCurrencySum", 0 }, { "TotalCurrencyCommission", 0 },
             { "OrdersCount", o.ordersCount }, { "PwnOrdersCountIssued", 0 }, { "PwnOrdersCountReceived", 0 },
             { "TotalCurrencyCost", 0 }, { "PayForm", pf }, { "Tax", tx } };
}

// Totals — ЗАВЖДИ повної форми (§4.7): друк споживача звертається до ключів прямо.
json TotalsJson(const ShiftTotals& t) {
    return { { "Real", OrderJson(t.real) }, { "Ret", OrderJson(t.ret) }, { "Cash", nullptr },
             { "Currency", nullptr }, { "ServiceInput", Money(t.serviceInput) },
             { "ServiceOutput", Money(t.serviceOutput) } };
}

json ShiftJson(const ShiftRecord& s) {
    const bool closed = !s.closed.empty();
    return { { "ShiftId", s.shiftId }, { "OpenShiftFiscalNum", s.openShiftFiscalNum },
             { "CloseShiftFiscalNum", NullIfEmpty(s.closeShiftFiscalNum) }, { "Testing", s.testing },
             { "Opened", s.opened }, { "OpenName", s.openName }, { "OpenSubjectKeyId", kSubjectKeyId },
             { "Closed", NullIfEmpty(s.closed) }, { "CloseName", NullIfEmpty(s.closeName) },
             { "CloseSubjectKeyId", closed ? json(kSubjectKeyId) : json(nullptr) },
             { "ZRepFiscalNum", NullIfEmpty(s.zRepFiscalNum) } };
}

// Візуалізація документа (Type 3): UTF-8 ([Опис] ~633-689).
std::string Visualization(const StoredDoc& d) {
    ParsedDoc p;
    std::string err;
    const bool ok = ParseDocument(d.originalXml, p, err);
    std::string t = (d.klass == DocClass::ZRep) ? "Z-звіт\n" : "Чек\n";
    t += "Локальний номер: " + std::to_string(d.localNum) + "\n";
    t += "Фіскальний номер: " + d.fiscalNum + "\n";
    if (ok && d.klass == DocClass::Check) t += "Сума: " + FormatMoney(p.totalSum) + "\n";
    return t;
}

}  // namespace

// Розгортання тіла /fs/cmd — ОДНОРАЗОВЕ й ЛІНИВЕ (Task 6, amendment A): і лямбда
// CommandNameOf, передана в faults_.Take(...), і HandleCmd отримують посилання на
// той самий локальний UnwrappedCmd із Handle; повторний виклик UnwrapCmd — no-op.
// Для цілей "doc" і generic "cmd" ця функція взагалі не викликається (§8.2): тіло
// лишається неторканим до спрацювання.
struct UnwrappedCmd {
    bool        done            = false;   // обчислено (успішно чи ні)
    bool        ok              = false;   // розгорнуто й розпарсено як JSON-об'єкт
    bool        signatureFailed = false;   // тіло було CMS, підпис/розгортання не пройшло
    std::string text;                      // розгорнутий JSON-текст (валідний лише якщо ok)
    bool        isSigned        = false;
    json        q;                         // розібраний об'єкт (валідний лише якщо ok)
};

namespace {

void UnwrapCmd(const std::string& body, UnwrappedCmd& m) {
    if (m.done) return;
    m.done = true;
    std::string text = StripJsonPrefix(body);          // рядок 1: JSON чи CMS — за першим символом
    if (text.empty() || text[0] != '{') {
        const oracle::VerifyOutcome v = oracle::Verify(body);
        std::string content;
        if (!v.accepted || !oracle::b64decode(v.contentB64, content)) { m.signatureFailed = true; return; }
        // Amendment B: без другого StripJsonPrefix і без перевірки першого символу тут —
        // лексер nlohmann сам пропускає BOM/пробіли (extern/nlohmann_json/include/nlohmann/
        // detail/input/lexer.hpp:1495-1516); власного механізму на цьому шляху нема.
        text = std::move(content);
        m.isSigned = true;
    }
    json q = json::parse(text, nullptr, false);
    if (q.is_discarded() || !q.is_object()) return;     // ok лишається false
    m.text = std::move(text);
    m.q    = std::move(q);
    m.ok   = true;
}

}  // namespace

std::time_t ServerNow(int skewSeconds) {
    return std::time(nullptr) + skewSeconds;
}

std::string HttpDate(std::time_t t) {
    static const char* kDays[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    std::tm g{};
    gmtime_s(&g, &t);
    char b[64];
    std::snprintf(b, sizeof(b), "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[g.tm_wday], g.tm_mday,
                  kMonths[g.tm_mon], g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string IsoLocal(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    std::tm copy = l;
    const long long offset = static_cast<long long>(_mkgmtime(&copy)) - static_cast<long long>(t);
    const char sign = offset < 0 ? '-' : '+';
    const long long a = offset < 0 ? -offset : offset;
    char b[48];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d%c%02lld:%02lld", l.tm_year + 1900,
                  l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec, sign, a / 3600, (a % 3600) / 60);
    return b;
}

std::string OrderDate(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d%02d%04d", l.tm_mday, l.tm_mon + 1, l.tm_year + 1900);
    return b;
}

std::string OrderTime(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d%02d%02d", l.tm_hour, l.tm_min, l.tm_sec);
    return b;
}

bool ParseDateTime(const std::string& s, long long& epoch) {
    if (s.rfind("/Date(", 0) == 0) {
        char* e = nullptr;
        const long long ms = std::strtoll(s.c_str() + 6, &e, 10);
        if (e == s.c_str() + 6) return false;
        epoch = ms / 1000;
        return true;
    }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return false;
    std::tm tmv{};
    tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
    tmv.tm_hour = h;        tmv.tm_min = m;     tmv.tm_sec = sec;
    size_t p = 19;
    if (p < s.size() && s[p] == '.') { ++p; while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p; }
    if (p >= s.size()) {                       // без зсуву — місцевий час
        tmv.tm_isdst = -1;
        const std::time_t lt = std::mktime(&tmv);
        if (lt == static_cast<std::time_t>(-1)) return false;
        epoch = static_cast<long long>(lt);
        return true;
    }
    const long long asUtc = static_cast<long long>(_mkgmtime(&tmv));
    if (s[p] == 'Z') { epoch = asUtc; return true; }
    if (s[p] == '+' || s[p] == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(s.c_str() + p + 1, "%2d:%2d", &oh, &om) != 2) return false;
        const long long off = oh * 3600LL + om * 60LL;
        epoch = (s[p] == '+') ? asUtc - off : asUtc + off;
        return true;
    }
    return false;
}

std::string ErrorBody(int code, const std::string& description) {
    return "Код помилки: " + std::to_string(code) + " " + ErrorCodeName(code) + "\r\n" + description;
}

minihttp::HeaderList PrroFsService::CommonHeaders() {
    int skew = 0;
    { std::lock_guard<std::mutex> lk(mx_); skew = faults_.dateSkewSeconds; }
    return { { "Date", HttpDate(ServerNow(skew)) } };
}

minihttp::Response PrroFsService::Handle(const minihttp::Request& req) {
    if (trace_) { std::printf("← %s %s (%zu B)\n", req.method.c_str(), req.uri.c_str(), req.body.size()); std::fflush(stdout); }
    std::string path = req.uri;
    const size_t qm = path.find('?');
    if (qm != std::string::npos) path.resize(qm);

    if (req.method == "GET" && path == "/ping") return Text(200, "prro_fs_emulator alive");
    if (req.method == "POST" && path == "/control") { std::lock_guard<std::mutex> lk(mx_); return HandleControl(req.body); }
    if (req.method == "GET" && path == "/control/state") { std::lock_guard<std::mutex> lk(mx_); return StateJson(); }

    const bool isDoc = (req.method == "POST" && path == "/fs/doc");
    const bool isCmd = (req.method == "POST" && path == "/fs/cmd");
    if (!isDoc && !isCmd) return Text(404, "unknown endpoint");

    // Розгортання тіла — ЛІНИВЕ: memo лишається порожнім, доки CommandNameOf (лише коли
    // взведено "cmd:<Команда>") чи HandleCmd його не торкнуться (Task 6, amendment A).
    UnwrappedCmd memo;

    // Збій береться під замком; пауза й утримання — БЕЗ замка (Review Focus 1).
    Fault f;
    bool faulted = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        faulted = faults_.Take(isDoc ? "doc" : "cmd",
                                [this, &req, &memo]() { return CommandNameOf(req.body, memo); }, f);
    }
    if (faulted) {
        if (trace_) std::printf("  збій: %s\n", FaultModeName(f.mode));
        if (f.mode == FaultMode::Delay)              std::this_thread::sleep_for(std::chrono::seconds(f.seconds));
        if (f.mode == FaultMode::Status)             return FaultStatus(f);
        if (f.mode == FaultMode::DropBeforeRegister) return FaultDrop(f);
    }

    minihttp::Response r;
    {
        std::lock_guard<std::mutex> lk(mx_);
        r = isDoc ? HandleDoc(req.body) : HandleCmd(req.body, memo);
    }
    if (faulted && f.mode == FaultMode::DropAfterRegister) return FaultDrop(f);   // стан змінено, відповідь не йде
    return r;
}

std::string PrroFsService::CommandNameOf(const std::string& body, UnwrappedCmd& memo) {
    UnwrapCmd(body, memo);
    return memo.ok ? StrField(memo.q, "Command") : std::string();
}

minihttp::Response PrroFsService::FaultStatus(const Fault& f) {
    minihttp::Response r;
    r.code = f.code;
    r.body = (f.code == 204) ? std::string() : f.body;
    if (f.code == 302)
        r.headers.push_back({ "Location", f.location.empty()
                                          ? "http://127.0.0.1:" + std::to_string(port_) + "/moved"
                                          : f.location });
    return r;
}

minihttp::Response PrroFsService::FaultDrop(const Fault& f) {
    minihttp::Response r;
    r.disposition = (f.holdSeconds > 0) ? minihttp::Disposition::HoldThenAbort : minihttp::Disposition::Abort;
    r.holdSeconds = f.holdSeconds;
    return r;
}

std::string PrroFsService::BuildTicket(const ParsedDoc* doc, const std::string& taxNum, std::time_t now,
                                       int errorCode, const std::string& errorTextUtf8) {
    std::string x = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n<TICKET>";
    x += "<UID>" + XmlEscape(doc ? doc->uid : std::string()) + "</UID>";
    x += "<ORDERDATE>" + OrderDate(now) + "</ORDERDATE>";
    x += "<ORDERTIME>" + OrderTime(now) + "</ORDERTIME>";
    if (doc) x += "<ORDERNUM>" + std::to_string(doc->orderNum) + "</ORDERNUM>";
    if (!taxNum.empty()) x += "<ORDERTAXNUM>" + taxNum + "</ORDERTAXNUM>";
    x += "<OFFLINESESSIONID>0</OFFLINESESSIONID><OFFLINESEED>0</OFFLINESEED>";
    x += "<ERRORCODE>" + std::to_string(errorCode) + "</ERRORCODE>";
    x += "<ERRORTEXT>" + XmlEscape(errorTextUtf8) + "</ERRORTEXT>";
    x += "<VER>1</VER></TICKET>";
    return Utf8ToCp1251(x);
}

minihttp::Response PrroFsService::Reject(int code, const std::string& text, const ParsedDoc* doc) {
    if (trace_) std::printf("  відмова: %d %s — %s\n", code, ErrorCodeName(code), text.c_str());
    if (faults_.rejectFormat == RejectFormat::Ticket) {
        const std::string der = oracle::Sign(BuildTicket(doc, std::string(),
                                                         ServerNow(faults_.dateSkewSeconds), code, text));
        if (der.empty()) return Text(500, "cannot sign ticket");
        return Binary(der);
    }
    return Text(400, ErrorBody(code, text));
}

minihttp::Response PrroFsService::HandleDoc(const std::string& body) {
    if (body.size() < kMinBody || body.size() > kMaxBody)
        return Text(416, "Недопустимий розмір повідомлення: " + std::to_string(body.size()) + " байт (допустимо 10…512000)");
    const oracle::VerifyOutcome v = oracle::Verify(body);
    std::string xml;
    if (!v.accepted || !oracle::b64decode(v.contentB64, xml))
        return Reject(kDocumentValidationError, "Підпис документа не пройшов перевірку"
                      + (v.errorText.empty() ? std::string() : " (" + v.errorText + ")"), nullptr);
    ParsedDoc d;
    std::string err;
    if (!ParseDocument(xml, d, err)) return Reject(kDocumentValidationError, "Документ не розбирається: " + err, nullptr);

    const std::time_t now = ServerNow(faults_.dateSkewSeconds);
    const SubmitResult sr = state_.Submit(d, xml, IsoLocal(now), static_cast<long long>(now));
    if (sr.errorCode != kOk) return Reject(sr.errorCode, sr.errorText, &d);

    const std::string der = oracle::Sign(BuildTicket(&d, sr.fiscalNum, now, kOk, std::string()));
    if (der.empty()) return Text(500, "cannot sign ticket");
    if (trace_) std::printf("  /fs/doc: прийнято ORDERNUM=%lld ORDERTAXNUM=%s\n", d.orderNum, sr.fiscalNum.c_str());
    return Binary(der);
}

minihttp::Response PrroFsService::HandleCmd(const std::string& body, UnwrappedCmd& memo) {
    if (body.size() < kMinBody || body.size() > kMaxBody)
        return Text(416, "Недопустимий розмір повідомлення: " + std::to_string(body.size()) + " байт (допустимо 10…512000)");
    try {
        // memo — та сама структура, яку (можливо) уже торкнулась CommandNameOf у Handle
        // (Task 6, amendment A): UnwrapCmd повторно не розбирає, якщо done уже true.
        UnwrapCmd(body, memo);
        if (memo.signatureFailed) return ErrorResponse(kDocumentValidationError, "Підпис команди не пройшов перевірку");
        if (!memo.ok) return ErrorResponse(kInvalidQueryParameter, "Запит не є JSON-об'єктом");
        const json& q = memo.q;

        const std::string cmd = StrField(q, "Command");
        const std::string uid = StrField(q, "UID");
        static const std::set<std::string> kSigned = { "Objects", "TransactionsRegistrarState", "ZRepExt",
                                                       "Shifts", "LastShiftTotals" };
        if (kSigned.count(cmd) && !memo.isSigned)
            return ErrorResponse(kDocumentValidationError, "Запит " + cmd + " має бути засвідчений КЕП");
        const std::time_t now = ServerNow(faults_.dateSkewSeconds);
        const std::string ts = IsoLocal(now);

        if (cmd == "ServerState") return Json(200, { { "UID", uid }, { "Timestamp", ts } });

        if (cmd == "Objects") {
            const std::vector<const RegistrarState*> regs = state_.Registrars();
            if (regs.empty()) return NoContent();
            return Json(200, { { "UID", uid }, { "Timestamp", ts }, { "TaxObjects", json::array({ TaxObjectJson(regs) }) } });
        }

        if (cmd == "TransactionsRegistrarState") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r) return NoContent();
            json j = {
                { "UID", uid }, { "Timestamp", ts },
                { "ShiftState", r->shiftOpen ? 1 : 0 },
                { "ShiftId", r->shiftId },
                { "OpenShiftFiscalNum", r->shiftOpen ? json(r->openShiftFiscalNum) : json(nullptr) },
                { "ZRepPresent", r->zRepPresent },
                { "Testing", r->testing },
                { "Name", r->shiftId ? json(r->name) : json(nullptr) },
                { "SubjectKeyId", r->shiftOpen ? json(kSubjectKeyId) : json(nullptr) },
                { "FirstLocalNum", r->shiftOpen ? r->firstLocalNum : 0LL },
                { "NextLocalNum", r->nextLocalNum },
                { "LastFiscalNum", NullIfEmpty(r->lastFiscalNum) },
                { "OfflineSupported", false }, { "ChiefCashier", true },
                { "OfflineSessionId", nullptr }, { "OfflineSeed", nullptr }, { "OfflineNextLocalNum", nullptr },
                { "OfflineSessionDuration", nullptr }, { "OfflineSessionsMonthlyDuration", nullptr },
                { "OfflineSessionRolledBack", nullptr }, { "OfflineSessionRollbackCmdUID", nullptr },
                { "Closed", false }
            };
            if (q.contains("IncludeTaxObject") && q["IncludeTaxObject"].is_boolean() && q["IncludeTaxObject"].get<bool>())
                j["TaxObject"] = TaxObjectJson(state_.Registrars());
            return Json(200, j);
        }

        if (cmd == "Shifts") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r) return NoContent();
            long long shiftId = 0;
            const bool byId = IntField(q, "ShiftId", shiftId);
            long long from = LLONG_MIN, to = LLONG_MAX;
            if (!byId) {
                const std::string f = StrField(q, "From"), t = StrField(q, "To");
                if (!f.empty() && !ParseDateTime(f, from)) return ErrorResponse(kInvalidQueryParameter, "From: " + f);
                if (!t.empty() && !ParseDateTime(t, to))   return ErrorResponse(kInvalidQueryParameter, "To: " + t);
            }
            json arr = json::array();
            for (const ShiftRecord& s : r->shifts) {
                if (byId ? (s.shiftId != shiftId) : (s.openedEpoch < from || s.openedEpoch > to)) continue;
                arr.push_back(ShiftJson(s));
            }
            if (arr.empty()) return NoContent();
            return Json(200, { { "UID", uid }, { "Shifts", arr } });
        }

        if (cmd == "LastShiftTotals") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r || r->shifts.empty()) return NoContent();
            json j = ShiftJson(r->shifts.back());
            j.erase("ShiftId");
            j.erase("OpenShiftFiscalNum");
            j.erase("CloseShiftFiscalNum");
            j["UID"]         = uid;
            j["ShiftState"]  = r->shiftOpen ? 1 : 0;
            j["ZRepPresent"] = r->zRepPresent;
            j["Totals"]      = r->shiftOpen ? TotalsJson(r->totals) : json(nullptr);   // [Опис] 1487
            return Json(200, j);
        }

        if (cmd == "CheckExt" || cmd == "ZRepExt") {
            const bool wantZ = (cmd == "ZRepExt");
            long long type = -1;
            if (!IntField(q, "Type", type) || type < 0 || type > 3)
                return ErrorResponse(kInvalidQueryParameter, "Непідтримуваний Type (0..3)");
            json j = { { "UID", uid }, { "Data", nullptr }, { "ShiftId", nullptr }, { "ResultCode", 0 }, { "ResultText", "OK" } };
            if (!wantZ) j["CabinetUrl"] = "";
            const std::string reg = StrField(q, "RegistrarNumFiscal");
            if (!state_.Find(reg)) { j["ResultCode"] = 4; j["ResultText"] = "ПРРО не зареєстрований"; return Json(200, j); }
            const StoredDoc* d = nullptr;
            const std::string numFiscal = StrField(q, "NumFiscal");
            long long numLocal = 0;
            if (!numFiscal.empty())                 d = state_.FindDocByFiscal(reg, numFiscal);
            else if (IntField(q, "NumLocal", numLocal)) d = state_.FindDoc(reg, numLocal);
            if (d && ((d->klass == DocClass::ZRep) != wantZ)) d = nullptr;   // чек не шукаємо як Z-звіт і навпаки
            if (!d) { j["ResultCode"] = 5; j["ResultText"] = "Документ не зареєстрований на ПРРО"; return Json(200, j); }
            j["ShiftId"] = d->shiftId;
            std::string data;
            if (type == 1) data = d->originalXml;
            if (type == 2) {
                std::string serverXml = d->originalXml;
                if (d->klass == DocClass::Check && !InsertOrderTaxNum(d->originalXml, d->fiscalNum, serverXml))
                    return Text(500, "cannot build server XML");
                data = oracle::Sign(serverXml);
                if (data.empty()) return Text(500, "cannot sign document");
            }
            if (type == 3) data = Visualization(*d);
            j["Data"] = oracle::b64encode(data);
            return Json(200, j);
        }

        return ErrorResponse(kInvalidQueryParameter, "Невідома команда: " + cmd);
    } catch (const std::exception& e) {
        return ErrorResponse(kInvalidQueryParameter, std::string("Некоректний запит: ") + e.what());
    }
}

minihttp::Response PrroFsService::HandleControl(const std::string& body) {
    try {
        const json q = json::parse(StripJsonPrefix(body), nullptr, false);
        if (q.is_discarded() || !q.is_object()) return Json(400, { { "ok", false }, { "error", "тіло не є JSON-об'єктом" } });
        const std::string action = StrField(q, "action");
        if (action == "reset") {
            if (!q.contains("registrars") || !q["registrars"].is_array())
                return Json(400, { { "ok", false }, { "error", "registrars — обов'язковий масив" } });
            std::vector<RegistrarSeed> seeds;
            for (const json& e : q["registrars"]) {
                if (!e.is_object()) return Json(400, { { "ok", false }, { "error", "елемент registrars — об'єкт" } });
                RegistrarSeed s;
                s.numFiscal = StrField(e, "numFiscal");
                if (s.numFiscal.empty() || s.numFiscal.size() > 18
                    || s.numFiscal.find_first_not_of("0123456789") != std::string::npos)
                    return Json(400, { { "ok", false }, { "error", "numFiscal — лише цифри, до 18" } });
                long long n = 1;
                if (e.contains("nextLocalNum") && (!IntField(e, "nextLocalNum", n) || n < 1))
                    return Json(400, { { "ok", false }, { "error", "nextLocalNum — ціле >= 1" } });
                s.nextLocalNum = n;
                seeds.push_back(s);
            }
            state_.Reset(seeds);
            faults_.Clear();
            return Json(200, { { "ok", true } });
        }

        if (action == "fault") {
            Fault f;
            if (!ParseFaultMode(StrField(q, "mode"), f.mode))
                return Json(400, { { "ok", false }, { "error", "mode: delay | status | dropBeforeRegister | dropAfterRegister" } });
            long long v = 0;
            if (f.mode == FaultMode::Delay) {
                if (!IntField(q, "seconds", v) || v < 0 || v > 600)
                    return Json(400, { { "ok", false }, { "error", "seconds — 0..600" } });
                f.seconds = static_cast<int>(v);
            }
            if (f.mode == FaultMode::Status) {
                if (!IntField(q, "code", v) || v < 100 || v > 599)
                    return Json(400, { { "ok", false }, { "error", "code — 100..599" } });
                f.code     = static_cast<int>(v);
                f.body     = StrField(q, "body");
                f.location = StrField(q, "location");
            }
            if ((f.mode == FaultMode::DropBeforeRegister || f.mode == FaultMode::DropAfterRegister)
                && q.contains("holdSeconds")) {
                if (!IntField(q, "holdSeconds", v) || v < 0 || v > 600)
                    return Json(400, { { "ok", false }, { "error", "holdSeconds — 0..600" } });
                f.holdSeconds = static_cast<int>(v);
            }
            switch (faults_.Arm(StrField(q, "target"), f)) {
                case FaultPlan::ArmResult::Ok:       return Json(200, { { "ok", true } });
                case FaultPlan::ArmResult::Conflict: return Json(409, { { "ok", false }, { "error", "для цієї цілі вже взведено збій" } });
                default:                             return Json(400, { { "ok", false }, { "error", "target: doc | cmd | cmd:<Команда>" } });
            }
        }

        if (action == "set") {
            bool any = false;
            long long v = 0;
            if (q.contains("dateSkewSeconds")) {
                if (!IntField(q, "dateSkewSeconds", v)) return Json(400, { { "ok", false }, { "error", "dateSkewSeconds — ціле" } });
                faults_.dateSkewSeconds = static_cast<int>(v);
                any = true;
            }
            if (q.contains("rejectFormat")) {
                const std::string rf = StrField(q, "rejectFormat");
                if (rf == "text")        faults_.rejectFormat = RejectFormat::Text;
                else if (rf == "ticket") faults_.rejectFormat = RejectFormat::Ticket;
                else return Json(400, { { "ok", false }, { "error", "rejectFormat: text | ticket" } });
                any = true;
            }
            if (!any) return Json(400, { { "ok", false }, { "error", "set: dateSkewSeconds і/або rejectFormat" } });
            return Json(200, { { "ok", true } });
        }

        return Json(400, { { "ok", false }, { "error", "невідома дія: " + action } });
    } catch (const std::exception& e) {
        return Json(400, { { "ok", false }, { "error", e.what() } });
    }
}

minihttp::Response PrroFsService::StateJson() {
    json regs = json::array();
    for (const RegistrarState* r : state_.Registrars())
        regs.push_back({ { "numFiscal", r->numFiscal }, { "shiftState", r->shiftOpen ? 1 : 0 },
                         { "shiftId", r->shiftId }, { "nextLocalNum", r->nextLocalNum },
                         { "firstLocalNum", r->firstLocalNum }, { "lastFiscalNum", NullIfEmpty(r->lastFiscalNum) },
                         { "zRepPresent", r->zRepPresent }, { "openShiftFiscalNum", NullIfEmpty(r->openShiftFiscalNum) },
                         { "shiftsCount", r->shifts.size() } });
    json docs = json::array();
    for (const StoredDoc& d : state_.Docs())
        docs.push_back({ { "registrar", d.registrar }, { "localNum", d.localNum }, { "fiscalNum", d.fiscalNum },
                         { "class", d.klass == DocClass::Check ? "CHECK" : "ZREP" }, { "docType", d.docType },
                         { "docSubType", d.docSubType }, { "shiftId", d.shiftId } });
    json faults = json::array();
    for (const auto& a : faults_.Armed())
        faults.push_back({ { "target", a.first }, { "mode", FaultModeName(a.second.mode) },
                           { "seconds", a.second.seconds }, { "code", a.second.code },
                           { "holdSeconds", a.second.holdSeconds } });
    return Json(200, { { "registrars", regs }, { "documents", docs }, { "faults", faults },
                       { "dateSkewSeconds", faults_.dateSkewSeconds },
                       { "rejectFormat", faults_.rejectFormat == RejectFormat::Ticket ? "ticket" : "text" } });
}

}  // namespace prrofs
