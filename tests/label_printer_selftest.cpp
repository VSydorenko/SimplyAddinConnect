#include <cstdio>
#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include "support/LabelEmulator.h"
#include "../src/drivers/label_printer/LabelModel.h"
#include "../src/drivers/label_printer/LabelUnits.h"
#include "../src/drivers/label_printer/GfEncoder.h"
#include "../src/drivers/label_printer/BarcodeZpl.h"
#include "../src/drivers/label_printer/LabelRaster.h"
#include "../src/drivers/label_printer/LabelZplGenerator.h"
#include "../src/drivers/label_printer/LabelPrinterDriver.h"
#include "../src/drivers/label_printer/LabelXml.h"
#include "../src/transport/Transport.h"
#include "../src/transport/Transport_SpoolerRaw.h"
#include "../src/core/AddInNative.h"
using namespace labelprinter;

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

static void TestUnits() {
    CHECK(mmToDots(60, 8) == 480, "60mm @8dpmm = 480 dots");
    CHECK(mmToDots(60, 12) == 720, "60mm @12dpmm = 720 dots (not nominal 709)");
    CHECK(mmToDots(40, 24) == 960, "40mm @24dpmm = 960 dots");
    CHECK(ptToDots(8, 8) == 23, "8pt @8dpmm = 23 dots");
}

static void TestGfEncoder() {
    // 8x2, усі чорні: bytesPerRow=1, total=2, hex "FF" на рядок
    Bitmap1 bm{8, 2, {0xFF, 0xFF}};
    std::string z = GfEncoder::EncodeGfa(bm, 0, 0, /*maxRowsPerTile=*/1000);
    CHECK(z.find("^FO0,0") != std::string::npos, "GFA has origin");
    CHECK(z.find("^GFA,2,2,1,") != std::string::npos, "GFA header a,b=2,c=2,d=1");
    CHECK(z.find("FFFF") != std::string::npos, "GFA data hex FFFF");
    CHECK(z.find("^FS") != std::string::npos, "GFA closed with ^FS");
}

static void TestGfTiling() {
    Bitmap1 bm{8, 5, {0xFF,0xFF,0xFF,0xFF,0xFF}};
    std::string z = GfEncoder::EncodeGfa(bm, 10, 20, /*maxRowsPerTile=*/2);
    // 5 рядків / 2 = 3 тайли (2+2+1)
    size_t n=0, pos=0; while ((pos=z.find("^GFA", pos))!=std::string::npos){++n;++pos;}
    CHECK(n == 3, "tiling splits 5 rows @2/tile into 3 ^GFA");
    CHECK(z.find("^FO10,20")!=std::string::npos && z.find("^FO10,22")!=std::string::npos && z.find("^FO10,24")!=std::string::npos, "tile origins offset by rows");
}

static void TestBarcodeZpl() {
    auto mk = [](const char* type){ BarcodeField b; b.fieldName="B"; b.type=type; b.geom={1,1,0,10,0}; return b; };
    CHECK(BarcodeZpl::Emit(mk("EAN13"), "4008110271538", 8).zpl.find("^BE")!=std::string::npos, "EAN13->^BE");
    CHECK(BarcodeZpl::Emit(mk("EAN13"), "4008110271538", 8).zpl.find("^FD400811027153^FS")!=std::string::npos, "EAN13 normalizes 13->12 digits");
    CHECK(BarcodeZpl::Emit(mk("ITF14"), "10012345678902", 8).zpl.find("^B2")!=std::string::npos, "ITF14->^B2 (not ^BC)");
    CHECK(BarcodeZpl::Emit(mk("Code128"),"ABC", 8).zpl.find("^BC")!=std::string::npos, "Code128->^BC");
    CHECK(BarcodeZpl::Emit(mk("QRCode"),"x", 8).zpl.find("^BQ")!=std::string::npos, "QR->^BQ");
    CHECK(BarcodeZpl::Emit(mk("DataMatrix"),"x", 8).zpl.find("^BX")!=std::string::npos, "DataMatrix->^BX");
    CHECK(BarcodeZpl::Emit(mk("PDF417"),"x", 8).zpl.find("^B7")!=std::string::npos, "PDF417->^B7");
    auto u = BarcodeZpl::Emit(mk("Code16k"), "x", 8);
    CHECK(!u.ok && u.errCode=="UNSUPPORTED_BARCODE", "Code16k -> UNSUPPORTED");
    CHECK(BarcodeZpl::EscapeFd("A^B~C").find("^FH")!=std::string::npos, "escape adds ^FH for special chars");
}

