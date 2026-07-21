// Standalone-емулятор термінала ПриватБанк для тестування З 1С без обладнання.
// Слухає TCP (default 2000), відповідає скриптованими JSON зі специфікації.
// pch НЕ підключаємо (правило tests/).
#include "support/TerminalEmulator.h"   // тягне winsock2.h ПЕРШИМ (до windows.h)
#include <windows.h>                    // SetConsoleOutputCP (winsock2 уже підключено вище)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

// Спільний стан емулятора для реалістичних сценаріїв: прогрес статусу операції + переривання.
// Дає змогу тестувати poller (СтатусТерминала) та interrupt (ПрерватьОперацию) — на відміну від
// миттєвих відповідей, коли операція завершується раніше, ніж встигаєш її перервати.
struct EmuState {
    std::atomic<int>  statusCode{0};       // поточний getLastStatMsgCode (0 = спокій)
    std::atomic<bool> interruptFlag{false};
};

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
        const int steps[] = {9, 1, 6, 3, 10};   // waiting card → card read → pin → auth → in progress
        for (int code : steps) {
            st.statusCode.store(code);
            for (int i = 0; i < 16; ++i) {       // ~0.8с на крок
                if (st.interruptFlag.load()) {   // прийшов interrupt → скасування
                    st.statusCode.store(0);
                    return R"({"method":"Purchase","step":0,"params":{"responseCode":"1001"},"error":true,"errorDescription":"Операція скасована"})";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        st.statusCode.store(0);
        return std::string(R"({"method":"Purchase","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","rrn":"555000111","amount":")") + amount + R"("},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("Refund", [](const nlohmann::json&){ return R"({"method":"Refund","step":0,"params":{"responseCode":"0000","invoiceNumber":"1002"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetReceiptInfo", [](const nlohmann::json&){ return R"({"method":"GetReceiptInfo","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","txnType":"1","trnStatus":"1"},"error":false,"errorDescription":""})"; });

    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("ECR terminal emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", emu.Port());
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
