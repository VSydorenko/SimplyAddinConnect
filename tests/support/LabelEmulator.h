#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <winsock2.h>

// Простий TCP-емулятор-захоплювач ZPL (лише тести).
// Приймає одне TCP-з'єднання на 127.0.0.1 і накопичує СИРИЙ вхідний потік
// у буфер (без 0x00-кадрування — принтер приймає ZPL суцільним потоком).
// API дзеркалить TerminalEmulator у частині Start/Stop/Port, але простіший:
// нема responder-ів, лише LastZpl() повертає накопичене.
class LabelEmulator {
public:
    ~LabelEmulator() { Stop(); }

    bool Start() { return Start(0); }
    bool Start(int port);                 // port==0 -> ефемерний; фактичний через getsockname
    int Port() const { return port_; }
    void Stop();

    // Повертає накопичений вхідний буфер (усі байти, прийняті від клієнта).
    std::string LastZpl() const;

private:
    void Run();

    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::atomic<SOCKET> client_{ INVALID_SOCKET };
    std::thread thread_;
    std::atomic<bool> running_{ false };
    bool started_ = false;
    int port_ = 0;

    mutable std::mutex bufMutex_;
    std::string buffer_;                  // накопичений сирий потік
};