// РЕГРЕС FIX B: EAN128 (GS1-128) FNC1-послідовність ">;>8" має лишитись КЕРУЮЧОЮ у ^FD,
// а не бути hex-екранованою під ^FH (стара версія клала ">;>8" у data, EscapeFd бив '>' -> "_3E",
// знищуючи FNC1/subset-C).
static void TestEan128Fnc1() {
    BarcodeField b; b.fieldName="B"; b.type="EAN128"; b.geom={1,1,0,10,0};
    auto e = BarcodeZpl::Emit(b, "0104820000000015", 8);   // all-digits payload
    CHECK(e.ok, "EAN128 emits ok");
    CHECK(e.zpl.find(">;>8") != std::string::npos, "EAN128 FNC1 sequence '>;>8' intact as control");
    CHECK(e.zpl.find("_3E") == std::string::npos, "EAN128 FNC1 '>' NOT hex-escaped to _3E");
}

// РЕГРЕС FIX F: ^BY тепер рахується з geom.width; надто вузьке поле -> ok=false (BARCODE_TOO_WIDE),
// достатньо широке -> ok. Стара версія жорстко ^BY2 без перевірки влізання.
static void TestBarcodeModuleWidth() {
    BarcodeField narrow; narrow.fieldName="B"; narrow.type="Code128"; narrow.geom={0,0,1.0,10,0}; // 1мм @8=8dots
    auto en = BarcodeZpl::Emit(narrow, "1234567890123456", 8);
    CHECK(!en.ok && (en.errCode=="BARCODE_TOO_WIDE" || en.errCode=="RENDER_ERROR"),
          "too-narrow field -> ok=false (BARCODE_TOO_WIDE)");
    BarcodeField wide = narrow; wide.geom={0,0,80.0,10,0};                                        // 80мм @8=640dots
    auto ew = BarcodeZpl::Emit(wide, "1234567890123456", 8);
    CHECK(ew.ok && ew.zpl.find("^BY") != std::string::npos, "wide-enough field -> ok, ^BY present");
    // поле без заданої ширини (width=0) лишається на дефолтному ^BY2 (сумісність)
    BarcodeField noW; noW.fieldName="B"; noW.type="EAN13"; noW.geom={1,1,0,10,0};
    CHECK(BarcodeZpl::Emit(noW,"4008110271538",8).zpl.find("^BY2")!=std::string::npos, "width=0 -> default ^BY2");
}

// РЕГРЕС FIX H: FieldGeom.orientation мапиться у ZPL-орієнтацію (перший параметр баркод-команди).
// Стара версія жорстко клала 'N', ігноруючи orientation.
static void TestBarcodeOrientation() {
    BarcodeField b; b.fieldName="B"; b.type="Code128"; b.geom={1,1,0,10,90};   // 90 -> R
    CHECK(BarcodeZpl::Emit(b,"ABC",8).zpl.find("^BCR,")!=std::string::npos, "orientation 90 -> ^BCR (rotated)");
    BarcodeField b0=b; b0.geom.orientation=0;
    CHECK(BarcodeZpl::Emit(b0,"ABC",8).zpl.find("^BCN,")!=std::string::npos, "orientation 0 -> ^BCN (normal)");
    BarcodeField b180=b; b180.geom.orientation=180;
    CHECK(BarcodeZpl::Emit(b180,"ABC",8).zpl.find("^BCI,")!=std::string::npos, "orientation 180 -> ^BCI (inverted)");
}

// РЕГРЕС FIX G: розширення/extensions -> зрозумілий UNSUPPORTED_BARCODE («відкладено до v2»).
static void TestDeferredBarcodeTypes() {
    BarcodeField a; a.fieldName="B"; a.type="EAN13Addon2"; a.geom={1,1,0,10,0};
    auto ea = BarcodeZpl::Emit(a,"12345",8);
    CHECK(!ea.ok && ea.errCode=="UNSUPPORTED_BARCODE" && ea.errDesc.find("v2")!=std::string::npos,
          "EAN13Addon2 -> UNSUPPORTED_BARCODE (відкладено до v2)");
}

