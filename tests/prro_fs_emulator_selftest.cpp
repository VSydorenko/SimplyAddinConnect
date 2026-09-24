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
#include <thread>

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
        CHECK(j.contains("Timestamp"), "LastShiftTotals: Timestamp присутній (F3, §6.4)");
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
        CHECK(j.contains("Timestamp"), "Shifts: Timestamp присутній (F3, §6.4)");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "ZRepExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumFiscal", tz }, { "Type", 1 }, { "UID", "u5" } }, true);
        json j = Body(r);
        std::string data;
        CHECK(r.code == 200 && j["ResultCode"] == "Ok" && oracle::b64decode(Data(j), data) && data == Fx("zrep_1251.xml", 4),
              "ZRepExt Type 1: побайтовий оригінал Z-звіту");
        CHECK(j.contains("Timestamp"), "ZRepExt: Timestamp присутній (F3, §6.4)");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", 2 }, { "Type", 2 }, { "UID", "u6" } }, false);
        json j = Body(r);
        std::string der, xml;
        const bool got = r.code == 200 && j["ResultCode"] == "Ok" && oracle::b64decode(Data(j), der) && !der.empty();
        const oracle::VerifyOutcome v = oracle::Verify(der);
        CHECK(got && v.accepted && oracle::b64decode(v.contentB64, xml), "CheckExt Type 2: Data — CMS, що проходить перевірку");
        CHECK(xml.find("<ORDERTAXNUM>" + t2 + "</ORDERTAXNUM></CHECKHEAD>") != std::string::npos,
              "CheckExt Type 2: у XML сервера вставлено ORDERTAXNUM чека");
        CHECK(j.contains("Timestamp"), "CheckExt: Timestamp присутній (F3, §6.4)");
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
        CHECK(r.code == 200 && Body(r)["ResultCode"] == "DocumentAbsent", "CheckExt неіснуючого номера: ResultCode DocumentAbsent");
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
        // Task 6, amendment B/C: пари правила 1 тут немає — на цьому шляху BOM і пробіли
        // пропускає лексер nlohmann (extern/nlohmann_json/include/nlohmann/detail/input/
        // lexer.hpp:1495-1516), власного механізму (другого StripJsonPrefix) у нашому коді
        // більше нема; перевірено 2026-09-24 — видалення StripJsonPrefix на підписаному
        // шляху дає 0 FAIL двічі.
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

// Парсинг RFC 1123 (для перевірки зсуву Date). -1 — не розібрано.
long long ParseHttpDate(const std::string& s) {
    static const char* kM = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int d = 0, y = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%*3s, %2d %3s %4d %2d:%2d:%2d GMT", &d, mon, &y, &h, &mi, &se) != 6) return -1;
    const char* p = std::strstr(kM, mon);
    if (!p) return -1;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = static_cast<int>(p - kM) / 3; t.tm_mday = d;
    t.tm_hour = h;        t.tm_min = mi;                            t.tm_sec = se;
    return static_cast<long long>(_mkgmtime(&t));
}

