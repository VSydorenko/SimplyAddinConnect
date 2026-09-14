// ecr_privatjson_selftest — харнес пілотного драйвера ECRPrivatJSON (кодек/класифікатор/
// емулятор/e2e) без 1С і без UAPKI. Свій хенд-ролед раннер (як wire_selftest). pch тут НЕ підключаємо.
#include "../src/platform/ResultEnvelope.h"
#include "../src/drivers/ecr_privatjson/EcrJsonCodec.h"
#include "../src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h"
#include "../src/transport/IFrameClassifier.h"
#include "support/TerminalEmulator.h"
#include "../src/transport/DeviceSession.h"
#include "../src/transport/Transport_TCP.h"
#include "../src/transport/NullTerminatedFramer.h"
#include "../src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h"
#include "../src/platform/JobEngine.h"
#include "../src/core/AddInNative.h"   // 1С-фасад: інстанціювання компоненти через CreateObject
#include "../src/components/AcquiringFacadeBase.h"
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

static int g_failed = 0;
#define CHECK(c,n) do{ if(c){std::printf("[PASS] %s\n",n);} else {std::printf("[FAIL] %s\n",n);++g_failed;} }while(0)

static std::vector<uint8_t> Bytes(const std::string& s){ return {s.begin(), s.end()}; }

static void TestResultEnvelope() {
    auto ok = ResultEnvelope::Ok({{"invoiceNumber", "42"}});
    auto j = ok.ToJson();
    CHECK(j["ok"] == true && j["code"] == "OK" && j["payload"]["invoiceNumber"] == "42",
          "ResultEnvelope::Ok → {ok:true, code:OK, payload}");

    auto fail = ResultEnvelope::Fail("TIMEOUT", "Немає відповіді термінала");
    auto jf = fail.ToJson();
    CHECK(jf["ok"] == false && jf["code"] == "TIMEOUT" && jf["description"] == "Немає відповіді термінала",
          "ResultEnvelope::Fail → {ok:false, code, description}");
}

static void TestEcrJsonCodec() {
    // BuildRequest: PingDevice без params → точні байти (nlohmann сортує ключі: method<step)
    auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
    CHECK(ping == Bytes(R"({"method":"PingDevice","step":0})"),
          "BuildRequest PingDevice → exact bytes, без делімітера");

    // Parse: повна відповідь Purchase
    auto pr = EcrJsonCodec::Parse(Bytes(
        R"({"method":"Purchase","step":0,"params":{"invoiceNumber":"42"},"error":false,"errorDescription":""})"));
    CHECK(pr.valid && pr.method == "Purchase" && pr.error == false && pr.params["invoiceNumber"] == "42",
          "Parse Purchase → поля method/error/params");

    // Parse: ServiceMessage deviceBusy → msgType заповнено
    auto db = EcrJsonCodec::Parse(Bytes(
        R"({"method":"ServiceMessage","step":0,"params":{"msgType":"deviceBusy"},"error":false,"errorDescription":""})"));
    CHECK(db.valid && db.method == "ServiceMessage" && db.msgType == "deviceBusy",
          "Parse ServiceMessage → msgType=deviceBusy");

    // PeekMethod: легкий парс
    std::string m, mt;
    CHECK(EcrJsonCodec::PeekMethod(Bytes(R"({"method":"ServiceMessage","params":{"msgType":"identify"}})"), m, mt)
          && m == "ServiceMessage" && mt == "identify", "PeekMethod → method+msgType");

    // Parse невалідного JSON → valid=false (без винятку)
    CHECK(EcrJsonCodec::Parse(Bytes("{not json")).valid == false, "Parse невалідного → valid=false, без кидка");

    // Parse ВАЛІДНОГО JSON з неправильним ТИПОМ поля (method як число, step як рядок) → valid=false,
    // без винятку: .value<T>() кидав би type_error(302) попри allow_exceptions=false — не має пробити межу 1С.
    CHECK(EcrJsonCodec::Parse(Bytes(R"({"method":123,"step":"x"})")).valid == false,
          "Parse типово-неправильного → valid=false, без кидка");
    { std::string mm, mmt;
      CHECK(EcrJsonCodec::PeekMethod(Bytes(R"({"method":123})"), mm, mmt) == false,
            "PeekMethod типово-неправильного → false, без кидка"); }
}

static void TestEcrClassifier() {
    EcrPrivatJsonClassifier clf;
    auto purchaseReq = EcrJsonCodec::BuildRequest("Purchase", 0, {{"amount","1.00"}});
    auto identifyReq = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","identify"}});
    auto correctReq  = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","correctTransaction"},{"amount","0.50"}});
    auto interruptReq= EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","interrupt"}});
    auto statReq     = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","getLastStatMsgCode"}});

    // Один іменований std::string: begin()/end() від ОДНОГО обʼєкта (пара тимчасових
    // std::string(s) давала б несумісні ітератори → UB/heap corruption).
    auto F = [](const char* s){ std::string t(s); return std::vector<uint8_t>(t.begin(), t.end()); };

    // 1) primary-відповідь: method збігається з primary
    { PendingView pv{ &purchaseReq, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"Purchase","step":0,"params":{},"error":false})"));
      CHECK(c.cls == FrameClass::PrimaryResponse, "Classify: Purchase-відповідь → PrimaryResponse"); }

    // 2) service-відповідь: identify під час identify
    { PendingView pv{ nullptr, &identifyReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: identify-відповідь → ServiceResponse"); }

    // 3) correctionTransmitted — ВІДПОВІДЬ на correctTransaction (service), НЕ Unsolicited
    { PendingView pv{ &purchaseReq, &correctReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"correctionTransmitted"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: correctionTransmitted → ServiceResponse"); }

    // 4) interruptTransmitted — відповідь на interrupt (service)
    { PendingView pv{ &purchaseReq, &interruptReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"interruptTransmitted"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: interruptTransmitted → ServiceResponse"); }

    // 5) deviceBusy за наявного primary → RejectPrimary{Busy}
    { PendingView pv{ &purchaseReq, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"}})"));
      CHECK(c.cls == FrameClass::RejectPrimary && c.reason == RejectReason::Busy,
            "Classify: deviceBusy+primary → RejectPrimary/Busy"); }

    // 6) methodNotImplemented при ОБОХ pending → RejectBoth (кадр не називає метод)
    { PendingView pv{ &purchaseReq, &statReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"methodNotImplemented"}})"));
      CHECK(c.cls == FrameClass::RejectBoth && c.reason == RejectReason::Unsupported,
            "Classify: methodNotImplemented+обидва → RejectBoth/Unsupported"); }

    // 7) немає відповідного pending → Unsolicited
    { PendingView pv{ nullptr, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"}})"));
      CHECK(c.cls == FrameClass::Unsolicited, "Classify: deviceBusy без primary → Unsolicited"); }
}

static void TestEmulatorPing() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&) {
        return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})";
    });
    CHECK(emu.Start(), "Emulator: стартував на ефемерному порту");

    DeviceSession session(std::make_unique<TransportTCP>("127.0.0.1", emu.Port()),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EcrPrivatJsonClassifier>());
    CHECK(session.Start(), "Emulator: DeviceSession підключився");

    auto req = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
    // хендшейк-кадр має провідний 0x00
    RequestResult r = session.RequestPrimary(req, 3000, FrameOptions{/*leadingDelimiter=*/true});
    CHECK(r.status == RequestStatus::Response, "Emulator: PingDevice → Response");
    auto pr = EcrJsonCodec::Parse(r.frame);
    CHECK(pr.valid && pr.method == "PingDevice" && pr.params["responseCode"] == "0000",
          "Emulator: відповідь PingDevice розібрано, responseCode=0000");

    session.Stop();
    emu.Stop();
}

static void TestConnStringParse() {
    EcrConnParams p;
    CHECK(EcrPrivatJsonDriver::ParseConnString("tcp://192.168.0.10:2000", p)
          && p.kind == EcrConnParams::Kind::Tcp && p.host == "192.168.0.10" && p.tcpPort == 2000,
          "ParseConnString: tcp://host:port");
    EcrConnParams c;
    CHECK(EcrPrivatJsonDriver::ParseConnString("COM3:115200,8,N,1", c)
          && c.kind == EcrConnParams::Kind::Com && c.comPort == "COM3" && c.baud == 115200,
          "ParseConnString: COM3:115200,8,N,1");
    EcrConnParams bad;
    CHECK(EcrPrivatJsonDriver::ParseConnString("garbage", bad) == false, "ParseConnString: сміття → false");
}

static void TestConnectReferenceScheme() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&) {
        return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& req) -> std::string {
        // Identify: відповідь із vendor/model
        if (req.contains("params") && req["params"].value("msgType","") == "identify")
            return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        return "";
    });
    CHECK(emu.Start(), "Connect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())),
          "Connect: еталонна схема (Ping+dc→Identify+dc→постійний конект) → true");
    CHECK(drv.IsConnected(), "Connect: постійна сесія на зв'язку");
    CHECK(drv.Vendor() == "PAX" && drv.Model() == "s800", "Connect: збережено vendor/model з Identify");

    drv.Disconnect();
    CHECK(!drv.IsConnected(), "Disconnect: сесію закрито");
    emu.Stop();
}

