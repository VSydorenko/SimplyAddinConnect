#include "../../core/pch.h"
#include "LabelXml.h"
#include "../../helpers/ServiceTools.h"
#include "pugixml.hpp"
#include <cctype>
#include <optional>

namespace labelprinter {

namespace {

constexpr const char* kTag = "LabelXml";

// --- Дрібні парсери атрибутів (толерантні, з дефолтами) ---

bool ParseBool(const char* s, bool def) {
    if (!s || !*s) return def;
    std::string v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    if (v == "true" || v == "1" || v == "yes") return true;
    if (v == "false" || v == "0" || v == "no") return false;
    return def;
}

double ParseDouble(const char* s, double def) {
    if (!s || !*s) return def;
    try { return std::stod(std::string(s)); } catch (...) { return def; }
}

int ParseInt(const char* s, int def) {
    if (!s || !*s) return def;
    try { return std::stoi(std::string(s)); } catch (...) { return def; }
}

// Атрибут як std::optional: відсутній != порожній рядок.
std::optional<std::string> OptAttr(const pugi::xml_node& node, const char* name) {
    pugi::xml_attribute a = node.attribute(name);
    if (!a) return std::nullopt;
    return std::string(a.value());
}

FieldGeom ReadGeom(const pugi::xml_node& n) {
    FieldGeom g;
    g.left = ParseDouble(n.attribute("Left").value(), 0);
    g.top = ParseDouble(n.attribute("Top").value(), 0);
    g.width = ParseDouble(n.attribute("Width").value(), 0);
    g.height = ParseDouble(n.attribute("Height").value(), 0);
    g.orientation = ParseInt(n.attribute("Orientation").value(), 0);
    return g;
}

void ReadTextField(const pugi::xml_node& n, LabelFormatting& fmt) {
    TextField t;
    t.fieldName = n.attribute("FieldName").value();
    t.geom = ReadGeom(n);
    t.fontName = n.attribute("FontName").value();
    t.fontSize = ParseInt(n.attribute("FontSize").value(), 0);
    t.fontStyle = n.attribute("FontStyle").value();
    if (pugi::xml_attribute a = n.attribute("Align")) t.align = a.value();
    if (pugi::xml_attribute a = n.attribute("VAlign")) t.vAlign = a.value();
    t.multiline = ParseBool(n.attribute("Multiline").value(), false);
    t.border = n.attribute("Border").value();
    t.borderWidth = ParseInt(n.attribute("BorderWidth").value(), 1);
    if (pugi::xml_attribute a = n.attribute("BorderStyle")) t.borderStyle = a.value();
    t.isStatic = ParseBool(n.attribute("Static").value(), false);
    t.defaultOrStaticValue = OptAttr(n, "Value");
    fmt.texts.push_back(std::move(t));
}

void ReadBarcodeField(const pugi::xml_node& n, LabelFormatting& fmt) {
    BarcodeField b;
    b.fieldName = n.attribute("FieldName").value();
    b.type = n.attribute("Type").value();
    b.geom = ReadGeom(n);
    b.printHRI = ParseBool(n.attribute("PrintHRI").value(), true);
    b.hriFontSize = ParseInt(n.attribute("FontSize").value(), 0);
    b.checkSymbol = ParseBool(n.attribute("CheckSymbol").value(), true);
    b.isStatic = ParseBool(n.attribute("Static").value(), false);
    b.staticValueBase64 = OptAttr(n, "ValueBase64");
    fmt.barcodes.push_back(std::move(b));
}

void ReadImageField(const pugi::xml_node& n, LabelFormatting& fmt) {
    ImageField im;
    im.fieldName = n.attribute("FieldName").value();
    im.geom = ReadGeom(n);
    im.border = n.attribute("Border").value();
    im.borderWidth = ParseInt(n.attribute("BorderWidth").value(), 1);
    if (pugi::xml_attribute a = n.attribute("BorderStyle")) im.borderStyle = a.value();
    im.isStatic = ParseBool(n.attribute("Static").value(), false);
    im.staticValueBase64 = OptAttr(n, "Value"); // Image.Formatting.Value — Base64 (static)
    fmt.images.push_back(std::move(im));
}

void ReadUserDataField(const pugi::xml_node& n, LabelFormatting& fmt) {
    UserDataField u;
    u.fieldName = n.attribute("FieldName").value();
    u.isStatic = ParseBool(n.attribute("Static").value(), false);
    u.defaultOrStaticValue = OptAttr(n, "Value");
    fmt.userData.push_back(std::move(u));
}

// Кореневий вузол пакета — <Data> (толерантно приймаємо й документ без обгортки).
pugi::xml_node RootData(const pugi::xml_document& doc) {
    pugi::xml_node d = doc.child("Data");
    if (d) return d;
    return doc.first_child();
}

} // namespace

bool LabelXml::ParseLabelsTable(const std::string& xml, LabelBatch& out, std::string& err) {
    out = LabelBatch{};
    err.clear();
    try {
        pugi::xml_document doc;
        pugi::xml_parse_result pr = doc.load_string(xml.c_str());
        if (!pr) {
            err = std::string("Помилка розбору XML LabelsTable: ") + pr.description();
            NEUTRAL_REPORT_ERROR(kTag, err);
            return false;
        }
        pugi::xml_node data = RootData(doc);
        if (!data) {
            err = "Порожній XML LabelsTable — немає кореневого елемента Data";
            NEUTRAL_REPORT_ERROR(kTag, err);
            return false;
        }

        // Formatting (опційне — присутнє лише при first)
        if (pugi::xml_node fnode = data.child("Formatting")) {
            LabelFormatting fmt;
            fmt.width = ParseDouble(fnode.attribute("Width").value(), 0);
            fmt.height = ParseDouble(fnode.attribute("Height").value(), 0);
            for (pugi::xml_node child : fnode.children()) {
                const std::string name = child.name();
                if (name == "Text") ReadTextField(child, fmt);
                else if (name == "Barcode") ReadBarcodeField(child, fmt);
                else if (name == "Image") ReadImageField(child, fmt);
                else if (name == "UserData") ReadUserDataField(child, fmt);
                // невідомі елементи ігноруємо
            }
            out.formatting = std::move(fmt);
        }

        // Labels/Label/Record
        if (pugi::xml_node labels = data.child("Labels")) {
            for (pugi::xml_node lnode : labels.children("Label")) {
                LabelInstance inst;
                inst.quantity = ParseInt(lnode.attribute("Quantity").value(), 1);
                for (pugi::xml_node rnode : lnode.children("Record")) {
                    LabelRecord rec;
                    rec.fieldName = rnode.attribute("FieldName").value();
                    rec.value = OptAttr(rnode, "Value"); // відсутній != порожній
                    inst.records.push_back(std::move(rec));
                }
                out.labels.push_back(std::move(inst));
            }
        }
        return true;
    } catch (const std::exception& ex) {
        err = std::string("Виняток під час розбору LabelsTable: ") + ex.what();
        NEUTRAL_REPORT_ERROR(kTag, err);
        return false;
    } catch (...) {
        err = "Невідомий виняток під час розбору LabelsTable";
        NEUTRAL_REPORT_ERROR(kTag, err);
        return false;
    }
}

bool LabelXml::ParseConnectionParameters(const std::string& xml, DeviceProfile& out, std::string& err) {
    err.clear();
    try {
        pugi::xml_document doc;
        pugi::xml_parse_result pr = doc.load_string(xml.c_str());
        if (!pr) {
            err = std::string("Помилка розбору XML ConnectionParameters: ") + pr.description();
            NEUTRAL_REPORT_ERROR(kTag, err);
            return false;
        }
        pugi::xml_node params = doc.child("Parameters");
        if (!params) params = doc.first_child();
        if (!params) {
            err = "Порожній XML ConnectionParameters — немає кореневого елемента Parameters";
            NEUTRAL_REPORT_ERROR(kTag, err);
            return false;
        }
        for (pugi::xml_node p : params.children("Parameter")) {
            const std::string name = p.attribute("Name").value();
            const std::string value = p.attribute("Value").value();
            if (name == "TransportKind") {
                std::string v;
                for (char c : value) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                out.transport = (v == "tcp") ? DeviceProfile::Transport::Tcp : DeviceProfile::Transport::Spooler;
            } else if (name == "PrinterName") {
                out.printerName = value;
            } else if (name == "Host") {
                out.host = value;
            } else if (name == "Port") {
                out.port = ParseInt(value.c_str(), out.port);
            } else if (name == "DotsPerMm") {
                out.dotsPerMm = ParseInt(value.c_str(), out.dotsPerMm);
            } else if (name == "Darkness") {
                out.darkness = ParseInt(value.c_str(), out.darkness);
            } else if (name == "Speed") {
                out.speed = ParseInt(value.c_str(), out.speed);
            } else if (name == "LabelWidthMm") {
                out.labelWidthMm = ParseDouble(value.c_str(), out.labelWidthMm);
            } else if (name == "LabelHeightMm") {
                out.labelHeightMm = ParseDouble(value.c_str(), out.labelHeightMm);
            } else if (name == "HomeXDots") {
                out.homeXDots = ParseInt(value.c_str(), out.homeXDots);
            } else if (name == "HomeYDots") {
                out.homeYDots = ParseInt(value.c_str(), out.homeYDots);
            }
            // невідомі параметри ігноруємо (вимога БПО)
        }
        return true;
    } catch (const std::exception& ex) {
        err = std::string("Виняток під час розбору ConnectionParameters: ") + ex.what();
        NEUTRAL_REPORT_ERROR(kTag, err);
        return false;
    } catch (...) {
        err = "Невідомий виняток під час розбору ConnectionParameters";
        NEUTRAL_REPORT_ERROR(kTag, err);
        return false;
    }
}

} // namespace labelprinter
