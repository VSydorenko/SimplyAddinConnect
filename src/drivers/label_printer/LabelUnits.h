#pragma once
#include <cmath>
namespace labelprinter {
inline long mmToDots(double mm, int dotsPerMm) { return std::lround(mm * dotsPerMm); }
inline long ptToDots(double pt, int dotsPerMm) { return std::lround(pt * dotsPerMm * 25.4 / 72.0); }
} // namespace labelprinter
