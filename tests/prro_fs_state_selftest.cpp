//
// @file tests/prro_fs_state_selftest.cpp
// @brief Рівень L-f1 гейта: чиста логіка імітації фіскального сервера ДПС (стан,
//        нумерація, підсумки, збої) і диспозиції MiniHttpServer. Без UAPKI, без 1С.
//        Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.1.
//        Критерій гейта — exit 0; кожна перевірка друкує [PASS]/[FAIL].
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
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/MiniHttpClient.h"
#include "support/FiscalServerState.h"
#include "support/FaultPlan.h"
#include "support/PrroFsFixtures.h"

#include <windows.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

#ifndef PRRO_FS_DATA_DIR
#  define PRRO_FS_DATA_DIR ""
#endif

static const char* kReg1       = "4000000001";
static const char* kReg2       = "4000000002";
static const char* kRegUnknown = "4999999999";

static std::string Fx(const char* name, long long n, const char* reg = kReg1) {
    return prrofs::LoadFixture(PRRO_FS_DATA_DIR, name, n, reg);
}

static prrofs::SubmitResult SubmitFx(prrofs::FiscalServerState& st, const char* name,
                                     long long n, const char* reg = kReg1) {
    const std::string xml = Fx(name, n, reg);
    prrofs::ParsedDoc d;
    std::string err;
    if (xml.empty() || !prrofs::ParseDocument(xml, d, err)) {
        std::printf("[FAIL] фікстура %s не розібралась: %s\n", name, err.c_str());
        ++g_failures;
        prrofs::SubmitResult bad;
        bad.errorCode = -1;
        return bad;
    }
    return st.Submit(d, xml, "2026-09-24T12:00:00+03:00", 1790240400LL);
}

static void TestMoney() {
    std::printf("== Гроші й перекодування ==\n");
    int64_t v = 0;
    CHECK(prrofs::ParseMoney("150.00", v) && v == 15000, "ParseMoney 150.00 -> 15000");
    CHECK(prrofs::ParseMoney("0.05", v) && v == 5, "ParseMoney 0.05 -> 5");
    CHECK(prrofs::ParseMoney("20", v) && v == 2000, "ParseMoney 20 -> 2000");
    CHECK(prrofs::ParseMoney("-1.5", v) && v == -150, "ParseMoney -1.5 -> -150");
    CHECK(!prrofs::ParseMoney("1.234", v) && !prrofs::ParseMoney("abc", v) && !prrofs::ParseMoney("", v),
          "ParseMoney відхиляє три знаки, нечисло й порожнечу");
    CHECK(prrofs::FormatMoney(15000) == "150.00" && prrofs::FormatMoney(-5) == "-0.05"
          && prrofs::FormatMoney(0) == "0.00", "FormatMoney");
    CHECK(prrofs::Utf8ToCp1251("ГОТІВКА") == "\xC3\xCE\xD2\xB2\xC2\xCA\xC0"
          && prrofs::Cp1251ToUtf8("\xC3\xCE\xD2\xB2\xC2\xCA\xC0") == "ГОТІВКА",
          "перекодування UTF-8 <-> windows-1251 (включно з українською І)");
}

static void TestParsing() {
    std::printf("== Розбір документів ==\n");
    prrofs::ParsedDoc d;
    std::string err;
    CHECK(prrofs::ParseDocument(Fx("check_sale_1251.xml", 7), d, err), "check_sale_1251 розібрано");
    CHECK(d.klass == prrofs::DocClass::Check && d.docType == 0 && d.docSubType == 0 && d.orderNum == 7
          && d.cashRegisterNum == kReg1, "заголовок: CHECK, DOCTYPE 0, DOCSUBTYPE 0, ORDERNUM, CASHREGISTERNUM");
    CHECK(d.cashier == "Тестовий Касир", "CASHIER перекодовано з windows-1251 у UTF-8");
    CHECK(d.totalSum == 15000 && d.pays.size() == 2 && !d.pays.empty() && d.pays[0].name == "ГОТІВКА",
          "CHECKTOTAL і CHECKPAY розібрано, PAYFORMNM у UTF-8");
    CHECK(d.taxes.size() == 1 && !d.taxes[0].sign && d.taxes[0].turnoverDiscount == d.taxes[0].turnover,
          "відсутні SIGN=false і TURNOVERDISCOUNT=TURNOVER (припущення §4.7)");

    prrofs::ParsedDoc u;
    CHECK(prrofs::ParseDocument(Fx("check_sale_utf8.xml", 3), u, err), "UTF-8 з BOM перед декларацією розібрано (Review Focus 4)");
    CHECK(u.taxes.size() == 1 && u.taxes[0].sign && u.taxes[0].turnoverDiscount == 9000,
          "наявні SIGN і TURNOVERDISCOUNT узято з документа");

    prrofs::ParsedDoc z;
    CHECK(prrofs::ParseDocument(Fx("zrep_1251.xml", 4), z, err) && z.klass == prrofs::DocClass::ZRep
          && z.orderNum == 4, "ZREP розібрано: клас ZRep, ORDERNUM з ZREPHEAD");

    prrofs::ParsedDoc bad;
    CHECK(!prrofs::ParseDocument("<FOO/>", bad, err), "невідомий корінь — відмова");
    CHECK(!prrofs::ParseDocument("<?xml version=\"1.0\" encoding=\"koi8-u\"?><CHECK/>", bad, err),
          "непідтримуване кодування — відмова");
    CHECK(!prrofs::ParseDocument("<CHECK><CHECKHEAD><ORDERNUM>x</ORDERNUM><CASHREGISTERNUM>1</CASHREGISTERNUM></CHECKHEAD></CHECK>", bad, err),
          "нечисловий ORDERNUM — відмова");
}

