# Label Printer Driver (LabelPrinter) Implementation Plan

> **СТАТУС: ВИКОНАНО** (2026-07-22, через `/виконай-задачу` — Workflow-оркестрація).
> Усі Task 1–13 реалізовано, пройдено адверсарне код-рев'ю (12 CONFIRMED знахідок усунено),
> синхронізовано документацію. **Гейт зелений на обох архітектурах:** `build_project.ps1 -WithTests`
> (x86+x64) + `run_tests.ps1 -NoUapki x64/x86` → PASS=8, FAIL/BLOCKED=0; L-p1 86 CHECK, L-p3 11 CHECK,
> DLL-експортів рівно 3. Коміти `b257d80..aa3c011` на гілці `design-label-printer`.
> TDD «red-кроки» (запусти-й-переконайся-що-падає) свідомо згорнуто у фінальну зелену верифікацію.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Реалізувати нативну 1С-компоненту «Принтер этикеток» (тип БПО `LabelPrinter`), що приймає стандартний XML-пакет `LabelsTable` і друкує етикетки на ZPL-принтер через spooler-RAW або TCP:9100, з гібридним рендером (нативні штрихкоди + растр GDI+ у `^GF`).

**Architecture:** Facade-агностичний драйвер поверх наявного ядра (`AddInNative`/`ResultEnvelope`/`ITransport`) за шаблоном ECRPrivatJSON. Шари: `AddinLabelPrinter` (БПО-фасад, XML↔BOOL/GetLastError) → `LabelPrinterDriver` (мульти-DeviceID, batch state machine) → `LabelZplGenerator`+`LabelRaster` (LabelModel→ZPL) → `ITransport` (`TransportSpoolerRaw`/`TransportTCP`). `DeviceSession`/`JobEngine` НЕ використовуються (друк односпрямований, синхронний).

**Tech Stack:** C++17, CMake (OBJECT-бібліотеки), GDI+ (`gdiplus.lib`), Winspool (`winspool.lib`), `pugixml` (XML, MIT), `nlohmann/json` (наявний, для `ResultEnvelope`). Тести — консольні exe з CHECK-макросами (стиль `tests/ecr_privatjson_selftest.cpp`), гейт = exit 0.

## Global Constraints

