// Standalone-емулятор ZPL-принтера для тестування З 1С без реального обладнання.
// Слухає TCP (default 9100), накопичує сирий вхідний ZPL-потік і друкує нові
// байти у stdout доти, доки не Ctrl-C. Аналог ecr_terminal_emulator_main.cpp.
// pch НЕ підключаємо (правило tests/).
#include "support/LabelEmulator.h"   // тягне winsock2.h ПЕРШИМ (до windows.h)
#include <windows.h>                 // SetConsoleOutputCP (winsock2 уже підключено вище)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);        // вивід у UTF-8 (кирилиця замість крякозябрів)
    int port = (argc > 1) ? std::atoi(argv[1]) : 9100;

    LabelEmulator emu;
    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("Label printer emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", emu.Port());
    std::fflush(stdout);

    // Періодично друкуємо нові байти накопиченого ZPL-потоку.
    size_t printed = 0;
    for (;;) {
        std::string buf = emu.LastZpl();
        if (buf.size() > printed) {
            std::fwrite(buf.data() + printed, 1, buf.size() - printed, stdout);
            std::fflush(stdout);
            printed = buf.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