static void TestInsertOrderTaxNum() {
    std::printf("== Вставка ORDERTAXNUM ==\n");
    const std::string x = Fx("check_sale_1251.xml", 2);
    const std::string elem = "<ORDERTAXNUM>100000002</ORDERTAXNUM>";
    std::string y;
    CHECK(prrofs::InsertOrderTaxNum(x, "100000002", y), "вставка в чек виконана");
    const size_t xClose = x.find("</CHECKHEAD>");
    const size_t yClose = y.find("</CHECKHEAD>");
    CHECK(yClose != std::string::npos && yClose >= elem.size()
          && y.compare(yClose - elem.size(), elem.size(), elem) == 0,
          "ORDERTAXNUM — останній елемент CHECKHEAD");
    CHECK(xClose != std::string::npos && y.size() == x.size() + elem.size()
          && y.compare(0, xClose, x, 0, xClose) == 0
          && y.compare(yClose, std::string::npos, x, xClose, std::string::npos) == 0,
          "решта байтів не змінена (кодування windows-1251 збережено)");
    std::string z;
    CHECK(prrofs::InsertOrderTaxNum(y, "100000099", z), "повторна вставка виконана");
    size_t count = 0;
    for (size_t p = z.find("<ORDERTAXNUM>"); p != std::string::npos; p = z.find("<ORDERTAXNUM>", p + 1)) ++count;
    CHECK(count == 1 && z.find("<ORDERTAXNUM>100000099</ORDERTAXNUM>") != std::string::npos,
          "наявний ORDERTAXNUM замінено, а не продубльовано");
    CHECK(!prrofs::InsertOrderTaxNum(Fx("zrep_1251.xml", 1), "1", z), "ZREP без CHECKHEAD — відмова");
}

