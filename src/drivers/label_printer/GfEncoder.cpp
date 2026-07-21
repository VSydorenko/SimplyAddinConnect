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
        const int remaining = bm.heightDots - row;
        const int tileRows = (maxRowsPerTile < remaining) ? maxRowsPerTile : remaining;
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
