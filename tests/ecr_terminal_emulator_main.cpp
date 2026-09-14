// Standalone-емулятор термінала ПриватБанк для тестування З 1С без обладнання.
// Слухає TCP (default 2000), відповідає скриптованими JSON зі специфікації.
// pch НЕ підключаємо (правило tests/).
//
// Ключова властивість: те, що емулятор віддає в GetReceiptInfo, мусить збігатися з тим, що
// він «провів» останньою фінансовою операцією. Інакше каса, звіряючи facts зі своїм наміром
// (docs/integration-1c/ecr-privatjson.md §5.5), детерміновано вирішує про гроші навпаки, ніж
// вирішила б із живим терміналом.
#include "support/TerminalEmulator.h"   // тягне winsock2.h ПЕРШИМ (до windows.h)
#include <windows.h>                    // SetConsoleOutputCP (winsock2 уже підключено вище)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

// Останній «проведений» чек. Рядки читають responder-и з кількох worker-потоків емулятора —
// тому лише під м'ютексом, копіями назовні.
struct Receipt {
    bool        has = false;
    std::string amount;
    std::string invoiceNumber;
    std::string rrn;
    std::string date;
    std::string time;
    std::string responseCode;
    std::string txnType;
    std::string trnStatus;
};

// Спільний стан емулятора: прогрес статусу операції, переривання й останній чек. Дає змогу
// тестувати poller (СтатусТерминала) та interrupt (ПрерватьОперацию) — на відміну від
// миттєвих відповідей, коли операція завершується раніше, ніж встигаєш її перервати.
struct EmuState {
    std::atomic<int>  statusCode{0};       // поточний getLastStatMsgCode (0 = спокій)
    std::atomic<bool> interruptFlag{false};
    std::atomic<long long> invoiceSeq{1000};   // монотонний номер чека (перший буде 1001)

    mutable std::mutex mutex;
    Receipt receipt;

    Receipt Snapshot() const { std::lock_guard<std::mutex> lk(mutex); return receipt; }
    void Store(const Receipt& r) { std::lock_guard<std::mutex> lk(mutex); receipt = r; }
    void SetCode(const std::string& rc) {
        std::lock_guard<std::mutex> lk(mutex);
        if (receipt.has) receipt.responseCode = rc;
    }
};

// Збірка кадру відповіді. Через nlohmann, а не конкатенацією: у amount приходить текст
// від каси, і сирою склейкою його в JSON заганяти не можна.
static std::string Frame(const std::string& method, const nlohmann::json& params,
                         bool error = false, const std::string& descr = "") {
    nlohmann::json j;
    j["method"]           = method;
    j["step"]             = 0;
    j["params"]           = params;
    j["error"]            = error;
    j["errorDescription"] = descr;
    return j.dump();
}

// Зафіксувати новий чек. ⚠️ Формат date/time узято з протоколу §5.30
// (docs/ECR_Privat_JSON_Protokol.md, рядки 2115-2135): date = DD.MM.YYYY, time = HH:MM:SS.
// ПІДЛЯГАЄ ЗВІРЦІ НА ЗАЛІЗІ (Task 10): у спеці §7 п.5 це відкрите питання разом із тим,
// чи монотонний invoiceNumber на живому N950.
static Receipt MakeReceipt(EmuState& st, const std::string& amount, const std::string& txnType) {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char dbuf[16] = {0}, tbuf[16] = {0}, rbuf[16] = {0};
    std::strftime(dbuf, sizeof(dbuf), "%d.%m.%Y", &tmv);
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    std::strftime(rbuf, sizeof(rbuf), "%m%d%H%M%S", &tmv);   // 10 цифр часу транзакції

    const long long seq = ++st.invoiceSeq;
    char tail[4] = {0};
    std::snprintf(tail, sizeof(tail), "%02d", static_cast<int>(seq % 100));

    Receipt r;
    r.has           = true;
    r.amount        = amount;
    r.invoiceNumber = std::to_string(seq);
    r.rrn           = std::string(rbuf) + tail;   // 12 цифр, різні на кожну операцію
    r.date          = dbuf;
    r.time          = tbuf;
    r.responseCode  = "0000";
    r.txnType       = txnType;
    r.trnStatus     = "1";
    return r;
}

