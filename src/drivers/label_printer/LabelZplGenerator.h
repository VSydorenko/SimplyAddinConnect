#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "LabelModel.h"

namespace labelprinter {

// Результат генерації ZPL однієї етикетки.
// ok==true  -> zpl містить повний блок ^XA…^XZ (одна етикетка, ^PQ<quantity> копій).
// ok==false -> errCode/errDesc описують причину (UNSUPPORTED_BARCODE, RENDER_ERROR, EXCEPTION).
struct GenResult {
    bool ok = false;
    std::vector<uint8_t> zpl;
    std::string errCode;
    std::string errDesc;
};

// Гібридний генератор ZPL: растр (текст/картинка/рамка) одним ^GF + нативні штрихкоди.
class LabelZplGenerator {
public:
    // Формує повну етикетку з розмітки формату та даних екземпляра.
    // Растеризує нештрихкодовий шар (LabelRaster -> GfEncoder), додає нативні штрихкоди
    // (BarcodeZpl::Emit) і кількість копій (^PQ<quantity>). Значення полів беруться з
    // записів екземпляра (за іменем), інакше — зі статичного/дефолтного значення поля.
    static GenResult BuildLabel(const LabelFormatting& fmt, const LabelInstance& inst, const DeviceProfile& dp);

    // Формує окремий ініціалізаційний блок ^XA…^XZ: темність, швидкість, розмір/довжина, home.
    // Значення — з DeviceProfile (ConnectionParameters).
    static std::vector<uint8_t> BuildInit(const DeviceProfile& dp);
};

} // namespace labelprinter
