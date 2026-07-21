#include <cstdio>
#include <string>
#include "../src/drivers/label_printer/LabelUnits.h"
#include "../src/drivers/label_printer/GfEncoder.h"
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

int main() {
    TestUnits();
    TestGfEncoder();
    TestGfTiling();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