static void PrintReceipt(const char* prefix, const Receipt& r) {
    if (!r.has) { std::printf("%s чека ще не було\n", prefix); std::fflush(stdout); return; }
    std::printf("%s amount=%s invoiceNumber=%s rrn=%s date=%s time=%s responseCode=%s\n",
                prefix, r.amount.c_str(), r.invoiceNumber.c_str(), r.rrn.c_str(),
                r.date.c_str(), r.time.c_str(), r.responseCode.c_str());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);        // вивід у UTF-8 (кирилиця замість крякозябрів)
    int port = (argc > 1) ? std::atoi(argv[1]) : 2000;
    EmuState st;
    TerminalEmulator emu;
    emu.SetLog([](const std::string& s){ std::printf("%s\n", s.c_str()); std::fflush(stdout); });
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [&st](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        if (mt == "getLastStatMsgCode")
            return std::string(R"({"method":"ServiceMessage","step":0,"params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":")")
                + std::to_string(st.statusCode.load()) + R"("},"error":false,"errorDescription":""})";
        if (mt == "interrupt") { st.interruptFlag.store(true); return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"interruptTransmitted"},"error":false,"errorDescription":""})"; }
        return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"methodNotImplemented"},"error":true,"errorDescription":"Not implemented"})";
    });
    emu.OnRequest("CheckConnection", [](const nlohmann::json&){ return R"({"method":"CheckConnection","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetTerminalInfo", [](const nlohmann::json&){ return R"({"method":"GetTerminalInfo","step":0,"params":{"version":"emu-1.0"},"error":false,"errorDescription":""})"; });
    // Purchase — РЕАЛІСТИЧНИЙ: ~4с з прогресом статусу (9→1→6→3→10), переривається interrupt-ом.
    // Дає вікно для НачатьОплату→ПрерватьОперацию і для poller-а (СтатусТерминала).
    emu.OnRequest("Purchase", [&st](const nlohmann::json& q) -> std::string {
        std::string amount = q.contains("params") ? q["params"].value("amount", std::string{}) : std::string{};
        st.interruptFlag.store(false);
        // ФАКТИ ЧЕКА ФІКСУЄМО ТУТ, ДО ЦИКЛУ. При обриві в польоті відповідь Purchase до каси
        // не дійде НІКОЛИ, а термінал операцію вже веде — саме це й моделюємо. Після реконекту
        // GetReceiptInfo віддасть ці самі amount/rrn, і звірка каси зійдеться.
        const Receipt rec = MakeReceipt(st, amount, "1");
        st.Store(rec);
        PrintReceipt("[ЧЕК] Purchase зафіксовано:", rec);

        const int steps[] = {9, 1, 6, 3, 10};   // waiting card → card read → pin → auth → in progress
        for (int code : steps) {
            st.statusCode.store(code);
            for (int i = 0; i < 16; ++i) {       // ~0.8с на крок
                if (st.interruptFlag.load()) {   // прийшов interrupt → скасування
                    st.statusCode.store(0);
                    st.SetCode("1001");          // чек лишається, але зі скасуванням
                    return R"({"method":"Purchase","step":0,"params":{"responseCode":"1001"},"error":true,"errorDescription":"Операція скасована"})";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        st.statusCode.store(0);
        const Receipt cur = st.Snapshot();
        return Frame("Purchase", nlohmann::json{
            {"responseCode",  cur.responseCode},
            {"invoiceNumber", cur.invoiceNumber},
            {"rrn",           cur.rrn},
            {"amount",        cur.amount},
            {"date",          cur.date},
            {"time",          cur.time}});
    });
    // Refund фіксує чек так само, як Purchase (txnType "2" — протокол §5.2): інакше після
    // обриву на поверненні GetReceiptInfo віддав би чек ПОПЕРЕДНЬОЇ оплати, і каса вирішила б
    // «чек не наш» там, де гроші реально пішли.
    emu.OnRequest("Refund", [&st](const nlohmann::json& q) -> std::string {
        std::string amount = q.contains("params") ? q["params"].value("amount", std::string{}) : std::string{};
        const Receipt rec = MakeReceipt(st, amount, "2");
        st.Store(rec);
        PrintReceipt("[ЧЕК] Refund зафіксовано:", rec);
        return Frame("Refund", nlohmann::json{
            {"responseCode",  rec.responseCode},
            {"invoiceNumber", rec.invoiceNumber},
            {"rrn",           rec.rrn},
            {"amount",        rec.amount},
            {"date",          rec.date},
            {"time",          rec.time}});
    });
    // GetReceiptInfo — відлуння останньої фінансової операції. Поля за протоколом §5.30.
    emu.OnRequest("GetReceiptInfo", [&st](const nlohmann::json& q) -> std::string {
        const Receipt r = st.Snapshot();
        if (!r.has) {
            // Операцій ще не було. Заглушка з ПОРОЖНІМИ полями: вигаданий invoiceNumber збивав би
            // звірку каси — вона побачила б «чужий чек» там, де чека просто немає.
            return Frame("GetReceiptInfo", nlohmann::json{
                {"responseCode", "0000"}, {"invoiceNumber", ""}, {"amount", ""}, {"rrn", ""},
                {"date", ""}, {"time", ""}, {"txnType", ""}, {"trnStatus", ""}});
        }
        // Емулятор тримає ОДИН чек (реальний термінал — пакет). Запит на конкретний номер
        // обслуговуємо останнім чеком, але кажемо про це вголос, щоб не збивати з пантелику.
        const std::string asked = q.contains("params") ? q["params"].value("invoiceNumber", std::string{}) : std::string{};
        if (!asked.empty() && asked != "0" && asked != r.invoiceNumber) {
            std::printf("  -> запит чека %s, емулятор тримає лише останній (%s) — віддаємо його\n",
                        asked.c_str(), r.invoiceNumber.c_str());
            std::fflush(stdout);
        }
        return Frame("GetReceiptInfo", nlohmann::json{
            {"responseCode",  r.responseCode},
            {"invoiceNumber", r.invoiceNumber},
            {"amount",        r.amount},
            {"rrn",           r.rrn},
            {"date",          r.date},
            {"time",          r.time},
            {"txnType",       r.txnType},
            {"trnStatus",     r.trnStatus}});
    });
    // Звіти (спека §5.17/§5.18): X-звіт і Звірка — receipt як у реального термінала.
    emu.OnRequest("Audit",  [](const nlohmann::json&){ return R"({"method":"Audit","step":0,"params":{"receipt":"[ X БАЛАНС ] ТЕРМ. EMU00001 Загальні підсумки: 0.00 ГРН","responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("Verify", [](const nlohmann::json&){ return R"({"method":"Verify","step":0,"params":{"receipt":"[ Z БАЛАНС ] ТЕРМ. EMU00001 ПІДСУМКИ ВИЛУЧЕНІ","responseCode":"0000"},"error":false,"errorDescription":""})"; });

    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("ECR terminal emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", emu.Port());
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