// РЕГРЕС FIX A: static-штрихкод задається Base64 (§5) -> генератор декодує його ПЕРЕД Emit.
// Стара версія подавала Base64 як-є; digitsOnly вихоплював цифри з base64-рядка -> хибний ^FD.
static void TestStaticBarcodeBase64() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width=60; fmt.height=40;
    BarcodeField bc; bc.fieldName="Bar"; bc.type="EAN13"; bc.isStatic=true;
    bc.staticValueBase64 = std::string("NDAwODExMDI3MTUzOA==");   // Base64("4008110271538")
    bc.geom={1,22,0,10,0};
    fmt.barcodes.push_back(bc);
    LabelInstance inst; inst.quantity=1;                          // немає запису для Bar -> береться static (Base64)
    DeviceProfile dp; dp.dotsPerMm=8;
    auto r = LabelZplGenerator::BuildLabel(fmt, inst, dp);
    CHECK(r.ok, "static-barcode Base64 label builds ok");
    std::string z(r.zpl.begin(), r.zpl.end());
    CHECK(z.find("^FD400811027153") != std::string::npos,
          "static barcode Base64 decoded to value (not raw base64 digits)");
}

static long CountBlack(const Bitmap1& bm) {
    long ones = 0;
    for (auto b : bm.rows) for (int i = 0; i < 8; ++i) ones += (b >> i) & 1;
    return ones;
}

static void TestRaster() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width = 20; fmt.height = 10;
    TextField tx; tx.fieldName = "T"; tx.geom = {1, 1, 18, 8, 0};
    tx.fontName = "Arial"; tx.fontSize = 8;
    tx.defaultOrStaticValue = std::string("Тест"); tx.isStatic = true;
    fmt.texts.push_back(tx);
    auto valueOf = [](const std::string&) -> std::optional<std::string> { return std::string("Тест"); };
    bool ok = false;
    Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8, ok);
    CHECK(ok, "render ok flag true on success");
    CHECK(bm.widthDots == 160 && bm.heightDots == 80, "raster size = label mm * dpmm");
    CHECK(CountBlack(bm) > 0, "rendered text produced black pixels");
}

static void TestRasterImage() {
    // Валідний 1x1 чорний PNG (System.Drawing) — має дати чорні пікселі в зоні картинки.
    const std::string kBlackPng1x1 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAXNSR0IArs4c6QAAAARnQU1BAACx"
        "jwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAANSURBVBhXY2BgYPgPAAEEAQBwIGULAAAAAElF"
        "TkSuQmCC";
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width = 20; fmt.height = 10;
    ImageField img; img.fieldName = "I"; img.geom = {1, 1, 18, 8, 0};
    img.isStatic = true; img.staticValueBase64 = kBlackPng1x1;
    fmt.images.push_back(img);
    auto valueOf = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
    bool ok = false;
    Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8, ok);
    CHECK(ok, "image render ok flag true on success");
    CHECK(bm.widthDots == 160 && bm.heightDots == 80 && !bm.rows.empty(), "image raster non-empty with correct size");
    CHECK(CountBlack(bm) > 0, "decoded PNG image produced black pixels");
}

static void TestGenerator() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width = 60; fmt.height = 40;
    TextField tx; tx.fieldName = "Name"; tx.geom = {1, 1, 55, 10, 0};
    tx.fontName = "Arial"; tx.fontSize = 8; fmt.texts.push_back(tx);
    BarcodeField bc; bc.fieldName = "Bar"; bc.type = "EAN13"; bc.geom = {1, 22, 0, 10, 0};
    fmt.barcodes.push_back(bc);
    LabelInstance inst; inst.quantity = 2;
    inst.records.push_back({"Name", std::string("Блокнот")});
    inst.records.push_back({"Bar",  std::string("4008110271538")});
    DeviceProfile dp; dp.dotsPerMm = 8;
    auto r = LabelZplGenerator::BuildLabel(fmt, inst, dp);
    std::string z(r.zpl.begin(), r.zpl.end());
    CHECK(r.ok, "generate ok");
    CHECK(z.rfind("^XA", 0) == 0 && z.find("^XZ") != std::string::npos, "wrapped ^XA..^XZ");
    CHECK(z.find("^PQ2") != std::string::npos, "quantity ^PQ2");
    CHECK(z.find("^GFA") != std::string::npos, "raster present");
    CHECK(z.find("^BE") != std::string::npos, "native EAN13 present");

    BarcodeField u = bc; u.type = "Code16k";
    LabelFormatting f2 = fmt; f2.barcodes = {u};
    auto r2 = LabelZplGenerator::BuildLabel(f2, inst, dp);
    CHECK(!r2.ok && r2.errCode == "UNSUPPORTED_BARCODE", "unsupported barcode propagates");

    auto init = LabelZplGenerator::BuildInit(dp);
    std::string zi(init.begin(), init.end());
    CHECK(zi.rfind("^XA", 0) == 0 && zi.find("^XZ") != std::string::npos, "init wrapped ^XA..^XZ");
    CHECK(zi.find("^PR") != std::string::npos && zi.find("^MD") != std::string::npos, "init has darkness+speed");
}