static void TestJobEngine() {
    JobEngine eng;
    CHECK(eng.State() == JobState::Idle, "JobEngine: стартовий стан Idle");

    std::atomic<bool> release{false};
    bool started = eng.Start([&]() -> ResultEnvelope {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ResultEnvelope::Ok({{"done", true}});
    });
    CHECK(started && eng.State() == JobState::Running, "JobEngine: Start → Running");
    CHECK(eng.Start([]{ return ResultEnvelope::Ok(); }) == false, "JobEngine: повторний Start під час Running → false");

    eng.RequestCancel();
    CHECK(eng.CancelRequested(), "JobEngine: RequestCancel виставляє прапорець");

    release.store(true);
    eng.Join();
    CHECK(eng.State() == JobState::Done, "JobEngine: після завершення op → Done");
    ResultEnvelope out;
    CHECK(eng.TryGetResult(out) && out.ok && out.payload["done"] == true, "JobEngine: результат op збережено");

    // op кидає → Error
    JobEngine eng2;
    eng2.Start([]() -> ResultEnvelope { throw std::runtime_error("boom"); });
    eng2.Join();
    CHECK(eng2.State() == JobState::Error, "JobEngine: виняток у op → Error");
}

// №12: два викликачі Start (ExecuteInternal і dispatcher-хук) на завершеному джобі.
// Без startMutex_ обидва пройдуть перевірку стану, обидва зроблять join того самого
// потоку й присвоєння joinable-потоку -> std::terminate (спека §2.2).
static void TestJobEngineStartRace() {
    for (int iter = 0; iter < 100; ++iter) {
        JobEngine eng;
        eng.Start([]{ return ResultEnvelope::Ok(); });
        eng.Join();                      // джоб завершений; Join() уже приєднав worker_
        eng.ResetToIdle();
        // Гонка, яку ловить цей сценарій: обидва потоки проходять перевірку стану під m_,
        // обидва доходять до worker_ = std::thread(...) — присвоєння в уже-joinable потік
        // (другий переможець) дає std::terminate. Подвійний join покриває сценарій нижче.

        std::atomic<int> wins{ 0 };
        std::atomic<bool> go{ false };
        // release тримає worker переможця в Running, доки ОБИДВА racer-и не віддали вердикт:
        // Start() лишає легітимний повторний запуск із Done (не лише з Idle/Error), тож
        // миттєвий op ([]{ return Ok(); }) міг би завершитись і перевести стан назад у Done
        // ще ДО виклику Start() другим потоком - другий Start() тоді теж чесно повернув би
        // true (це не гонка double-join, а звичайний послідовний рестарт) і wins==2 без
        // жодного terminate. Блокуючи op до release, тримаємо стан Running на весь час
        // виконання racer-ів - програвший гарантовано бачить Running і отримує false.
        std::atomic<bool> release{ false };
        auto racer = [&]{
            while (!go.load()) std::this_thread::yield();
            if (eng.Start([&release]{
                    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    return ResultEnvelope::Ok();
                })) wins.fetch_add(1);
        };
        std::thread t1(racer), t2(racer);
        go.store(true);
        t1.join(); t2.join();
        release.store(true);   // обидва вердикти вже є - дозволити worker-у завершитись
        eng.Join();
        if (wins.load() != 1) {
            CHECK(false, "JobEngineStartRace: рівно один Start повернув true");
            return;
        }
    }
    CHECK(true, "JobEngineStartRace: 100 ітерацій, рівно один переможець, без terminate");

    // Другий сценарій - БЕЗ попереднього Join: worker_ завершеного джоба лишається joinable,
    // тож без startMutex_ обидва потоки роблять join(worker_) того самого потоку (теж terminate).
    for (int iter = 0; iter < 100; ++iter) {
        JobEngine eng;
        eng.Start([]{ return ResultEnvelope::Ok(); });
        while (eng.State() == JobState::Running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        eng.ResetToIdle();               // Done -> Idle, worker_ НЕ приєднано

        std::atomic<int> wins{ 0 };
        std::atomic<bool> go{ false };
        // Той самий прийом, що в першому сценарії вище: блокуємо op переможця до вердикту
        // ОБОХ racer-ів, інакше миттєвий op дав би легітимний послідовний рестарт (Done -> Running)
        // і wins==2 без жодної реальної гонки-помилки.
        std::atomic<bool> release{ false };
        auto racer = [&]{
            while (!go.load()) std::this_thread::yield();
            if (eng.Start([&release]{
                    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    return ResultEnvelope::Ok();
                })) wins.fetch_add(1);
        };
        std::thread t1(racer), t2(racer);
        go.store(true);
        t1.join(); t2.join();
        release.store(true);
        eng.Join();
        if (wins.load() != 1) {
            CHECK(false, "JobEngineStartRace: без Join - рівно один Start повернув true");
            return;
        }
    }
    CHECK(true, "JobEngineStartRace: 100 ітерацій без попереднього Join, без подвійного join");
}

// Спільний стан емулятора для сценаріїв життєвого циклу: лічильники й керований статус.
struct EmuBase {
    std::atomic<int> pings{ 0 };
    std::atomic<int> purchases{ 0 };
    std::atomic<int> receipts{ 0 };
    std::atomic<int> statusCode{ 0 };   // що віддавати на getLastStatMsgCode
    std::atomic<bool> silentPing{ false };
    std::atomic<int> statusPolls{ 0 };   // скільки разів питали getLastStatMsgCode
};

static void BaseHandlers(TerminalEmulator& emu, EmuBase& st) {
    emu.OnRequest("PingDevice", [&st](const nlohmann::json&) -> std::string {
        st.pings.fetch_add(1);
        if (st.silentPing.load()) return "";      // монополія: термінал тримає стару сесію
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [&st](const nlohmann::json& q) -> std::string {
        const auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify")
            return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") {
            st.statusPolls.fetch_add(1);
            return std::string(R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":")")
                 + std::to_string(st.statusCode.load()) + R"("},"error":false})";
        }
        return "";
    });
    emu.OnRequest("GetReceiptInfo", [&st](const nlohmann::json&) {
        st.receipts.fetch_add(1);
        return R"({"method":"GetReceiptInfo","params":{"responseCode":"0000","invoiceNumber":"77","rrn":"555000111","amount":"100.51","txnType":"1"},"error":false})";
    });
}

// Дочекатися умови (мс) - щоб тести не спали фіксовано.
template <class F>
static bool WaitFor(F cond, int timeoutMs = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// Безпечний доступ до payload.outcome (рев'ю Task 5: діагностованість гейта).
// Прямий OutcomeObj(env).value(...) на конверті БЕЗ ключа "outcome" кидає
// nlohmann::json::type_error 306, і main() його не ловить: процес падає ДО того, як
// CHECK надрукує [FAIL] з іменем перевірки. Тобто регресія, що прибирає ключ, у гейті
// виглядає як краш із порожнім виводом замість читаного «яка саме перевірка не пройшла».
// Порожній об'єкт тут дає дефолтні значення у .value(), тож CHECK падає нормально.
static nlohmann::json OutcomeObj(const ResultEnvelope& env) {
    return (env.payload.is_object() && env.payload.contains("outcome") &&
            env.payload["outcome"].is_object())
               ? env.payload["outcome"] : nlohmann::json::object();
}

// Знімок долі як JSON-об'єкт outcome.
static nlohmann::json OutcomeOf(EcrPrivatJsonDriver& drv) {
    return OutcomeObj(drv.InquireLastOutcome());
}

// №15: після реконекту Ready дає лише протокольний Ping, а не відкритий сокет.
static void TestPingAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "PingReconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex; std::vector<std::string> states;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        states.push_back(j.value("state", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "PingReconnect: Connect");
    const int pingsAfterConnect = st.pings.load();

    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "PingReconnect: після обриву Подключен=Ложь");
    CHECK(WaitFor([&]{ return drv.IsReady(); }), "PingReconnect: після Ping зв'язок знову Ready");
    CHECK(st.pings.load() == pingsAfterConnect + 1,
          "PingReconnect: рівно один PingDevice на новому з'єднанні");

    drv.Disconnect();
    std::lock_guard<std::mutex> lk(evMutex);
    // ready(Connect) -> connecting(dropped) -> ready(Ping) -> disconnected(closed)
    CHECK(states.size() >= 4 && states[0] == "ready" && states[1] == "connecting" &&
          states[2] == "ready" && states.back() == "disconnected",
          "PingReconnect: послідовність подій connection правильна");
    emu.Stop();
}