// Сценарії споживача 2 і 3: обриви до й після реєстрації (+ «мовчати довше за таймаут»).
void ScenarioDrops(int port) {
    std::printf("== Сценарії 2-3: обриви ==\n");
    CHECK(Reset(port), "reset для обривів");
    std::string t;
    CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 1), t), "зміну відкрито");

    // --- Обрив ПІСЛЯ реєстрації ---
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropAfterRegister" } }).code == 200,
          "збій dropAfterRegister взведено");
    const minihttp::ClientResult a = PostDoc(port, "check_sale_1251.xml", 2);
    CHECK(a.connected && !a.responded && a.rst, "чек №2: з'єднання розірвано без відповіді (НетОтвета, RST)");
    // Правило 2: стан доводиться парою предикатів — номер зайнято І документ збережено.
    CHECK(RegState(port)["nextLocalNum"] == 3 && HasDoc(port, 2),
          "стан: чек №2 ЗАРЕЄСТРОВАНО (NextLocalNum = 3, документ №2 є)");
    const minihttp::ClientResult again = PostDoc(port, "check_sale_1251.xml", 2);
    CHECK(again.code == 400 && LastNumber(again.body) == 3, "повтор №2 -> «повинен дорівнювати 3»");
    const minihttp::ClientResult ce = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                      { "NumLocal", 2 }, { "Type", 2 }, { "UID", "d1" } }, false);
    CHECK(ce.code == 200 && Body(ce)["ResultCode"] == "Ok" && !Data(Body(ce)).empty(),
          "CheckExt №2 після обриву: знайдено (ResultCode Ok, Data є)");
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 3), t), "наступний чек іде з №3");

    // --- Обрив ДО реєстрації ---
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropBeforeRegister" } }).code == 200,
          "збій dropBeforeRegister взведено");
    const minihttp::ClientResult b = PostDoc(port, "check_sale_1251.xml", 4);
    CHECK(b.connected && !b.responded && b.rst, "чек №4: з'єднання розірвано без відповіді (RST)");
    CHECK(RegState(port)["nextLocalNum"] == 4 && !HasDoc(port, 4),
          "стан: чек №4 НЕ зареєстровано (NextLocalNum = 4, документа №4 немає)");
    const minihttp::ClientResult ce2 = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                       { "NumLocal", 4 }, { "Type", 2 }, { "UID", "d2" } }, false);
    CHECK(ce2.code == 200 && Body(ce2)["ResultCode"] == "DocumentAbsent", "CheckExt №4: DocumentAbsent");
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 4), t), "повторна відправка №4 з тим самим номером проходить");

    // --- «Мовчати довше за таймаут» + Review Focus 1 (архітекторська правка після коду-рев'ю:
    // булевий CHECK ss.code==200 не міг зчервоніти детерміновано — /fs/cmd відправлявся
    // ЛИШЕ після власного таймауту doc-клієнта (~1 с у вікні утримання 2 с), тож лишалось
    // ~1 с утримання проти 1000 мс таймауту cmd — гонка, яку вирішувала гранулярність
    // SO_RCVTIMEO Windows (~8-12 мс понад номінал), а не сам замок).
    //
    // Новий дизайн: doc-запит (утримання 3 с) іде в ОКРЕМОМУ потоці з клієнтським
    // таймаутом 5000 мс (>> утримання), щоб цей потік НЕ відпускав doc-з'єднання по
    // СВОЄМУ таймауту раніше, ніж завершиться утримання; /fs/cmd-проба також отримує
    // таймаут 5000 мс (>> утримання), щоб elapsedMs відбивав РЕАЛЬНЕ очікування на mx_
    // (Review Focus 1), а не штучну стелю власного таймауту.
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropAfterRegister" },
                          { "holdSeconds", 3 } }).code == 200, "утримання після реєстрації взведено (3 с)");

    minihttp::ClientResult c;
    std::chrono::steady_clock::time_point docDoneAt;
    std::thread docThread([&]() {
        c = PostDoc(port, "check_sale_1251.xml", 5, 5000);
        docDoneAt = std::chrono::steady_clock::now();
    });

    // Правило 2, перша половина пари стану («зареєстровано, утримання ще не скінчилось»):
    // короткий (<= ~300 мс, ЩОБ НЕ конкурувати з тим самим mx_, який тестуємо нижче) retry
    // на /control/state — доводить №5 ЗАРЕЄСТРОВАНО ще до відправки проби, а не «почекали
    // й сподіваємось». Довший бюджет тут зробив би сам доказ нерозрізненним від симптому
    // (обидва чекають на той самий mx_).
    bool registered = false;
    for (int i = 0; i < 6 && !registered; ++i) {
        const minihttp::ClientResult probe = minihttp::Fetch(port, "GET", "/control/state", "", "", 40);
        if (probe.responded && probe.code == 200) {
            const json st = Body(probe);
            for (const json& r : st.value("registrars", json::array()))
                if (r.value("numFiscal", std::string()) == kReg && r.value("nextLocalNum", 0LL) == 6) { registered = true; break; }
        }
        if (!registered) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(registered, "стан: №5 зареєстровано (доведено /control/state перед пробою — правило 2)");

    // Проба /fs/cmd — надсилається одразу після спроби довести стан, ПОКИ doc-потік ще
    // не повернувся; таймаут 5000 мс (> утримання 3 с) — elapsedMs покаже РЕАЛЬНЕ очікування.
    const minihttp::ClientResult ss = PostCmd(port, { { "Command", "ServerState" }, { "UID", "h" } }, false, 5000);
    const std::chrono::steady_clock::time_point cmdDoneAt = std::chrono::steady_clock::now();
    std::printf("  виміряно: /fs/cmd під час утримання — %lld мс\n", ss.elapsedMs);
    // Правило 3: поріг 800 мс лежить між зміряними 2026-09-24 значеннями — без збою
    // ~3-6 мс, зі збоєм «mx_ під замком під час утримання» (Step 6, Review Focus 1)
    // ~2700-2750 мс (обидва виміряно build+run цим самим сценарієм, не розрахунком).
    CHECK(ss.code == 200 && ss.elapsedMs < 800,
          "під час утримання /fs/cmd відповідає ШВИДКО (elapsedMs < 800 мс — mx_ не тримається під час утримання)");

    docThread.join();
    // Правило 2, друга половина пари: проба дійсно завершилась ДО завершення doc-запиту —
    // тобто отримана відповідь стосується саме вікна утримання, а не випадково пізнішого
    // моменту (коли утримання вже скінчилось і mx_ у будь-якому разі вільний).
    CHECK(cmdDoneAt < docDoneAt, "проба /fs/cmd завершилась ДО завершення doc-запиту (друга половина пари — правило 2)");
    // Утримання 3 с < клієнтський таймаут 5 с, тож клієнт мусить побачити саме RST
    // (AbortConnection після завершення утримання), а не власний таймаут.
    CHECK(c.connected && !c.responded && c.rst, "утримання: doc-клієнт бачить розрив (RST) після ~3 с утримання");
    CHECK(RegState(port)["nextLocalNum"] == 6 && HasDoc(port, 5), "утримання після реєстрації: №5 зареєстровано (повторно, після join)");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropBeforeRegister" },
                          { "holdSeconds", 2 } }).code == 200, "утримання до реєстрації взведено");
    const minihttp::ClientResult e = PostDoc(port, "check_sale_1251.xml", 6, 1000);
    CHECK(!e.responded && e.timedOut, "утримання до реєстрації: клієнт не отримав нічого");
    CHECK(RegState(port)["nextLocalNum"] == 6 && !HasDoc(port, 6), "утримання до реєстрації: №6 НЕ зареєстровано");
}

