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