// №16: silence монополії - Подключен=Ложь весь час, операції відбиваються 18, Ping повторюється.
static void TestSilenceAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) {
        st.purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "Silence: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Silence: Connect");

    st.silentPing.store(true);       // термінал ще вважає стару сесію живою
    const int pingsBefore = st.pings.load();
    const int purchasesBefore = st.purchases.load();
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "Silence: Ready знято");
    // ⚠️ Дефект брифу (виправлено за вказівкою координатора 2026-09-14): між обривом і
    // Purchase() тут стояло лише очікування !IsReady() (~20 мс). Але DeviceSession-супервізор
    // (ReconnectLoop, DeviceSession.cpp) робить БЕЗУМОВНИЙ backoff-сон
    // (cfg.reconnectDelayMs=1000 мс, DeviceSession.h:37) ПЕРЕД самою першою спробою Close->Open —
    // це не ретрай після невдачі, а затримка перед першою спробою. У момент виклику Purchase()
    // сокет фізично ще закритий (IsConnected()=false), тож ExecuteInternal віддав би
    // NOT_CONNECTED замість RECONNECTING - тест перевіряв би не той стан. Той самий механізм,
    // що вже врахований у TestThreeStatesThreeCodes (сценарій в) - тут застосовано той самий
    // рецепт: дочекатися реального перепідключення сокета, тоді перевіряти RECONNECTING.
    // Доказ стану - ПАРА предикатів (спека §6.5): один лише !IsReady() правдивий і в
    // Connecting, і в Disconnected - для Connecting потрібні ОБИДВА: IsConnected()==true
    // (сокет перепідключився) і !IsReady() (термінал мовчить, монополія).
    for (int i = 0; i < 1000 && !drv.IsConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(drv.IsConnected() && !drv.IsReady(),
          "Silence: сокет перепідключився, Ready лишається false (Connecting)");

    ResultEnvelope p = drv.Purchase("10.00");
    CHECK(!p.ok && p.code == "RECONNECTING", "Silence: операція під silence -> RECONNECTING");
    CHECK(st.purchases.load() == purchasesBefore, "Silence: Purchase на дріт не пішов");

    CHECK(WaitFor([&]{ return st.pings.load() >= pingsBefore + 2; }, 20000),
          "Silence: Ping повторюється з backoff (>=2 спроби)");
    st.silentPing.store(false);      // термінал відпустив стару сесію
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "Silence: після першої відповіді -> Ready");
    drv.Disconnect();
    emu.Stop();
}

// №17: deviceBusy на Ping = «живий, зайнятий нашою операцією» -> теж Ready.
static void TestPingBusyIsReady() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    // Перший Ping після реконекту отримує deviceBusy (термінал веде операцію).
    emu.OnRequest("PingDevice", [&st](const nlohmann::json&) -> std::string {
        const int n = st.pings.fetch_add(1);
        if (n >= 1) return R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"},"error":false})";
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    CHECK(emu.Start(), "PingBusy: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "PingBusy: Connect");
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "PingBusy: Ready знято після обриву");
    CHECK(WaitFor([&]{ return drv.IsReady(); }), "PingBusy: deviceBusy на Ping -> Ready (живий, зайнятий)");
    drv.Disconnect();
    emu.Stop();
}

// №23: Ready з мертвої епохи не записується - інакше прапорець завис би на мертвому сокеті.
// №23: Ready з мертвої епохи не записується - інакше прапорець завис би на мертвому сокеті.
//
// ⚠️ Предикат ОНОВЛЕНО (рев'ю Task 4, спека §4.9.1/§4.9.2, 2026-09-14). Предикат першої редакції
// «немає двох ready підряд» був істинний і З ДЕФЕКТОМ епохи, і без нього: із дефектом Ready на
// мертвому сокеті просто ЗАСТРЯГАЄ (жодного НОВОГО ready взагалі не буде, up=true побачить
// needReady==false і Ping на новому сокеті не зробить) - тому «двох підряд» і не траплялось.
// Предикат не розрізняв нічого (40 прогонів без жодного FAIL - задокументовано в task-4-report.md).
//
// Новий предикат архітектора: після обриву Ready настає ЛИШЕ ПІСЛЯ НОВОГО Ping НА НОВОМУ сокеті -
// емулятор бачить >= 2 PingDevice, і фінальний ready іде СТРОГО після другого. Перший Ping отримує
// відповідь і одразу вмирає (DropAfterNextResponse) - якщо Ready застряг би одразу після нього
// (дефект епохи), другого Ping ніколи не буде, і ready лишиться "після першого" назавжди.
//
// Це переплетення двох потоків - джоб (пише Ready) і хук (пише Connecting) - тест НЕ контролює:
// обидва порядки легальні. У порядку «джоб першим» обидва (старий і новий код) поводяться
// однаково (короткий ready перед connecting, епоха коректно росте на виході з Ready - це НІКОЛИ
// не було дефектом). Дефект проявляється ЛИШЕ в порядку «хук першим»: Connecting->Connecting
// без інкременту епохи (стара редакція) дає застряглий Ready. Тому негативна верифікація тут,
// як і зазначено в спеці, ІМОВІРНІСНА - детермінований варіант (№23-біс, шов
// SetBeforeReadyHookForTest) заплановано в Task 5.
static void TestLinkEpochRejectsStaleReady() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "LinkEpoch: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex; std::vector<std::string> states;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        states.push_back(j.value("state", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "LinkEpoch: Connect");
    const int pingsAfterConnect = st.pings.load();

    emu.DropAfterNextResponse();   // відповість на Ping реконекту й одразу зникне
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "LinkEpoch: Ready знято після першого обриву");
    // Другий обрив стався одразу після відповіді на Ping: Ready з тієї епохи має бути відкинутий,
    // а зв'язок відновитись лише наступним Ping - на живому сокеті.
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "LinkEpoch: зрештою Ready на живому сокеті");
    // Новий предикат: фінальний ready настав СТРОГО після ДРУГОГО PingDevice (перший на живому
    // з'єднанні відповів і одразу зник разом із DropAfterNextResponse), не одразу після першого.
    // Якщо Ready застряг на мертвому сокеті одразу після першого - саме той дефект, що ловить ця
    // перевірка (гонка «хук пише Connecting раніше за джоб Ready» - не в кожному прогоні).
    CHECK(st.pings.load() >= pingsAfterConnect + 2,
          "LinkEpoch: фінальний ready настав після ДРУГОГО PingDevice, не першого (мертвий сокет)");

    // Замінює старий CHECK «немає двох ready підряд» (рев'ю Task 4, фікс-раунд 2): той нічого
    // не доводив і за новим предикатом (сам архітектор упав на ньому в гейті - task-4-report.md,
    // фікс-раунд 1). Новий інваріант - те, що ГАРАНТУЄ серіалізація linkEmitMutex_ (спека §4.9.3,
    // фікс-раунд 2): порядок подій connection = порядок переходів, тож стан ОСТАННЬОЇ отриманої
    // події відповідає ПОТОЧНОМУ стану - інверсія журналу (ready останнім при фактичному
    // Connecting) неможлива. Негативна верифікація (без linkEmitMutex_) - імовірнісна, як і
    // раніше: task-4-report.md фіксує спостережену частоту.
    {
        std::lock_guard<std::mutex> lk(evMutex);
        CHECK(!states.empty() && ((states.back() == "ready") == drv.IsReady()),
              "LinkEpoch: стан останньої події connection відповідає IsReady() (без інверсії журналу)");
    }

    drv.Disconnect();
    emu.Stop();
}