- **Спека:** `docs/superpowers/specs/2026-07-20-label-printer-driver-design.md` — джерело правди; при розбіжності план поступається спеці.
- **Ядро НЕ змінюємо** (`src/core/`, `src/platform/`, `src/transport/Transport*.{h,cpp}`, `DeviceSession`). Дозволено лише ДОДАВати: `Transport_SpoolerRaw.{h,cpp}`, `src/drivers/label_printer/*`, `src/components/AddinLabelPrinter.*`, тестові файли, правки `CMake/components.cmake` та `tests/CMakeLists.txt`.
- **PCH:** `#include "../core/pch.h"` (або `../../core/pch.h`) — ПЕРШИЙ рядок кожного `.cpp`; у `.h` — ніколи.
- **Логування:** лише макроси `ServiceTools` (`REPORT_*` у фасаді; `NEUTRAL_REPORT_*` у драйвері/растрі/транспорті — 1-й арг = ім'я компоненти). Повідомлення — конкатенацією, з великої, без крапки. printf-стиль заборонено.
- **Помилки:** бізнес-помилки → `ResultEnvelope::Fail(code, description)`, не `throw` за межу 1С; код, що може кинути, — у try/catch з `Fail` у catch.
- **Рядки:** `ServiceTools::SafeMB2WCHAR`/`SafeWCHAR2MB`; вхід тексту з 1С — UTF-16 через `VH`.
- **Одиниці:** роздільність — `dotsPerMm ∈ {8,12,24}` (не номінальний DPI). `mmToDots(mm) = lround(mm * dotsPerMm)`. `ptToDots(pt) = lround(pt * dotsPerMm * 25.4 / 72.0)`.
- **Платформа:** Windows x86 + x64, Release. Збірка — `build_project.ps1 -WithTests` (без UAPKI). Гейт — `run_tests.ps1 -NoUapki [x64|x86]`.
- **Мова коду/коментарів:** українська/російська (дотримуйся мови сусідніх файлів; шаблон ECR — укр.).
- **Штрихкоди:** підтримувані типи — нативні ZPL; `Code16K` та інші без нативної команди → `ResultEnvelope::Fail("UNSUPPORTED_BARCODE", …)`. Растрового фолбеку штрихкодів НЕМАЄ.
- **Семантика результату:** успіх = *прийнято* (spooler: job у черзі; tcp: байти в сокет), НЕ фізичний друк. Payload: `acceptedInstances`, `acceptedCopies`. Термін "printed" не вживати.

---

## File Structure

| Файл | Відповідальність |
|---|---|
| `src/drivers/label_printer/LabelModel.h` | Типізовані POD-структури етикетки (дзеркало `LabelsTable`) + `DeviceProfile` |
| `src/drivers/label_printer/LabelUnits.h` | `mmToDots`/`ptToDots` (inline, чисті функції) |
| `src/drivers/label_printer/GfEncoder.{h,cpp}` | 1-bit bitmap → `^GFA` (з тайлінгом); детерміновано, без GDI+ |
| `src/drivers/label_printer/LabelRaster.{h,cpp}` | текст/картинка/рамка → 1-bit bitmap (GDI+); RAII GDI+ runtime |
| `src/drivers/label_printer/BarcodeZpl.{h,cpp}` | тип символіки → нативна ZPL-команда; `^FD`-escaping; `UNSUPPORTED` |
| `src/drivers/label_printer/LabelZplGenerator.{h,cpp}` | `LabelModel`+`DeviceProfile` → повний ZPL (`^XA…^XZ`); `InitializePrinter` |
| `src/drivers/label_printer/LabelXml.{h,cpp}` | `LabelsTable`/`ConnectionParameters` XML → `LabelModel`/`DeviceProfile` (pugixml) |
| `src/drivers/label_printer/LabelPrinterDriver.{h,cpp}` | оркестрація: `map<DeviceID>`, batch SM, `MapResult`, concurrency |
| `src/transport/Transport_SpoolerRaw.{h,cpp}` | Winspool RAW `ITransport` + тест-шов |
| `src/components/AddinLabelPrinter.{h,cpp}` | БПО-фасад (системні + функціональні методи, XML I/O, GetLastError) |
| `tests/support/LabelEmulator.{h,cpp}` | TCP-емулятор-захоплювач ZPL (для e2e + standalone) |
| `tests/label_printer_selftest.cpp` | L-p1/L-p2 selftest (CHECK-и + transport-e2e) |
| `tests/label_printer_emulator.cpp` | standalone EXE емулятора для ручного тесту з 1С |
| `tests/label_native_host.cpp` | L-p3: DLL → компонента `LabelPrinter` → БПО-методи проти емулятора |
| `extern/pugixml/` | сабмодуль pugixml (MIT) |

---

## Task 1: Каркас (моделі, одиниці, CMake, порожній selftest)

**Files:**
- Create: `src/drivers/label_printer/LabelModel.h`
- Create: `src/drivers/label_printer/LabelUnits.h`
- Create: `tests/label_printer_selftest.cpp`
- Modify: `CMake/components.cmake` (нова OBJECT-ціль `driver_label_printer_component`)
- Modify: `tests/CMakeLists.txt` (ціль `label_printer_selftest`)

**Interfaces:**
- Produces: структури `TextField/BarcodeField/ImageField/UserDataField/LabelFormatting/LabelRecord/LabelInstance/LabelBatch/DeviceProfile`; `mmToDots(double,int)->long`; `ptToDots(double,int)->long`.

- [x] **Step 1: Створити `LabelModel.h`** (дзеркало §5 спеки, value-семантика через `std::optional`)

```cpp
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace labelprinter {

struct FieldGeom { double left=0, top=0, width=0, height=0; int orientation=0; }; // мм; 0/90/180/270

struct TextField {
    std::string fieldName; FieldGeom geom;
    std::string fontName; int fontSize=0; std::string fontStyle;      // "Bold Italic Underline StrikeOut"
    std::string align="Left", vAlign="Top"; bool multiline=false;
    std::string border; int borderWidth=1; std::string borderStyle="Solid";
    bool isStatic=false;
    std::optional<std::string> defaultOrStaticValue;                  // Formatting.Value
};
struct BarcodeField {
    std::string fieldName; FieldGeom geom;
    std::string type; bool printHRI=true; int hriFontSize=0; bool checkSymbol=true;
    bool isStatic=false;
    std::optional<std::string> staticValueBase64;                    // Formatting.ValueBase64 (static)
};
struct ImageField {
    std::string fieldName; FieldGeom geom;
    std::string border; int borderWidth=1; std::string borderStyle="Solid";
    bool isStatic=false;
    std::optional<std::string> staticValueBase64;                    // Formatting.Value (Base64, static)
};
struct UserDataField { std::string fieldName; bool isStatic=false; std::optional<std::string> defaultOrStaticValue; };

struct LabelFormatting {
    double width=0, height=0;                                        // мм
    std::vector<TextField> texts; std::vector<BarcodeField> barcodes;
    std::vector<ImageField> images; std::vector<UserDataField> userData;
};
struct LabelRecord { std::string fieldName; std::optional<std::string> value; }; // відсутній ≠ порожній
struct LabelInstance { int quantity=1; std::vector<LabelRecord> records; };      // Image record.value — Base64
struct LabelBatch { std::optional<LabelFormatting> formatting; std::vector<LabelInstance> labels; };

struct DeviceProfile {
    enum class Transport { Spooler, Tcp } transport = Transport::Spooler;
    std::string printerName;                                         // spooler
    std::string host; int port = 9100;                              // tcp
    int dotsPerMm = 8;                                              // 8/12/24
    int darkness = 10; int speed = 4;                              // per-model діапазони
    double labelWidthMm = 0, labelHeightMm = 0; int homeXDots = 0, homeYDots = 0;
};

} // namespace labelprinter
```

- [x] **Step 2: Створити `LabelUnits.h`**

```cpp
#pragma once
#include <cmath>
namespace labelprinter {
inline long mmToDots(double mm, int dotsPerMm) { return std::lround(mm * dotsPerMm); }
inline long ptToDots(double pt, int dotsPerMm) { return std::lround(pt * dotsPerMm * 25.4 / 72.0); }
} // namespace labelprinter
```

- [x] **Step 3: Створити `tests/label_printer_selftest.cpp` (скелет + CHECK-макрос + перший тест одиниць)**

```cpp
#include <cstdio>
#include <string>
#include "../src/drivers/label_printer/LabelUnits.h"
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

int main() {
    TestUnits();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
```

- [x] **Step 4: Додати OBJECT-ціль у `CMake/components.cmake`** (за зразком `driver_ecr_privatjson_component`, рядки ~184-205; поки лише хедери — `.cpp` додаватимуться в наступних тасках)

```cmake
add_library(driver_label_printer_component OBJECT
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/LabelModel.h
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/LabelUnits.h
)
set_target_properties(driver_label_printer_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_include_directories(driver_label_printer_component PRIVATE
    include ${CMAKE_SOURCE_DIR} src ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(driver_label_printer_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(driver_label_printer_component base_component spdlog nlohmann_json)
```

(Ще НЕ додавати `$<TARGET_OBJECTS:driver_label_printer_component>` у фінальну DLL — зробимо в Task 12, коли зʼявиться фасад.)

- [x] **Step 5: Додати тестову ціль у `tests/CMakeLists.txt`** (за зразком `ecr_privatjson_selftest`, рядки ~84-112)

```cmake
add_executable(label_printer_selftest ${CMAKE_SOURCE_DIR}/tests/label_printer_selftest.cpp)
target_include_directories(label_printer_selftest PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
set_target_properties(label_printer_selftest PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
```

- [x] **Step 6: Зібрати й запустити**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests`
Then: `bin/Release/label_printer_selftest_x64.exe`
Expected: усі `[PASS]`, `ALL PASS`, exit 0.

- [x] **Step 7: Commit**

```bash
git add src/drivers/label_printer/LabelModel.h src/drivers/label_printer/LabelUnits.h tests/label_printer_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "feat(label): каркас драйвера принтера етикеток — LabelModel, одиниці, selftest"
```

---

## Task 2: `GfEncoder` — 1-bit bitmap → `^GFA` (детерміновано, з тайлінгом)

**Files:**
- Create: `src/drivers/label_printer/GfEncoder.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (додати `TestGfEncoder`)
- Modify: `CMake/components.cmake` (додати `GfEncoder.cpp` у ціль)

**Interfaces:**
- Consumes: —
- Produces: `struct Bitmap1{ int widthDots, heightDots; std::vector<uint8_t> rows; };` (rows = packed 1bpp, MSB-first, `bytesPerRow=ceil(w/8)`, 1=чорний); `std::string GfEncoder::EncodeGfa(const Bitmap1&, int xDots, int yDots, int maxRowsPerTile);` → рядок ZPL із одного або кількох `^FO..^GFA..^FS` (тайлінг за `maxRowsPerTile`).

- [x] **Step 1: Написати тест `TestGfEncoder`** (2×1-байтна рядок-матриця → відомий hex)

```cpp
// у selftest, #include "../src/drivers/label_printer/GfEncoder.h"
static void TestGfEncoder() {
    // 8x2, усі чорні: bytesPerRow=1, total=2, hex "FF" на рядок
    Bitmap1 bm{8, 2, {0xFF, 0xFF}};
    std::string z = GfEncoder::EncodeGfa(bm, 0, 0, /*maxRowsPerTile=*/1000);
    CHECK(z.find("^FO0,0") != std::string::npos, "GFA has origin");
    CHECK(z.find("^GFA,2,2,1,") != std::string::npos, "GFA header a,b=2,c=2,d=1");
    CHECK(z.find("FFFF") != std::string::npos, "GFA data hex FFFF");
    CHECK(z.find("^FS") != std::string::npos, "GFA closed with ^FS");
}
```

- [x] **Step 2: Запустити — має впасти на компіляції** (немає `GfEncoder.h`)

Run: `powershell -File build_project.ps1 -WithTests`
Expected: помилка компіляції `GfEncoder.h: No such file`.

- [x] **Step 3: Створити `GfEncoder.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace labelprinter {
struct Bitmap1 { int widthDots=0, heightDots=0; std::vector<uint8_t> rows; }; // 1bpp MSB-first, 1=чорний
class GfEncoder {
public:
    // Один або кілька ^FO..^GFA..^FS; тайлінг по рядках, якщо heightDots > maxRowsPerTile.
    static std::string EncodeGfa(const Bitmap1& bm, int xDots, int yDots, int maxRowsPerTile);
};
} // namespace labelprinter
```

- [x] **Step 4: Створити `GfEncoder.cpp`**

```cpp
#include "../../core/pch.h"
#include "GfEncoder.h"
#include <cstdio>
namespace labelprinter {
static void appendHex(std::string& out, const uint8_t* p, size_t n) {
    static const char* H = "0123456789ABCDEF";
    for (size_t i=0;i<n;++i){ out.push_back(H[p[i]>>4]); out.push_back(H[p[i]&0xF]); }
}
std::string GfEncoder::EncodeGfa(const Bitmap1& bm, int xDots, int yDots, int maxRowsPerTile) {
    const int bpr = (bm.widthDots + 7) / 8;
    std::string out;
    int row = 0;
    while (row < bm.heightDots) {
        const int tileRows = std::min(maxRowsPerTile, bm.heightDots - row);
        const int total = bpr * tileRows;
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "^FO%d,%d^GFA,%d,%d,%d,", xDots, yDots + row, total, total, bpr);
        out += hdr;
        appendHex(out, bm.rows.data() + static_cast<size_t>(row) * bpr, static_cast<size_t>(total));
        out += "^FS";
        row += tileRows;
    }
    return out;
}
} // namespace labelprinter
```

- [x] **Step 5: Додати `GfEncoder.cpp` у CMake-ціль**

```cmake
# у add_library(driver_label_printer_component OBJECT ... ) додати рядок:
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/GfEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/GfEncoder.h
# і залінкувати selftest із об'єктами драйвера — у tests/CMakeLists.txt ціль label_printer_selftest:
# target_sources(label_printer_selftest PRIVATE $<TARGET_OBJECTS:driver_label_printer_component>)
```

- [x] **Step 6: Зібрати й запустити — PASS**

Run: `powershell -File build_project.ps1 -WithTests` → `bin/Release/label_printer_selftest_x64.exe`
Expected: `TestGfEncoder` усі `[PASS]`, exit 0.

- [x] **Step 7: Додати тест тайлінгу**

```cpp
static void TestGfTiling() {
    Bitmap1 bm{8, 5, {0xFF,0xFF,0xFF,0xFF,0xFF}};
    std::string z = GfEncoder::EncodeGfa(bm, 10, 20, /*maxRowsPerTile=*/2);
    // 5 рядків / 2 = 3 тайли (2+2+1)
    size_t n=0, pos=0; while ((pos=z.find("^GFA", pos))!=std::string::npos){++n;++pos;}
    CHECK(n == 3, "tiling splits 5 rows @2/tile into 3 ^GFA");
    CHECK(z.find("^FO10,20")!=std::string::npos && z.find("^FO10,22")!=std::string::npos && z.find("^FO10,24")!=std::string::npos, "tile origins offset by rows");
}
```

- [x] **Step 8: Запустити — PASS, потім Commit**

Run: rebuild + run selftest → усі PASS.
```bash
git add src/drivers/label_printer/GfEncoder.h src/drivers/label_printer/GfEncoder.cpp tests/label_printer_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "feat(label): ^GFA-енкодер 1-bit bitmap із тайлінгом + golden-тести"
```

---

## Task 3: `BarcodeZpl` — тип символіки → нативна ZPL-команда + `^FD`-escaping

**Files:**
- Create: `src/drivers/label_printer/BarcodeZpl.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestBarcodeZpl`)
- Modify: `CMake/components.cmake`

