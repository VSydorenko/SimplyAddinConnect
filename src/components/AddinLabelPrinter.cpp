#include "../core/pch.h"
#include "AddinLabelPrinter.h"
#include "../helpers/ServiceTools.h"
#include "../drivers/label_printer/LabelXml.h"
#include <string>

using namespace labelprinter;

// Реєстрація компоненти через статичний член класу + анти-стрип reference.
REGISTER_COMPONENT(u"LabelPrinter", AddinLabelPrinter)

namespace {
// Версія вимог до інтерфейсу БПО (ревізія контракту «Подключаемое оборудование»).
constexpr int kInterfaceRevision = 4007;

// Числовий код помилки з машинного коду ResultEnvelope для GetLastError (LONG):
// цифровий код (напр. код відповіді пристрою) -> як є; рядкова таксономія драйвера
// -> стабільні числові коди; невідомий нечисловий -> -1.
int CodeToInt(const std::string& code) {
    if (code.empty()) return -1;
    bool numeric = true;
    for (char c : code) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) { try { return std::stoi(code); } catch (...) { return -1; } }
    if (code == "OK")                  return 0;
    if (code == "NOT_CONNECTED")       return 1;
    if (code == "BAD_INPUT")           return 2;
    if (code == "TRANSPORT_ERROR")     return 3;
    if (code == "UNSUPPORTED_BARCODE") return 4;
    if (code == "BARCODE_TOO_WIDE")    return 5;
    if (code == "RENDER_ERROR")        return 6;
    if (code == "EXCEPTION")           return 7;
    return -1;   // невідомий нечисловий код
}

// DriverDescription — паспорт драйвера для БПО (тип обладнання, версії, прапорці).
std::string BuildDriverDescriptionXml() {
    const std::string ver = AddInNative::version();
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DriverDescription "
        "Name=\"Драйвер принтера этикеток (SimplyAddinConnect)\" "
        "Description=\"Друк етикеток на ZPL-принтер через spooler-RAW або TCP:9100\" "
        "EquipmentType=\"LabelPrinter\" "
        "IntegrationComponent=\"false\" "
        "MainDriverInstalled=\"true\" "
        "DriverVersion=\"" + ver + "\" "
        "IntegrationComponentVersion=\"" + ver + "\" "
        "IsEmulator=\"false\" "
        "LocalizationSupported=\"false\" "
        "AutoSetup=\"false\" "
        "LogIsEnabled=\"false\" "
        "LogPath=\"\"/>";
}

// TableParameters — опис форми налаштувань підключення (транспорт/порт/DPI/…).
std::string BuildTableParametersXml() {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Parameters>"
        "<Parameter Name=\"TransportKind\" Caption=\"Транспорт\" Type=\"String\" DefaultValue=\"spooler\"/>"
        "<Parameter Name=\"PrinterName\" Caption=\"Ім'я черги Windows\" Type=\"String\" DefaultValue=\"\"/>"
        "<Parameter Name=\"Host\" Caption=\"IP-адреса\" Type=\"String\" DefaultValue=\"\"/>"
        "<Parameter Name=\"Port\" Caption=\"Порт\" Type=\"Number\" DefaultValue=\"9100\"/>"
        "<Parameter Name=\"DotsPerMm\" Caption=\"Точок на мм (8/12/24)\" Type=\"Number\" DefaultValue=\"8\"/>"
        "<Parameter Name=\"Darkness\" Caption=\"Темність\" Type=\"Number\" DefaultValue=\"10\"/>"
        "<Parameter Name=\"Speed\" Caption=\"Швидкість\" Type=\"Number\" DefaultValue=\"4\"/>"
        "<Parameter Name=\"LabelWidthMm\" Caption=\"Ширина етикетки, мм\" Type=\"Number\" DefaultValue=\"0\"/>"
        "<Parameter Name=\"LabelHeightMm\" Caption=\"Висота етикетки, мм\" Type=\"Number\" DefaultValue=\"0\"/>"
        "</Parameters>";
}
} // namespace

