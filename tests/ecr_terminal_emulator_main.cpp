// Standalone-емулятор термінала ПриватБанк для тестування З 1С без обладнання.
// Слухає TCP (default 2000), відповідає скриптованими JSON зі специфікації.
// pch НЕ підключаємо (правило tests/).
//
// Два призначення, які тримають форму цього файлу:
//  1) відлуння чека — те, що емулятор віддає в GetReceiptInfo, мусить збігатися з тим, що
//     він «провів» останнім Purchase, інакше каса, звіряючи facts зі своїм наміром
//     (docs/integration-1c/ecr-privatjson.md §5.5), детерміновано вирішує про гроші навпаки;
//  2) керування режимами з консолі — обрив, мовчання, відмова, зайнятість — щоб тестувальник
//     з 1С відтворював аварійні гілки без перезапуску інструмента.
#include "support/TerminalEmulator.h"   // тягне winsock2.h ПЕРШИМ (до windows.h)
#include <windows.h>                    // SetConsoleOutputCP (winsock2 уже підключено вище)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

// Код відмови для режиму `reject`. Узято з протоколу: §5.1.2 окремо каже, що за деяких
// responseCode (наприклад 05, 96) чек на касу ПЕРЕДАЄТЬСЯ, лише без частини атрибутів, —
// тобто це відмова хоста, а не аварія термінала. Саме така відмова й потрібна, щоб із 1С
// дістати другий рядок таблиці рішень (§5.5): «facts наші, responseCode <> 0000 → операція
// відхилена». Коди 10xx (error:true, скорочена відповідь) дали б натомість factsOk:false.
static const char* const kDeclineCode = "05";

// Останній «проведений» чек. Рядки читають і responder-и (кілька worker-потоків емулятора),
// і консольний потік — тому лише під м'ютексом, копіями назовні.
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
    std::string pan;             ///< маскований номер картки, як у протоколі §5.30
};

// Спільний стан емулятора: прогрес статусу операції, переривання, режими з консолі
// й останній чек. Дає змогу тестувати poller (СтатусТерминала), interrupt (ПрерватьОперацию)
// та аварійні гілки — на відміну від миттєвих відповідей, коли операція завершується
// раніше, ніж встигаєш її перервати.
struct EmuState {
    std::atomic<int>  statusCode{0};       // поточний getLastStatMsgCode (0 = спокій)
    std::atomic<bool> interruptFlag{false};

    std::atomic<bool> silent{false};       // Ping без відповіді (сокет живий) → код 18 у 1С
    std::atomic<bool> reject{false};       // Purchase/GetReceiptInfo віддають responseCode <> 0000
    std::atomic<int>  busyCode{-1};        // -1 = вимкнено; інакше утримуваний getLastStatMsgCode
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
// від каси, і сирий склейкою його в JSON заганяти не можна.
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
    // Маскований PAN за протоколом (§5.1 відповідь Purchase, §5.30 чек): ім'я поля саме
    // "pan", не "cardPAN" - останнього в специфікації немає взагалі. Маска як у прикладах
    // протоколу: перші 6 і останні 4 цифри відкриті. Останні 4 різні на кожну операцію,
    // щоб звірка каси розрізняла дві оплати.
    char pbuf[24] = {0};
    std::snprintf(pbuf, sizeof(pbuf), "473118XXXXXX%04d", static_cast<int>(seq % 10000));
    r.pan           = pbuf;
    return r;
}

// responseCode, який віддаємо ЗАРАЗ. Режим reject накриває збережений код на момент
// відповіді (а не на момент фіксації) — так Purchase і GetReceiptInfo завжди узгоджені
// між собою, у якому б порядку тестувальник не вмикав режим.
static std::string CodeNow(const EmuState& st, const Receipt& r) {
    return st.reject.load() ? std::string(kDeclineCode) : r.responseCode;
}

static void PrintReceipt(const char* prefix, const Receipt& r) {
    if (!r.has) { std::printf("%s чека ще не було\n", prefix); std::fflush(stdout); return; }
    std::printf("%s amount=%s invoiceNumber=%s rrn=%s pan=%s date=%s time=%s responseCode=%s\n",
                prefix, r.amount.c_str(), r.invoiceNumber.c_str(), r.rrn.c_str(), r.pan.c_str(),
                r.date.c_str(), r.time.c_str(), r.responseCode.c_str());
    std::fflush(stdout);
}

