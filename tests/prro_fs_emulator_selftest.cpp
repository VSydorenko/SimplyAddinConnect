//
// @file tests/prro_fs_emulator_selftest.cpp
// @brief prro_fs_emulator --self-test: сценарії споживача на рівні протоколу, без 1С
//        (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.2).
//        Сервер піднімається in-process на вільному порту 18200..18299, клієнт —
//        MiniHttpClient. Крипто вже ініціалізовано в main (oracle::Init).
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/MiniHttpClient.h"
#include "support/PrroFsFixtures.h"
#include "support/PrroFsService.h"
#include "support/UapkiOracle.h"

#include <windows.h>

using nlohmann::json;

#ifndef PRRO_FS_DATA_DIR
#  define PRRO_FS_DATA_DIR ""
#endif

namespace {

int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

const char* kReg = "4000000001";

// LastNumber — з tests/support/PrroFsFixtures.h (Task 3), не дублюється тут
// (архітекторське рішення: Task 6 користується тим самим хелпером).
using prrofs::LastNumber;

std::string Fx(const char* name, long long n, const char* reg = kReg) {
    return prrofs::LoadFixture(PRRO_FS_DATA_DIR, name, n, reg);
}

minihttp::ClientResult PostDoc(int port, const char* fixture, long long n, int timeoutMs = 5000) {
    return minihttp::Fetch(port, "POST", "/fs/doc", oracle::Sign(Fx(fixture, n)),
                           "application/octet-stream", timeoutMs);
}

minihttp::ClientResult PostCmd(int port, const json& q, bool sign, int timeoutMs = 5000) {
    const std::string text = q.dump();
    if (!sign) return minihttp::Fetch(port, "POST", "/fs/cmd", text, "application/json", timeoutMs);
    return minihttp::Fetch(port, "POST", "/fs/cmd", oracle::Sign(text), "application/octet-stream", timeoutMs);
}

minihttp::ClientResult Control(int port, const json& q) {
    return minihttp::Fetch(port, "POST", "/control", q.dump(), "application/json", 5000);
}

json Body(const minihttp::ClientResult& r) {
    json j = json::parse(r.body, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

json State(int port) {
    return Body(minihttp::Fetch(port, "GET", "/control/state", "", "", 5000));
}

std::string Data(const json& j) {
    return (j.contains("Data") && j["Data"].is_string()) ? j["Data"].get<std::string>() : std::string();
}

bool Reset(int port, long long next = 1) {
    const minihttp::ClientResult r = Control(port, { { "action", "reset" },
        { "registrars", json::array({ { { "numFiscal", kReg }, { "nextLocalNum", next } } }) } });
    return r.responded && r.code == 200;
}

// Квитанція: CMS -> XML. true, якщо підпис прийнято, ERRORCODE 0 і є ORDERTAXNUM.
bool TicketOk(const minihttp::ClientResult& r, std::string& taxNum) {
    taxNum.clear();
    if (!r.responded || r.code != 200) return false;
    const oracle::VerifyOutcome v = oracle::Verify(r.body);
    std::string xml;
    if (!v.accepted || !oracle::b64decode(v.contentB64, xml)) return false;
    if (xml.find("<ERRORCODE>0</ERRORCODE>") == std::string::npos) return false;
    const size_t a = xml.find("<ORDERTAXNUM>");
    const size_t b = xml.find("</ORDERTAXNUM>");
    if (a == std::string::npos || b == std::string::npos) return false;
    taxNum = xml.substr(a + 13, b - a - 13);
    return !taxNum.empty();
}

json RegState(int port) {
    json st = State(port);
    if (st.contains("registrars") && st["registrars"].is_array())
        for (const json& r : st["registrars"]) if (r.value("numFiscal", std::string()) == kReg) return r;
    return json::object();
}

bool HasDoc(int port, long long localNum) {
    json st = State(port);
    if (st.contains("documents") && st["documents"].is_array())
        for (const json& d : st["documents"])
            if (d.value("registrar", std::string()) == kReg && d.value("localNum", 0LL) == localNum) return true;
    return false;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

void ScenarioPing(int port) {
    std::printf("== /ping і невідомий шлях ==\n");
    const minihttp::ClientResult p = minihttp::Fetch(port, "GET", "/ping", "", "", 3000);
    CHECK(p.responded && p.code == 200 && p.body == "prro_fs_emulator alive",
          "/ping: тіло ідентифікує сервіс (не старий оракул)");
    CHECK(p.headers.count("date") == 1, "/ping: заголовок Date");
    const minihttp::ClientResult nf = minihttp::Fetch(port, "GET", "/doc", "", "", 3000);
    CHECK(nf.responded && nf.code == 404 && nf.headers.count("date") == 1, "невідомий шлях -> 404 з Date");
}

// Сценарій споживача 1: зміна повністю + запити, якими продукт з'ясовує стан.
void ScenarioShift(int port) {
    std::printf("== Сценарій 1: зміна ==\n");
    CHECK(Reset(port), "reset: ПРРО зареєстровано");
    std::string t1, t2, t3, tz, tc;
    CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 1), t1), "відкриття зміни прийнято, квитанція з ORDERTAXNUM");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", kReg }, { "UID", "u1" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["ShiftState"] == 1 && j["Totals"].is_object(), "LastShiftTotals: зміна відкрита, Totals є");
        json& t = j["Totals"];
        CHECK(t["Real"].is_object() && t["Real"]["OrdersCount"] == 0 && t["Real"]["PayForm"].is_array()
              && t["Real"]["PayForm"].empty() && t["Real"]["Tax"].is_array(),
              "Totals без чеків: Real — об'єкт з нулями й порожніми масивами (Review Focus 5)");
        CHECK(t["Ret"].is_object() && t["ServiceInput"].is_number() && t["ServiceOutput"].is_number(),
              "Totals без чеків: Ret, ServiceInput, ServiceOutput присутні");
    }
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 2), t2), "чек продажу прийнято");
    CHECK(TicketOk(PostDoc(port, "check_return_1251.xml", 3), t3), "повернення прийнято");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "TransactionsRegistrarState" }, { "NumFiscal", kReg }, { "UID", "u2" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["ShiftState"] == 1 && j["NextLocalNum"] == 4,
              "TransactionsRegistrarState: зміна відкрита, NextLocalNum = 4");
        CHECK(j["Name"] == "Тестовий Касир", "TransactionsRegistrarState: Name з CASHIER документа відкриття");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", kReg }, { "UID", "u3" } }, true);
        json j = Body(r);
        json& real = j["Totals"]["Real"];
        CHECK(r.code == 200 && real["OrdersCount"] == 1 && real["Sum"] == 150.0, "LastShiftTotals: Real = один чек на 150.00");
        CHECK(real["PayForm"].size() == 2 && real["Tax"].size() == 1 && real["Tax"][0].contains("TurnoverDiscount"),
              "LastShiftTotals: PayForm/Tax агреговано, TurnoverDiscount є");
        CHECK(j["Totals"]["Ret"]["OrdersCount"] == 1 && j["Totals"]["Ret"]["Sum"] == 40.0,
              "LastShiftTotals: Ret = повернення на 40.00");
    }
    CHECK(TicketOk(PostDoc(port, "zrep_1251.xml", 4), tz), "Z-звіт прийнято");
    CHECK(TicketOk(PostDoc(port, "close_shift_1251.xml", 5), tc), "закриття зміни прийнято");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "Shifts" }, { "NumFiscal", kReg }, { "UID", "u4" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["Shifts"].size() == 1 && j["Shifts"][0]["ZRepFiscalNum"] == tz,
              "Shifts: ZRepFiscalNum = фіскальний номер Z-звіту");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "ZRepExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumFiscal", tz }, { "Type", 1 }, { "UID", "u5" } }, true);
        json j = Body(r);
        std::string data;
        CHECK(r.code == 200 && j["ResultCode"] == 0 && oracle::b64decode(Data(j), data) && data == Fx("zrep_1251.xml", 4),
              "ZRepExt Type 1: побайтовий оригінал Z-звіту");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", 2 }, { "Type", 2 }, { "UID", "u6" } }, false);
        json j = Body(r);
        std::string der, xml;
        const bool got = r.code == 200 && j["ResultCode"] == 0 && oracle::b64decode(Data(j), der) && !der.empty();
        const oracle::VerifyOutcome v = oracle::Verify(der);
        CHECK(got && v.accepted && oracle::b64decode(v.contentB64, xml), "CheckExt Type 2: Data — CMS, що проходить перевірку");
        CHECK(xml.find("<ORDERTAXNUM>" + t2 + "</ORDERTAXNUM></CHECKHEAD>") != std::string::npos,
              "CheckExt Type 2: у XML сервера вставлено ORDERTAXNUM чека");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", "2" }, { "Type", 3 }, { "UID", "u7" } }, false);
        std::string text;
        CHECK(r.code == 200 && oracle::b64decode(Data(Body(r)), text) && text.find(t2) != std::string::npos
              && text.find("Чек") != std::string::npos,
              "CheckExt Type 3: UTF-8 візуалізація з фіскальним номером; NumLocal рядком приймається (Review Focus 3)");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", 99 }, { "Type", 2 }, { "UID", "u8" } }, false);
        CHECK(r.code == 200 && Body(r)["ResultCode"] == 5, "CheckExt неіснуючого номера: ResultCode 5 DocumentAbsent");
    }
}