static void TestStateFlow() {
    std::printf("== FiscalServerState: зміна й нумерація ==\n");
    prrofs::FiscalServerState st;
    st.Reset({ { kReg1, 1 }, { kReg2, 10 } });
    const prrofs::RegistrarState* r = st.Find(kReg1);
    CHECK(r && r->nextLocalNum == 1 && !r->shiftOpen && st.Find(kReg2) && st.Find(kReg2)->numLocal == 2,
          "reset: два ПРРО, NextLocalNum із сіду, зміна закрита, порядковий номер");

    prrofs::SubmitResult x = SubmitFx(st, "check_sale_1251.xml", 1);
    CHECK(x.errorCode == prrofs::kShiftNotOpened, "чек при закритій зміні -> 5 ShiftNotOpened");
    x = SubmitFx(st, "open_shift_1251.xml", 2);
    CHECK(x.errorCode == prrofs::kCheckLocalNumberInvalid && prrofs::LastNumber(x.errorText) == 1,
          "неправильний номер -> 7, останнє число тексту N = 1");
    CHECK(x.errorText.find("Номер документа повинен дорівнювати") != std::string::npos,
          "текст коду 7 містить фразу споживача");
    CHECK(st.Find(kReg1)->nextLocalNum == 1 && st.Docs().empty(),
          "відхилені документи номер не займають і не зберігаються");
    CHECK(SubmitFx(st, "open_shift_1251.xml", 1, kRegUnknown).errorCode == prrofs::kTransactionsRegistrarAbsent,
          "невідомий ПРРО -> 1 TransactionsRegistrarAbsent");

    x = SubmitFx(st, "open_shift_1251.xml", 1);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && x.fiscalNum == "100000001", "відкриття прийнято, перший фіскальний номер 100000001");
    CHECK(r->shiftOpen && r->shiftId == 1 && r->firstLocalNum == 1 && r->nextLocalNum == 2
          && r->openShiftFiscalNum == "100000001" && r->lastFiscalNum == "100000001",
          "стан після відкриття: зміна 1, FirstLocalNum 1, NextLocalNum 2, LastFiscalNum");
    CHECK(r->name == "Тестовий Касир", "Name = CASHIER документа відкриття (UTF-8)");
    CHECK(SubmitFx(st, "open_shift_1251.xml", 2).errorCode == prrofs::kShiftAlreadyOpened,
          "повторне відкриття -> 4 ShiftAlreadyOpened");
    CHECK(SubmitFx(st, "check_sale_1251.xml", 2).errorCode == prrofs::kOk, "чек №2 прийнято");
    CHECK(SubmitFx(st, "close_shift_1251.xml", 3).errorCode == prrofs::kLastDocumentMustBeZRep,
          "закриття без Z-звіту -> 6 LastDocumentMustBeZRep");
    x = SubmitFx(st, "zrep_1251.xml", 3);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && r->zRepPresent && !r->shifts.empty()
          && r->shifts.back().zRepFiscalNum == x.fiscalNum, "Z-звіт прийнято, ZRepFiscalNum у зміні");
    CHECK(SubmitFx(st, "zrep_1251.xml", 4).errorCode == prrofs::kZRepAlreadyRegistered,
          "другий Z-звіт -> 8 ZRepAlreadyRegistered");
    x = SubmitFx(st, "close_shift_1251.xml", 4);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && !r->shiftOpen && r->lastFiscalNum.empty()
          && !r->shifts.back().closed.empty() && r->shifts.back().closeShiftFiscalNum == x.fiscalNum,
          "закриття: зміна закрита, LastFiscalNum порожній (null), Closed заповнено");

    x = SubmitFx(st, "open_shift_1251.xml", 10, kReg2);
    CHECK(x.errorCode == prrofs::kOk && x.fiscalNum == "100000005",
          "фіскальна нумерація наскрізна між ПРРО (п'ятий прийнятий -> 100000005)");

    const prrofs::StoredDoc* d = st.FindDoc(kReg1, 2);
    CHECK(d && d->originalXml == Fx("check_sale_1251.xml", 2) && d->fiscalNum == "100000002" && d->shiftId == 1,
          "документ №2 збережено побайтово, з фіскальним номером і зміною");
    CHECK(st.FindDocByFiscal(kReg1, "100000002") == d && st.FindDoc(kReg1, 99) == nullptr,
          "пошук за фіскальним номером; неіснуючий -> nullptr");
}

static void TestTotals() {
    std::printf("== FiscalServerState: підсумки зміни ==\n");
    prrofs::FiscalServerState st;
    st.Reset({ { kReg1, 1 } });
    SubmitFx(st, "open_shift_1251.xml", 1);
    const prrofs::ShiftTotals& t0 = st.Find(kReg1)->totals;
    CHECK(t0.real.ordersCount == 0 && t0.real.payForms.empty() && t0.ret.ordersCount == 0 && t0.serviceInput == 0,
          "підсумки відкритої зміни без чеків — нулі");
    const bool allOk =
        SubmitFx(st, "check_sale_1251.xml", 2).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_sale_utf8.xml", 3).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_return_1251.xml", 4).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_deposit_1251.xml", 5).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_issue_1251.xml", 6).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_storno_1251.xml", 7).errorCode == prrofs::kOk;
    CHECK(allOk, "шість чеків різних підтипів прийнято");
    const prrofs::ShiftTotals& t = st.Find(kReg1)->totals;
    CHECK(t.real.ordersCount == 2 && t.real.sum == 25000, "Real: 2 чеки на 250.00 (сторно не враховано)");
    CHECK(t.ret.ordersCount == 1 && t.ret.sum == 4000, "Ret: 1 повернення на 40.00");
    CHECK(t.serviceInput == 50000 && t.serviceOutput == 20000, "ServiceInput 500.00, ServiceOutput 200.00");
    CHECK(t.real.payForms.size() == 2 && t.real.payForms[0].code == 0 && t.real.payForms[0].sum == 20000
          && t.real.payForms[1].code == 1 && t.real.payForms[1].sum == 5000, "PayForm агреговано за кодом");
    CHECK(!t.real.payForms.empty() && t.real.payForms[0].name == "ГОТІВКА", "PayFormName у UTF-8");
    const prrofs::TaxTotal* noSign = nullptr;
    const prrofs::TaxTotal* withSign = nullptr;
    for (const prrofs::TaxTotal& x : t.real.taxes) (x.sign ? withSign : noSign) = &x;
    CHECK(t.real.taxes.size() == 2, "Tax: різний SIGN -> два окремі рядки");
    CHECK(noSign && noSign->turnover == 15000 && noSign->turnoverDiscount == 15000 && noSign->sum == 2500
          && noSign->prc == 2000 && noSign->letter == "А" && noSign->name == "ПДВ",
          "рядок без SIGN: суми, відсоток, літера й назва; TurnoverDiscount = Turnover");
    CHECK(withSign && withSign->turnoverDiscount == 9000 && withSign->sourceSum == 10000,
          "рядок із SIGN: TurnoverDiscount з документа");
}

