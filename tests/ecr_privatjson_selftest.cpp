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
    TestDriverPurchaseHappy();
    TestDriverStrictTerminalParams();
    TestDriverReports();
    TestDriverStatusPoll();
    TestDriverInterrupt();
    TestDriverAsync();
    TestDriverAsyncCancel();
    TestDriverCancelNoWedge();
    TestFacadeSmoke();
    TestVoidFallbackOnlyOnUnsupported();
    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}