// Сценарій 8 споживача + механіка збоїв: 204/5xx/302, delay, 409, фільтр цілі.
void ScenarioStatus(int port) {
    std::printf("== Сценарій 8: збої-статуси й фільтр цілі ==\n");
    CHECK(Reset(port), "reset для збоїв-статусів");
    for (int code : { 204, 503 }) {
        CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", code } }).code == 200,
              "збій status взведено");
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 1);
        CHECK(r.responded && r.code == code, code == 204 ? "status 204 віддано" : "status 503 віддано");
        CHECK(RegState(port)["nextLocalNum"] == 1, "status-збій: документ не оброблено (NextLocalNum = 1)");
    }
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", 302 } }).code == 200,
          "збій 302 взведено");
    const minihttp::ClientResult r302 = PostDoc(port, "open_shift_1251.xml", 1);
    CHECK(r302.code == 302 && r302.headers.count("location") == 1
          && r302.headers.at("location") == "http://127.0.0.1:" + std::to_string(port) + "/moved",
          "302 з Location за замовчуванням");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "delay" }, { "seconds", 1 } }).code == 200,
          "збій delay 1 с взведено");
    std::string t;
    const minihttp::ClientResult d = PostDoc(port, "open_shift_1251.xml", 1, 5000);
    std::printf("  виміряно: відповідь із delay 1 с — %lld мс\n", d.elapsedMs);
    // Правило 3: поріг 900 мс лежить між зміряними 2026-09-24 значеннями того самого
    // запиту (open_shift_1251.xml, той самий handler) — без delay (sleep_for у
    // PrroFsService::Handle тимчасово прибрано, негативна верифікація) ~18 мс, із
    // delay 1 с ~1020 мс. 900 лежить строго між обома — поріг не зсунуто.
    CHECK(d.elapsedMs >= 900 && TicketOk(d, t), "delay: відповідь після паузи, документ прийнято (виміряно 2026-09-24: 18 мс / 1020 мс)");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "delay" }, { "seconds", 1 } }).code == 200,
          "delay знову взведено");
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", 500 } }).code == 409,
          "друге взведення тієї самої цілі -> 409");
    CHECK(Control(port, { { "action", "fault" }, { "target", "nope" }, { "mode", "delay" }, { "seconds", 1 } }).code == 400,
          "некоректна ціль -> 400");
    CHECK(Reset(port) && State(port)["faults"].is_array() && State(port)["faults"].empty(), "reset очищає взведені збої");

    CHECK(Control(port, { { "action", "fault" }, { "target", "cmd:CheckExt" }, { "mode", "status" }, { "code", 503 } }).code == 200,
          "збій cmd:CheckExt взведено");
    const minihttp::ClientResult s1 = PostCmd(port, { { "Command", "ServerState" }, { "UID", "f1" } }, false);
    CHECK(s1.code == 200, "інша команда збій cmd:CheckExt не витрачає");
    const minihttp::ClientResult s2 = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                      { "NumLocal", 1 }, { "Type", 0 }, { "UID", "f2" } }, false);
    CHECK(s2.code == 503 && State(port)["faults"].is_array() && State(port)["faults"].empty(),
          "CheckExt отримав 503, збій витрачено");
}