static void TestFaultPlan() {
    std::printf("== FaultPlan ==\n");
    using prrofs::Fault;
    using prrofs::FaultMode;
    using prrofs::FaultPlan;
    int calls = 0;
    auto name = [&calls](const char* n) {
        return std::function<std::string()>([&calls, n]() { ++calls; return std::string(n); });
    };

    FaultPlan fp;
    Fault drop;
    drop.mode = FaultMode::DropAfterRegister;
    CHECK(fp.Arm("doc", drop) == FaultPlan::ArmResult::Ok, "Arm doc -> Ok");
    CHECK(fp.Arm("doc", drop) == FaultPlan::ArmResult::Conflict, "повторне взведення тієї самої цілі -> Conflict (черги немає)");
    CHECK(fp.Arm("foo", drop) == FaultPlan::ArmResult::BadTarget && fp.Arm("cmd:", drop) == FaultPlan::ArmResult::BadTarget,
          "некоректна ціль -> BadTarget");
    Fault got;
    CHECK(!fp.Take("cmd", name("ServerState"), got), "збій doc не спрацьовує на cmd");
    CHECK(fp.Take("doc", nullptr, got) && got.mode == FaultMode::DropAfterRegister, "збій doc спрацював на doc");
    CHECK(!fp.Take("doc", nullptr, got), "вдруге не спрацьовує (витрачено)");

    Fault s503;
    s503.mode = FaultMode::Status;
    s503.code = 503;
    CHECK(fp.Arm("cmd", s503) == FaultPlan::ArmResult::Ok, "Arm cmd -> Ok");
    calls = 0;
    CHECK(fp.Take("cmd", name("ServerState"), got) && got.code == 503, "загальний cmd спрацював");
    CHECK(calls == 0, "лише загальний cmd: ім'я команди не розбиралось");

    Fault s500 = s503;
    s500.code = 500;
    fp.Arm("cmd", s500);
    fp.Arm("cmd:CheckExt", s503);
    calls = 0;
    CHECK(fp.Take("cmd", name("CheckExt"), got) && got.code == 503, "cmd:CheckExt має перевагу над cmd");
    CHECK(calls == 1 && fp.Armed().size() == 1 && fp.Armed()[0].first == "cmd",
          "витрачено рівно один збій — загальний cmd лишився");
    fp.Arm("cmd:CheckExt", s503);
    CHECK(fp.Take("cmd", name("ServerState"), got) && got.code == 500, "інша команда бере загальний cmd");
    CHECK(fp.Armed().size() == 1 && fp.Armed()[0].first == "cmd:CheckExt", "cmd:CheckExt лишився для CheckExt");

    fp.dateSkewSeconds = 40;
    fp.rejectFormat = prrofs::RejectFormat::Ticket;
    fp.Clear();
    CHECK(fp.Armed().empty() && fp.dateSkewSeconds == 0 && fp.rejectFormat == prrofs::RejectFormat::Text,
          "Clear (reset) скидає збої й постійні налаштування");
    FaultMode m = FaultMode::Delay;
    CHECK(prrofs::ParseFaultMode("dropBeforeRegister", m) && m == FaultMode::DropBeforeRegister
          && !prrofs::ParseFaultMode("drop", m), "ParseFaultMode: відома назва — так, невідома — ні");
}