AddinLabelPrinter::AddinLabelPrinter() {
    REPORT_INFO("Ініціалізація компоненти LabelPrinter");
    RegisterMethods();
}

AddinLabelPrinter::~AddinLabelPrinter() {
    REPORT_INFO("Завершення роботи компоненти LabelPrinter");
    ServiceTools::DisableComponentLogging(this);
}

bool AddinLabelPrinter::mapEnvToBool(const ResultEnvelope& env) {
    if (!env.ok) {
        lastErrorCode_ = CodeToInt(env.code);
        lastErrorDesc_ = env.description;
    } else {
        lastErrorCode_ = 0;
        lastErrorDesc_.clear();
    }
    return env.ok;
}

void AddinLabelPrinter::RegisterMethods() {
    // ================= СИСТЕМНІ методи БПО =================

    // Версія вимог до інтерфейсу драйвера — LONG, без параметрів.
    AddFunction(u"GetInterfaceRevision", u"ПолучитьРевизиюИнтерфейса",
        Ret([]() -> int { return kInterfaceRevision; }));

    // Паспорт драйвера: OUT DriverDescription (XML) -> BOOL.
    AddFunction(u"GetDescription", u"ПолучитьОписание",
        Ret([this](VH driverDescription) -> bool {
            try {
                driverDescription = BuildDriverDescriptionXml();
                lastErrorCode_ = 0; lastErrorDesc_.clear();
                return true;
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ПолучитьОписание: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverDescription", u"ОписаниеДрайвера", /*required*/false, {} } });

    // Остання помилка: OUT ErrorDescription (опис) -> LONG (код).
    AddFunction(u"GetLastError", u"ПолучитьОшибку",
        Ret([this](VH errorDescription) -> int {
            errorDescription = lastErrorDesc_;
            return lastErrorCode_;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ErrorDescription", u"ОписаниеОшибки", false, {} } });

    // Форма параметрів підключення: IN EquipmentType, OUT TableParameters (XML) -> BOOL.
    AddFunction(u"EquipmentParameters", u"ПараметрыОборудования",
        Ret([this](VH equipmentType, VH tableParameters) -> bool {
            try {
                (void)equipmentType;
                tableParameters = BuildTableParametersXml();
                lastErrorCode_ = 0; lastErrorDesc_.clear();
                return true;
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ПараметрыОборудования: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"EquipmentType", u"ТипОборудования", false, {} },
            ParamSpec{ u"TableParameters", u"ТаблицаПараметров", false, {} } });

    // Підключення: OUT DeviceID, IN EquipmentType, IN ConnectionParameters (XML) -> BOOL.
    AddFunction(u"ConnectEquipment", u"ПодключитьОборудование",
        Ret([this](VH deviceId, VH equipmentType, VH connectionParameters) -> bool {
            try {
                (void)equipmentType;
                std::string xml = static_cast<std::string>(connectionParameters);
                DeviceProfile profile; std::string err;
                if (!LabelXml::ParseConnectionParameters(xml, profile, err)) {
                    lastErrorCode_ = -1; lastErrorDesc_ = err.empty() ? "Некоректні параметри підключення" : err;
                    REPORT_ERROR(std::string("Помилка розбору ConnectionParameters: ") + lastErrorDesc_);
                    return false;
                }
                std::string id = driver_.Connect(profile);
                if (id.empty()) {
                    lastErrorCode_ = -1; lastErrorDesc_ = "Не вдалося зареєструвати пристрій";
                    return false;
                }
                deviceId = id;
                lastErrorCode_ = 0; lastErrorDesc_.clear();
                return true;
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ПодключитьОборудование: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID", u"ИдентификаторУстройства", false, {} },
            ParamSpec{ u"EquipmentType", u"ТипОборудования", false, {} },
            ParamSpec{ u"ConnectionParameters", u"ПараметрыПодключения", true, {} } });

    // Відключення: IN DeviceID -> BOOL.
    AddFunction(u"DisconnectEquipment", u"ОтключитьОборудование",
        Ret([this](VH deviceId) -> bool {
            try {
                driver_.Disconnect(static_cast<std::string>(deviceId));
                lastErrorCode_ = 0; lastErrorDesc_.clear();
                return true;
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ОтключитьОборудование: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИдентификаторУстройства", true, {} } });

    // Тест обладнання: IN EquipmentType, IN ConnectionParameters, OUT Description, OUT DemoModeIsActivated -> BOOL.
    AddFunction(u"EquipmentTest", u"ТестированиеОборудования",
        Ret([this](VH equipmentType, VH connectionParameters, VH description, VH demoModeIsActivated) -> bool {
            try {
                (void)equipmentType;
                demoModeIsActivated = false;
                std::string xml = static_cast<std::string>(connectionParameters);
                DeviceProfile profile; std::string err;
                if (!LabelXml::ParseConnectionParameters(xml, profile, err)) {
                    lastErrorCode_ = -1; lastErrorDesc_ = err.empty() ? "Некоректні параметри підключення" : err;
                    description = lastErrorDesc_;
                    return false;
                }
                description = std::string("Параметри підключення коректні; демо-режим не використовується");
                lastErrorCode_ = 0; lastErrorDesc_.clear();
                return true;
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ТестированиеОборудования: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"EquipmentType", u"ТипОборудования", false, {} },
            ParamSpec{ u"ConnectionParameters", u"ПараметрыПодключения", true, {} },
            ParamSpec{ u"Description", u"Описание", false, {} },
            ParamSpec{ u"DemoModeIsActivated", u"ДемоРежимАктивирован", false, {} } });

    // Інформація застосунку — приймаємо й ігноруємо (немає ліцензійної логіки) -> BOOL.
    AddFunction(u"SetApplicationInformation", u"УстановитьИнформациюПриложения",
        Ret([this](VH applicationSettings) -> bool {
            (void)applicationSettings;
            lastErrorCode_ = 0; lastErrorDesc_.clear();
            return true;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ApplicationSettings", u"НастройкиПриложения", false, {} } });

    // ================= ФУНКЦІОНАЛЬНІ методи =================

    // Ініціалізація принтера: IN DeviceID -> BOOL.
    AddFunction(u"InitializePrinter", u"ИнициализацияПринтера",
        Ret([this](VH deviceId) -> bool {
            try {
                return mapEnvToBool(driver_.InitializePrinter(static_cast<std::string>(deviceId)));
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ИнициализацияПринтера: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИдентификаторУстройства", true, {} } });

    // Друк етикеток: IN DeviceID, IN LabelsTable (XML), IN PackageStatus -> BOOL.
    AddFunction(u"PrintLabels", u"ПечатьЭтикеток",
        Ret([this](VH deviceId, VH labelsTable, VH packageStatus) -> bool {
            try {
                std::string xml = static_cast<std::string>(labelsTable);
                LabelBatch batch; std::string err;
                if (!LabelXml::ParseLabelsTable(xml, batch, err)) {
                    lastErrorCode_ = -1; lastErrorDesc_ = err.empty() ? "Некоректний пакет LabelsTable" : err;
                    REPORT_ERROR(std::string("Помилка розбору LabelsTable: ") + lastErrorDesc_);
                    return false;
                }
                return mapEnvToBool(driver_.PrintLabels(
                    static_cast<std::string>(deviceId), batch, static_cast<std::string>(packageStatus)));
            } catch (const std::exception& e) {
                lastErrorCode_ = -1; lastErrorDesc_ = e.what();
                REPORT_ERROR(std::string("Помилка ПечатьЭтикеток: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID", u"ИдентификаторУстройства", true, {} },
            ParamSpec{ u"LabelsTable", u"ТаблицаЭтикеток", true, {} },
            ParamSpec{ u"PackageStatus", u"СтатусПакета", true, {} } });

    REPORT_INFO("Реєстрація методів LabelPrinter завершена");
}
