#pragma once
#include "../core/AddInNative.h"
#include "../drivers/label_printer/LabelPrinterDriver.h"
#include <vector>
#include <string>

/// Компонента 1С «Принтер этикеток» (тип БПО LabelPrinter).
/// БПО-фасад над LabelPrinterDriver: приймає/віддає XML (LabelsTable,
/// ConnectionParameters, DriverDescription), мапить ResultEnvelope драйвера у
/// BOOL + збережений lastError (код+опис для ПолучитьОшибку/GetLastError).
/// Друк синхронний і односпрямований — DeviceSession/JobEngine не задіяні.
class AddinLabelPrinter : public AddInNative {
public:
    static std::vector<std::u16string> names;
    AddinLabelPrinter();
    virtual ~AddinLabelPrinter();

private:
    void RegisterMethods();

    // Мапить результат драйвера у BOOL для 1С; при !ok зберігає код+опис
    // для наступного ПолучитьОшибку/GetLastError.
    bool mapEnvToBool(const ResultEnvelope& env);

    labelprinter::LabelPrinterDriver driver_;
    int lastErrorCode_ = 0;         ///< код останньої помилки (0 = немає)
    std::string lastErrorDesc_;     ///< опис останньої помилки для ПолучитьОшибку
};
