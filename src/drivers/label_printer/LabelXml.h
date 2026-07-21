#pragma once
#include <string>
#include "LabelModel.h"

namespace labelprinter {

// Адаптер XML-контрактів БПО «Принтер этикеток» -> типізована LabelModel.
// ParseLabelsTable: <Data><Formatting.../><Labels><Label><Record/></Label></Labels></Data>
//   (розд. 3.7 контракту). Formatting присутнє лише при PackageStatus="first".
// ParseConnectionParameters: <Parameters><Parameter Name="..." Value="..."/></Parameters> (§8/§10).
// Невідомі параметри/атрибути ігноруються (вимога БПО).
// Value-семантика: відсутній атрибут Value != порожній рядок -> std::optional без значення.
class LabelXml {
public:
    static bool ParseLabelsTable(const std::string& xml, LabelBatch& out, std::string& err);
    static bool ParseConnectionParameters(const std::string& xml, DeviceProfile& out, std::string& err);
};

} // namespace labelprinter
