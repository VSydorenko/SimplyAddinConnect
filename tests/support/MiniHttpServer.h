#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <winsock2.h>

// Мінімальний HTTP/1.1-сервер для тестових харнесів (лише тести, лише loopback).
//
// З'явився на заміну ix::HttpServer: ixwebsocket тягнувся в репозиторій заради
// одного цього сервера в ручному uapki_fiscal_emulator (і заради транспорту
// TransportWSClient, якого не створював жоден драйвер). Тримати мережеву
// бібліотеку й механізм її умовного підключення заради ~300 рядків — дорожче,
// ніж мати ці рядки своїми.
//
// Свідомі обмеження (нам їх достатньо, розширювати без потреби не варто):
//   * HTTP/1.1 з "Connection: close" — на кожен запит окреме з'єднання;
//     keep-alive НЕ підтримується;
//   * тіло лише за Content-Length; на "Transfer-Encoding: chunked" сервер
//     чесно відповідає 411, а не мовчки віддає порожнє тіло;
//   * без TLS, без стиснення, без cookie — оракул слухає лише 127.0.0.1.
//
// Модель потоків ДЗЕРКАЛИТЬ ix::HttpServer: кожне з'єднання обробляється своїм
// потоком, тож обробник МУСИТЬ бути потокобезпечним. uapki_fiscal_emulator на це
// й розрахований — у нього глобальний стан UAPKI під власним м'ютексом.
namespace minihttp {

// Розібраний запит. headers — імена НОРМАЛІЗОВАНІ в нижній регістр
// (HTTP-заголовки регістронезалежні, а шукати зручніше за одним написанням).
struct Request {
    std::string method;                          // "GET", "POST", ...
    std::string uri;                             // шлях РАЗОМ із query, як у request-line
    std::string body;
    std::map<std::string, std::string> headers;
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Що зробити з з'єднанням після обробника.
enum class Disposition {
    Send,           // штатно: відповідь + shutdown(SD_SEND)
    Abort,          // закрити БЕЗ відповіді, RST (SO_LINGER {1,0}) — «обрив» для тестів
    HoldThenAbort   // мовчати holdSeconds, потім RST; Stop() перериває очікування
};

// Відповідь. Порожній reason -> сервер підставить стандартний текст для code.
struct Response {
    int         code = 200;
    std::string reason;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    HeaderList  headers;                          // додаткові заголовки (Location тощо)
    Disposition disposition = Disposition::Send;
    int         holdSeconds = 0;                  // лише для HoldThenAbort
};

using Handler = std::function<Response(const Request&)>;

// Заголовки, що додаються до КОЖНОЇ відповіді — і обробника, і власних відмов
// транспорту (400/411/413/431). Імітація ДПС ставить так заголовок Date.
using CommonHeadersFn = std::function<HeaderList()>;

// Winsock: ініціалізація/зупинка на процес (WSAStartup/WSACleanup).
bool InitNetwork();
void ShutdownNetwork();

// Стандартний текст статусу ("OK", "Not Found", ...) — для тих, кому треба
// надрукувати його поруч із кодом.
const char* ReasonFor(int code);

class Server {
public:
    Server(int port, std::string host);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void SetHandler(Handler h);
    void SetCommonHeaders(CommonHeadersFn f);

    // bind+listen. false + текст помилки, якщо порт зайнятий/недоступний —
    // саме за цим викликач шукає наступний вільний порт.
    // УВАГА: SO_REUSEADDR свідомо НЕ виставляється. На Windows він дозволяє
    // прив'язатися до вже зайнятого порту, і пошук вільного порту тихо ламався б
    // (bind «успішний», а запити йдуть чужому слухачу).
    std::pair<bool, std::string> Listen();

    void Start();   // піднімає accept-потік
    void Stop();    // закриває слухача, чекає на потоки з'єднань

private:
    void AcceptLoop();
    void Serve(SOCKET client);
    HeaderList CommonHeaders() const;

    int         port_;
    std::string host_;
    Handler     handler_;
    CommonHeadersFn commonHeaders_;
    // Утримання з'єднання (HoldThenAbort): Stop() будить очікування через holdCv_.
    std::mutex              holdMx_;
    std::condition_variable holdCv_;

    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::thread         acceptThread_;
    std::atomic<bool>   stopping_{ false };

    // Потоки з'єднань detach-нуті; Stop() чекає, поки лічильник впаде до нуля.
    std::atomic<int>        active_{ 0 };
    std::mutex              activeMx_;
    std::condition_variable activeCv_;
};

}  // namespace minihttp