// Сервер на першому вільному порту 18100..18199.
static std::unique_ptr<minihttp::Server> StartServer(minihttp::Handler h,
                                                     minihttp::CommonHeadersFn common, int& port) {
    for (int p = 18100; p < 18200; ++p) {
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(p, "127.0.0.1"));
        s->SetHandler(h);
        if (common) s->SetCommonHeaders(common);
        if (s->Listen().first) { s->Start(); port = p; return s; }
    }
    port = 0;
    return nullptr;
}

static void TestTransport() {
    std::printf("== Транспорт MiniHttpServer: заголовки й диспозиції ==\n");
    static const char* kDate = "Thu, 24 Sep 2026 09:00:00 GMT";
    auto common  = []() { return minihttp::HeaderList{ { "Date", kDate } }; };
    auto handler = [](const minihttp::Request& rq) -> minihttp::Response {
        minihttp::Response r;
        if (rq.uri == "/send")   { r.body = "ok"; r.headers.push_back({ "X-Extra", "1" }); return r; }
        if (rq.uri == "/abort")  { r.disposition = minihttp::Disposition::Abort; return r; }
        if (rq.uri == "/hold")   { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 2;  return r; }
        if (rq.uri == "/hold30") { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 30; return r; }
        r.code = 404;
        return r;
    };
    int port = 0;
    std::unique_ptr<minihttp::Server> srv = StartServer(handler, common, port);
    CHECK(srv != nullptr, "сервер піднявся на вільному порту 18100..18199");
    if (!srv) return;

    minihttp::ClientResult a = minihttp::Fetch(port, "GET", "/send", "", "", 3000);
    CHECK(a.responded && a.code == 200 && a.body == "ok", "Send: відповідь 200 з тілом");
    CHECK(a.headers.count("date") == 1 && a.headers["date"] == kDate, "Send: спільний заголовок Date присутній");
    CHECK(a.headers.count("x-extra") == 1, "Send: власний заголовок відповіді (Response::headers) присутній");

    minihttp::ClientResult t = minihttp::FetchRaw(port,
        "POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n", 3000);
    CHECK(t.responded && t.code == 411, "транспортна відмова 411 віддана самим сервером");
    CHECK(t.headers.count("date") == 1, "транспортна відмова теж має Date (спільні заголовки на кожній відповіді)");

    minihttp::ClientResult b = minihttp::Fetch(port, "GET", "/abort", "", "", 3000);
    CHECK(b.connected && !b.responded, "Abort: з'єднання було, відповіді немає");
    CHECK(b.reset && !b.timedOut, "Abort: клієнт бачить розрив (RST), а не таймаут");

    // Правило 3: утримання 2 с, клієнтський таймаут 1 с. Числа в звіті задачі — з виміру.
    minihttp::ClientResult c = minihttp::Fetch(port, "GET", "/hold", "", "", 1000);
    CHECK(!c.responded && c.timedOut, "HoldThenAbort: клієнт із таймаутом 1 с не отримав нічого");
    minihttp::ClientResult d = minihttp::Fetch(port, "GET", "/hold", "", "", 5000);
    CHECK(!d.responded && d.reset && d.elapsedMs >= 1800,
          "HoldThenAbort: після ~2 с тиші — розрив (elapsedMs >= 1800)");

    // Stop() має перервати утримання, а не чекати holdSeconds.
    std::thread cli([port]() { minihttp::Fetch(port, "GET", "/hold30", "", "", 40000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    srv->Stop();
    const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
    cli.join();
    std::printf("  виміряно: Stop() під час утримання 30 с зайняв %lld мс\n", stopMs);
    // Правило 3: поріг двома вимірами. Виміряно 2026-09-24: з перериванням (notify_all
    // у Stop()) — 0 мс; без переривання (holdCv_.notify_all() тимчасово прибрано,
    // негативна верифікація) — 5000 мс (стеля activeCv_.wait_for(5s), MiniHttpServer.cpp:323).
    // Поріг 2000 мс лежить строго між обома вимірами.
    CHECK(stopMs < 2000, "Stop() перериває утримання (< 2000 мс; виміряно 2026-09-24: 0 мс / 5000 мс)");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    if (!minihttp::InitNetwork()) { std::printf("[FAIL] WSAStartup\n"); return 1; }

    TestTransport();
    TestMoney();
    TestParsing();
    TestInsertOrderTaxNum();
    TestStateFlow();
    TestTotals();
    TestFaultPlan();

    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