**Interfaces:**
- Produces: `struct BarcodeEmit{ bool ok; std::string zpl; std::string errCode; std::string errDesc; };`
  `BarcodeZpl::Emit(const BarcodeField&, const std::string& value, int dotsPerMm) -> BarcodeEmit`. Успіх → ZPL-фрагмент `^FOx,y^BY..<cmd>^FD<escaped>^FS`; невідомий тип → `ok=false, errCode="UNSUPPORTED_BARCODE"`.
  `std::string BarcodeZpl::EscapeFd(const std::string& raw)` — hex-escaping через `^FH` для `^ ~ >` і control-байтів.

- [x] **Step 1: Тести** (мапінг + UNSUPPORTED + escaping + нормалізація EAN13)

```cpp
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
```

- [x] **Step 2: Запустити — FAIL (компіляція)**. Run build; Expected: `BarcodeZpl.h` not found.

- [x] **Step 3: `BarcodeZpl.h`**

```cpp
#pragma once
#include <string>
#include "LabelModel.h"
namespace labelprinter {
struct BarcodeEmit { bool ok=false; std::string zpl, errCode, errDesc; };
class BarcodeZpl {
public:
    static BarcodeEmit Emit(const BarcodeField& f, const std::string& value, int dotsPerMm);
    static std::string EscapeFd(const std::string& raw); // ^FH hex-escaping для ^ ~ > і control
};
} // namespace labelprinter
```

- [x] **Step 4: `BarcodeZpl.cpp`** (мапінг за виправленою таблицею §6.1; мінімальна нормалізація/escaping)