// Сценарій 7 споживача (зсув Date) + альтернативний формат відмови.
void ScenarioSkewAndReject(int port) {
    std::printf("== Сценарій 7: зсув годинника; формат відмови ==\n");
    CHECK(Reset(port), "reset для зсуву годинника");
    CHECK(Control(port, { { "action", "set" }, { "dateSkewSeconds", 40 } }).code == 200, "dateSkewSeconds = 40");
    const minihttp::ClientResult p = minihttp::Fetch(port, "GET", "/ping", "", "", 3000);
    const bool hasDate = p.headers.count("date") == 1;
    const long long skewDate = hasDate
        ? ParseHttpDate(p.headers.at("date")) - static_cast<long long>(std::time(nullptr))
        : -999999;
    std::printf("  виміряно: зсув Date = %lld с\n", skewDate);
    // Заголовок Date гарантує CommonHeaders() (перевірено окремо в ScenarioPing), але
    // .at("date") тут викликається лише за hasDate у ТІЙ САМІЙ && — відсутність заголовка
    // дає [FAIL], а не std::out_of_range.
    CHECK(hasDate && skewDate >= 35 && skewDate <= 45, "Date зсунуто на ~40 с");
    long long ts = 0;
    const minihttp::ClientResult s = PostCmd(port, { { "Command", "ServerState" }, { "UID", "k" } }, false);
    json js = Body(s);
    const bool tsOk = js.contains("Timestamp") && js["Timestamp"].is_string()
                      && prrofs::ParseDateTime(js["Timestamp"].get<std::string>(), ts);
    const long long skewTs = ts - static_cast<long long>(std::time(nullptr));
    CHECK(tsOk && skewTs >= 35 && skewTs <= 45, "Timestamp ServerState теж зсунуто (годинник сервера один)");
    CHECK(Control(port, { { "action", "set" }, { "dateSkewSeconds", 0 } }).code == 200, "зсув знято");

    CHECK(Control(port, { { "action", "set" }, { "rejectFormat", "ticket" } }).code == 200, "rejectFormat = ticket");
    const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 5);
    const oracle::VerifyOutcome v = oracle::Verify(r.body);
    std::string xml;
    CHECK(r.code == 200 && v.accepted && oracle::b64decode(v.contentB64, xml)
          && xml.find("<ERRORCODE>7</ERRORCODE>") != std::string::npos
          && xml.find("<ORDERTAXNUM>") == std::string::npos,
          "rejectFormat ticket: 200 + підписана квитанція з ERRORCODE 7 без ORDERTAXNUM");
    CHECK(Control(port, { { "action", "set" }, { "rejectFormat", "bogus" } }).code == 400, "невідомий rejectFormat -> 400");
    CHECK(Reset(port) && State(port)["rejectFormat"] == "text" && State(port)["dateSkewSeconds"] == 0,
          "reset повертає постійні налаштування до типових");
}

