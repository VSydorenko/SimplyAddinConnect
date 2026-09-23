//
// @file tests/prro_fs_state_selftest.cpp
// @brief Рівень L-f1 гейта: чиста логіка імітації фіскального сервера ДПС (стан,
//        нумерація, підсумки, збої) і диспозиції MiniHttpServer. Без UAPKI, без 1С.
//        Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.1.
//        Критерій гейта — exit 0; кожна перевірка друкує [PASS]/[FAIL].
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/MiniHttpClient.h"

#include <windows.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

// Сервер на першому вільному порту 18100..18199.
static std::unique_ptr<minihttp::Server> StartServer(minihttp::Handler h,
                                                     minihttp::CommonHeadersFn common, int& port) {
    for (int p = 18100; p < 18200; ++p) {
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(p, "127.0.0.1"));
        s->SetHandler(h);
        if (common) s->SetCommonHeaders(common);
        if (s->Listen().first) { s->Start(); port = p; return s; }
    }
    port = 0;
    return nullptr;
}

static void TestTransport() {
    std::printf("== Транспорт MiniHttpServer: заголовки й диспозиції ==\n");
    static const char* kDate = "Thu, 24 Sep 2026 09:00:00 GMT";
    auto common  = []() { return minihttp::HeaderList{ { "Date", kDate } }; };
    // minihttp::Request — і struct (запит), і функція-клієнт (MiniHttpClient.h);
    // в одній TU обидва в namespace minihttp, і за правилом приховування імені
    // класу функцією якісне ім'я типу тут резолвиться у функцію. Тому — явний
    // elaborated-type-specifier "struct", щоб узяти саме тип.
    auto handler = [](const struct minihttp::Request& rq) -> minihttp::Response {
        minihttp::Response r;
        if (rq.uri == "/send")   { r.body = "ok"; r.headers.push_back({ "X-Extra", "1" }); return r; }
        if (rq.uri == "/abort")  { r.disposition = minihttp::Disposition::Abort; return r; }
        if (rq.uri == "/hold")   { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 2;  return r; }
        if (rq.uri == "/hold30") { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 30; return r; }
        r.code = 404;
        return r;
    };
    int port = 0;
    std::unique_ptr<minihttp::Server> srv = StartServer(handler, common, port);
    CHECK(srv != nullptr, "сервер піднявся на вільному порту 18100..18199");
    if (!srv) return;

    minihttp::ClientResult a = minihttp::Request(port, "GET", "/send", "", "", 3000);
    CHECK(a.responded && a.code == 200 && a.body == "ok", "Send: відповідь 200 з тілом");
    CHECK(a.headers.count("date") == 1 && a.headers["date"] == kDate, "Send: спільний заголовок Date присутній");
    CHECK(a.headers.count("x-extra") == 1, "Send: власний заголовок відповіді (Response::headers) присутній");

    minihttp::ClientResult t = minihttp::RawRequest(port,
        "POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n", 3000);
    CHECK(t.responded && t.code == 411, "транспортна відмова 411 віддана самим сервером");
    CHECK(t.headers.count("date") == 1, "транспортна відмова теж має Date (спільні заголовки на кожній відповіді)");

    minihttp::ClientResult b = minihttp::Request(port, "GET", "/abort", "", "", 3000);
    CHECK(b.connected && !b.responded, "Abort: з'єднання було, відповіді немає");
    CHECK(b.reset && !b.timedOut, "Abort: клієнт бачить розрив (RST), а не таймаут");

    // Правило 3: утримання 2 с, клієнтський таймаут 1 с. Числа в звіті задачі — з виміру.
    minihttp::ClientResult c = minihttp::Request(port, "GET", "/hold", "", "", 1000);
    CHECK(!c.responded && c.timedOut, "HoldThenAbort: клієнт із таймаутом 1 с не отримав нічого");
    minihttp::ClientResult d = minihttp::Request(port, "GET", "/hold", "", "", 5000);
    CHECK(!d.responded && d.reset && d.elapsedMs >= 1800,
          "HoldThenAbort: після ~2 с тиші — розрив (elapsedMs >= 1800)");

    // Stop() має перервати утримання, а не чекати holdSeconds.
    std::thread cli([port]() { minihttp::Request(port, "GET", "/hold30", "", "", 40000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    srv->Stop();
    const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
    cli.join();
    std::printf("  виміряно: Stop() під час утримання 30 с зайняв %lld мс\n", stopMs);
    CHECK(stopMs < 2000, "Stop() перериває утримання (< 2000 мс; без переривання — ~5000 мс стелі Stop)");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    if (!minihttp::InitNetwork()) { std::printf("[FAIL] WSAStartup\n"); return 1; }

    TestTransport();

    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