// РЕГРЕС FIX E: реальний збій рендеру растру (некоректний розмір) -> BuildLabel = RENDER_ERROR,
// а не мовчазне ok=true без ^GF. На старому коді Render повертав порожній растр, ^GF просто
// опускався, а BuildLabel казав ok=true — тобто друкувалась етикетка без растрового шару.
static void TestRenderErrorPropagates() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width = 0; fmt.height = 40;   // W=0 dots -> реальний збій рендеру
    // Додаємо текст, щоб растровий шар був змістовним (не «легітимно-порожній»).
    TextField tx; tx.fieldName = "Name"; tx.geom = {1, 1, 55, 10, 0}; tx.fontName = "Arial"; tx.fontSize = 8;
    fmt.texts.push_back(tx);
    LabelInstance inst; inst.quantity = 1;
    inst.records.push_back({"Name", std::string("Тест")});
    DeviceProfile dp; dp.dotsPerMm = 8;
    auto r = LabelZplGenerator::BuildLabel(fmt, inst, dp);
    CHECK(!r.ok && r.errCode == "RENDER_ERROR", "render failure -> BuildLabel RENDER_ERROR (not silent ok)");
}

static void TestSpooler() {
    TransportSpoolerRaw t("FakePrinter");
    std::vector<uint8_t> got;
    t.SetSendFunctionForTest([&](const std::vector<uint8_t>& d){ got = d; return (int)d.size(); });
    CHECK(t.Open(), "spooler Open (stubbed)");
    std::vector<uint8_t> payload = {'^','X','A','^','X','Z'};
    CHECK(t.Send(payload) == (int)payload.size(), "Send returns full byte count");
    CHECK(got == payload, "test seam captured exact bytes");
    CHECK(t.IsOpen(), "IsOpen true after Open");
    t.Close();
    CHECK(!t.IsOpen(), "IsOpen false after Close");
}

static void TestDriverBatch() {
    GdiplusRuntime gdi;
    LabelPrinterDriver drv;
    std::vector<uint8_t> captured;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&) -> std::unique_ptr<ITransport> {
        struct Fake : ITransport {
            std::vector<uint8_t>* out; bool open=false;
            bool Open() override {open=true;return true;}
            bool Close() override {open=false;return true;}
            bool IsOpen() const override {return open;}
            int Send(const std::vector<uint8_t>& d) override {out->insert(out->end(),d.begin(),d.end());return (int)d.size();}
            void SetDataReceivedCallback(DataReceivedCallback) override{}
            void SetErrorCallback(ErrorCallback) override{}
            void SetConnectionStateCallback(ConnectionStateCallback) override{}
        };
        auto f=std::make_unique<Fake>(); f->out=&captured; return f;
    });
    DeviceProfile dp; dp.dotsPerMm=8;
    std::string id = drv.Connect(dp);
    CHECK(!id.empty() && drv.IsConnected(id), "connected, has id");

    LabelBatch b; LabelFormatting fmt; fmt.width=60; fmt.height=40; b.formatting=fmt;
    b.labels.push_back({1, {}});
    // "regular" без "first" -> помилка
    CHECK(!drv.PrintLabels(id, b, "regular").ok, "regular before first -> fail");
    // "first" зберігає formatting і друкує
    auto r = drv.PrintLabels(id, b, "first");
    CHECK(r.ok && r.payload["acceptedInstances"]==1, "first prints, accepted=1");
    // "regular"/"last" без formatting використовує кеш
    LabelBatch b2; b2.labels.push_back({3, {}});
    auto r2 = drv.PrintLabels(id, b2, "last");
    CHECK(r2.ok && r2.payload["acceptedCopies"]==3, "last uses cached formatting, copies=3");
    // після "last" кеш очищено -> "regular" знову fail
    CHECK(!drv.PrintLabels(id, b2, "regular").ok, "cache cleared after last");
    drv.Disconnect(id); CHECK(!drv.IsConnected(id), "disconnected");
}

