#pragma once
#include <functional>
#include <optional>
#include <string>
#include "LabelModel.h"
#include "GfEncoder.h"

namespace labelprinter {

// RAII-обгортка над ініціалізацією GDI+, процес-широка, з ref-count.
// Ініціалізує/деініціалізує GDI+ рівно один раз незалежно від кількості
// живих екземплярів. НЕ можна створювати/руйнувати з DllMain (заборона
// GdiplusStartup/GdiplusShutdown у контексті loader-lock). Non-copyable.
class GdiplusRuntime {
public:
    GdiplusRuntime();
    ~GdiplusRuntime();
    GdiplusRuntime(const GdiplusRuntime&) = delete;
    GdiplusRuntime& operator=(const GdiplusRuntime&) = delete;
};

// Рендер розмітки етикетки у 1-бітний растр засобами GDI+.
// Обробляє лише текст, картинки та рамки; ділянки штрихкодів лишає порожніми
// (їх формує нативний ZPL-генератор). Перед викликом Render у стеку викликів
// має жити принаймні один GdiplusRuntime.
class LabelRaster {
public:
    // Повертає значення поля за іменем (запис/дефолт) або std::nullopt.
    using ValueOf = std::function<std::optional<std::string>(const std::string&)>;

    // fmt      — розмітка (мм); valueOf — джерело значень полів; dotsPerMm — щільність.
    // ok       — out: true при успіху (у т.ч. легітимно-порожній растр без text/image/border);
    //            false ЛИШЕ при реальному збої GDI+ (некоректний розмір, Bitmap/Graphics не
    //            ініціалізовано, LockBits fail, виняток) — тоді викликач має підняти RENDER_ERROR.
    // Результат — растр розміром mmToDots(width) x mmToDots(height), 1bpp MSB-first, 1=чорний.
    static Bitmap1 Render(const LabelFormatting& fmt, const ValueOf& valueOf, int dotsPerMm, bool& ok);
};

} // namespace labelprinter