```cpp
#include "../../core/pch.h"
#include "BarcodeZpl.h"
#include "LabelUnits.h"
#include <cstdio>
#include <cctype>
namespace labelprinter {

std::string BarcodeZpl::EscapeFd(const std::string& raw) {
    bool need=false; for (unsigned char c: raw) if (c=='^'||c=='~'||c=='>'||c<0x20){need=true;break;}
    if (!need) return "^FD" + raw;
    static const char* H="0123456789ABCDEF";
    std::string out="^FH_^FD";
    for (unsigned char c: raw) {
        if (c=='^'||c=='~'||c=='>'||c<0x20||c=='_') { out.push_back('_'); out.push_back(H[c>>4]); out.push_back(H[c&0xF]); }
        else out.push_back((char)c);
    }
    return out;
}

// повертає рядок цифр без нецифрових символів
static std::string digitsOnly(const std::string& s){ std::string o; for(char c:s) if(std::isdigit((unsigned char)c)) o.push_back(c); return o; }

BarcodeEmit BarcodeZpl::Emit(const BarcodeField& f, const std::string& value, int dotsPerMm) {
    BarcodeEmit e;
    const long x = mmToDots(f.geom.left, dotsPerMm), y = mmToDots(f.geom.top, dotsPerMm);
    const long h = mmToDots(f.geom.height, dotsPerMm);
    const char hri = f.printHRI ? 'Y' : 'N';
    char buf[256];
    const std::string& t = f.type;
    std::string cmd, data = value;

    if (t=="EAN13") {                       // ^BE приймає 12 цифр
        std::string d = digitsOnly(value); if (d.size()==13) d = d.substr(0,12);
        std::snprintf(buf,sizeof(buf),"^BEN,%ld,%c,N", h, hri); cmd=buf; data=d;
    } else if (t=="EAN8") {
        std::string d = digitsOnly(value); if (d.size()==8) d=d.substr(0,7);
        std::snprintf(buf,sizeof(buf),"^B8N,%ld,%c,N", h, hri); cmd=buf; data=d;
    } else if (t=="Code128"||t=="EAN128") {
        std::snprintf(buf,sizeof(buf),"^BCN,%ld,%c,N,N", h, hri); cmd=buf;
        if (t=="EAN128") data = ">;>8" + value;  // FNC1 (GS1-128); повний GS1-препроцесор — окремо
    } else if (t=="Code39") { std::snprintf(buf,sizeof(buf),"^B3N,N,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="Code93")  { std::snprintf(buf,sizeof(buf),"^BAN,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="ITF14")   { std::snprintf(buf,sizeof(buf),"^B2N,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="QRCode")  { cmd="^BQN,2,5"; }
    else if (t=="DataMatrix"){ cmd="^BXN,5,200"; }
    else if (t=="PDF417")  { cmd="^B7N,5,5"; }
    else if (t=="GS1DataBarExpandedStacked"){ std::snprintf(buf,sizeof(buf),"^BRN,6,5,2,,%ld",h); cmd=buf; }
    else { e.ok=false; e.errCode="UNSUPPORTED_BARCODE"; e.errDesc="Тип штрихкоду не підтримується нативно: "+t; return e; }

    std::snprintf(buf,sizeof(buf),"^FO%ld,%ld^BY2", x, y);
    e.zpl = std::string(buf) + cmd + EscapeFd(data) + "^FS";
    e.ok = true; return e;
}
} // namespace labelprinter
```

- [x] **Step 5: CMake** — додати `BarcodeZpl.cpp/.h` у ціль (як у Task 2 Step 5).

- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add src/drivers/label_printer/BarcodeZpl.h src/drivers/label_printer/BarcodeZpl.cpp tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(label): нативний ZPL-мапінг штрихкодів (виправлений) + ^FH-escaping + UNSUPPORTED"
```

---

## Task 4: `LabelRaster` — текст/картинка/рамка → 1-bit (GDI+)

**Files:**
- Create: `src/drivers/label_printer/LabelRaster.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestRaster` — структурні перевірки, не golden-байти)
- Modify: `CMake/components.cmake`

**Interfaces:**
- Consumes: `Bitmap1` (Task 2), `LabelFormatting`/`*Field` (Task 1), одиниці (Task 1).
- Produces: `class GdiplusRuntime { public: GdiplusRuntime(); ~GdiplusRuntime(); };` (RAII, process-wide ref-counted, НЕ з DllMain);
  `Bitmap1 LabelRaster::Render(const LabelFormatting& fmt, const std::function<std::optional<std::string>(const std::string&)>& valueOf, int dotsPerMm);` — рендерить лише Text/Image/Border у 1-bit розміром `mmToDots(width)×mmToDots(height)`; ділянки штрихкодів лишає порожніми; `valueOf(fieldName)` дає значення поля (default/record).

- [x] **Step 1: Тест `TestRaster`** (розмір + непорожність; шрифт — bundled `tests/data/DejaVuSans.ttf` за наявності, інакше системний Arial із tolerance)

```cpp
static void TestRaster() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width=20; fmt.height=10;
    TextField tx; tx.fieldName="T"; tx.geom={1,1,18,8}; tx.fontName="Arial"; tx.fontSize=8;
    tx.defaultOrStaticValue="Тест"; tx.isStatic=true; fmt.texts.push_back(tx);
    auto valueOf = [&](const std::string&)->std::optional<std::string>{ return std::string("Тест"); };
    Bitmap1 bm = LabelRaster::Render(fmt, valueOf, 8);
    CHECK(bm.widthDots==160 && bm.heightDots==80, "raster size = label mm * dpmm");
    long ones=0; for(auto b: bm.rows) for(int i=0;i<8;i++) ones += (b>>i)&1;
    CHECK(ones > 0, "rendered text produced black pixels");
}
```

- [x] **Step 2: Запустити — FAIL (немає `LabelRaster.h`).**

- [x] **Step 3: `LabelRaster.h`**

```cpp
#pragma once
#include <functional>
#include <optional>
#include <string>
#include "LabelModel.h"
#include "GfEncoder.h"
namespace labelprinter {
class GdiplusRuntime {                        // process-wide ref-counted, RAII (НЕ з DllMain)
public: GdiplusRuntime(); ~GdiplusRuntime();
        GdiplusRuntime(const GdiplusRuntime&)=delete; GdiplusRuntime& operator=(const GdiplusRuntime&)=delete;
};
class LabelRaster {
public:
    using ValueOf = std::function<std::optional<std::string>(const std::string&)>;
    static Bitmap1 Render(const LabelFormatting& fmt, const ValueOf& valueOf, int dotsPerMm);
};
} // namespace labelprinter
```

- [x] **Step 4: `LabelRaster.cpp`** (GDI+; grayscale→threshold у 1-bit; UnitPixel; порядок біт MSB-first)

```cpp
#include "../../core/pch.h"
#include "LabelRaster.h"
#include "LabelUnits.h"
#include "../../helpers/ServiceTools.h"
#include <windows.h>
#include <gdiplus.h>
#include <atomic>
#include <mutex>
namespace labelprinter {
using namespace Gdiplus;

static std::mutex g_gdiMutex; static std::atomic<int> g_gdiRefs{0}; static ULONG_PTR g_gdiToken=0;
GdiplusRuntime::GdiplusRuntime(){ std::lock_guard<std::mutex> lk(g_gdiMutex);
    if (g_gdiRefs.fetch_add(1)==0){ GdiplusStartupInput in; GdiplusStartup(&g_gdiToken,&in,nullptr); } }
GdiplusRuntime::~GdiplusRuntime(){ std::lock_guard<std::mutex> lk(g_gdiMutex);
    if (g_gdiRefs.fetch_sub(1)==1){ GdiplusShutdown(g_gdiToken); g_gdiToken=0; } }

Bitmap1 LabelRaster::Render(const LabelFormatting& fmt, const ValueOf& valueOf, int dotsPerMm) {
    const int W = (int)mmToDots(fmt.width, dotsPerMm), H = (int)mmToDots(fmt.height, dotsPerMm);
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(255,255,255,255));
    g.SetTextRenderingHint(TextRenderingHintSingleBitPerPixelGridFit); // без антиаліасу
    SolidBrush black(Color(255,0,0,0));
    for (const auto& t : fmt.texts) {
        auto v = t.isStatic ? t.defaultOrStaticValue : valueOf(t.fieldName);
        if (!v) v = t.defaultOrStaticValue;                 // дефолт при non-static
        if (!v) continue;
        std::wstring w = ServiceTools::SafeMB2WCHAR(*v);     // UTF-8 -> UTF-16
        std::wstring fam = ServiceTools::SafeMB2WCHAR(t.fontName.empty() ? std::string("Arial") : t.fontName);
        FontFamily ff(fam.c_str());
        Font font(&ff, (REAL)ptToDots(t.fontSize, dotsPerMm), FontStyleRegular, UnitPixel);
        RectF rc((REAL)mmToDots(t.geom.left,dotsPerMm),(REAL)mmToDots(t.geom.top,dotsPerMm),
                 (REAL)mmToDots(t.geom.width,dotsPerMm),(REAL)mmToDots(t.geom.height,dotsPerMm));
        g.DrawString(w.c_str(), -1, &font, rc, nullptr, &black);
    }
    // TODO у наступних кроках: Image (Base64->Image), Border (DrawRectangle). Зараз — текст.