void ScenarioFormats(int port) {
    std::printf("== Формати відповідей і відмов ==\n");
    CHECK(Reset(port), "reset для перевірок формату");
    {
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 7);
        CHECK(r.responded && r.code == 400 && StartsWith(r.body, "Код помилки: 7 CheckLocalNumberInvalid"),
              "неправильний локальний номер: 400 + «Код помилки: 7 CheckLocalNumberInvalid»");
        CHECK(r.body.find("Номер документа повинен дорівнювати") != std::string::npos && LastNumber(r.body) == 1,
              "текст містить фразу, N = 1 — останнє число");
        CHECK(r.headers.count("date") == 1, "відмова теж має Date");
    }
    {
        const minihttp::ClientResult a = minihttp::Fetch(port, "POST", "/fs/doc", std::string(9, 'x'), "application/octet-stream", 5000);
        const minihttp::ClientResult b = minihttp::Fetch(port, "POST", "/fs/doc", std::string(512001, 'x'), "application/octet-stream", 10000);
        const minihttp::ClientResult c = minihttp::Fetch(port, "POST", "/fs/cmd", std::string(9, ' '), "application/json", 5000);
        CHECK(a.code == 416 && b.code == 416 && c.code == 416, "розмір поза 10…512000 -> 416 (doc 9 і 512001 байт, cmd 9 байт)");
    }
    {
        std::string der = oracle::Sign(Fx("open_shift_1251.xml", 1));
        const size_t pos = der.find("<DOCTYPE>100</DOCTYPE>");
        if (pos != std::string::npos) der[pos + 9] = static_cast<char>(der[pos + 9] ^ 0x01);   // '1' -> '0' у підписаному вмісті
        const minihttp::ClientResult r = minihttp::Fetch(port, "POST", "/fs/doc", der, "application/octet-stream", 5000);
        CHECK(pos != std::string::npos && r.code == 400 && StartsWith(r.body, "Код помилки: 9 DocumentValidationError"),
              "зіпсований CMS -> 400, код 9");
        CHECK(RegState(port)["nextLocalNum"] == 1, "відхилений документ номер не зайняв (NextLocalNum = 1)");
    }
    {
        const std::string der = oracle::Sign(Fx("open_shift_1251.xml", 1, "4999999999"));
        const minihttp::ClientResult r = minihttp::Fetch(port, "POST", "/fs/doc", der, "application/octet-stream", 5000);
        CHECK(r.code == 400 && StartsWith(r.body, "Код помилки: 1 TransactionsRegistrarAbsent"), "документ невідомого ПРРО -> 400, код 1");
    }
    {
        const minihttp::ClientResult a = PostCmd(port, { { "Command", "Objects" }, { "UID", "o1" } }, false);
        CHECK(a.code == 400 && StartsWith(a.body, "Код помилки: 9"), "Objects без підпису -> 400, код 9");
        const minihttp::ClientResult b = PostCmd(port, { { "Command", "Objects" }, { "UID", "o2" } }, true);
        json j = Body(b);
        CHECK(b.code == 200 && j["TaxObjects"][0]["TransactionsRegistrars"][0]["NumFiscal"] == 4000000001LL,
              "Objects: ПРРО з reset, NumFiscal числом");
        const minihttp::ClientResult c = minihttp::Fetch(port, "POST", "/fs/cmd",
            "\xEF\xBB\xBF  {\"Command\":\"ServerState\",\"UID\":\"s1\"}", "application/json", 5000);
        json jc = Body(c);
        CHECK(c.code == 200 && jc["UID"] == "s1" && jc.contains("Timestamp"),
              "ServerState без підпису, з BOM і пробілами -> 200 (Review Focus 2)");
        // Той самий сценарій BOM+пробіли, але ВСЕРЕДИНІ підписаного CMS (Review Focus 2:
        // прогалина закрита — перевіряємо і непідписаний, і підписаний шлях StripJsonPrefix).
        const json sIn = { { "Command", "ServerState" }, { "UID", "s2" } };
        const minihttp::ClientResult cs = minihttp::Fetch(port, "POST", "/fs/cmd",
            oracle::Sign("\xEF\xBB\xBF  " + sIn.dump()), "application/octet-stream", 5000);
        json jcs = Body(cs);
        CHECK(cs.code == 200 && jcs["UID"] == "s2" && jcs.contains("Timestamp"),
              "ServerState підписаний, з BOM і пробілами всередині CMS -> 200, UID з запиту (Review Focus 2)");
        const minihttp::ClientResult d = PostCmd(port, { { "Command", "NoSuchCommand" }, { "UID", "x" } }, true);
        CHECK(d.code == 400 && StartsWith(d.body, "Код помилки: 11"), "невідома команда -> 400, код 11");
        const minihttp::ClientResult e = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", "4999999999" }, { "UID", "x" } }, true);
        CHECK(e.code == 204 && e.body.empty(), "LastShiftTotals невідомого ПРРО -> 204 без тіла");
        const minihttp::ClientResult f = PostCmd(port, { { "Command", "TransactionsRegistrarState" }, { "NumFiscal", 4000000001LL }, { "UID", "x" } }, true);
        CHECK(f.code == 200 && Body(f)["NextLocalNum"] == 1, "TransactionsRegistrarState: NumFiscal числом приймається (Review Focus 3)");
    }
    // Сценарій 5 споживача: лічильник каси розійшовся із сервером — розбіжність створюється
    // чесно, через reset з іншим nextLocalNum (§8.4), а не підробленою відповіддю.
    CHECK(Reset(port, 5), "reset із nextLocalNum = 5 (розбіжність лічильника, сценарій 5)");
    {
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 1);
        CHECK(r.code == 400 && LastNumber(r.body) == 5, "каса шле №1, сервер чекає №5 -> «повинен дорівнювати 5»");
        std::string t;
        CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 5), t), "після самовідновлення лічильника №5 проходить");
    }
}

}  // namespace

int RunPrroFsSelfTest() {
    std::printf("\n==== prro_fs_emulator self-test ====\n");
    if (!minihttp::InitNetwork()) { std::printf("[FAIL] WSAStartup\n"); return 1; }

    std::unique_ptr<prrofs::PrroFsService> svc;
    std::unique_ptr<minihttp::Server>      srv;
    int port = 0;
    for (int p = 18200; p < 18300 && !srv; ++p) {
        std::unique_ptr<prrofs::PrroFsService> s(new prrofs::PrroFsService(p));
        std::unique_ptr<minihttp::Server>      h(new minihttp::Server(p, "127.0.0.1"));
        prrofs::PrroFsService* raw = s.get();
        raw->SetTrace(false);
        h->SetHandler([raw](const minihttp::Request& r) { return raw->Handle(r); });
        h->SetCommonHeaders([raw]() { return raw->CommonHeaders(); });
        if (h->Listen().first) { h->Start(); svc = std::move(s); srv = std::move(h); port = p; }
    }
    CHECK(srv != nullptr, "сервер self-test піднявся на вільному порту 18200..18299");
    if (srv) {
        ScenarioPing(port);
        ScenarioShift(port);
        ScenarioFormats(port);
        srv->Stop();
    }
    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
