#include "../../core/pch.h"
#include "LabelRaster.h"
#include "LabelUnits.h"
#include "../../helpers/ServiceTools.h"
#include <windows.h>
#include <objbase.h>
#include <gdiplus.h>
#include <atomic>
#include <mutex>
#include <array>
#include <vector>

namespace labelprinter {
using namespace Gdiplus;

// ---------------------------------------------------------------------------
// GdiplusRuntime — процес-широка ref-count ініціалізація GDI+
// ---------------------------------------------------------------------------
namespace {
std::mutex g_gdiMutex;
std::atomic<int> g_gdiRefs{0};
ULONG_PTR g_gdiToken = 0;
} // namespace

GdiplusRuntime::GdiplusRuntime() {
    std::lock_guard<std::mutex> lk(g_gdiMutex);
    if (g_gdiRefs.fetch_add(1) == 0) {
        GdiplusStartupInput in;
        Status st = GdiplusStartup(&g_gdiToken, &in, nullptr);
        if (st != Ok) {
            g_gdiRefs.fetch_sub(1);
            g_gdiToken = 0;
            NEUTRAL_REPORT_ERROR("LabelRaster", std::string("Не вдалося ініціалізувати GDI+, код: ") + std::to_string((int)st));
        }
    }
}

GdiplusRuntime::~GdiplusRuntime() {
    std::lock_guard<std::mutex> lk(g_gdiMutex);
    if (g_gdiToken != 0 && g_gdiRefs.fetch_sub(1) == 1) {
        GdiplusShutdown(g_gdiToken);
        g_gdiToken = 0;
    }
}

// ---------------------------------------------------------------------------
// Локальні хелпери
// ---------------------------------------------------------------------------
namespace {

// Чи задає рядок border реальну рамку (не порожньо і не "None").
bool HasBorder(const std::string& border) {
    if (border.empty()) return false;
    return border != "None" && border != "none";
}

// Малює рамку прямокутником у координатах-дотах.
void DrawBorder(Graphics& g, const FieldGeom& geom, int borderWidth, int dotsPerMm) {
    REAL x = (REAL)mmToDots(geom.left, dotsPerMm);
    REAL y = (REAL)mmToDots(geom.top, dotsPerMm);
    REAL w = (REAL)mmToDots(geom.width, dotsPerMm);
    REAL h = (REAL)mmToDots(geom.height, dotsPerMm);
    REAL pw = (REAL)((borderWidth > 0) ? borderWidth : 1);
    Pen pen(Color(255, 0, 0, 0), pw);
    g.DrawRectangle(&pen, x, y, w, h);
}

// Декодує Base64-картинку у Gdiplus::Image через IStream поверх HGLOBAL.
// Власник HGLOBAL передається потоку (GMEM_MOVEABLE + fDeleteOnRelease=TRUE).
// Повертає nullptr на будь-яку помилку (лог усередині).
Image* LoadImageFromBase64(const std::string& b64) {
    std::vector<uint8_t> bytes;
    if (!LabelRaster::DecodeBase64(b64, bytes) || bytes.empty()) {
        NEUTRAL_REPORT_WARN("LabelRaster", "Некоректний або порожній Base64 картинки");
        return nullptr;
    }
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hMem) {
        NEUTRAL_REPORT_ERROR("LabelRaster", "GlobalAlloc для картинки не вдався");
        return nullptr;
    }
    void* p = ::GlobalLock(hMem);
    if (!p) {
        ::GlobalFree(hMem);
        NEUTRAL_REPORT_ERROR("LabelRaster", "GlobalLock для картинки не вдався");
        return nullptr;
    }
    std::memcpy(p, bytes.data(), bytes.size());
    ::GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (::CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK || !stream) {
        ::GlobalFree(hMem);
        NEUTRAL_REPORT_ERROR("LabelRaster", "CreateStreamOnHGlobal не вдався");
        return nullptr;
    }
    Image* img = Image::FromStream(stream);   // копіює дані; далі stream можна звільнити
    stream->Release();                        // разом із ним і HGLOBAL (fDeleteOnRelease)
    if (!img || img->GetLastStatus() != Ok) {
        delete img;
        NEUTRAL_REPORT_WARN("LabelRaster", "GDI+ не розпізнав формат картинки");
        return nullptr;
    }
    return img;
}

} // namespace