// №23-біс: епоха зв'язку, ДЕТЕРМІНОВАНО (рев'ю Task 4, I1; рекомендація архітектора - шов
// SetBeforeReadyHookForTest, спека design.md рядок 1027).
//
// №23 ловить дефект I1 лише в переплетенні потоків «хук пише Connecting РАНІШЕ за джоб Ready» -
// це один із ДВОХ легальних порядків, тест його не контролює (звідси 3 FAIL із 15 у негативній
// верифікації I1, а не 15 із 15 - див. task-4-report.md). Тут переплетення ПРИМУШУЄМО хуком:
// SetBeforeReadyHookForTest кличеться в EnsureReady МІЖ отриманням Response на Ping і
// SetLinkState(Ready, "", epoch) - точнісінько у вікні, де стара редакція коду відкидала б
// інкремент епохи (стан уже Connecting, друга транзиція Connecting->Connecting).
static void TestLinkEpochStaleReadyDeterministic() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "LinkEpochBis: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "LinkEpochBis: Connect");

    // Перший обрив: Ready -> Connecting. Ця транзиція НІКОЛИ не була дефектною (епоха коректно
    // росте на виході з Ready і в старій, і в новій редакції) - вона лише готує стан Connecting,
    // з якого стартує дефектна ДРУГА транзиція нижче.
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "LinkEpochBis: Ready знято після першого обриву");

    // Хук: точно в момент, коли відновлювальний Ping ЩОЙНО отримав відповідь (стан УСЕ ЩЕ
    // Connecting - SetLinkState(Ready, epoch) іще не викликано), тест сам рве з'єднання ЗНОВУ.
    // Це друга, дефектна транзиція I1: Connecting -> Connecting (стан не змінюється, подія не
    // емітується - тому спостерігати її напряму через events нічим, лише через побічний ефект
    // на epoch).
    //
    // ⚠️ Чекаємо ЗМІНИ drv.LinkEpoch() - ТОГО САМОГО значення, яке guard у SetLinkState
    // звіряє - а НЕ !drv.IsConnected() (рев'ю Task 4, фікс-раунд 2: щілина в детермінізмі).
    // IsConnected() відбиває СИНХРОННИЙ стан DeviceSession (reader-потік), тоді як виклик
    // нашого хука стану (а отже й ++linkEpoch_ у SetLinkState(Connecting)) лише СТАВИТЬСЯ
    // в чергу й виконується АСИНХРОННО на dispatcher-потоці. Запас у 3-4 порядки практично
    // робив стару синхронізацію надійною, але формальної гарантії не давав - синхронізація
    // МАЄ бути на тому самому значенні, яке перевіряє код під тестом.
    const std::uint64_t epochBeforeSecondDrop = drv.LinkEpoch();
    std::atomic<bool> hookFired{ false };
    drv.SetBeforeReadyHookForTest([&]{
        hookFired.store(true);
        emu.DropConnection();
        for (int i = 0; i < 250 && drv.LinkEpoch() == epochBeforeSecondDrop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    CHECK(WaitFor([&]{ return hookFired.load(); }, 20000), "LinkEpochBis: хук спрацював (Ping отримав відповідь)");
    // Хук (виконаний синхронно на потоці джоба) уже дочекався зміни епохи ВСЕРЕДИНІ себе -
    // до цього моменту (виходу з EnsureReady, що йде одразу після виклику хука) вона гарантовано
    // інша, без жодного вікна гонки.
    CHECK(drv.LinkEpoch() != epochBeforeSecondDrop,
          "LinkEpochBis: епоха з'єднання просунулась (друга транзиція зареєстрована)");
    // Ключова перевірка I1: одразу після виходу з ЦЬОГО циклу EnsureReady, Ready НЕ повинен
    // бути записаний зі старою (знятою ДО хука) епохою - guard має її відкинути. Це і є
    // дефект I1, зловлений детерміновано: без фіксу епоха не зрушила б, і Ready записався б
    // на вже мертвому сокеті.
    CHECK(!drv.IsReady(), "LinkEpochBis: Ready НЕ записано зі старою епохою (дефект I1 відсутній)");

    // Зрештою (після реального реконекту й НОВОГО Ping на НОВОМУ сокеті) зв'язок відновлюється.
    drv.SetBeforeReadyHookForTest(nullptr);   // прибрати хук - далі звичайний потік без утручань
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "LinkEpochBis: зрештою Ready на живому сокеті");

    drv.Disconnect();
    emu.Stop();
}

// №6: SendFailed при ЖИВОМУ з'єднанні - з'ясування одразу, факти в тому ж виклику.
// Шов ламає рівно відправку Purchase (кодом, що НЕ закриває сокет), тож реконекту не буде
// й хук стану ніколи б не спрацював - саме цей шлях і перевіряємо.
static void TestSendFailedResolvesInline() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) {
        st.purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "SendFailed: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<bool> breakPurchase{ false };
    drv.SetTransportFactoryForTest([&breakPurchase](const EcrConnParams& p) -> std::unique_ptr<ITransport> {
        auto t = std::make_unique<TransportTCP>(p.host, p.tcpPort);
        t->SetSendFunctionForTest([&breakPurchase](SOCKET s, const char* buf, int len) -> int {
            const std::string data(buf, static_cast<std::size_t>(len));
            if (breakPurchase.load() && data.find("\"Purchase\"") != std::string::npos) {
                // WSAENOBUFS, а НЕ CONNRESET/ABORTED: Send має провалитись БЕЗ закриття сокета.
                WSASetLastError(WSAENOBUFS);
                return -1;
            }
            return ::send(s, buf, len, 0);
        });
        return t;
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SendFailed: Connect");

    breakPurchase.store(true);
    ResultEnvelope env = drv.Purchase("100.51");
    breakPurchase.store(false);

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "SendFailed: операція -> код 17");
    const auto oc = OutcomeObj(env);
    CHECK(oc.value("reason", std::string{}) == "SEND_FAILED", "SendFailed: reason=SEND_FAILED");
    CHECK(oc.value("state", std::string{}) == "resolved", "SendFailed: знімок з'явився в тому ж виклику");
    CHECK(oc.value("channelConnected", false) == true, "SendFailed: channelConnected=true (сокет живий)");
    CHECK(oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "555000111",
          "SendFailed: факти чека у знімку");
    CHECK(st.purchases.load() == 0, "SendFailed: Purchase на термінал не дійшов");
    drv.Disconnect();
    emu.Stop();
}

// №7: термінал не звільнився до ліміту - чесний TERMINAL_BUSY, канал усе одно відкрито.
static void TestTerminalBusyUntilLimit() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                     // «виконується» - і так завжди
    emu.OnRequest("Purchase", [](const nlohmann::json&) -> std::string { return ""; });   // мовчить -> Timeout
    emu.OnRequest("Audit", [](const nlohmann::json&) {
        return R"({"method":"Audit","params":{"responseCode":"0000","receipt":"X"},"error":false})";
    });
    CHECK(emu.Start(), "TerminalBusy: емулятор стартував");

    EcrPrivatJsonDriver drv;
    drv.SetOutcomeTimingForTest(/*idleWaitMs=*/2000, /*syncWaitMs=*/8000);
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "TerminalBusy: Connect");

    ResultEnvelope env = drv.Execute("Purchase",
        nlohmann::json{{"amount","100.51"},{"discount",""},{"merchantId","0"},{"facepay","false"}}, 1500);
    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "TerminalBusy: операція -> код 17");
    CHECK(OutcomeObj(env).value("reason", std::string{}) == "TIMEOUT", "TerminalBusy: reason=TIMEOUT");

    // Знімок читаємо ОКРЕМО, а не з env: Timeout ставить desync, тож за повним критерієм §4.2
    // синхронного очікування немає - 17 повертається негайно, а з'ясування йде після реконекту
    // (крок 0 Ping -> крок 1 полінг). Ліміт спокою скорочено швом до 2 с.
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }, 30000),
          "TerminalBusy: знімок зроблено після реконекту");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("terminalIdle", true) == false, "TerminalBusy: terminalIdle=false");
    CHECK(oc.value("factsCode", std::string{}) == "TERMINAL_BUSY", "TerminalBusy: factsCode=TERMINAL_BUSY");
    CHECK(oc["facts"].is_object() && oc["facts"].empty(), "TerminalBusy: фактів чека немає");
    CHECK(st.receipts.load() == 0, "TerminalBusy: GetReceiptInfo не питали (термінал зайнятий)");

    // Канал відкрито попри те, що спокою не дочекались: інакше desync не зняв би ніхто.
    ResultEnvelope a = drv.Audit("0");
    CHECK(a.code != "DESYNC", "TerminalBusy: після ліміту сесія не лишилась у desync");
    drv.Disconnect();
    emu.Stop();
}

// №13: самозцілення - намір є, джоба немає, зв'язок живий; InquireLastOutcome запускає роботу.
static void TestSelfHealingFromInquire() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "SelfHeal: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SelfHeal: Connect");

    // Вікно спеки §4.4: MarkPending без старту джоба (шов) - Pending, з якого нікому вийти.
    drv.MarkPendingForTest("Purchase", "100.51", "TIMEOUT");
    CHECK(OutcomeOf(drv).value("state", std::string{}) != "none", "SelfHeal: намір зафіксовано");

    ResultEnvelope snap = drv.InquireLastOutcome();   // має САМ стартувати відновлення
    CHECK(!snap.ok && snap.code == "UNKNOWN_OUTCOME", "SelfHeal: InquireLastOutcome -> конверт 17");
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "SelfHeal: джоб стартував сам і довів справу до resolved");
    CHECK(st.receipts.load() >= 1, "SelfHeal: чек справді запитано");
    drv.Disconnect();
    emu.Stop();
}

// №25: Отключить під backoff-сном джоба. Без SleepInterruptible Disconnect висів би на
// recoveryJob_.Join() до кінця інтервалу (до 15 с) - саме в центральному сценарії монополії.
//
// ⚠️ Тест мусить зловити джоб САМЕ У СНІ. Якщо кликати Отключить одразу після першого Ping,
// джоб стоїть у DeviceSession::DoRequest (cv_.wait_for на відповідь), і Stop() завершує його
// миттєво незалежно від SleepInterruptible - тест зеленів би й без фіксу. Тому Ping-таймаут
// скорочено швом, і Отключить кличеться, коли ДРУГИЙ Ping уже таймаутнув.
static void TestDisconnectInterruptsBackoff() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "DisconnectBackoff: емулятор стартував");

    constexpr int kPingMs = 200;
    EcrPrivatJsonDriver drv;
    drv.SetOutcomeTimingForTest(/*idleWaitMs=*/2000, /*syncWaitMs=*/2000, /*pingTimeoutMs=*/kPingMs);
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "DisconnectBackoff: Connect");

    st.silentPing.store(true);                       // термінал мовчить -> кожен Ping таймаутить
    const int pingsBefore = st.pings.load();
    emu.DropConnection();
    // pings >= 2: перший Ping таймаутнув (200 мс), джоб проспав backoff, відправив другий.
    // ⚠️ Це НЕ друга ітерація ОДНОГО EnsureReady з backoff, подвоєним до 2000 мс, як
    // припускав план. Кожен Timeout Ping усередині EnsureReady сам по собі вмикає примусовий
    // реконект DeviceSession-супервізора (DoRequest виставляє reconnectRequested_=true на
    // БУДЬ-ЯКОМУ primary-таймауті, включно з нашим власним Ping - DeviceSession.cpp:220-222).
    // Цей реконект рве з'єднання ПОСЕРЕД сну першого виклику: умова циклу IsConnected()
    // провалюється, виклик виходить (EXIT loop-cond false), а наступний up=true від хука
    // стартує СВІЖИЙ EnsureReady із backoff, знову скинутим на kReconnectDelayMs=1000 мс.
    // Тому й другий Ping - це перша ітерація ДРУГОГО виклику, а не друга ітерація першого.
    CHECK(WaitFor([&]{ return st.pings.load() >= pingsBefore + 2; }, 20000),
          "DisconnectBackoff: джоб зробив другий Ping (перший цикл backoff пройдено)");
    // Даємо другому Ping таймаутнути - після цього джоб (свіжий виклик EnsureReady, backoff=1000)
    // щойно увійшов у сон і перебуватиме в ньому ще ~1000 мс, коли ми покличемо Отключить нижче.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPingMs + 50));

    const auto t0 = std::chrono::steady_clock::now();
    drv.Disconnect();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    // Поріг 300 мс (не 1000, як у плані): Отключить кличеться приблизно на 60-й мс свіжого
    // 1000-мілісекундного сну (див. коментар вище), тож БЕЗ SleepInterruptible лишається
    // чекати ще ~940 мс - це МЕНШЕ за 1000, і старий поріг тому не ловив дефект узагалі
    // (тест зеленів в обох станах). Виміряно емпірично (2026-09-14): з SleepInterruptible
    // elapsed~=38 мс, без нього (голий sleep_for) elapsed~=940 мс - восьмикратний запас
    // з обох боків порогу 300 мс.
    CHECK(elapsed < 300, "DisconnectBackoff: Отключить повернувся < 300 мс, не чекав backoff");
    emu.Stop();
}