static void PrintHelp() {
    std::printf(
        "\nКоманди емулятора:\n"
        "  drop            розірвати активне з'єднання (обрив у польоті); наступний accept\n"
        "                  емулятор робить сам - драйвер реконектиться в живий емулятор\n"
        "  dropafter       закрити з'єднання ОДРАЗУ після наступної відповіді\n"
        "                  (\"термінал відповів і зник\")\n"
        "  silent on|off   Ping лишається без відповіді, сокет живий -> у 1С код 18\n"
        "                  RECONNECTING. ⚠️ ВМИКАТИ ПІСЛЯ Подключить: хендшейк теж іде\n"
        "                  Ping-ом, і при silent on підключення просто не відбудеться.\n"
        "                  Код 18 видно після реконекту, тобто зв'язка: silent on -> drop\n"
        "  reject on|off   Purchase і ПолучитьЧек віддають responseCode=%s (відмова хоста)\n"
        "                  замість 0000 - другий рядок таблиці рішень каси (§5.5)\n"
        "  busy <код>      утримувати getLastStatMsgCode = <код> (напр. busy 3);\n"
        "  busy off        зняти утримання - статус знову йде за ходом операції\n"
        "  status          поточні режими й останній чек\n"
        "  help            цей список\n"
        "  quit            вихід\n\n", kDeclineCode);
    std::fflush(stdout);
}

static void PrintStatus(const EmuState& st) {
    std::printf("Режими: silent=%s reject=%s busy=%s\n",
                st.silent.load() ? "on" : "off",
                st.reject.load() ? "on" : "off",
                st.busyCode.load() < 0 ? "off" : std::to_string(st.busyCode.load()).c_str());
    PrintReceipt("Останній чек:", st.Snapshot());
}