// Формат ResultCode у CheckExt/ZRepExt: типово — ім'я enum рядком, resultCodeFormat=number —
// число (§9.2 [Опис]/[споживач]). Перевіряється саме ТИП значення (is_string/is_number_integer),
// не лише його рівність, — щоб не пропустити регрес, де хелпер ігнорує режим.
void ScenarioResultCodeFormat(int port) {
    std::printf("== Формат ResultCode (name/number) ==\n");
    CHECK(Reset(port), "reset для перевірки resultCodeFormat");
    std::string t1;
    CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 1), t1), "відкриття зміни прийнято (документ NumLocal=1)");

    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                          { "NumLocal", 1 }, { "Type", 0 }, { "UID", "rc1" } }, false);
        json j = Body(r);
        CHECK(r.code == 200 && j["ResultCode"].is_string() && j["ResultCode"] == "Ok",
              "типово (name): знайдений документ -> ResultCode \"Ok\" рядком");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", "4999999999" },
                                                          { "NumLocal", 1 }, { "Type", 0 }, { "UID", "rc2" } }, false);
        json j = Body(r);
        CHECK(r.code == 200 && j["ResultCode"].is_string() && j["ResultCode"] == "TransactionsRegistrarNotRegistered",
              "типово (name): невідомий ПРРО -> ResultCode \"TransactionsRegistrarNotRegistered\" рядком");
    }

    CHECK(Control(port, { { "action", "set" }, { "resultCodeFormat", "number" } }).code == 200, "resultCodeFormat = number");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                          { "NumLocal", 1 }, { "Type", 0 }, { "UID", "rc3" } }, false);
        json j = Body(r);
        CHECK(r.code == 200 && j["ResultCode"].is_number_integer() && j["ResultCode"] == 0,
              "number: знайдений документ -> ResultCode 0 числом");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                          { "NumLocal", 99 }, { "Type", 0 }, { "UID", "rc4" } }, false);
        json j = Body(r);
        CHECK(r.code == 200 && j["ResultCode"].is_number_integer() && j["ResultCode"] == 5,
              "number: неіснуючий документ -> ResultCode 5 числом");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", "4999999999" },
                                                          { "NumLocal", 1 }, { "Type", 0 }, { "UID", "rc5" } }, false);
        json j = Body(r);
        CHECK(r.code == 200 && j["ResultCode"].is_number_integer() && j["ResultCode"] == 4,
              "number: невідомий ПРРО -> ResultCode 4 числом");
    }
    CHECK(Control(port, { { "action", "set" }, { "resultCodeFormat", "bogus" } }).code == 400, "невідомий resultCodeFormat -> 400");
    CHECK(Reset(port) && State(port)["resultCodeFormat"] == "name", "reset повертає resultCodeFormat до \"name\"");
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
        ScenarioDrops(port);
        ScenarioStatus(port);
        ScenarioSkewAndReject(port);
        ScenarioResultCodeFormat(port);
        srv->Stop();
    }
    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