// №2 + №3: обрив у польоті -> 17 негайно (pending, facts=null); після реконекту фоновий
// джоб дочікується спокою й робить знімок -> resolved, рівно одна подія outcome.
static void TestOutcomeAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                       // термінал іще веде операцію
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();                      // прийняв оплату й зник
        return "";
    });
    CHECK(emu.Start(), "OutcomeReconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<int> outcomeEvents{ 0 };
    drv.SetEventHandler([&](const std::string& ev, const std::string&) {
        if (ev == "outcome") outcomeEvents.fetch_add(1);
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "OutcomeReconnect: Connect");

    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "OutcomeReconnect(№2): 17 повернуто до реконекту");
    {
        const auto oc = OutcomeObj(env);
        CHECK(oc.value("state", std::string{}) == "pending", "OutcomeReconnect(№2): state=pending");
        CHECK(oc["facts"].is_null(), "OutcomeReconnect(№2): facts=null, поки доля невідома");
        CHECK(oc.value("reason", std::string{}) == "DISCONNECTED", "OutcomeReconnect(№2): reason=DISCONNECTED");
        CHECK(oc.value("channelConnected", true) == false, "OutcomeReconnect(№2): channelConnected=false");
    }

    // Реконект -> Ping -> Ready -> полінг статусу. Відпускаємо термінал після кількох полінгів.
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "OutcomeReconnect(№3): зв'язок відновлено Ping-ом");
    const int pollsAtReady = st.statusPolls.load();
    CHECK(WaitFor([&]{ return st.statusPolls.load() >= pollsAtReady + 2; }),
          "OutcomeReconnect(№3): джоб полить статус");
    st.statusCode.store(0);                        // термінал звільнився

    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "OutcomeReconnect(№3): знімок зроблено (resolved)");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("terminalIdle", false) == true, "OutcomeReconnect(№3): terminalIdle=true");
    CHECK(oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "555000111",
          "OutcomeReconnect(№3): факти чека у знімку");
    CHECK(oc.value("generation", 0ull) == 1ull, "OutcomeReconnect(№3): generation=1 (перше питання)");
    CHECK(outcomeEvents.load() == 1, "OutcomeReconnect(№3): подія outcome рівно одна");
    drv.Disconnect();
    emu.Stop();
}

// №8: Отключить посеред Pending з активним джобом - без падінь і зависань; знімок переживає
// перепідключення (Connect НЕ скидає lastOutcome_: факти належать операції, не сесії).
static void TestDisconnectDuringPending() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                       // джоб застрягне в полінгу
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "DisconnectPending: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "DisconnectPending: Connect");
    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(env.code == "UNKNOWN_OUTCOME", "DisconnectPending: 17 отримано");
    CHECK(WaitFor([&]{ return st.statusPolls.load() > 0 && drv.IsReady(); }, 20000),
          "DisconnectPending: джоб працює (Ready + полінг)");

    const auto beforeGen = OutcomeOf(drv).value("generation", 0ull);
    drv.Disconnect();                              // посеред роботи джоба
    CHECK(!drv.IsReady(), "DisconnectPending: після Отключить зв'язку немає");

    CHECK(drv.Connect(conn), "DisconnectPending: повторний Connect");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("state", std::string{}) != "none" && oc.value("generation", 0ull) == beforeGen,
          "DisconnectPending: lastOutcome_ переживає Отключить/Подключить");
    drv.Disconnect();
    emu.Stop();
}

// №9: повторний обрив УЖЕ під час з'ясування. Перший джоб виходить ABORTED саме через
// Disconnected від PollStatusOnce (покоління не мінялось, closing_ не ставився); після
// другого реконекту хук бачить Pending -> джоб знову з кроку 0. Знімок один.
static void TestSecondDropDuringCapture() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "SecondDrop: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<int> outcomeEvents{ 0 };
    drv.SetEventHandler([&](const std::string& ev, const std::string&) {
        if (ev == "outcome") outcomeEvents.fetch_add(1);
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SecondDrop: Connect");
    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(env.code == "UNKNOWN_OUTCOME", "SecondDrop: 17 отримано (gen=1)");

    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "SecondDrop: перший реконект, Ready");
    const int polls = st.statusPolls.load();
    CHECK(WaitFor([&]{ return st.statusPolls.load() > polls; }), "SecondDrop: джоб полить статус");
    emu.DropConnection();                          // другий обрив ПОСЕРЕД з'ясування
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "SecondDrop: Ready знято вдруге");
    CHECK(OutcomeOf(drv).value("state", std::string{}) == "pending",
          "SecondDrop: намір лишився Pending (перший джоб вийшов без запису)");

    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "SecondDrop: другий реконект, Ready");
    st.statusCode.store(0);
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "SecondDrop: знімок зроблено після другого реконекту");
    CHECK(OutcomeOf(drv).value("generation", 0ull) == 1ull, "SecondDrop: покоління те саме (1)");
    CHECK(outcomeEvents.load() == 1, "SecondDrop: подія outcome одна, не дві");
    drv.Disconnect();
    emu.Stop();
}

// №14: Stopped як тригер. Сесію зупиняє інший потік під час синхронної операції -
// FinishPendingLocked завершує запит статусом Stopped, а термінал МІГ устигнути.
static void TestStoppedIsTrigger() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::seconds(5));    // «касир вводить пін»
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"5"},"error":false})";
    });
    CHECK(emu.Start(), "Stopped: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "Stopped: Connect");

    std::thread stopper([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        drv.StopSessionForTest();
    });
    ResultEnvelope env = drv.Purchase("100.51");
    stopper.join();

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "Stopped: операція -> код 17");
    CHECK(OutcomeObj(env).value("reason", std::string{}) == "STOPPED", "Stopped: reason=STOPPED");
    CHECK(OutcomeObj(env).value("state", std::string{}) == "pending", "Stopped: state=pending");

    // Ручний реконект із 1С (спека §4.4в): Connect бачить Pending і запускає з'ясування сам.
    drv.Disconnect();
    CHECK(drv.Connect(conn), "Stopped: повторний Connect");
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "Stopped: після Connect джоб зробив знімок (§4.4в)");
    drv.Disconnect();
    emu.Stop();
}

static void TestDriverPurchaseHappy() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&){
        return R"({"method":"Purchase","step":0,"params":{"responseCode":"0000","invoiceNumber":"42","rrn":"123"},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("Refund", [](const nlohmann::json&){
        return R"({"method":"Refund","step":0,"params":{"responseCode":"1002"},"error":true,"errorDescription":"EMV Decline"})";
    });
    CHECK(emu.Start(), "DriverPurchase: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "DriverPurchase: Connect");

    auto pr = drv.Purchase("100.00");
    CHECK(pr.ok && pr.code == "0000" && pr.payload["invoiceNumber"] == "42",
          "Purchase happy → ok, code 0000, invoiceNumber");

    auto rf = drv.Refund("50.00", "123");
    CHECK(!rf.ok && rf.code == "1002" && rf.description == "EMV Decline",
          "Refund declined → !ok, code 1002, description");

    drv.Disconnect();
    emu.Stop();
}