    // 32bpp -> 1bpp threshold
    const int bpr=(W+7)/8; Bitmap1 out; out.widthDots=W; out.heightDots=H; out.rows.assign((size_t)bpr*H,0);
    BitmapData bd; Rect r(0,0,W,H); bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd);
    for (int yy=0; yy<H; ++yy){ const uint8_t* px=(const uint8_t*)bd.Scan0 + (size_t)yy*bd.Stride;
        for (int xx=0; xx<W; ++xx){ int lum=(px[xx*4+2]+px[xx*4+1]+px[xx*4+0])/3;
            if (lum < 128) out.rows[(size_t)yy*bpr + xx/8] |= (uint8_t)(0x80 >> (xx%8)); } }
    bmp.UnlockBits(&bd);
    return out;
}
} // namespace labelprinter
```

- [x] **Step 5: CMake** — додати `LabelRaster.cpp/.h`; **selftest лінкує `gdiplus`**:

```cmake
# tests/CMakeLists.txt, ціль label_printer_selftest:
target_link_libraries(label_printer_selftest PRIVATE gdiplus)
```

- [x] **Step 6: Зібрати, запустити — PASS.**

Run: rebuild; run selftest. Expected: `TestRaster` PASS (розмір 160×80, чорні пікселі є).

- [x] **Step 7: Додати рендер Image і Border** (два кроки: тест непорожності при Image з валідним Base64 1×1 PNG; імплементація декоду Base64→`IStream`→`Image`, `DrawImage` у прямокутник; `DrawRectangle` для border). Запустити — PASS.

- [x] **Step 8: Commit**

```bash
git add src/drivers/label_printer/LabelRaster.* tests/label_printer_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "feat(label): растр GDI+ (текст/картинка/рамка) -> 1-bit; RAII GDI+ runtime"
```

---

## Task 5: `LabelZplGenerator` — повний ZPL з екземпляра + `InitializePrinter`

**Files:**
- Create: `src/drivers/label_printer/LabelZplGenerator.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestGenerator`)
- Modify: `CMake/components.cmake`

**Interfaces:**
- Consumes: `GfEncoder`, `LabelRaster`, `BarcodeZpl`, `LabelModel`, одиниці.
- Produces:
  `struct GenResult{ bool ok; std::vector<uint8_t> zpl; std::string errCode, errDesc; };`
  `LabelZplGenerator::BuildLabel(const LabelFormatting&, const LabelInstance&, const DeviceProfile&) -> GenResult;`
  `std::vector<uint8_t> LabelZplGenerator::BuildInit(const DeviceProfile&);`
  Внутрішній `valueOf(instance, formatting, fieldName)` реалізує value-семантику (record → default).

- [x] **Step 1: Тест `TestGenerator`** (структура: `^XA`/`^XZ`, `^PQ`, растровий `^GFA`, нативний `^BE`; `UNSUPPORTED` пробрасується)

```cpp
static void TestGenerator() {
    GdiplusRuntime gdi;
    LabelFormatting fmt; fmt.width=60; fmt.height=40;
    TextField tx; tx.fieldName="Name"; tx.geom={1,1,55,10}; tx.fontName="Arial"; tx.fontSize=8; fmt.texts.push_back(tx);
    BarcodeField bc; bc.fieldName="Bar"; bc.type="EAN13"; bc.geom={1,22,0,10,0}; fmt.barcodes.push_back(bc);
    LabelInstance inst; inst.quantity=2;
    inst.records.push_back({"Name", std::string("Блокнот")});
    inst.records.push_back({"Bar",  std::string("4008110271538")});
    DeviceProfile dp; dp.dotsPerMm=8;
    auto r = LabelZplGenerator::BuildLabel(fmt, inst, dp);
    std::string z(r.zpl.begin(), r.zpl.end());
    CHECK(r.ok, "generate ok");
    CHECK(z.rfind("^XA",0)==0 && z.find("^XZ")!=std::string::npos, "wrapped ^XA..^XZ");
    CHECK(z.find("^PQ2")!=std::string::npos, "quantity ^PQ2");
    CHECK(z.find("^GFA")!=std::string::npos, "raster present");
    CHECK(z.find("^BE")!=std::string::npos, "native EAN13 present");

    BarcodeField u=bc; u.type="Code16k"; LabelFormatting f2=fmt; f2.barcodes={u};
    auto r2 = LabelZplGenerator::BuildLabel(f2, inst, dp);
    CHECK(!r2.ok && r2.errCode=="UNSUPPORTED_BARCODE", "unsupported barcode propagates");
}
```

- [x] **Step 2: FAIL** (немає `LabelZplGenerator.h`).

- [x] **Step 3–4: `LabelZplGenerator.{h,cpp}`** — реалізація:
  1) `valueOf`: шукає `record.value` за `fieldName`; якщо відсутній → `defaultOrStaticValue`;
  2) растр усього нетекстового-штрихкодового шару через `LabelRaster::Render` → `GfEncoder::EncodeGfa` (з `maxRowsPerTile` із профілю/ліміту);
  3) кожен `BarcodeField` → `BarcodeZpl::Emit(value)`; при `!ok` → повернути `GenResult{false, {}, errCode, errDesc}`;
  4) зібрати `^XA` + `^GFA…` + бар코ди + `^PQ<quantity>` + `^XZ`.
  `BuildInit`: `^XA` + `~SD<darkness>`/`^MD` + `^PR<speed>` + `^PW`/`^LL` + `^LH` + `^XZ`.

```cpp
// LabelZplGenerator.h
#pragma once
#include <vector>
#include <cstdint>
#include "LabelModel.h"
namespace labelprinter {
struct GenResult { bool ok=false; std::vector<uint8_t> zpl; std::string errCode, errDesc; };
class LabelZplGenerator {
public:
    static GenResult BuildLabel(const LabelFormatting& fmt, const LabelInstance& inst, const DeviceProfile& dp);
    static std::vector<uint8_t> BuildInit(const DeviceProfile& dp);
};
} // namespace labelprinter
```

(Імплементація `.cpp` — за пунктами 1–4 вище; `valueOf` як лямбда, що замикає `inst`+`fmt`.)

- [x] **Step 5: CMake** — додати файли.
- [x] **Step 6: Зібрати, запустити — PASS.**
- [x] **Step 7: Commit**

```bash
git add src/drivers/label_printer/LabelZplGenerator.* tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(label): LabelZplGenerator — повна етикетка (^GF+нативні штрихкоди+^PQ) + InitializePrinter"
```

---

## Task 6: `TransportSpoolerRaw` — Winspool `ITransport`

**Files:**
- Create: `src/transport/Transport_SpoolerRaw.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestSpooler` через тест-шов)
- Modify: `CMake/components.cmake` (додати у `transport_component`; лінк `winspool` — на фінал/тести у Task 12)

**Interfaces:**
- Produces: `class TransportSpoolerRaw : public ITransport` з ctor `explicit TransportSpoolerRaw(std::string printerName);`
  метод `void SetSendFunctionForTest(std::function<int(const std::vector<uint8_t>&)>);` (перехоплення `WritePrinter`-етапу).

- [x] **Step 1: Тест `TestSpooler`** (через тест-шов — без реального принтера)

```cpp
static void TestSpooler() {
    TransportSpoolerRaw t("FakePrinter");
    std::vector<uint8_t> got;
    t.SetSendFunctionForTest([&](const std::vector<uint8_t>& d){ got=d; return (int)d.size(); });
    CHECK(t.Open(), "spooler Open (stubbed)");
    std::vector<uint8_t> payload = {'^','X','A','^','X','Z'};
    CHECK(t.Send(payload)==(int)payload.size(), "Send returns full byte count");
    CHECK(got==payload, "test seam captured exact bytes");
    CHECK(t.IsOpen(), "IsOpen true after Open");
    t.Close();
}
```

- [x] **Step 2: FAIL.**
- [x] **Step 3–4: `Transport_SpoolerRaw.{h,cpp}`** — реалізація:
  - `.h`: успадковує `ITransport`, реалізує `Open/Close/IsOpen/Send` + 3 колбек-сеттери (no-op, лише зберігають), + `SetSendFunctionForTest`.
  - `.cpp`: `Open()` → `OpenPrinterW(name)`; `Send()` → якщо є тест-функція, викликати її; інакше повна послідовність `StartDocPrinterW(DOC_INFO_1{pDatatype=L"RAW", pOutputFile=NULL})` → `StartPagePrinter` → `WritePrinter` (перевірка `pcWritten==size`, all-or-error) → `EndPagePrinter` → `EndDocPrinter`, з teardown залежно від досягнутого стану й збереженням `GetLastError`; повертає к-сть байтів або -1. Логування — `NEUTRAL_REPORT_*`.

- [x] **Step 5: CMake** — додати у `transport_component` (source list). Лінк `winspool` — у Task 12.
- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add src/transport/Transport_SpoolerRaw.* tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(transport): TransportSpoolerRaw (Winspool RAW) з тест-шовом"
```

