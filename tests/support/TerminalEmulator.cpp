#include "TerminalEmulator.h"
#include <ws2tcpip.h>

bool TerminalEmulator::Start() {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    started_ = true;

    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    listen_.store(l);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   // ефемерний порт
    if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) return false;
    int len = sizeof(addr);
    if (getsockname(l, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) return false;
    port_ = ntohs(addr.sin_port);
    if (listen(l, 1) == SOCKET_ERROR) return false;

    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void TerminalEmulator::Stop() {
    running_.store(false);
    SOCKET l = listen_.exchange(INVALID_SOCKET);
    if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
    SOCKET c = client_.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
    if (thread_.joinable()) thread_.join();
    if (started_) { WSACleanup(); started_ = false; }
}

void TerminalEmulator::Run() {
    while (running_.load()) {
        SOCKET c = accept(listen_.load(), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;              // listen-сокет закрито у Stop()
        client_.store(c);
        // Закриваємо тільки якщо саме цей потік «виграв» гонку зі Stop() (уникнення
        // подвійного closesocket того самого хендла — Stop() міг уже його забрати й закрити).
        if (!running_.load()) { if (client_.exchange(INVALID_SOCKET) == c) closesocket(c); return; }

        std::vector<uint8_t> buf; char tmp[4096];
        while (running_.load()) {
            int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (n <= 0) break;                        // клієнт закрив (dc еталонної схеми)
            for (int i = 0; i < n; ++i) {
                if (tmp[i] == 0) { if (!buf.empty()) HandleFrame(c, buf); buf.clear(); }
                else buf.push_back(static_cast<uint8_t>(tmp[i]));
            }
        }
        SOCKET old = client_.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);   // закрити цього клієнта, чекати наступного
    }
}

void TerminalEmulator::HandleFrame(SOCKET c, const std::vector<uint8_t>& frame) {
    auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    std::string method = j.value("method", std::string{});
    auto it = handlers_.find(method);
    if (it == handlers_.end()) return;   // нема сценарію — мовчимо

    std::string resp = it->second(j);
    std::vector<uint8_t> out(resp.begin(), resp.end());
    out.push_back(0);   // термінатор кадру
    int off = 0, total = static_cast<int>(out.size());
    while (off < total) {
        int s = ::send(c, reinterpret_cast<const char*>(out.data()) + off, total - off, 0);
        if (s <= 0) break;
        off += s;
    }
}