// ---- Реальний термінал строгий до складу params (інцидент 2026-08-29, Newland N950) ----
// Перша оплата на реальному N950 → {"code":"1000","description":"Введіть discount"}:
// драйвер слав лише amount. Спека §5.1.1: запит Purchase містить amount+discount+
// merchantId+facepay; §5.2.1: Refund — amount+discount+merchantId+rrn. subMerchant
// дозволено слати ЛИШЕ після реєстрації субмерчанта в банку — за замовчуванням
// поля НЕ має бути. Емулятор тут валідує запит так само строго, як термінал.
static void TestDriverStrictTerminalParams() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"NEWLAND","model":"N950"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    // Строга перевірка як у прошивці N950: бракує поля → responseCode 1000.
    auto strict = [](const nlohmann::json& q, const char* method, bool needFacepay, bool needRrn) -> std::string {
        auto p = q.value("params", nlohmann::json::object());
        auto reject = [&](const std::string& what) {
            return std::string(R"({"method":")") + method +
                   R"(","step":0,"params":{"responseCode":"1000"},"error":true,"errorDescription":"Введіть )" + what + R"("})";
        };
        if (!p.contains("amount"))      return reject("amount");
        if (!p.contains("discount"))    return reject("discount");
        if (!p.contains("merchantId"))  return reject("merchantId");
        if (needFacepay && !p.contains("facepay")) return reject("facepay");
        if (needRrn && !p.contains("rrn"))         return reject("rrn");
        if (p.contains("subMerchant"))  return reject("subMerchant НЕ дозволено без реєстрації");
        return std::string(R"({"method":")") + method +
               R"(","step":0,"params":{"responseCode":"0000","invoiceNumber":"42"},"error":false,"errorDescription":""})";
    };
    emu.OnRequest("Purchase", [&](const nlohmann::json& q){ return strict(q, "Purchase", /*facepay*/true,  /*rrn*/false); });
    emu.OnRequest("Refund",   [&](const nlohmann::json& q){ return strict(q, "Refund",   /*facepay*/false, /*rrn*/true);  });
    CHECK(emu.Start(), "StrictParams: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "StrictParams: Connect");

    auto pr = drv.Purchase("1.00");
    CHECK(pr.ok && pr.code == "0000",
          "Purchase проти строгого термінала → ok (дефолти discount/merchantId/facepay)");

    auto rf = drv.Refund("1.00", "123456");
    CHECK(rf.ok && rf.code == "0000",
          "Refund проти строгого термінала → ok (дефолти discount/merchantId)");

    drv.Disconnect();
    emu.Stop();
}

// ---- Звіти для звірки з обліковою системою: Audit (X-баланс) і Verify (Звірка/Z) ----
// Спека §5.17/§5.18: обидва запити — {merchantId}; відповідь — {receipt, responseCode}.
static void TestDriverReports() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"NEWLAND","model":"N950"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    // Ехо merchantId у відповідь — щоб перевірити і дефолт "0", і явне значення.
    auto report = [](const nlohmann::json& q, const char* method) -> std::string {
        auto p = q.value("params", nlohmann::json::object());
        if (!p.contains("merchantId"))
            return std::string(R"({"method":")") + method + R"(","step":0,"params":{"responseCode":"1000"},"error":true,"errorDescription":"Введіть merchantId"})";
        nlohmann::json r = {
            {"method", method}, {"step", 0},
            {"params", {{"receipt", std::string("[ ") + method + " OK ]"},
                        {"merchantId", p["merchantId"]},
                        {"responseCode", "0000"}}},
            {"error", false}, {"errorDescription", ""}
        };
        return r.dump();
    };
    emu.OnRequest("Audit",  [&](const nlohmann::json& q){ return report(q, "Audit");  });
    emu.OnRequest("Verify", [&](const nlohmann::json& q){ return report(q, "Verify"); });
    CHECK(emu.Start(), "Reports: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Reports: Connect");

    auto x = drv.Audit();
    CHECK(x.ok && x.code == "0000" && x.payload["merchantId"] == "0"
              && x.payload["receipt"] == "[ Audit OK ]",
          "Audit (X-звіт) → ok, merchantId дефолт 0, receipt");

    auto x2 = drv.Audit("2");
    CHECK(x2.ok && x2.payload["merchantId"] == "2", "Audit з явним merchantId");

    auto v = drv.Verify();
    CHECK(v.ok && v.code == "0000" && v.payload["merchantId"] == "0"
              && v.payload["receipt"] == "[ Verify OK ]",
          "Verify (Звірка) → ok, merchantId дефолт 0, receipt");

    drv.Disconnect();
    emu.Stop();
}

static void TestDriverStatusPoll() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"3"},"error":false})";
        return "";
    });
    // Purchase відповідає не одразу — даємо poller-у шанс завершити хоча б один цикл (>kPollIntervalMs).
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"7"},"error":false})";
    });
    CHECK(emu.Start(), "StatusPoll: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "StatusPoll: Connect");
    auto pr = drv.Purchase("10.00");
    CHECK(pr.ok && pr.code == "0000", "StatusPoll: Purchase завершився ok");
    CHECK(drv.LastStatus() == 3, "StatusPoll: poller зафіксував LastStatMsgCode=3 під час операції");
    drv.Disconnect();
    emu.Stop();
}

static void TestDriverInterrupt() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    std::atomic<bool> interruptSeen{false};
    emu.OnRequest("ServiceMessage", [&](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"6"},"error":false})";
        if (mt == "interrupt") { interruptSeen.store(true); return R"({"method":"ServiceMessage","params":{"msgType":"interruptTransmitted"},"error":false})"; }
        return "";
    });
    // Purchase «висить», доки не прийде interrupt: емулятор чекає прапорець, тоді віддає 1001.
    emu.OnRequest("Purchase", [&](const nlohmann::json&)->std::string{
        for (int i = 0; i < 100 && !interruptSeen.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return R"({"method":"Purchase","params":{"responseCode":"1001"},"error":true,"errorDescription":"Oперація скасов."})";
    });
    CHECK(emu.Start(), "Interrupt: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Interrupt: Connect");

    std::thread canceller([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(300)); drv.RequestInterrupt(); });
    auto pr = drv.Purchase("10.00");
    canceller.join();
    CHECK(interruptSeen.load(), "Interrupt: термінал отримав interrupt");
    CHECK(!pr.ok && pr.code == "1001", "Interrupt: Purchase завершився responseCode 1001 (скасовано)");
    drv.Disconnect();
    emu.Stop();
}

static void TestDriverAsync() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"9"},"error":false})";
    });
    CHECK(emu.Start(), "Async: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Async: Connect");
    CHECK(drv.StartPurchase("25.00"), "Async: StartPurchase → true (запущено)");
    CHECK(drv.StartPurchase("25.00") == false, "Async: повторний StartPurchase під час виконання → false");

    ResultEnvelope out;
    for (int i = 0; i < 200 && drv.OperationState() == JobState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv.OperationState() == JobState::Done, "Async: операція завершилась (Done)");
    CHECK(drv.TryGetOperationResult(out) && out.ok && out.payload["invoiceNumber"] == "9",
          "Async: результат доступний (ok, invoiceNumber=9)");
    drv.Disconnect();
    emu.Stop();
}

// Скасування асинхронної операції ОДРАЗУ після StartPurchase: перевіряє, що cancel,
// виставлений негайно після Start, НЕ губиться (fix E — ExecuteInternal не ре-ресетить
// interruptRequested_) і НЕ зависає (fix A — RequestCancel гардовано).
static void TestDriverAsyncCancel() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    std::atomic<bool> interruptSeen{false};
    emu.OnRequest("ServiceMessage", [&](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"6"},"error":false})";
        if (mt == "interrupt") { interruptSeen.store(true); return R"({"method":"ServiceMessage","params":{"msgType":"interruptTransmitted"},"error":false})"; }
        return "";
    });
    emu.OnRequest("Purchase", [&](const nlohmann::json&)->std::string{
        for (int i = 0; i < 100 && !interruptSeen.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return R"({"method":"Purchase","params":{"responseCode":"1001"},"error":true,"errorDescription":"Oперація скасов."})";
    });
    CHECK(emu.Start(), "AsyncCancel: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "AsyncCancel: Connect");
    CHECK(drv.StartPurchase("10.00"), "AsyncCancel: StartPurchase → true");
    drv.CancelOperation();   // ОДРАЗУ після Start — cancel не має загубитись (fix E)

    // Після CancelOperation стан — Interrupting (гардований перехід із Running); чекаємо,
    // доки worker не завершиться (Done/Error), тому умова — «поки Running АБО Interrupting».
    for (int i = 0; i < 300 && (drv.OperationState() == JobState::Running
                             || drv.OperationState() == JobState::Interrupting); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(interruptSeen.load(), "AsyncCancel: термінал отримав interrupt (cancel не загубився)");
    CHECK(drv.OperationState() == JobState::Done, "AsyncCancel: операція завершилась (Done)");
    ResultEnvelope out;
    CHECK(drv.TryGetOperationResult(out) && out.code == "1001", "AsyncCancel: результат code=1001 (скасовано)");
    drv.Disconnect();
    emu.Stop();
}

// CancelOperation з Idle (без активної операції) і після Done НЕ повинен «заклинити» драйвер:
// наступний StartPurchase має стартувати (fix A — RequestCancel гардовано, не тягне у Interrupting).
static void TestDriverCancelNoWedge() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&){ return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})"; });
    CHECK(emu.Start(), "NoWedge: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "NoWedge: Connect");

    drv.CancelOperation();   // з Idle — без активної операції
    CHECK(drv.StartPurchase("5.00"), "NoWedge: StartPurchase після Cancel(Idle) → true (не завис)");
    for (int i = 0; i < 200 && drv.OperationState() == JobState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv.OperationState() == JobState::Done, "NoWedge: перша операція завершилась (Done)");

    drv.CancelOperation();   // після Done
    CHECK(drv.StartPurchase("5.00"), "NoWedge: StartPurchase після Cancel(Done) → true (не завис)");
    for (int i = 0; i < 200 && drv.OperationState() == JobState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv.OperationState() == JobState::Done, "NoWedge: друга операція завершилась (Done)");
    drv.Disconnect();
    emu.Stop();
}