---

## Task 7: `LabelPrinterDriver` — оркестрація, batch state machine, concurrency

**Files:**
- Create: `src/drivers/label_printer/LabelPrinterDriver.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestDriverBatch`)
- Modify: `CMake/components.cmake`

**Interfaces:**
- Consumes: `LabelZplGenerator`, `ITransport`, `ResultEnvelope`, `DeviceProfile`, `LabelBatch`.
- Produces:
  `std::string Connect(const DeviceProfile&);` → DeviceID (генерується детерміновано, напр. лічильник рядком);
  `ResultEnvelope InitializePrinter(const std::string& deviceId);`
  `ResultEnvelope PrintLabels(const std::string& deviceId, const LabelBatch& batch, const std::string& packageStatus);`
  `void Disconnect(const std::string& deviceId);` `bool IsConnected(const std::string& deviceId) const;`
  Тест-хук: `void SetTransportFactoryForTest(std::function<std::unique_ptr<ITransport>(const DeviceProfile&)>);`

- [x] **Step 1: Тест `TestDriverBatch`** (state machine + мульти-device через фейковий транспорт)

```cpp
static void TestDriverBatch() {
    GdiplusRuntime gdi;
    LabelPrinterDriver drv;
    std::vector<uint8_t> captured;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&){
        struct Fake : ITransport { std::vector<uint8_t>* out; bool open=false;
            bool Open() override {open=true;return true;} bool Close() override {open=false;return true;}
            bool IsOpen() const override {return open;} int Send(const std::vector<uint8_t>& d) override {out->insert(out->end(),d.begin(),d.end());return (int)d.size();}
            void SetDataReceivedCallback(DataReceivedCallback) override{} void SetErrorCallback(ErrorCallback) override{} void SetConnectionStateCallback(ConnectionStateCallback) override{} };
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
    // "regular" без formatting використовує кеш
    LabelBatch b2; b2.labels.push_back({3, {}});
    auto r2 = drv.PrintLabels(id, b2, "last");
    CHECK(r2.ok && r2.payload["acceptedCopies"]==3, "last uses cached formatting, copies=3");
    // після "last" кеш очищено -> "regular" знову fail
    CHECK(!drv.PrintLabels(id, b2, "regular").ok, "cache cleared after last");
    drv.Disconnect(id); CHECK(!drv.IsConnected(id), "disconnected");
}
```

- [x] **Step 2: FAIL.**
- [x] **Step 3–4: `LabelPrinterDriver.{h,cpp}`** — реалізація:
  - `DeviceContext{ std::unique_ptr<ITransport> transport; std::optional<LabelFormatting> cachedFormatting; DeviceProfile profile; std::mutex m; }`;
  - `std::map<std::string, std::unique_ptr<DeviceContext>> devices_; std::mutex registryMutex_;`
  - state machine у `PrintLabels`: `first` → set cache (reset old); `regular`/`last` без cache → `Fail("BAD_INPUT",…)`; для кожного instance → `LabelZplGenerator::BuildLabel` (при `!ok` → повернути з накопиченим `failedIndex`); `transport->Send`; `last` → clear cache;
  - concurrency: per-device `m` тримати навколо генерації+Send, `registryMutex_` — лише навколо map lookup/insert (не під I/O);
  - `MapResult`: успіх → `Ok({acceptedInstances, acceptedCopies})`.
  - фабрика транспорту: за замовчуванням `MakeTransport(profile)` (spooler/tcp), у тестах — підмінна.

- [x] **Step 5: CMake** — додати файли.
- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add src/drivers/label_printer/LabelPrinterDriver.* tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(label): LabelPrinterDriver — мульти-DeviceID, batch state machine, concurrency, MapResult"
```

---

## Task 8: `LabelXml` — pugixml-адаптер (`LabelsTable`/`ConnectionParameters` → модель)

**Files:**
- Create: `extern/pugixml/` (сабмодуль), `src/drivers/label_printer/LabelXml.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestXml`), `CMake/components.cmake`, `.gitmodules`

**Interfaces:**
- Produces: `bool LabelXml::ParseLabelsTable(const std::string& xml, LabelBatch& out, std::string& err);`
  `bool LabelXml::ParseConnectionParameters(const std::string& xml, DeviceProfile& out, std::string& err);`

- [x] **Step 1: Додати pugixml** — `git submodule add https://github.com/zeux/pugixml extern/pugixml` (MIT). Додати include-шлях у ціль драйвера.
- [x] **Step 2: Тест `TestXml`** — розпарсити приклад із контракту 3.7 (той самий XML) і перевірити value-семантику:

