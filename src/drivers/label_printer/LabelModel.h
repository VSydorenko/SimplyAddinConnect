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