// Дефект 1.2 спеки: після desync драйвер віддавав результат ЧУЖОГО чека як результат
// операції. Емулятор: Purchase мовчить (-> Timeout+desync), GetReceiptInfo віддає завідомо
// іншу суму/RRN. Каса має отримати код 17, а не «успішну» чужу транзакцію.
static void TestForeignReceiptNotCredited() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    // Purchase не відповідає ніколи -> primary Timeout при живому сокеті -> desync.
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{ return ""; });
    // Чужий чек: інша сума, інший RRN - саме він раніше видавався за наш результат.
    emu.OnRequest("GetReceiptInfo", [](const nlohmann::json&){
        return R"({"method":"GetReceiptInfo","params":{"responseCode":"0000","invoiceNumber":"11","rrn":"999000111","amount":"777.77"},"error":false})";
    });
    CHECK(emu.Start(), "ForeignReceipt: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "ForeignReceipt: Connect");

    // Рахуємо події: рівно ОДНА подія result на операцію, і в ній немає чужого payload.
    std::atomic<int> resultEvents{ 0 };
    std::string lastResultData;
    std::mutex evMutex;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "result") return;
        resultEvents.fetch_add(1);
        std::lock_guard<std::mutex> lk(evMutex);
        lastResultData = data;
    });

    ResultEnvelope env = drv.Execute("Purchase",
        nlohmann::json{{"amount","100.51"},{"discount",""},{"merchantId","0"},{"facepay","false"}}, 2000);

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "ForeignReceipt: операція -> ok=false, code=UNKNOWN_OUTCOME");
    CHECK(env.payload.contains("outcome") && env.payload["outcome"].is_object(),
          "ForeignReceipt: payload.outcome присутній");
    const auto& oc = env.payload["outcome"];
    // reason за таблицею 4.2: статус Timeout має пріоритет над IsDesynchronized(), тож "TIMEOUT".
    // Значення навмисно збігається з тим, яке дасть повна таблиця в Task 2 — тест не переписується.
    const std::string state = oc.value("state", std::string{});
    CHECK((state == "resolved" || state == "pending") && oc.value("reason", std::string{}) == "TIMEOUT",
          "ForeignReceipt: outcome.state заповнено, outcome.reason=TIMEOUT");
    // Гонка з супервізором (див. шапку завдання): факти - АБО чужий чек (запит виграв гонку),
    // АБО невдача запиту. Обидва виходи легітимні; тест перевіряє не їх, а відсутність підміни.
    const bool gotForeign = oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "999000111";
    const bool gotFailure = oc.value("factsOk", true) == false;
    CHECK(gotForeign || gotFailure,
          "ForeignReceipt: outcome.facts описує САМЕ запит чека (чужий чек або чесна невдача)");
    if (gotForeign)
        CHECK(oc.value("factsOk", false) == true && oc.value("factsCode", std::string{}) == "0000",
              "ForeignReceipt: чужий чек прийшов - factsOk/factsCode це відображають");
    // Головне: жодне поле чужого чека НЕ на топ-рівні payload - там раніше стояли rrn/amount.
    CHECK(!env.payload.contains("rrn") && !env.payload.contains("amount") && !env.payload.contains("invoiceNumber"),
          "ForeignReceipt: поля чужого чека НЕ на топ-рівні payload операції");
    CHECK(resultEvents.load() == 1, "ForeignReceipt: рівно одна подія result на операцію");
    {
        std::lock_guard<std::mutex> lk(evMutex);
        auto j = nlohmann::json::parse(lastResultData, nullptr, false);
        CHECK(!j.is_discarded() && j.value("code", std::string{}) == "UNKNOWN_OUTCOME",
              "ForeignReceipt: подія result несе код 17, не чужий чек");
    }
    drv.Disconnect();
    emu.Stop();
}

// №10: нефінансовий метод при збої НЕ створює наміру - каса нічого не з'ясовує.
static void TestNonFinancialNotTracked() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Audit", [](const nlohmann::json&)->std::string{ return ""; });   // мовчить -> Timeout
    CHECK(emu.Start(), "NonFinancial: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "NonFinancial: Connect");

    ResultEnvelope env = drv.Execute("Audit", nlohmann::json{{"merchantId","0"}}, 1500);
    CHECK(!env.ok && env.code == "TIMEOUT", "NonFinancial: Audit при мовчанні -> TIMEOUT, не 17");

    ResultEnvelope oc = drv.InquireLastOutcome();
    CHECK(oc.ok && oc.code == "OK" && OutcomeObj(oc).value("state", std::string{}) == "none",
          "NonFinancial: намір не створено (state=none)");
    drv.Disconnect();
    emu.Stop();
}

// №11: ИсходПоследнейОперацииJSON без жодної операції - не помилка, а «нема про що питати».
static void TestInquireNone() {
    EcrPrivatJsonDriver drv;                       // навіть без Connect
    ResultEnvelope env = drv.InquireLastOutcome();
    CHECK(env.ok && env.code == "OK", "InquireNone: ok=true, code=OK");
    CHECK(env.payload.contains("outcome") &&
          OutcomeObj(env).value("state", std::string{}) == "none",
          "InquireNone: outcome.state=none");
    CHECK(OutcomeObj(env).value("channelConnected", true) == false,
          "InquireNone: channelConnected=false без сесії");
}