```cpp
static void TestXml() {
    const char* xml = R"(<?xml version="1.0"?><Data>
      <Formatting Width="60" Height="40">
        <Text FieldName="Name" Left="1" Top="1" Width="55" Height="10" FontName="Tahoma" FontSize="8"/>
        <Barcode FieldName="Bar" Type="EAN13" Left="1" Top="22" Height="10" PrintHRI="true" FontSize="8"/>
      </Formatting>
      <Labels>
        <Label Quantity="2"><Record FieldName="Name" Value="Блокнот"/><Record FieldName="Bar" Value="4008110271538"/></Label>
      </Labels></Data>)";
    LabelBatch b; std::string err;
    CHECK(LabelXml::ParseLabelsTable(xml, b, err), "parse ok");
    CHECK(b.formatting && b.formatting->width==60 && b.formatting->height==40, "formatting geometry");
    CHECK(b.formatting->texts.size()==1 && b.formatting->barcodes.size()==1, "fields parsed");
    CHECK(b.labels.size()==1 && b.labels[0].quantity==2, "labels + quantity");
    CHECK(b.labels[0].records[0].value.has_value() && *b.labels[0].records[0].value=="Блокнот", "record value present");
}
```

- [x] **Step 3: FAIL.**
- [x] **Step 4: `LabelXml.{h,cpp}`** — pugixml: `Formatting` (атрибути → поля; типи Text/Barcode/Image/UserData); `Static`/`Value`/`ValueBase64` → `optional`; `Labels/Label/Record`; `ConnectionParameters` `<Parameter Name Value>` → `DeviceProfile` (невідомі — ігнорувати).
- [x] **Step 5: CMake** — файли + pugixml.
- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add extern/pugixml .gitmodules src/drivers/label_printer/LabelXml.* tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(label): XML-адаптер LabelsTable/ConnectionParameters -> LabelModel (pugixml)"
```

---

## Task 9: `AddinLabelPrinter` — БПО-фасад

**Files:**
- Create: `src/components/AddinLabelPrinter.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestFacadeSmoke`), `CMake/components.cmake` (ціль `label_facade_component` + у фінальну DLL), `HEADER_FILES`/`SOURCE_FILES`

**Interfaces:**
- Consumes: `AddInNative`, `LabelPrinterDriver`, `LabelXml`.
- Produces: компонента `LabelPrinter` з методами §10 (EN/RU). Патерн — 1:1 як `AddinECRPrivatJSON` (`REGISTER_COMPONENT`, `RegisterMethods`, `runSync`), але БПО-семантика (`BOOL`+OUT+`GetLastError`).

- [x] **Step 1: Тест `TestFacadeSmoke`** (реєстрація методів через `AddInNative::CreateObject`, як `ecr_privatjson_selftest` рядки ~421-431)

```cpp
static void TestFacadeSmoke() {
    auto* c = AddInNative::CreateObject(u"LabelPrinter");
    CHECK(c != nullptr, "LabelPrinter component created");
    CHECK(c->GetNMethods() >= 8, "registers >=8 BPO methods");
    delete c;
}
```

- [x] **Step 2: FAIL.**
- [x] **Step 3–4: `AddinLabelPrinter.{h,cpp}`**:
  - `.h`: `class AddinLabelPrinter : public AddInNative { public: static std::vector<std::u16string> names; AddinLabelPrinter(); ~AddinLabelPrinter(); private: void RegisterMethods(); LabelPrinterDriver driver_; int lastErrorCode_=0; std::string lastErrorDesc_; std::string activeDeviceId_; };`
  - `.cpp`: `REGISTER_COMPONENT(u"LabelPrinter", AddinLabelPrinter)`; `RegisterMethods`: `GetInterfaceRevision`/`ПолучитьРевизиюИнтерфейса` (Ret→int 4007), `GetDescription`/`ПолучитьОписание` (формує `DriverDescription` XML → OUT/result), `GetLastError`/`ПолучитьОшибку` (Ret→int code, опис — через result/OUT), `EquipmentParameters`, `ConnectEquipment`/`ПодключитьОборудование` (parse ConnectionParameters → driver.Connect → result=DeviceID), `DisconnectEquipment`, `EquipmentTest`, `InitializePrinter`/`ИнициализацияПринтера`, `PrintLabels`/`ПечатьЭтикеток` (parse LabelsTable → driver.PrintLabels; `ResultEnvelope`→BOOL, при !ok зберегти lastError). Кожен хендлер — try/catch → зберегти lastError, повернути false.
  - Хелпер `mapEnvToBool(env)`: якщо `!env.ok` → `lastErrorCode_=…; lastErrorDesc_=env.description;` return `env.ok`.

- [x] **Step 5: CMake** — ціль `label_facade_component` (deps `+driver_label_printer_component`), додати `$<TARGET_OBJECTS:label_facade_component>` і `$<TARGET_OBJECTS:driver_label_printer_component>` у фінальну DLL (рядки ~317-324), файли — у `HEADER_FILES`/`SOURCE_FILES`. Selftest лінкує `label_facade_component`+драйвер+транспорт+base+helpers+pugixml+gdiplus.
- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add src/components/AddinLabelPrinter.* tests/label_printer_selftest.cpp CMake/components.cmake
git commit -m "feat(label): БПО-фасад AddinLabelPrinter (системні+функціональні методи, XML I/O, GetLastError)"
```

---

## Task 10: `LabelEmulator` + transport-e2e (L-p2)

**Files:**
- Create: `tests/support/LabelEmulator.{h,cpp}`
- Modify: `tests/label_printer_selftest.cpp` (`TestTransportE2E`), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `class LabelEmulator { public: bool Start(int port); void Stop(); std::string LastZpl() const; int Port() const; };` — Winsock TCP-сервер (localhost), приймає сирий потік, зберігає в буфер; патерн — `tests/support/TerminalEmulator.*`.

- [x] **Step 1: Тест `TestTransportE2E`** (драйвер → `TransportTCP` → емулятор → отриманий ZPL)

```cpp
static void TestTransportE2E() {
    GdiplusRuntime gdi;
    LabelEmulator emu; CHECK(emu.Start(0), "emulator started");
    LabelPrinterDriver drv; DeviceProfile dp; dp.transport=DeviceProfile::Transport::Tcp;
    dp.host="127.0.0.1"; dp.port=emu.Port(); dp.dotsPerMm=8;
    std::string id = drv.Connect(dp);
    LabelBatch b; LabelFormatting fmt; fmt.width=60; fmt.height=40;
    BarcodeField bc; bc.fieldName="Bar"; bc.type="EAN13"; bc.geom={1,22,0,10,0}; fmt.barcodes.push_back(bc);
    b.formatting=fmt; b.labels.push_back({1,{ {"Bar", std::string("4008110271538")} }});
    CHECK(drv.PrintLabels(id, b, "first").ok, "print ok");
    // дати сокету доставити
    std::string z = emu.LastZpl();
    CHECK(z.find("^XA")!=std::string::npos && z.find("^BE")!=std::string::npos, "emulator received ZPL with EAN13");
    drv.Disconnect(id); emu.Stop();
}
```

