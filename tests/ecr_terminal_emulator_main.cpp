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

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);        // вивід у UTF-8 (кирилиця замість крякозябрів)
    int port = (argc > 1) ? std::atoi(argv[1]) : 2000;
    TerminalEmulator emu;
    emu.SetLog([](const std::string& s){ std::printf("%s\n", s.c_str()); std::fflush(stdout); });
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false,"errorDescription":""})";
        if (mt == "interrupt") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"interruptTransmitted"},"error":false,"errorDescription":""})";
        return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"methodNotImplemented"},"error":true,"errorDescription":"Not implemented"})";
    });
    emu.OnRequest("CheckConnection", [](const nlohmann::json&){ return R"({"method":"CheckConnection","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetTerminalInfo", [](const nlohmann::json&){ return R"({"method":"GetTerminalInfo","step":0,"params":{"version":"emu-1.0"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("Purchase", [](const nlohmann::json& q){
        std::string amount = q.contains("params") ? q["params"].value("amount", std::string{}) : std::string{};
        return std::string(R"({"method":"Purchase","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","rrn":"555000111","amount":")") + amount + R"("},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("Refund", [](const nlohmann::json&){ return R"({"method":"Refund","step":0,"params":{"responseCode":"0000","invoiceNumber":"1002"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetReceiptInfo", [](const nlohmann::json&){ return R"({"method":"GetReceiptInfo","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","txnType":"1","trnStatus":"1"},"error":false,"errorDescription":""})"; });

    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("ECR terminal emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", emu.Port());
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
