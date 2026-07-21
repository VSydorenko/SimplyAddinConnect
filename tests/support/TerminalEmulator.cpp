#include "TerminalEmulator.h"
#include <ws2tcpip.h>

bool TerminalEmulator::Start(int port) {
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

void TerminalEmulator::Stop() {
    running_.store(false);
    SOCKET l = listen_.exchange(INVALID_SOCKET);
    if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
    SOCKET c = client_.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
    if (thread_.joinable()) thread_.join();
    // Дочекатися всіх frame-worker-ів (можуть висіти у responder-і/send) ПЕРЕД WSACleanup.
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
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
                if (tmp[i] == 0) {
                    // Кожен повний кадр — окремий worker (responder може блокувати; read-loop
                    // лишається вільним для наступних кадрів poller-а/interrupt).
                    if (!buf.empty()) workers_.emplace_back([this, c, frame = buf] { HandleFrame(c, frame); });
                    buf.clear();
                }
                else buf.push_back(static_cast<uint8_t>(tmp[i]));
            }
        }
        SOCKET old = client_.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);   // закрити цього клієнта, чекати наступного
    }
}

void TerminalEmulator::Log(const std::string& line) {
    if (!log_) return;
    std::lock_guard<std::mutex> lk(logMutex_);
    log_(line);
}

void TerminalEmulator::HandleFrame(SOCKET c, const std::vector<uint8_t>& frame) {
    std::string raw(frame.begin(), frame.end());
    auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) { Log("RECV (невалідний JSON): " + raw); return; }
    std::string method = j.value("method", std::string{});
    Log("RECV [" + method + "] " + raw);
    auto it = handlers_.find(method);
    if (it == handlers_.end()) { Log("  -> нема сценарію для [" + method + "], мовчимо"); return; }

    std::string resp = it->second(j);
    if (resp.empty()) { Log("  -> responder без відповіді"); return; }
    Log("SEND " + resp);
    std::vector<uint8_t> out(resp.begin(), resp.end());
    out.push_back(0);   // термінатор кадру
    // Send під m'ютексом: байти відповідей різних worker-ів не перемішуються на сокеті.
    std::lock_guard<std::mutex> lk(sendMutex_);
    int off = 0, total = static_cast<int>(out.size());
    while (off < total) {
        int s = ::send(c, reinterpret_cast<const char*>(out.data()) + off, total - off, 0);
        if (s <= 0) break;
        off += s;
    }
}
