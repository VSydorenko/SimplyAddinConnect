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
