#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <winsock2.h>

// Протокол-обізнаний емулятор термінала ПриватБанк (лише тести).
// Приймає одне TCP-з'єднання на localhost, ріже вхід по 0x00, парсить method,
// викликає зареєстрований responder і шле його відповідь кадром із 0x00.
// Модель сокетів — за RawTcpEchoServer (wire_selftest.cpp).
class TerminalEmulator {
public:
    using Responder = std::function<std::string(const nlohmann::json& request)>;

    ~TerminalEmulator() { Stop(); }
    bool Start() { return Start(0); }
    bool Start(int port);
    int Port() const { return port_; }
    void Stop();

    /// Зареєструвати відповідь на кадр із заданим method (responder повертає JSON-рядок).
    void OnRequest(std::string method, Responder responder) { handlers_[std::move(method)] = std::move(responder); }

private:
    void Run();
    void HandleFrame(SOCKET c, const std::vector<uint8_t>& frame);

    std::map<std::string, Responder> handlers_;
    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::atomic<SOCKET> client_{ INVALID_SOCKET };
    std::thread thread_;
    std::atomic<bool> running_{ false };
    bool started_ = false;
    int port_ = 0;

    // Асинхронна обробка кадрів: на КОЖЕН повний кадр — окремий worker-потік, що обчислює
    // відповідь (responder може блокувати) і шле її ПІД sendMutex_ (щоб байти різних
    // відповідей не перемішувались на сокеті). Так повільний primary-responder не блокує
    // читання наступних кадрів (poller getLastStatMsgCode / interrupt під час операції).
    std::mutex sendMutex_;
    std::vector<std::thread> workers_;
};