// №20: ручний Отключить під час Connecting -> подія disconnected(closed), не dropped.
static void TestLinkStateOnDisconnect() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        return "";
    });
    CHECK(emu.Start(), "LinkDisconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex;
    std::vector<std::pair<std::string, std::string>> conn;   // (state, reason)
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        conn.emplace_back(j.value("state", std::string{}), j.value("reason", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "LinkDisconnect: Connect");
    CHECK(drv.IsReady(), "LinkDisconnect: після Connect зв'язок Ready");

    emu.DropConnection();
    for (int i = 0; i < 100 && drv.IsReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!drv.IsReady(), "LinkDisconnect: після обриву Ready знято");

    drv.Disconnect();
    std::lock_guard<std::mutex> lk(evMutex);
    CHECK(conn.size() >= 3, "LinkDisconnect: події connection надійшли");
    CHECK(conn.front().first == "ready", "LinkDisconnect: перша подія - ready (Connect)");
    bool sawDropped = false, sawClosed = false;
    for (const auto& e : conn) {
        if (e.first == "connecting"   && e.second == "dropped") sawDropped = true;
        if (e.first == "disconnected" && e.second == "closed")  sawClosed  = true;
    }
    CHECK(sawDropped, "LinkDisconnect: обрив -> connecting(dropped)");
    CHECK(sawClosed,  "LinkDisconnect: Отключить -> disconnected(closed), не dropped");
    emu.Stop();
}

// №22: три стани - три різні коди. Без правильного порядку перевірок (а) і (б) дали б 18.
static void TestThreeStatesThreeCodes() {
    TerminalEmulator emu;
    // Після реконекту термінал МОВЧИТЬ на Ping (модель монополії, §2.3): інакше в Task 4
    // фоновий джоб підняв би Ready за ~1 с після обриву, і крок (в) став би флакі -
    // оплата встигала б пройти замість RECONNECTING. Хендшейк кроку 1 Connect() при цьому
    // відповідає нормально: прапорець вмикається вже після успішного Connect.
    std::atomic<bool> silentPing{ false };
    emu.OnRequest("PingDevice", [&silentPing](const nlohmann::json&) -> std::string {
        if (silentPing.load()) return "";
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        return "";
    });
    std::atomic<int> purchaseSeen{ 0 };
    emu.OnRequest("Purchase", [&](const nlohmann::json&)->std::string{
        purchaseSeen.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "ThreeStates: емулятор стартував");

    EcrPrivatJsonDriver drv;
    // (а) без жодного Connect
    ResultEnvelope a = drv.Purchase("10.00");
    CHECK(!a.ok && a.code == "NOT_CONNECTED", "ThreeStates(а): Purchase без Connect -> NOT_CONNECTED");

    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "ThreeStates: Connect");
    // (в) під Connecting: рвемо з'єднання й одразу пробуємо платити
    silentPing.store(true);          // термінал ще тримає стару сесію - Ready не повернеться
    emu.DropConnection();
    for (int i = 0; i < 100 && drv.IsReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Ready падає МИТТЄВО (хук на дроп), а IsConnected() ще ні: DeviceSession-супервізор
    // тримає ФІКСОВАНИЙ backoff (SessionConfig::reconnectDelayMs=1000мс, DeviceSession.h)
    // ПЕРЕД будь-якою спробою Close->Open, навіть для щойно виявленого обриву. Без цього
    // очікування Purchase() застав би сокет ще закритим і дав би NOT_CONNECTED замість
    // RECONNECTING - не тому що порядок перевірок неправильний, а тому що TCP-реконект
    // фізично не встиг. Чекаємо саме той стан, який тестує (в): сокет уже перепідключився
    // (IsConnected()=true), термінал мовчить на Ping (silentPing) - Ready лишається false.
    for (int i = 0; i < 200 && !drv.IsConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(drv.IsConnected() && !drv.IsReady(),
          "ThreeStates(в): сокет перепідключився, Ready лишається false (Connecting)");
    const int before = purchaseSeen.load();
    ResultEnvelope c = drv.Purchase("10.00");
    CHECK(!c.ok && c.code == "RECONNECTING", "ThreeStates(в): Purchase під Connecting -> RECONNECTING");
    CHECK(purchaseSeen.load() == before, "ThreeStates(в): на дріт нічого не пішло");

    // (б) одразу після Отключить
    drv.Disconnect();
    ResultEnvelope b = drv.Purchase("10.00");
    CHECK(!b.ok && b.code == "NOT_CONNECTED", "ThreeStates(б): Purchase після Отключить -> NOT_CONNECTED, не 18");
    emu.Stop();
}

// №24: повторний Connect() мусить давати Ready. Дефолтна епоха 0 у SetLinkState зробила б
// це мовчазним no-op (linkEpoch_ уже >= 1 після першого Отключить) - Ready не став би,
// хук up=true запустив би зайвий Ping, і перша оплата зміни отримала б CONCURRENT.
static void TestReconnectByHandKeepsReady() {
    TerminalEmulator emu;
    std::atomic<int> pings{ 0 };
    std::atomic<int> purchases{ 0 };
    emu.OnRequest("PingDevice", [&pings](const nlohmann::json&) {
        pings.fetch_add(1);
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [&purchases](const nlohmann::json&) {
        purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"3"},"error":false})";
    });
    CHECK(emu.Start(), "ReconnectByHand: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "ReconnectByHand: перший Connect");
    drv.Disconnect();
    CHECK(drv.Connect(conn), "ReconnectByHand: другий Connect");
    CHECK(drv.IsReady(), "ReconnectByHand: Подключен=Истина одразу після другого Connect");

    const int pingsAfterConnect = pings.load();      // лише хендшейки кроку 1 (по одному на Connect)
    ResultEnvelope p = drv.Purchase("10.00");
    CHECK(p.ok && p.code == "0000", "ReconnectByHand: перша оплата після Connect проходить");
    CHECK(p.code != "CONCURRENT", "ReconnectByHand: не CONCURRENT");
    CHECK(pings.load() == pingsAfterConnect,
          "ReconnectByHand: на персистентній сесії нуль зайвих PingDevice");
    CHECK(purchases.load() == 1, "ReconnectByHand: оплата дійшла до термінала");
    drv.Disconnect();
    emu.Stop();
}

// --- 1С-фасад: смоук через AddInNative::CreateObject ------------------------
// Мінімальний мок платформи 1С (IMemoryManager/IAddInDefBase) для інстанціювання
// компоненти в процесі — достатньо для реєстрації методів і смоук-виклику.
class MockMem : public IMemoryManager {
public:
    bool ADDIN_API AllocMemory(void** p, unsigned long n) override { *p = malloc(n); return *p != nullptr; }
    void ADDIN_API FreeMemory(void** p) override { if (p && *p) { free(*p); *p = nullptr; } }
};
class MockConn : public IAddInDefBase {
public:
    bool ADDIN_API AddError(unsigned short, const WCHAR_T*, const WCHAR_T*, long) override { return true; }
    bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
    bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return true; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 0; }
    bool ADDIN_API ExternalEvent(WCHAR_T*, WCHAR_T*, WCHAR_T*) override { return true; }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
};

static void TestFacadeSmoke() {
    AddInNative* comp = AddInNative::CreateObject(u"ECRPrivatJSON");
    CHECK(comp != nullptr, "Facade: CreateObject(ECRPrivatJSON) → не null");
    if (!comp) return;
    MockMem mem; MockConn conn;
    comp->Init(&conn);
    comp->setMemManager(&mem);
    long n = comp->GetNMethods();
    CHECK(n >= 10, "Facade: зареєстровано >=10 методів");
    delete comp;
}

// ==================== RunVoid: відкат на повернення лише при UNSUPPORTED ====================
//
// ⚠️ Це про гроші. TIMEOUT/DISCONNECTED/DESYNC/SEND_FAILED/BAD_RESPONSE означають
// «невідомо, чи виконалось»: термінал МІГ скасувати операцію, а відповідь не дійшла.
// Відкат на Refund після такого зрушив би гроші ДВІЧІ. UNSUPPORTED — єдина відповідь,
// що гарантує: команда термінала не досягла й нічого не сталося.

namespace {

/// Лічильники викликів фейкового драйвера — саме їх перевіряє тест.
struct FakeAcquiringState {
    ResultEnvelope voidResult = AcquiringUnsupported("Скасування");
    int voidCalls = 0;
    int refundCalls = 0;
};

class FakeAcquiring : public IAcquiringDriver {
public:
    explicit FakeAcquiring(FakeAcquiringState* s) : s_(s) {}
    ResultEnvelope Open(const std::map<std::string, std::string>&) override { return ResultEnvelope::Ok(); }
    void Close() override {}
    bool IsConnected() const override { return true; }
    std::string Vendor() const override { return "FAKE"; }
    std::string Model() const override { return "FAKE-1"; }
    std::string DriverName() const override { return "Фейковий драйвер"; }
    std::string DriverDescription() const override { return "Лише для тесту"; }
    std::string SettingsXml() const override { return "<Settings/>"; }
    AcquiringCapabilities Capabilities() const override { return {}; }
    std::string TargetKey(const std::map<std::string, std::string>&) const override {
        return "fake://void-probe";   // ціль фіксована: тест перевіряє RunVoid, не ProbeDevice
    }
    ResultEnvelope Probe() override { return ResultEnvelope::Ok(); }
    ResultEnvelope Void(double, const std::string&) override {
        ++s_->voidCalls;
        return s_->voidResult;
    }
    ResultEnvelope Refund(double, const std::string&) override {
        ++s_->refundCalls;
        return ResultEnvelope::Ok({ { "rrn", "R-1" } });
    }
private:
    FakeAcquiringState* s_;
};

/// Мінімальний конкретний фасад: методів у 1С не реєструє (RegisterSystemMethods не
/// кличеться), потрібен лише щоб дістати RunVoid із фейковим драйвером під ним.
class VoidProbeFacade : public AcquiringFacadeBase {
public:
    explicit VoidProbeFacade(FakeAcquiringState* s) : s_(s) {}
    using AcquiringFacadeBase::RunVoid;   // відкриваємо protected-метод для тесту
protected:
    int InterfaceRevision() const override { return 3004; }
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override {
        return std::make_unique<FakeAcquiring>(s_);
    }
private:
    FakeAcquiringState* s_;
};

} // namespace

static void TestVoidFallbackOnlyOnUnsupported() {
    // 1) Власної операції void протокол не має -> штатний шлях: повернення за RRN.
    {
        FakeAcquiringState st;
        st.voidResult = AcquiringUnsupported("Скасування");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        CHECK(r.ok, "Void=UNSUPPORTED + VoidAsRefund -> скасування виконано поверненням");
        CHECK(st.voidCalls == 1 && st.refundCalls == 1,
              "Void=UNSUPPORTED -> Void спитано, Refund викликано рівно раз");
    }
    // 2) TIMEOUT: термінал МІГ скасувати. Відкату бути НЕ МОЖЕ — інакше подвійний рух грошей.
    {
        FakeAcquiringState st;
        st.voidResult = ResultEnvelope::Fail("TIMEOUT", "Термінал не відповів");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        CHECK(!r.ok && r.code == "TIMEOUT", "Void=TIMEOUT -> відмова з тим самим кодом");
        CHECK(st.refundCalls == 0, "Void=TIMEOUT -> Refund НЕ викликано (подвійний рух грошей)");
    }
    // 3) Решта «невідомо, чи виконалось» — так само без відкату.
    for (const char* code : { "DISCONNECTED", "DESYNC", "SEND_FAILED", "BAD_RESPONSE" }) {
        FakeAcquiringState st;
        st.voidResult = ResultEnvelope::Fail(code, "Збій зв'язку");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        const std::string name =
            std::string("Void=") + code + " -> відмова без відкату на повернення";
        CHECK(!r.ok && r.code == code && st.refundCalls == 0, name.c_str());
    }
}

int main() {
    TestResultEnvelope();
    TestEcrJsonCodec();
    TestEcrClassifier();
    TestEmulatorPing();
    TestConnStringParse();
    TestConnectReferenceScheme();
    TestJobEngine();
    TestJobEngineStartRace();
    TestDriverPurchaseHappy();
    TestDriverStrictTerminalParams();
    TestDriverReports();
    TestDriverStatusPoll();
    TestDriverInterrupt();
    TestDriverAsync();
    TestDriverAsyncCancel();
    TestDriverCancelNoWedge();
    TestForeignReceiptNotCredited();
    TestNonFinancialNotTracked();
    TestInquireNone();
    TestLinkStateOnDisconnect();
    TestThreeStatesThreeCodes();
    TestReconnectByHandKeepsReady();
    TestPingAfterReconnect();
    TestSilenceAfterReconnect();
    TestPingBusyIsReady();
    TestLinkEpochRejectsStaleReady();
    TestLinkEpochStaleReadyDeterministic();
    TestSendFailedResolvesInline();
    TestTerminalBusyUntilLimit();
    TestSelfHealingFromInquire();
    TestDisconnectInterruptsBackoff();
    TestOutcomeAfterReconnect();
    TestDisconnectDuringPending();
    TestSecondDropDuringCapture();
    TestStoppedIsTrigger();
    TestFacadeSmoke();
    TestVoidFallbackOnlyOnUnsupported();
    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}
