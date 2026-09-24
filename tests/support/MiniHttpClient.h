#pragma once
// MiniHttpClient — мінімальний HTTP/1.1-клієнт для самотестів (лише loopback).
// Розрізняє три наслідки запиту, які для імітації ДПС і є предметом перевірки:
// відповідь отримано / з'єднання розірвано без відповіді / клієнтський таймаут.
// Winsock ініціалізує викликач (minihttp::InitNetwork()).
#include <map>
#include <string>

namespace minihttp {

// Семантика полів (кожне нижче — ОКРЕМИЙ, не взаємовиключний прапор; для випадку
// «розрив» типово responded==false і рівно один з {reset, timedOut} true, а rst —
// вужче підмноження reset):
struct ClientResult {
    bool        connected = false;   // TCP-з'єднання встановлено (connect() пройшов)
    bool        responded = false;   // отримано status-line і заголовки (HTTP-відповідь)
    bool        reset     = false;   // з'єднання закрито до будь-якої відповіді: RST
                                      // АБО штатне закриття (FIN) без status-line —
                                      // з погляду клієнта обидва "розрив без відповіді"
    bool        rst       = false;   // ВУЖЧЕ за reset: true лише коли розрив підтверджено
                                      // саме кодом WSAECONNRESET (10054 — виміряно 2026-09-24
                                      // на AbortConnection()/SO_LINGER{1,0} у MiniHttpServer;
                                      // WSAECONNABORTED/10053 не спостережено). false і для
                                      // штатного FIN без відповіді (recv()==0), і для таймауту.
    bool        timedOut  = false;   // клієнтський таймаут (SO_RCVTIMEO/SO_SNDTIMEO) до
                                      // будь-якої відповіді
    int         code      = 0;       // HTTP статус-код (валідний лише якщо responded)
    std::map<std::string, std::string> headers;   // імена — у нижньому регістрі
    std::string body;                // тіло відповіді (валідне лише якщо responded)
    long long   elapsedMs = 0;       // час від connect() до закриття з'єднання чи помилки
};

// Надіслати сирі байти запиту й прочитати відповідь до закриття з'єднання.
// Ім'я Fetch (не Request) — свідомо: minihttp::Request — це ще й struct (розібраний
// запит сервера, MiniHttpServer.h); в одній TU з обома хедерами якісне ім'я типу
// без "struct" ховається за іменем функції (правило приховування імені класу).
ClientResult FetchRaw(int port, const std::string& raw, int timeoutMs);

// Звичайний запит: Content-Length за тілом, Connection: close.
ClientResult Fetch(int port, const std::string& method, const std::string& path,
                   const std::string& body, const std::string& contentType, int timeoutMs);

}  // namespace minihttp