// on/off-аргумент команди. Повертає false, якщо аргумент не розпізнано.
static bool ParseOnOff(const std::string& arg, bool& value) {
    if (arg == "on" || arg == "1")  { value = true;  return true; }
    if (arg == "off" || arg == "0") { value = false; return true; }
    return false;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);        // вивід у UTF-8 (кирилиця замість крякозябрів)
    int port = (argc > 1) ? std::atoi(argv[1]) : 2000;
    EmuState st;
    TerminalEmulator emu;
    emu.SetLog([](const std::string& s){ std::printf("%s\n", s.c_str()); std::fflush(stdout); });
    // Ping — єдина точка, якої стосується silent: порожня відповідь означає «мовчимо»,
    // сокет при цьому лишається живим (TerminalEmulator.cpp: responder без відповіді).
    emu.OnRequest("PingDevice", [&st](const nlohmann::json&) -> std::string {
        if (st.silent.load()) return std::string{};
        return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("ServiceMessage", [&st](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        if (mt == "getLastStatMsgCode") {
            const int held = st.busyCode.load();
            const int code = (held >= 0) ? held : st.statusCode.load();
            return std::string(R"({"method":"ServiceMessage","step":0,"params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":")")
                + std::to_string(code) + R"("},"error":false,"errorDescription":""})";
        }
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
        // не дійде НІКОЛИ, а термінал операцію вже веде - саме це й моделюємо. Після реконекту
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
        const Receipt cur = st.Snapshot();       // режими могли змінитись за ці ~4с
        return Frame("Purchase", nlohmann::json{
            {"responseCode",  CodeNow(st, cur)},
            {"invoiceNumber", cur.invoiceNumber},
            {"rrn",           cur.rrn},
            {"amount",        cur.amount},
            {"pan",           cur.pan},
            {"date",          cur.date},
            {"time",          cur.time}});
    });
    // Refund фіксує чек так само, як Purchase (txnType "2" - протокол §5.2): інакше після
    // обриву на поверненні GetReceiptInfo віддав би чек ПОПЕРЕДНЬОЇ оплати, і каса вирішила б
    // «чек не наш» там, де гроші реально пішли.
    emu.OnRequest("Refund", [&st](const nlohmann::json& q) -> std::string {
        std::string amount = q.contains("params") ? q["params"].value("amount", std::string{}) : std::string{};
        const Receipt rec = MakeReceipt(st, amount, "2");
        st.Store(rec);
        PrintReceipt("[ЧЕК] Refund зафіксовано:", rec);
        return Frame("Refund", nlohmann::json{
            {"responseCode",  CodeNow(st, rec)},
            {"invoiceNumber", rec.invoiceNumber},
            {"rrn",           rec.rrn},
            {"amount",        rec.amount},
            {"pan",           rec.pan},
            {"date",          rec.date},
            {"time",          rec.time}});
    });
    // GetReceiptInfo — відлуння останньої фінансової операції. Поля за протоколом §5.30.
    emu.OnRequest("GetReceiptInfo", [&st](const nlohmann::json& q) -> std::string {
        const Receipt r = st.Snapshot();
        if (!r.has) {
            // Операцій ще не було. Заглушка з ПОРОЖНІМИ полями: вигаданий invoiceNumber збивав би
            // звірку каси - вона побачила б «чужий чек» там, де чека просто немає.
            return Frame("GetReceiptInfo", nlohmann::json{
                {"responseCode", "0000"}, {"invoiceNumber", ""}, {"amount", ""}, {"rrn", ""},
                {"pan", ""}, {"date", ""}, {"time", ""}, {"txnType", ""}, {"trnStatus", ""}});
        }
        // Емулятор тримає ОДИН чек (реальний термінал - пакет). Запит на конкретний номер
        // обслуговуємо останнім чеком, але кажемо про це вголос, щоб не збивати з пантелику.
        const std::string asked = q.contains("params") ? q["params"].value("invoiceNumber", std::string{}) : std::string{};
        if (!asked.empty() && asked != "0" && asked != r.invoiceNumber) {
            std::printf("  -> запит чека %s, емулятор тримає лише останній (%s) - віддаємо його\n",
                        asked.c_str(), r.invoiceNumber.c_str());
            std::fflush(stdout);
        }
        return Frame("GetReceiptInfo", nlohmann::json{
            {"responseCode",  CodeNow(st, r)},
            {"invoiceNumber", r.invoiceNumber},
            {"amount",        r.amount},
            {"rrn",           r.rrn},
            {"pan",           r.pan},
            {"date",          r.date},
            {"time",          r.time},
            {"txnType",       r.txnType},
            {"trnStatus",     r.trnStatus}});
    });
    // Звіти (спека §5.17/§5.18): X-звіт і Звірка — receipt як у реального термінала.
    emu.OnRequest("Audit",  [](const nlohmann::json&){ return R"({"method":"Audit","step":0,"params":{"receipt":"[ X БАЛАНС ] ТЕРМ. EMU00001 Загальні підсумки: 0.00 ГРН","responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("Verify", [](const nlohmann::json&){ return R"({"method":"Verify","step":0,"params":{"receipt":"[ Z БАЛАНС ] ТЕРМ. EMU00001 ПІДСУМКИ ВИЛУЧЕНІ","responseCode":"0000"},"error":false,"errorDescription":""})"; });

    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("ECR terminal emulator слухає 127.0.0.1:%d — help для списку команд, quit/Ctrl-C для виходу\n", emu.Port());
    std::fflush(stdout);

    // Консольний REPL іде НА ГОЛОВНОМУ ПОТОЦІ: окремий потік читання stdin довелося б будити
    // з блокуючого getline, щоб процес міг вийти, а так вихід із циклу = вихід із main.
    // Робота емулятора при цьому в його власному потоці (TerminalEmulator::Run).
    std::string line;
    while (std::getline(std::cin, line)) {
        // Перенаправлений stdin (скрипт, що годує емулятор командами) приносить BOM на
        // першому рядку й CR у кінці - без цього перша ж команда не розпізнається.
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
                             && static_cast<unsigned char>(line[1]) == 0xBB
                             && static_cast<unsigned char>(line[2]) == 0xBF) line.erase(0, 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd >> arg;
        if (cmd.empty()) continue;
        if (cmd == "drop") {
            emu.DropConnection();
            std::printf("З'єднання розірвано з боку термінала; чекаю наступний accept\n");
        } else if (cmd == "dropafter") {
            emu.DropAfterNextResponse();
            std::printf("Наступну відповідь буде відправлено, і одразу по ній - обрив\n");
        } else if (cmd == "silent") {
            bool v = false;
            if (!ParseOnOff(arg, v)) { std::printf("Очікую: silent on | silent off\n"); }
            else { st.silent.store(v); std::printf("silent=%s (Ping %s)\n", v ? "on" : "off",
                                                   v ? "лишається без відповіді" : "відповідає штатно"); }
        } else if (cmd == "reject") {
            bool v = false;
            if (!ParseOnOff(arg, v)) { std::printf("Очікую: reject on | reject off\n"); }
            else { st.reject.store(v); std::printf("reject=%s (responseCode %s)\n", v ? "on" : "off",
                                                   v ? kDeclineCode : "0000"); }
        } else if (cmd == "busy") {
            if (arg == "off" || arg.empty()) { st.busyCode.store(-1); std::printf("busy знято - статус іде за операцією\n"); }
            else {
                const int code = std::atoi(arg.c_str());
                if (code < 0) { std::printf("Очікую: busy <невід'ємний код> | busy off\n"); }
                else { st.busyCode.store(code); std::printf("busy=%d - getLastStatMsgCode утримується\n", code); }
            }
        } else if (cmd == "status") {
            PrintStatus(st);
        } else if (cmd == "help" || cmd == "?") {
            PrintHelp();
        } else if (cmd == "quit" || cmd == "exit") {
            std::printf("Зупиняю емулятор\n");
            std::fflush(stdout);
            emu.Stop();
            return 0;
        } else {
            std::printf("Невідома команда '%s' - help для списку\n", cmd.c_str());
        }
        std::fflush(stdout);
    }

    // stdin закрито (запуск без консолі, перенаправлення з порожнього файлу тощо) - команд
    // не буде, але емулятор мусить лишитись на порту до Ctrl-C.
    std::printf("stdin закрито - консольні команди недоступні, працюю до Ctrl-C\n");
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