- [x] **Step 2: FAIL.**
- [x] **Step 3–4: `LabelEmulator.{h,cpp}`** — за зразком `TerminalEmulator` (Winsock listen на 127.0.0.1, accept у потоці, recv у буфер до закриття; `Port()==0` → ОС обирає вільний порт, повернути фактичний).
- [x] **Step 5: CMake** — `support/LabelEmulator.cpp` у selftest; лінк `ws2_32`.
- [x] **Step 6: Зібрати, запустити — PASS. Commit.**

```bash
git add tests/support/LabelEmulator.* tests/label_printer_selftest.cpp tests/CMakeLists.txt
git commit -m "test(label): TCP-емулятор-захоплювач ZPL + transport-e2e (L-p2)"
```

---

## Task 11: `label_printer_emulator` (standalone) + `label_native_host` (L-p3)

**Files:**
- Create: `tests/label_printer_emulator.cpp`, `tests/label_native_host.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `LabelEmulator`, головна DLL (`LoadLibraryW`+`GetClassObject`), `IComponentBase`.

- [x] **Step 1: `label_printer_emulator.cpp`** — standalone EXE: `LabelEmulator` на порту (arg, default 9100), друкує отриманий ZPL у файл/stdout доти, доки не Ctrl-C. (Аналог `ecr_terminal_emulator`.)
- [x] **Step 2: `label_native_host.cpp`** — за зразком `tests/ecr_native_host.cpp`:
  `LoadLibraryW(головна DLL)` → `GetClassObject(L"LabelPrinter", &comp)` → `Init`+`setMemManager` (моки) → `FindMethod(L"ПодключитьОборудование")` + виклик із `ConnectionParameters` XML (`tcp://127.0.0.1:<port>`) → `FindMethod(L"ПечатьЭтикеток")` + `LabelsTable` XML проти in-process `LabelEmulator` → перевірити BOOL і що емулятор отримав ZPL з `^XA`. Exit 0 = OK.

```cpp
// ключова перевірка в native_host
bool ok = callBoolMethod(comp, L"ПечатьЭтикеток", { deviceId, kLabelsTableXml, L"first" });
assert(ok);
assert(emu.LastZpl().find("^XA") != std::string::npos);
```

- [x] **Step 3: CMake** — цілі `label_printer_emulator`, `label_native_host` (лінк `ws2_32`; `label_native_host` — як `ecr_native_host`, вантажить DLL).
- [x] **Step 4: Зібрати; запустити `label_native_host_x64.exe` — exit 0.**
- [x] **Step 5: Commit**

```bash
git add tests/label_printer_emulator.cpp tests/label_native_host.cpp tests/CMakeLists.txt
git commit -m "test(label): standalone-емулятор (для 1С) + label_native_host (компонента через DLL, L-p3)"
```

---

## Task 12: CMake фінал-лінк (winspool/gdiplus) + інтеграція в `run_tests.ps1`

**Files:**
- Modify: `CMake/components.cmake` (лінк на `${TARGET}` і тест-цілі), `tests/CMakeLists.txt`, `run_tests.ps1`

- [x] **Step 1: Лінк системних бібліотек на ФІНАЛЬНУ DLL** (бо збірка через `$<TARGET_OBJECTS>` — звірено `components.cmake:315`)

```cmake
target_link_libraries(${TARGET} PRIVATE winspool gdiplus)
```

- [x] **Step 2: Лінк на тест-цілі, що лінкують об'єкти напряму** — `label_printer_selftest`, `label_native_host`: `target_link_libraries(<ціль> PRIVATE winspool gdiplus ws2_32)`.
- [x] **Step 3: Додати етапи в `run_tests.ps1`** (за зразком L0.7 та L2-ecr, рядки ~334-389): етап `L-p1` (`label_printer_selftest`), `L-p3` (`label_native_host`); критерій — усі `[PASS]`/exit 0; у підсумкову таблицю.
- [x] **Step 4: Повний гейт x64**

Run: `powershell -File build_project.ps1 -WithTests`
Then: `powershell -File run_tests.ps1 -NoUapki x64`
Expected: L-p1 та L-p3 — PASS; підсумкова таблиця без FAIL; exit 0.

- [x] **Step 5: Повний гейт x86** — `powershell -File run_tests.ps1 -NoUapki x86` → PASS.
- [x] **Step 6: Commit**

```bash
git add CMake/components.cmake tests/CMakeLists.txt run_tests.ps1
git commit -m "build(label): лінк winspool/gdiplus на фінал+тести; L-p1/L-p3 у run_tests"
```

---

## Task 13: Документація (архітектура + 1С-інтеграція)

**Files:**
- Create: `docs/architecture/label_printer.md`, `docs/integration-1c/label_printer.md`
- Modify: `docs/architecture/README.md`

- [x] **Step 1: `docs/architecture/label_printer.md`** — шари, LabelModel, генератор (гібрид), транспорти, batch SM, межі модулів (з §4/§14 спеки). Джерело правди про підсистему (CLAUDE.md).
- [x] **Step 2: `docs/integration-1c/label_printer.md`** — як зареєструвати компоненту `LabelPrinter` як «Подключаемое оборудование», параметри підключення, деплой-нота **Generic / Text Only**, приклад `LabelsTable`, поведінка `UNSUPPORTED_BARCODE`.
- [x] **Step 3: Додати посилання в `docs/architecture/README.md`.**
- [x] **Step 4: Commit**

```bash
git add docs/architecture/label_printer.md docs/integration-1c/label_printer.md docs/architecture/README.md
git commit -m "docs(label): архітектура підсистеми + 1С-інтеграція драйвера принтера етикеток"
```

---

## Self-Review (виконано автором плану)

**Spec coverage:** §2 рішення → усі задачі; §5 LabelModel → Task 1/8; §6 генерація (штрихкоди/`^GF`/escaping/init/units) → Task 2–5; §7 растр GDI+ → Task 4; §8 транспорти → Task 6 (+TCP наявний); §9 драйвер (SM/concurrency/MapResult) → Task 7; §10 БПО-фасад → Task 9; §12 тести (L-p1..L-p4) → Task 2–12; §13 CMake → Task 1,12; §15 помилки → наскрізно; §16 ризики (тайлінг/GDI+/символіки) → відображені в тестах. **Прогалин немає.**

**Placeholder scan:** реальний код/тести в кожному кроці; «TODO» лише всередині коду Task 4 Step 4 як явний маркер того, що Image/Border додаються Step 7 тієї ж задачі (не прогалина плану).

**Type consistency:** `Bitmap1`, `DeviceProfile`, `GenResult`, `BarcodeEmit`, `LabelBatch`, `mmToDots/ptToDots`, `LabelZplGenerator::BuildLabel/BuildInit`, `LabelPrinterDriver::{Connect,PrintLabels,InitializePrinter}` — імена узгоджені між задачами.

**Порядок:** знизу вгору — кожна задача тестовна самостійно й спирається лише на попередні.