// РЕГРЕС FIX C: друк ТЕКСТОВОЇ етикетки ЧЕРЕЗ драйвер БЕЗ власного GdiplusRuntime у тесті.
// Драйвер сам тримає GDI+ живим (член gdiplus_), тож у продакшн-шляху BuildLabel->Render
// растровий шар присутній (^GFA у захопленому ZPL). На старому коді (без члена драйвера)
// GDI+ не був ініціалізований у цьому шляху -> растр випадав, ^GFA відсутній.
static void TestDriverTextRasterNoGdiplus() {
    LabelPrinterDriver drv;                       // жодного GdiplusRuntime з боку тесту
    std::vector<uint8_t> captured;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&) -> std::unique_ptr<ITransport> {
        struct Fake : ITransport {
            std::vector<uint8_t>* out; bool open=false;
            bool Open() override {open=true;return true;}
            bool Close() override {open=false;return true;}
            bool IsOpen() const override {return open;}
            int Send(const std::vector<uint8_t>& d) override {out->insert(out->end(),d.begin(),d.end());return (int)d.size();}
            void SetDataReceivedCallback(DataReceivedCallback) override{}
            void SetErrorCallback(ErrorCallback) override{}
            void SetConnectionStateCallback(ConnectionStateCallback) override{}
        };
        auto f=std::make_unique<Fake>(); f->out=&captured; return f;
    });
    DeviceProfile dp; dp.dotsPerMm=8;
    std::string id = drv.Connect(dp);
    LabelBatch b; LabelFormatting fmt; fmt.width=60; fmt.height=40;
    TextField tx; tx.fieldName="Name"; tx.geom={1,1,55,10,0}; tx.fontName="Arial"; tx.fontSize=8;
    fmt.texts.push_back(tx);
    b.formatting=fmt;
    b.labels.push_back({1, { {"Name", std::string("Тест")} }});
    auto r = drv.PrintLabels(id, b, "first");
    CHECK(r.ok, "driver prints text label without test-side GdiplusRuntime");
    std::string z(captured.begin(), captured.end());
    CHECK(z.find("^GFA") != std::string::npos, "raster ^GFA present via driver-owned GDI+ runtime");
    drv.Disconnect(id);
}

// РЕГРЕС FIX I: усі транспортні відмови (немає транспорту / Open провалився / Send прийняв
// менше байтів) зводяться до єдиного коду TRANSPORT_ERROR (таксономія §9). Стара версія
// повертала NO_TRANSPORT/OPEN_FAILED/SEND_FAILED, яких у §9 немає.
static void TestTransportErrorTaxonomy() {
    GdiplusRuntime gdi;
    // 1) Send приймає менше байтів -> TRANSPORT_ERROR.
    {
        LabelPrinterDriver drv;
        drv.SetTransportFactoryForTest([](const DeviceProfile&) -> std::unique_ptr<ITransport> {
            struct ShortSend : ITransport {
                bool open=false;
                bool Open() override {open=true;return true;}
                bool Close() override {open=false;return true;}
                bool IsOpen() const override {return open;}
                int Send(const std::vector<uint8_t>&) override {return 0;}   // прийняв 0 байтів
                void SetDataReceivedCallback(DataReceivedCallback) override{}
                void SetErrorCallback(ErrorCallback) override{}
                void SetConnectionStateCallback(ConnectionStateCallback) override{}
            };
            return std::make_unique<ShortSend>();
        });
        DeviceProfile dp; dp.dotsPerMm=8;
        std::string id = drv.Connect(dp);
        LabelBatch b; LabelFormatting fmt; fmt.width=60; fmt.height=40; b.formatting=fmt;
        b.labels.push_back({1, {}});
        auto r = drv.PrintLabels(id, b, "first");
        CHECK(!r.ok && r.code == "TRANSPORT_ERROR", "short Send -> code TRANSPORT_ERROR (not SEND_FAILED)");
        drv.Disconnect(id);
    }
    // 2) Open провалюється -> TRANSPORT_ERROR (через InitializePrinter).
    {
        LabelPrinterDriver drv;
        drv.SetTransportFactoryForTest([](const DeviceProfile&) -> std::unique_ptr<ITransport> {
            struct OpenFail : ITransport {
                bool Open() override {return false;}                          // не відкривається
                bool Close() override {return true;}
                bool IsOpen() const override {return false;}
                int Send(const std::vector<uint8_t>& d) override {return (int)d.size();}
                void SetDataReceivedCallback(DataReceivedCallback) override{}
                void SetErrorCallback(ErrorCallback) override{}
                void SetConnectionStateCallback(ConnectionStateCallback) override{}
            };
            return std::make_unique<OpenFail>();
        });
        DeviceProfile dp; dp.dotsPerMm=8;
        std::string id = drv.Connect(dp);
        auto r = drv.InitializePrinter(id);
        CHECK(!r.ok && r.code == "TRANSPORT_ERROR", "Open failure -> code TRANSPORT_ERROR (not OPEN_FAILED)");
        drv.Disconnect(id);
    }
}

