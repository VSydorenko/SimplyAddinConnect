#include "LabelEmulator.h"
#include <ws2tcpip.h>

bool LabelEmulator::Start(int port) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    started_ = true;

    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    listen_.store(l);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));   // port==0 лишає ефемерний
    if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) return false;
    int len = sizeof(addr);
    if (getsockname(l, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) return false;
    port_ = ntohs(addr.sin_port);
    if (listen(l, 1) == SOCKET_ERROR) return false;

    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void LabelEmulator::Stop() {
    running_.store(false);
    SOCKET l = listen_.exchange(INVALID_SOCKET);
    if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
    SOCKET c = client_.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
    if (thread_.joinable()) thread_.join();
    if (started_) { WSACleanup(); started_ = false; }
}

std::string LabelEmulator::LastZpl() const {
    std::lock_guard<std::mutex> lk(bufMutex_);
    return buffer_;
}

void LabelEmulator::Run() {
    while (running_.load()) {
        SOCKET c = accept(listen_.load(), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;              // listen-сокет закрито у Stop()
        client_.store(c);
        // Закриваємо тільки якщо саме цей потік «виграв» гонку зі Stop().
        if (!running_.load()) { if (client_.exchange(INVALID_SOCKET) == c) closesocket(c); return; }

        char tmp[4096];
        while (running_.load()) {
            int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (n <= 0) break;                        // клієнт закрив або помилка
            std::lock_guard<std::mutex> lk(bufMutex_);
            buffer_.append(tmp, static_cast<size_t>(n));
        }
        SOCKET old = client_.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);  // закрити цього клієнта, чекати наступного
    }
}
