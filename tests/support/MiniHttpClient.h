#pragma once
// MiniHttpClient — мінімальний HTTP/1.1-клієнт для самотестів (лише loopback).
// Розрізняє три наслідки запиту, які для імітації ДПС і є предметом перевірки:
// відповідь отримано / з'єднання розірвано без відповіді / клієнтський таймаут.
// Winsock ініціалізує викликач (minihttp::InitNetwork()).
#include <map>
#include <string>

namespace minihttp {

struct ClientResult {
    bool        connected = false;   // TCP-з'єднання встановлено
    bool        responded = false;   // отримано status-line і заголовки
    bool        reset     = false;   // розрив без відповіді (RST або закриття до status-line)
    bool        timedOut  = false;   // клієнтський таймаут до будь-якої відповіді
    int         code      = 0;
    std::map<std::string, std::string> headers;   // імена — у нижньому регістрі
    std::string body;
    long long   elapsedMs = 0;
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
