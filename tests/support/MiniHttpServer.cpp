// MiniHttpServer — мінімальний HTTP/1.1-сервер для тестових харнесів.
// Опис і межі застосовності — у MiniHttpServer.h.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include <winsock2.h>   // строго перед windows.h/ws2tcpip.h
#include <ws2tcpip.h>

#include "MiniHttpServer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

namespace minihttp {
namespace {

// Стеля на заголовки й на тіло. Перша боронить від «нескінченного» заголовкового
// потоку, друга — від спроби з'їсти пам'ять тілом. Прикладну межу (1 MiB) ставить
// сам оракул; тут запас, щоб він устиг відповісти 413 своїм текстом.
constexpr size_t MAX_HEADER_BYTES = 64u * 1024u;
constexpr size_t MAX_BODY_BYTES   = 8u * 1024u * 1024u;

// Таймаути сокета з'єднання: клієнт, який відкрив сокет і мовчить, не має
// тримати потік вічно.
constexpr int RECV_TIMEOUT_MS = 15000;
constexpr int SEND_TIMEOUT_MS = 15000;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string WsaErrText(int code) {
    char buf[256] = { 0 };
    std::snprintf(buf, sizeof(buf), "WSA error %d", code);
    return std::string(buf);
}

// Надіслати весь буфер (send може віддати менше, ніж просили).
bool SendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int chunk = static_cast<int>(std::min<size_t>(len - sent, 64u * 1024u));
        const int n = send(s, data + sent, chunk, 0);
        if (n == SOCKET_ERROR || n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void SendRaw(SOCKET s, int code, const std::string& reason,
             const std::string& ctype, const std::string& body) {
    char head[512];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 %d %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                code, reason.c_str(), ctype.c_str(), body.size());
    if (n <= 0) return;
    if (!SendAll(s, head, static_cast<size_t>(n))) return;
    if (!body.empty()) SendAll(s, body.data(), body.size());
}

}  // namespace

const char* ReasonFor(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Status";
    }
}

bool InitNetwork() {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
}

void ShutdownNetwork() {
    WSACleanup();
}

Server::Server(int port, std::string host)
    : port_(port), host_(std::move(host)) {}

Server::~Server() {
    Stop();
}

void Server::SetHandler(Handler h) {
    handler_ = std::move(h);
}

std::pair<bool, std::string> Server::Listen() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return { false, WsaErrText(WSAGetLastError()) };

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return { false, "невірна адреса: " + host_ };
    }

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        closesocket(s);
        return { false, WsaErrText(err) };
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        closesocket(s);
        return { false, WsaErrText(err) };
    }

    listen_.store(s);
    return { true, std::string() };
}

void Server::Start() {
    if (listen_.load() == INVALID_SOCKET) return;
    stopping_.store(false);
    acceptThread_ = std::thread(&Server::AcceptLoop, this);
}

void Server::AcceptLoop() {
    for (;;) {
        const SOCKET ls = listen_.load();
        if (ls == INVALID_SOCKET || stopping_.load()) break;

        const SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            // Stop() закрив слухача — це штатний вихід, а не збій.
            if (stopping_.load()) break;
            continue;
        }
        if (stopping_.load()) { closesocket(c); break; }

        active_.fetch_add(1);
        std::thread([this, c] {
            Serve(c);
            closesocket(c);
            if (active_.fetch_sub(1) == 1) {
                std::lock_guard<std::mutex> lk(activeMx_);
                activeCv_.notify_all();
            }
        }).detach();
    }
}

void Server::Serve(SOCKET client) {
    DWORD rcv = RECV_TIMEOUT_MS, snd = SEND_TIMEOUT_MS;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));

    // --- 1. Заголовки: читаємо до порожнього рядка -----------------------------
    std::string buf;
    size_t headEnd = std::string::npos;
    char chunk[8192];
    for (;;) {
        headEnd = buf.find("\r\n\r\n");
        if (headEnd != std::string::npos) break;
        if (buf.size() > MAX_HEADER_BYTES) {
            SendRaw(client, 431, ReasonFor(431), "text/plain; charset=utf-8",
                    "headers too large");
            return;
        }
        const int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) return;   // клієнт відпав або таймаут — відповідати нема кому
        buf.append(chunk, static_cast<size_t>(n));
    }

    const std::string head = buf.substr(0, headEnd);
    std::string rest = buf.substr(headEnd + 4);

    // --- 2. Request-line -------------------------------------------------------
    const size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);

    Request req;
    {
        const size_t sp1 = requestLine.find(' ');
        const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : requestLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            SendRaw(client, 400, ReasonFor(400), "text/plain; charset=utf-8", "bad request line");
            return;
        }
        req.method = requestLine.substr(0, sp1);
        req.uri    = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    // --- 3. Заголовки у мапу (імена — у нижній регістр) ------------------------
    if (lineEnd != std::string::npos) {
        size_t pos = lineEnd + 2;
        while (pos < head.size()) {
            size_t eol = head.find("\r\n", pos);
            if (eol == std::string::npos) eol = head.size();
            const std::string line = head.substr(pos, eol - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                req.headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
            }
            pos = eol + 2;
        }
    }

    // --- 4. Тіло за Content-Length --------------------------------------------
    // chunked свідомо НЕ підтримуємо: краще чесний 411, ніж мовчки порожнє тіло,
    // яке оракул прийме за «підпис не надіслали».
    const auto teIt = req.headers.find("transfer-encoding");
    if (teIt != req.headers.end() && ToLower(teIt->second).find("chunked") != std::string::npos) {
        SendRaw(client, 411, ReasonFor(411), "text/plain; charset=utf-8",
                "chunked transfer-encoding not supported, use Content-Length");
        return;
    }

    size_t contentLength = 0;
    const auto clIt = req.headers.find("content-length");
    if (clIt != req.headers.end()) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(clIt->second.c_str(), &end, 10);
        if (end == clIt->second.c_str() || v > MAX_BODY_BYTES) {
            SendRaw(client, 413, ReasonFor(413), "text/plain; charset=utf-8", "body too large");
            return;
        }
        contentLength = static_cast<size_t>(v);
    }

    req.body = std::move(rest);
    while (req.body.size() < contentLength) {
        const int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) return;   // обірване тіло — відповідати нема сенсу
        req.body.append(chunk, static_cast<size_t>(n));
    }
    if (req.body.size() > contentLength) req.body.resize(contentLength);

    // --- 5. Обробник -----------------------------------------------------------
    Response resp;
    if (handler_) {
        resp = handler_(req);
    } else {
        resp.code = 500;
        resp.body = "no handler";
    }
    const std::string reason = resp.reason.empty() ? std::string(ReasonFor(resp.code)) : resp.reason;
    SendRaw(client, resp.code, reason, resp.contentType, resp.body);

    // Дати клієнту дочитати відповідь до закриття сокета.
    shutdown(client, SD_SEND);
}

void Server::Stop() {
    if (stopping_.exchange(true)) return;

    const SOCKET ls = listen_.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) closesocket(ls);   // будить accept()

    if (acceptThread_.joinable()) acceptThread_.join();

    // Потоки з'єднань detach-нуті: чекаємо на лічильник, але не вічно —
    // зависле з'єднання не має заблокувати вихід з програми.
    std::unique_lock<std::mutex> lk(activeMx_);
    activeCv_.wait_for(lk, std::chrono::seconds(5), [this] { return active_.load() == 0; });
}

}  // namespace minihttp
