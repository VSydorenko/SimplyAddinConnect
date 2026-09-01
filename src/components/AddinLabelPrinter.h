#pragma once
#include "BpoFacadeBase.h"
#include "../drivers/label_printer/LabelPrinterDriver.h"
#include <string>
#include <vector>

/// Компонента 1С «Принтер этикеток» — БПО-фасад над LabelPrinterDriver.
///
/// Реалізує контракт «Подключаемое оборудование» (docs/architecture/bpo-contract.md §2):
/// системна половина — у BpoFacadeBase, тут лише гачки під тип обладнання плюс два
/// функціональні методи. Ревізія — 3004 (§3.4 спеки): гілок за ревізією для друку
/// етикеток у конфігурації немає, а з 4000 1С починає кликати УстановитьЛокализацию,
/// якого ми не реалізуємо.
///
/// ⚠️ ОДИН об'єкт = ОДИН пристрій. Контракт накопичує параметри на об'єкті ДО
/// Подключить, тож параметри другого принтера затерли б перший. Сам
/// LabelPrinterDriver лишається мульти-пристроєвим — це обмеження ФАСАДУ.
///
/// Друк синхронний і односпрямований — DeviceSession/JobEngine не задіяні.
class AddinLabelPrinter : public BpoFacadeBase {
public:
    static std::vector<std::u16string> names;
    AddinLabelPrinter();
    ~AddinLabelPrinter() override;

protected:
    int InterfaceRevision() const override { return 3004; }
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    // BuildActionsXml/RunAction — дефолтні: додаткових дій у принтера немає (§3.5 спеки).

private:
    void RegisterPrinterMethods();

    labelprinter::LabelPrinterDriver driver_;
};