// ---------------------------------------------------------------------------
// LabelRaster::DecodeBase64 — спільний Base64-декодер (див. заголовок).
// ---------------------------------------------------------------------------
bool LabelRaster::DecodeBase64(const std::string& in, std::vector<uint8_t>& out) {
    static const auto makeTable = []() {
        std::array<int, 256> t;
        t.fill(-1);
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[(unsigned char)alpha[i]] = i;
        return t;
    };
    static const std::array<int, 256> table = makeTable();

    out.clear();
    int acc = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = table[c];
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Розбір стилю шрифту з моделі ("Bold Italic Underline StrikeOut") у бітмаску GDI+.
static INT ParseFontStyle(const std::string& s) {
    INT st = FontStyleRegular;
    if (s.find("Bold")      != std::string::npos) st |= FontStyleBold;
    if (s.find("Italic")    != std::string::npos) st |= FontStyleItalic;
    if (s.find("Underline") != std::string::npos) st |= FontStyleUnderline;
    if (s.find("StrikeOut") != std::string::npos) st |= FontStyleStrikeout;
    return st;
}

// Горизонтальне/вертикальне вирівнювання + перенос рядків текстового поля у StringFormat.
static void ApplyTextFormat(StringFormat& sf, const TextField& t) {
    sf.SetAlignment(t.align == "Center" ? StringAlignmentCenter
                    : t.align == "Right" ? StringAlignmentFar : StringAlignmentNear);
    sf.SetLineAlignment(t.vAlign == "Center" ? StringAlignmentCenter
                        : t.vAlign == "Bottom" ? StringAlignmentFar : StringAlignmentNear);
    if (!t.multiline) sf.SetFormatFlags(StringFormatFlagsNoWrap);
}

// LabelRaster::Render
// ---------------------------------------------------------------------------
Bitmap1 LabelRaster::Render(const LabelFormatting& fmt, const ValueOf& valueOf, int dotsPerMm, bool& ok) {
    ok = false;                                                      // успіх підтверджуємо лише в кінці
    Bitmap1 out;
    const int W = (int)mmToDots(fmt.width, dotsPerMm);
    const int H = (int)mmToDots(fmt.height, dotsPerMm);
    if (W <= 0 || H <= 0) {
        NEUTRAL_REPORT_ERROR("LabelRaster", "Некоректний розмір етикетки в дотах: " + std::to_string(W) + "x" + std::to_string(H));
        return out;                                                  // ok лишається false — реальний збій
    }

    // Орієнтація растрових полів (текст/картинка) у v1 не реалізована (штрихкоди — через ZPL-орієнтацію).
    // Замість тихого ігнорування — явна помилка рендеру (каже викликачу підняти RENDER_ERROR).
    for (const auto& tf : fmt.texts)
        if (tf.geom.orientation != 0) {
            NEUTRAL_REPORT_ERROR("LabelRaster", "Орієнтація текстового поля !=0 не підтримується у v1 (растр): поле " + tf.fieldName);
            return out;                                              // ok=false -> RENDER_ERROR
        }
    for (const auto& imf : fmt.images)
        if (imf.geom.orientation != 0) {
            NEUTRAL_REPORT_ERROR("LabelRaster", "Орієнтація поля-картинки !=0 не підтримується у v1 (растр): поле " + imf.fieldName);
            return out;                                              // ok=false -> RENDER_ERROR
        }

    try {
        Bitmap bmp(W, H, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() != Ok) {                             // GDI+ не ініціалізовано / збій алокації
            NEUTRAL_REPORT_ERROR("LabelRaster", "GDI+ Bitmap не створено, код: " + std::to_string((int)bmp.GetLastStatus()));
            return Bitmap1{};
        }
        Graphics g(&bmp);
        if (g.GetLastStatus() != Ok) {
            NEUTRAL_REPORT_ERROR("LabelRaster", "GDI+ Graphics не створено, код: " + std::to_string((int)g.GetLastStatus()));
            return Bitmap1{};
        }
        g.Clear(Color(255, 255, 255, 255));                          // білий фон
        g.SetTextRenderingHint(TextRenderingHintSingleBitPerPixelGridFit); // без антиаліасу — чисте 1-біт
        SolidBrush black(Color(255, 0, 0, 0));

        // --- Текст ---
        for (const auto& t : fmt.texts) {
            std::optional<std::string> v = t.isStatic ? t.defaultOrStaticValue : valueOf(t.fieldName);
            if (!v) v = t.defaultOrStaticValue;                      // дефолт при відсутньому записі
            if (v) {
                std::wstring text = ServiceTools::U16StringToWString(ServiceTools::SafeMB2WCHAR(v->c_str()));
                std::string famU8 = t.fontName.empty() ? std::string("Arial") : t.fontName;
                std::wstring fam = ServiceTools::U16StringToWString(ServiceTools::SafeMB2WCHAR(famU8.c_str()));
                FontFamily ff(fam.c_str());
                REAL px = (REAL)ptToDots((double)t.fontSize, dotsPerMm);
                if (px < (REAL)1) px = (REAL)1;
                Font font(&ff, px, ParseFontStyle(t.fontStyle), UnitPixel);
                RectF rc((REAL)mmToDots(t.geom.left, dotsPerMm), (REAL)mmToDots(t.geom.top, dotsPerMm),
                         (REAL)mmToDots(t.geom.width, dotsPerMm), (REAL)mmToDots(t.geom.height, dotsPerMm));
                StringFormat sf;
                ApplyTextFormat(sf, t);
                g.DrawString(text.c_str(), -1, &font, rc, &sf, &black);
            }
            if (HasBorder(t.border)) DrawBorder(g, t.geom, t.borderWidth, dotsPerMm);
        }

        // --- Картинки ---
        for (const auto& im : fmt.images) {
            std::optional<std::string> b64 = im.isStatic ? im.staticValueBase64 : valueOf(im.fieldName);
            if (!b64) b64 = im.staticValueBase64;                    // дефолт при відсутньому записі
            if (b64 && !b64->empty()) {
                Image* img = LoadImageFromBase64(*b64);
                if (img) {
                    RectF rc((REAL)mmToDots(im.geom.left, dotsPerMm), (REAL)mmToDots(im.geom.top, dotsPerMm),
                             (REAL)mmToDots(im.geom.width, dotsPerMm), (REAL)mmToDots(im.geom.height, dotsPerMm));
                    g.DrawImage(img, rc);
                    delete img;
                }
            }
            if (HasBorder(im.border)) DrawBorder(g, im.geom, im.borderWidth, dotsPerMm);
        }

        // --- 32bpp -> 1bpp (поріг за яскравістю; MSB-first) ---
        const int bpr = (W + 7) / 8;
        out.widthDots = W;
        out.heightDots = H;
        out.rows.assign((size_t)bpr * H, 0);

        BitmapData bd;
        Rect r(0, 0, W, H);
        if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
            NEUTRAL_REPORT_ERROR("LabelRaster", "LockBits не вдався");
            out = Bitmap1{};
            return out;
        }
        for (int yy = 0; yy < H; ++yy) {
            const uint8_t* px = (const uint8_t*)bd.Scan0 + (size_t)yy * bd.Stride;
            for (int xx = 0; xx < W; ++xx) {
                int lum = (px[xx * 4 + 2] + px[xx * 4 + 1] + px[xx * 4 + 0]) / 3; // R+G+B
                if (lum < 128) out.rows[(size_t)yy * bpr + xx / 8] |= (uint8_t)(0x80 >> (xx % 8));
            }
        }
        bmp.UnlockBits(&bd);
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("LabelRaster", std::string("Виняток під час рендеру растру: ") + e.what());
        return Bitmap1{};                                            // ok=false — реальний збій
    } catch (...) {
        NEUTRAL_REPORT_ERROR("LabelRaster", "Невідомий виняток під час рендеру растру");
        return Bitmap1{};                                            // ok=false — реальний збій
    }

    ok = true;                                                       // растр сформовано штатно
    return out;
}

} // namespace labelprinter
