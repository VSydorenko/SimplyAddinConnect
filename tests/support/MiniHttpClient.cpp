// MiniHttpClient — реалізація. Опис — у MiniHttpClient.h.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include <winsock2.h>   // строго перед windows.h/ws2tcpip.h
#include <ws2tcpip.h>

#include "MiniHttpClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

namespace minihttp {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

ClientResult FetchRaw(int port, const std::string& raw, int timeoutMs) {
    ClientResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto stamp = [&r, t0]() {
        r.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    };

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { stamp(); return r; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        stamp();
        return r;
    }
    r.connected = true;
    const DWORD to = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    size_t sent = 0;
    while (sent < raw.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(raw.size() - sent, 64u * 1024u));
        const int n = send(s, raw.data() + sent, chunk, 0);
        if (n == SOCKET_ERROR || n <= 0) break;
        sent += static_cast<size_t>(n);
    }

    std::string buf;
    char chunk[8192];
    for (;;) {
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n > 0) { buf.append(chunk, static_cast<size_t>(n)); continue; }
        if (n == 0) break;                                   // штатне закриття (FIN)
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) r.timedOut = true;          // клієнтський таймаут
        else                     r.reset    = true;          // WSAECONNRESET тощо
        break;
    }
    closesocket(s);
    stamp();

    const size_t headEnd = buf.find("\r\n\r\n");
    if (buf.compare(0, 5, "HTTP/") != 0 || headEnd == std::string::npos) {
        // Відповіді не було. Закриття без жодного байта — теж розрив з погляду клієнта.
        if (!r.timedOut) r.reset = true;
        return r;
    }
    r.responded = true;
    const size_t sp = buf.find(' ');
    r.code = (sp != std::string::npos && sp < headEnd) ? std::atoi(buf.c_str() + sp + 1) : 0;

    size_t pos = buf.find("\r\n");                          // кінець status-line
    while (pos != std::string::npos && pos < headEnd) {
        const size_t eol  = buf.find("\r\n", pos + 2);
        const std::string line = buf.substr(pos + 2, eol - pos - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            r.headers[Lower(line.substr(0, colon))] = v;
        }
        pos = eol;
    }
    r.body = buf.substr(headEnd + 4);
    return r;
}

ClientResult Fetch(int port, const std::string& method, const std::string& path,
                   const std::string& body, const std::string& contentType, int timeoutMs) {
    std::string raw = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!contentType.empty()) raw += "Content-Type: " + contentType + "\r\n";
    raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    raw += "Connection: close\r\n\r\n";
    raw += body;
    return FetchRaw(port, raw, timeoutMs);
}

}  // namespace minihttp
