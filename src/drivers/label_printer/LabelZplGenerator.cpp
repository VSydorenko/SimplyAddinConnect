#include "../../core/pch.h"
#include "LabelZplGenerator.h"
#include "LabelUnits.h"
#include "LabelRaster.h"
#include "GfEncoder.h"
#include "BarcodeZpl.h"
#include "../../helpers/ServiceTools.h"
#include <algorithm>
#include <optional>

namespace labelprinter {

namespace {
const char* kTag = "LabelZplGenerator";

// Розумний ліміт рядків на один ^GF-тайл: тримаємо блок під ~60000 байтів,
// щоб не впертися в ліміти поля ^GF / памʼяті принтера на великих етикетках.
constexpr int kMaxTileBytes = 60000;

std::vector<uint8_t> ToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
} // namespace

GenResult LabelZplGenerator::BuildLabel(const LabelFormatting& fmt, const LabelInstance& inst, const DeviceProfile& dp) {
    GenResult r;
    try {
        // valueOf: значення поля за іменем беремо із записів екземпляра; якщо запису
        // немає — повертаємо nullopt (растр сам підставить статичний/дефолтний текст поля).
        auto valueOf = [&inst](const std::string& name) -> std::optional<std::string> {
            for (const auto& rec : inst.records) {
                if (rec.fieldName == name) return rec.value;
            }
            return std::nullopt;
        };

        std::string out = "^XA";

        // Геометрія формату — авторитет саме розмітки пакета (§6.4).
        const long wDots = mmToDots(fmt.width, dp.dotsPerMm);
        const long hDots = mmToDots(fmt.height, dp.dotsPerMm);
        if (wDots > 0) out += "^PW" + std::to_string(wDots);
        if (hDots > 0) out += "^LL" + std::to_string(hDots);

        // Растровий шар (текст/картинка/рамка) одним або кількома ^GF.
        // Реальний збій GDI+ (некоректний розмір/неініціалізований рантайм/LockBits) —
        // це RENDER_ERROR, а не мовчазна втрата растру; легітимно-порожній растр -> ok=true.
        bool renderOk = false;
        Bitmap1 bm = LabelRaster::Render(fmt, valueOf, dp.dotsPerMm, renderOk);
        if (!renderOk) {
            NEUTRAL_REPORT_ERROR(kTag, "Збій рендеру растрового шару етикетки (GDI+)");
            r.ok = false;
            r.errCode = "RENDER_ERROR";
            r.errDesc = "Збій рендеру растрового шару етикетки";
            return r;
        }
        if (bm.widthDots > 0 && bm.heightDots > 0 && !bm.rows.empty()) {
            const int bpr = (bm.widthDots + 7) / 8;
            const int maxRowsPerTile = (bpr > 0) ? (std::max)(1, kMaxTileBytes / bpr) : bm.heightDots;
            out += GfEncoder::EncodeGfa(bm, 0, 0, maxRowsPerTile);
        }

        // Нативні штрихкоди.
        for (const auto& bc : fmt.barcodes) {
            std::optional<std::string> v = valueOf(bc.fieldName);
            std::string data;
            if (v) {
                data = *v;                            // динамічний штрихкод — простий текст (§5)
            } else if (bc.staticValueBase64) {
                // static-штрихкод: §5 каже ValueBase64 — це Base64, тож декодуємо у корисне значення.
                std::vector<uint8_t> decoded;
                if (!LabelRaster::DecodeBase64(*bc.staticValueBase64, decoded)) {
                    NEUTRAL_REPORT_WARN(kTag, "Некоректний Base64 static-штрихкоду поля: " + bc.fieldName);
                    r.ok = false;
                    r.errCode = "BAD_INPUT";
                    r.errDesc = "Некоректний Base64 static-штрихкоду поля: " + bc.fieldName;
                    return r;
                }
                data.assign(decoded.begin(), decoded.end());
            }

            BarcodeEmit be = BarcodeZpl::Emit(bc, data, dp.dotsPerMm);
            if (!be.ok) {
                NEUTRAL_REPORT_WARN(kTag, "Штрихкод не згенеровано: " + be.errCode + " (" + be.errDesc + ")");
                r.ok = false;
                r.errCode = be.errCode;
                r.errDesc = be.errDesc;
                return r;
            }
            out += be.zpl;
        }

        // Кількість копій.
        const int quantity = (inst.quantity > 0) ? inst.quantity : 1;
        out += "^PQ" + std::to_string(quantity);

        out += "^XZ";

        r.ok = true;
        r.zpl = ToBytes(out);
        return r;
    } catch (const std::exception& ex) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Виняток під час генерації етикетки: ") + ex.what());
        r.ok = false;
        r.errCode = "EXCEPTION";
        r.errDesc = ex.what();
        return r;
    } catch (...) {
        NEUTRAL_REPORT_ERROR(kTag, "Невідомий виняток під час генерації етикетки");
        r.ok = false;
        r.errCode = "EXCEPTION";
        r.errDesc = "Невідомий виняток";
        return r;
    }
}

std::vector<uint8_t> LabelZplGenerator::BuildInit(const DeviceProfile& dp) {
    std::string out = "^XA";

    // Темність (media darkness). Значення — з профілю; діапазон валідується на рівні фасаду/XML.
    out += "^MD" + std::to_string(dp.darkness);

    // Швидкість друку.
    out += "^PR" + std::to_string(dp.speed);

    // Розмір/довжина етикетки з профілю (мм -> dots). Для конкретного формату
    // ^PW/^LL перевизначаються у BuildLabel (Formatting — авторитет).
    const long wDots = mmToDots(dp.labelWidthMm, dp.dotsPerMm);
    const long hDots = mmToDots(dp.labelHeightMm, dp.dotsPerMm);
    if (wDots > 0) out += "^PW" + std::to_string(wDots);
    if (hDots > 0) out += "^LL" + std::to_string(hDots);

    // Home (зсув початку етикетки).
    out += "^LH" + std::to_string(dp.homeXDots) + "," + std::to_string(dp.homeYDots);

    out += "^XZ";
    return std::vector<uint8_t>(out.begin(), out.end());
}

} // namespace labelprinter