// РЕГРЕС FIX D: контекст пристрою тримається shared_ptr; Lookup віддає shared-копію.
// In-flight PrintLabels, що блокує в Send (тримає ctx->m + shared-власника), лишається
// валідним, поки паралельний Disconnect ТОГО Ж DeviceID робить erase з мапи. На старому
// коді Lookup віддавав сирий вказівник у unique_ptr-контекст -> потенційний UAF при
// конкурентному Disconnect. Тут перевіряємо, що операція завершується коректно й без краху.
static void TestDriverConcurrentDisconnect() {
    std::atomic<bool> inSend{false};
    std::atomic<bool> allowSend{false};
    LabelPrinterDriver drv;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&) -> std::unique_ptr<ITransport> {
        struct Gated : ITransport {
            std::atomic<bool>* inSend; std::atomic<bool>* allow; bool open=false;
            bool Open() override {open=true;return true;}
            bool Close() override {open=false;return true;}
            bool IsOpen() const override {return open;}
            int Send(const std::vector<uint8_t>& d) override {
                inSend->store(true);
                while (!allow->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return (int)d.size();
            }
            void SetDataReceivedCallback(DataReceivedCallback) override{}
            void SetErrorCallback(ErrorCallback) override{}
            void SetConnectionStateCallback(ConnectionStateCallback) override{}
        };
        auto g=std::make_unique<Gated>(); g->inSend=&inSend; g->allow=&allowSend; return g;
    });
    DeviceProfile dp; dp.dotsPerMm=8;
    std::string id = drv.Connect(dp);

    LabelBatch b; LabelFormatting fmt; fmt.width=60; fmt.height=40; b.formatting=fmt;
    b.labels.push_back({1, {}});

    std::atomic<bool> printOk{false};
    std::thread worker([&]{ printOk.store(drv.PrintLabels(id, b, "first").ok); });

    // Чекаємо, поки друк зайде в Send (тримає ctx->m + shared-копію контексту).
    for (int i=0; i<500 && !inSend.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(inSend.load(), "print reached in-flight Send");

    // Конкурентний Disconnect: erase з мапи, потім блокує на ctx->m до кінця Send.
    std::thread disc([&]{ drv.Disconnect(id); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    allowSend.store(true);           // відпускаємо Send

    worker.join();
    disc.join();
    CHECK(printOk.load(), "in-flight print completed OK despite concurrent Disconnect");
    CHECK(!drv.IsConnected(id), "device disconnected after concurrent Disconnect");
}

static void TestXml() {
    const char* xml = R"(<?xml version="1.0"?><Data>
      <Formatting Width="60" Height="40">
        <Text FieldName="Name" Left="1" Top="1" Width="55" Height="10" FontName="Tahoma" FontSize="8"/>
        <Barcode FieldName="Bar" Type="EAN13" Left="1" Top="22" Height="10" PrintHRI="true" FontSize="8"/>
        <UserData FieldName="U" Static="true" Value="X"/>
      </Formatting>
      <Labels>
        <Label Quantity="2"><Record FieldName="Name" Value="Блокнот"/><Record FieldName="Bar" Value="4008110271538"/></Label>
        <Label><Record FieldName="Name" Value=""/><Record FieldName="Bar"/></Label>
      </Labels></Data>)";
    LabelBatch b; std::string err;
    CHECK(LabelXml::ParseLabelsTable(xml, b, err), "parse LabelsTable ok");
    CHECK(b.formatting && b.formatting->width == 60 && b.formatting->height == 40, "formatting geometry");
    CHECK(b.formatting->texts.size() == 1 && b.formatting->barcodes.size() == 1, "fields parsed");
    CHECK(b.formatting->userData.size() == 1 && b.formatting->userData[0].isStatic &&
          b.formatting->userData[0].defaultOrStaticValue.has_value(), "userdata static value parsed");
    CHECK(b.labels.size() == 2 && b.labels[0].quantity == 2, "labels + quantity");
    CHECK(b.labels[1].quantity == 1, "missing Quantity defaults to 1");
    CHECK(b.labels[0].records[0].value.has_value() && *b.labels[0].records[0].value == "Блокнот", "record value present");
    // value-семантика: порожній рядок (Value="") != відсутній атрибут (немає Value)
    CHECK(b.labels[1].records[0].value.has_value() && b.labels[1].records[0].value->empty(),
          "empty Value attribute -> present-but-empty optional");
    CHECK(!b.labels[1].records[1].value.has_value(), "missing Value attribute -> nullopt (absent != empty)");

    // XML без Formatting (regular/last пакет) -> formatting відсутнє, labels є
    const char* xml2 = R"(<Data><Labels><Label Quantity="3"><Record FieldName="Name" Value="A"/></Label></Labels></Data>)";
    LabelBatch b2; std::string err2;
    CHECK(LabelXml::ParseLabelsTable(xml2, b2, err2), "parse without Formatting ok");
    CHECK(!b2.formatting.has_value() && b2.labels.size() == 1 && b2.labels[0].quantity == 3, "no formatting, labels cached upstream");

    // ConnectionParameters
    const char* cp = R"(<?xml version="1.0" encoding="UTF-8"?><Parameters>
      <Parameter Name="TransportKind" Value="tcp"/>
      <Parameter Name="Host" Value="192.168.0.50"/>
      <Parameter Name="Port" Value="9100"/>
      <Parameter Name="DotsPerMm" Value="12"/>
      <Parameter Name="Darkness" Value="20"/>
      <Parameter Name="UnknownParam" Value="ignore-me"/>
    </Parameters>)";
    DeviceProfile dp; std::string errc;
    CHECK(LabelXml::ParseConnectionParameters(cp, dp, errc), "parse ConnectionParameters ok");
    CHECK(dp.transport == DeviceProfile::Transport::Tcp, "TransportKind=tcp");
    CHECK(dp.host == "192.168.0.50" && dp.port == 9100, "host+port parsed");
    CHECK(dp.dotsPerMm == 12 && dp.darkness == 20, "dotsPerMm+darkness parsed, unknown ignored");

    // Некоректний XML -> false + err
    LabelBatch bbad; std::string errbad;
    CHECK(!LabelXml::ParseLabelsTable("<Data><Labels>", bbad, errbad) && !errbad.empty(), "malformed XML -> fail with err");
}

