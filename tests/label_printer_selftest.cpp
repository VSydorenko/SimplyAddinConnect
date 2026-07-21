#include <cstdio>
#include <string>
#include <optional>
#include "../src/drivers/label_printer/LabelModel.h"
#include "../src/drivers/label_printer/LabelUnits.h"
#include "../src/drivers/label_printer/GfEncoder.h"
#include "../src/drivers/label_printer/BarcodeZpl.h"
#include "../src/drivers/label_printer/LabelRaster.h"
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
    Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8);
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
    Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8);
    CHECK(bm.widthDots == 160 && bm.heightDots == 80 && !bm.rows.empty(), "image raster non-empty with correct size");
    CHECK(CountBlack(bm) > 0, "decoded PNG image produced black pixels");
}

int main() {
    TestUnits();
    TestGfEncoder();
    TestGfTiling();
    TestBarcodeZpl();
    TestRaster();
    TestRasterImage();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