// Мінімальний мок платформи 1С (IMemoryManager/IAddInDefBase) — достатньо, щоб
// інстанціювати компоненту в процесі й перевірити реєстрацію БПО-методів.
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
    AddInNative* comp = AddInNative::CreateObject(u"LabelPrinter");
    CHECK(comp != nullptr, "Facade: CreateObject(LabelPrinter) -> не null");
    if (!comp) return;
    MockMem mem; MockConn conn;
    comp->Init(&conn);
    comp->setMemManager(&mem);
    CHECK(comp->GetNMethods() >= 8, "Facade: зареєстровано >=8 БПО-методів");
    delete comp;
}

// L-p2: наскрізний шлях драйвер -> реальний TransportTCP -> LabelEmulator.
// Друк іде у справжній сокет; емулятор в окремому потоці накопичує ZPL.
static void TestTransportE2E() {
    GdiplusRuntime gdi;
    LabelEmulator emu;
    CHECK(emu.Start(0), "emulator started");
    LabelPrinterDriver drv;
    DeviceProfile dp;
    dp.transport = DeviceProfile::Transport::Tcp;
    dp.host = "127.0.0.1"; dp.port = emu.Port(); dp.dotsPerMm = 8;
    std::string id = drv.Connect(dp);

    LabelBatch b;
    LabelFormatting fmt; fmt.width = 60; fmt.height = 40;
    BarcodeField bc; bc.fieldName = "Bar"; bc.type = "EAN13"; bc.geom = {1, 22, 0, 10, 0};
    fmt.barcodes.push_back(bc);
    b.formatting = fmt;
    b.labels.push_back({1, { {"Bar", std::string("4008110271538")} }});
    CHECK(drv.PrintLabels(id, b, "first").ok, "print ok over TCP");

    // Дати сокету доставити: чекаємо на повний кадр (^XZ) з коротким ретраєм.
    std::string z;
    for (int i = 0; i < 50; ++i) {
        z = emu.LastZpl();
        if (z.find("^XZ") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(z.find("^XA") != std::string::npos, "emulator received ZPL start ^XA");
    CHECK(z.find("^BE") != std::string::npos, "emulator received native EAN13 ^BE");

    drv.Disconnect(id);
    emu.Stop();
}

// Фікс #1: стилі шрифту + вирівнювання застосовуються в растрі тексту.
static void TestRasterFontStyle() {
    GdiplusRuntime gdi;
    auto blackPixels = [&](const std::string& style) -> long {
        LabelFormatting fmt; fmt.width = 30; fmt.height = 10;
        TextField tx; tx.fieldName = "T"; tx.geom = {1, 1, 28, 8};
        tx.fontName = "Arial"; tx.fontSize = 8; tx.fontStyle = style;
        tx.align = "Center"; tx.isStatic = true; tx.defaultOrStaticValue = "Ціна 100";
        fmt.texts.push_back(tx);
        auto valueOf = [&](const std::string&) -> std::optional<std::string> { return std::string("Ціна 100"); };
        bool ok = false;
        Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8, ok);
        if (!ok) return -1;
        long n = 0; for (auto b : bm.rows) for (int i = 0; i < 8; ++i) n += (b >> i) & 1;
        return n;
    };
    long reg = blackPixels("");
    long bold = blackPixels("Bold");
    CHECK(reg > 0, "styled/aligned text render ok (Center align path)");
    CHECK(bold > reg, "Bold produces more black pixels than Regular (FontStyle applied)");
}

// Фікс #4: часткове-друку accounting доходить у description помилки (для GetLastError у 1С).
static void TestPartialPrintAccounting() {
    GdiplusRuntime gdi;
    LabelPrinterDriver drv;
    int sendCount = 0;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&) -> std::unique_ptr<ITransport> {
        struct Fake : ITransport {
            int* n; bool open = false;
            bool Open() override { open = true; return true; }
            bool Close() override { open = false; return true; }
            bool IsOpen() const override { return open; }
            int Send(const std::vector<uint8_t>& d) override { ++(*n); return (*n >= 2) ? -1 : (int)d.size(); }
            void SetDataReceivedCallback(DataReceivedCallback) override {}
            void SetErrorCallback(ErrorCallback) override {}
            void SetConnectionStateCallback(ConnectionStateCallback) override {}
        };
        auto f = std::make_unique<Fake>(); f->n = &sendCount; return f;
    });
    DeviceProfile dp; dp.dotsPerMm = 8;
    std::string id = drv.Connect(dp);
    LabelBatch b; LabelFormatting fmt; fmt.width = 60; fmt.height = 40; b.formatting = fmt;
    b.labels.push_back({1, {}});
    b.labels.push_back({1, {}});
    b.labels.push_back({1, {}});
    ResultEnvelope r = drv.PrintLabels(id, b, "first");
    CHECK(!r.ok, "partial send failure -> not ok");
    CHECK(r.description.find("прийнято 1 з 3") != std::string::npos, "error description carries 'accepted N of M'");
    drv.Disconnect(id);
}

int main() {
    TestUnits();
    TestGfEncoder();
    TestGfTiling();
    TestBarcodeZpl();
    TestEan128Fnc1();
    TestBarcodeModuleWidth();
    TestBarcodeOrientation();
    TestDeferredBarcodeTypes();
    TestStaticBarcodeBase64();
    TestRaster();
    TestRasterImage();
    TestGenerator();
    TestRenderErrorPropagates();
    TestSpooler();
    TestDriverBatch();
    TestDriverTextRasterNoGdiplus();
    TestTransportErrorTaxonomy();
    TestDriverConcurrentDisconnect();
    TestXml();
    TestFacadeSmoke();
    TestTransportE2E();
    TestRasterFontStyle();
    TestPartialPrintAccounting();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
