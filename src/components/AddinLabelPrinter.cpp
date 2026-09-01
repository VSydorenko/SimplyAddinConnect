#include "../core/pch.h"
#include "AddinLabelPrinter.h"
#include "../helpers/ServiceTools.h"
#include "../drivers/label_printer/LabelXml.h"
#include <string>

using namespace labelprinter;

// Реєстрація компоненти через статичний член класу + анти-стрип reference.
REGISTER_COMPONENT(u"LabelPrinter", AddinLabelPrinter)

namespace {

/// Людський опис цілі підключення для РезультатТеста — адміністратор має бачити,
/// що саме перевірялось, а не просто «помилка».
std::string DescribeTarget(const DeviceProfile& p) {
    if (p.transport == DeviceProfile::Transport::Tcp)
        return "tcp://" + p.host + ":" + std::to_string(p.port);
    return "черга Windows \"" + p.printerName + "\"";
}

/// Обов'язкові для обраного транспорту поля профілю.
///
/// ⚠️ Свідомо ТУТ, а не в LabelXml::ProfileFromParameters: її контракт —
/// «порожня мапа → дефолти, а не помилка» — закріплений тестом і ламати його не
/// можна. Без цієї перевірки порожній Host при tcp дійшов би до транспорту й
/// адміністратор побачив би «принтер недоступний» замість «не заповнено параметр».
bool ValidateProfile(const DeviceProfile& p, std::string& err) {
    if (p.transport == DeviceProfile::Transport::Tcp) {
        if (p.host.empty()) {
            err = "Не заповнено параметр Host — IP-адресу мережевого принтера";
            return false;
        }
    } else if (p.printerName.empty()) {
        err = "Не заповнено параметр PrinterName — ім'я черги друку Windows";
        return false;
    }
    return true;
}

} // namespace

AddinLabelPrinter::AddinLabelPrinter() {
    REPORT_INFO("Ініціалізація компоненти LabelPrinter");
    RegisterSystemMethods();
    RegisterPrinterMethods();
}

AddinLabelPrinter::~AddinLabelPrinter() {
    REPORT_INFO("Завершення роботи компоненти LabelPrinter");
    if (!DeviceId().empty()) driver_.Disconnect(DeviceId());
}

// ============================ гачки BpoFacadeBase ============================

BpoFacadeBase::DriverInfo AddinLabelPrinter::BuildDriverInfo() const {
    // logEnabled=false: вимога ІТС про лог за замовчуванням стосується ККТ і
    // POSTerminal; принтер етикеток у той перелік не входить, і параметрів
    // LogPath/LogLevel у його формі налаштувань немає.
    return DriverInfo{ "Драйвер принтера этикеток (SimplyAddinConnect)",
                       "Друк етикеток на ZPL-принтер через spooler-RAW або TCP:9100",
                       "LabelPrinter",
                       /*logEnabled*/ false };
}

// Опис форми налаштувань підключення (транспорт/порт/DPI/…).
//
// ФОРМАТ СУВОРИЙ. Форма налаштувань БПО (Catalogs.ПодключаемоеОборудование, ФормаНастройки)
// читає XML послідовно і входить у розбір лише за умови кореневого вузла `Settings`:
// корінь `Parameters` не проходить цю умову, і форма мовчки лишається БЕЗ ЖОДНОГО поля.
// Тип параметра читається з атрибута `TypeValue` ("String"/"Number"/"Boolean"), а не `Type`.
// Розпізнаються: Page@Caption, Group@Caption, Parameter@Name/@Caption/@TypeValue/
// @DefaultValue/@Description/@FieldFormat/@ReadOnly і вкладений ChoiceList/Item@Value
// (текст вузла — представлення). Решта атрибутів ігнорується.
// Джерела: ІТС «Разработка драйвера для подключения оборудования локально к устройству
// пользователя» (розділ ТаблицаПараметров) + розбір парсера у конфігурації УНФ.
std::string AddinLabelPrinter::BuildSettingsXml() const {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Settings>"
        "<Page Caption=\"Параметри\">"
        "<Group Caption=\"Підключення\">"
        "<Parameter Name=\"TransportKind\" Caption=\"Транспорт\" TypeValue=\"String\" DefaultValue=\"spooler\""
        " Description=\"spooler — черга Windows (USB/локальний); tcp — мережевий принтер\">"
        "<ChoiceList>"
        "<Item Value=\"spooler\">Черга Windows (USB/локальний)</Item>"
        "<Item Value=\"tcp\">Мережевий (TCP)</Item>"
        "</ChoiceList>"
        "</Parameter>"
        "<Parameter Name=\"PrinterName\" Caption=\"Ім'я черги Windows\" TypeValue=\"String\" DefaultValue=\"\""
        " Description=\"Лише для транспорту spooler: точне ім'я черги як у «Пристрої та принтери»\"/>"
        "<Parameter Name=\"Host\" Caption=\"IP-адреса\" TypeValue=\"String\" DefaultValue=\"\""
        " Description=\"Лише для транспорту tcp\"/>"
        "<Parameter Name=\"Port\" Caption=\"Порт\" TypeValue=\"Number\" DefaultValue=\"9100\""
        " Description=\"Лише для транспорту tcp; RAW-друк — стандартно 9100\"/>"
        "</Group>"
        "<Group Caption=\"Друк\">"
        "<Parameter Name=\"DotsPerMm\" Caption=\"Точок на мм\" TypeValue=\"Number\" DefaultValue=\"8\""
        " Description=\"Роздільність друкувальної головки в точках на мм (не номінальний dpi)\">"
        "<ChoiceList>"
        "<Item Value=\"8\">8 (≈203 dpi)</Item>"
        "<Item Value=\"12\">12 (≈300 dpi)</Item>"
        "<Item Value=\"24\">24 (≈600 dpi)</Item>"
        "</ChoiceList>"
        "</Parameter>"
        "<Parameter Name=\"Darkness\" Caption=\"Темність\" TypeValue=\"Number\" DefaultValue=\"10\"/>"
        "<Parameter Name=\"Speed\" Caption=\"Швидкість\" TypeValue=\"Number\" DefaultValue=\"4\"/>"
        "<Parameter Name=\"LabelWidthMm\" Caption=\"Ширина етикетки, мм\" TypeValue=\"Number\" DefaultValue=\"0\""
        " Description=\"0 — не задавати (^PW)\"/>"
        "<Parameter Name=\"LabelHeightMm\" Caption=\"Висота етикетки, мм\" TypeValue=\"Number\" DefaultValue=\"0\""
        " Description=\"0 — не задавати (^LL)\"/>"
        "</Group>"
        "</Page>"
        "</Settings>";
}

bool AddinLabelPrinter::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ Enums/ТипыПодключаемогоОборудования —
    // ПринтерЭтикеток, а не англійський рядок LabelPrinter з таблиці ІТС.
    // Приймаємо обидва: коштує нічого, страхує від наступної редакції.
    return value == "ПринтерЭтикеток" || ToUpperAscii(value) == "LABELPRINTER";
}

bool AddinLabelPrinter::OpenDevice(std::string& deviceIdOut) {
    DeviceProfile profile;
    std::string err;
    if (!LabelXml::ProfileFromParameters(Params(), profile, err)) {
        const std::string text = err.empty() ? std::string("Некоректні параметри підключення") : err;
        SetError(CodeToInt("BAD_INPUT"), text);
        REPORT_ERROR("Помилка підключення принтера: " + text);
        return false;
    }
    if (!ValidateProfile(profile, err)) {
        SetError(CodeToInt("BAD_INPUT"), err);
        REPORT_ERROR("Помилка підключення принтера: " + err);
        return false;
    }
    const std::string id = driver_.Connect(profile);
    if (id.empty()) {
        SetError(CodeToInt("TRANSPORT_ERROR"), "Не вдалося зареєструвати пристрій");
        REPORT_ERROR("Не вдалося зареєструвати пристрій принтера");
        return false;
    }
    // Повторний Подключить БЕЗ Отключить — штатний сценарій 1С (перепідключення
    // обладнання після помилки друку). Без цього старий пристрій лишався б у
    // реєстрі драйвера з ВІДКРИТИМ сокетом, а новий відкрив би ДРУГУ сесію до
    // того самого принтера. Знімаємо старий лише КОЛИ НОВИЙ УЖЕ зареєстровано:
    // тоді невдале перепідключення не лишає deviceId_ на вже знятому пристрої.
    // Двох живих сокетів це не дає — Connect транспорт не відкриває (лінива Open).
    if (!DeviceId().empty() && DeviceId() != id) driver_.Disconnect(DeviceId());
    deviceIdOut = id;   // прокидуємо СПРАВЖНІЙ id драйвера — лог фасаду й драйвера збігається
    return true;
}

void AddinLabelPrinter::CloseDevice() {
    if (!DeviceId().empty()) driver_.Disconnect(DeviceId());
}

bool AddinLabelPrinter::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має
    DeviceProfile profile;
    std::string err;
    if (!LabelXml::ProfileFromParameters(Params(), profile, err)) {
        const std::string text = err.empty() ? std::string("Некоректні параметри підключення") : err;
        SetError(CodeToInt("BAD_INPUT"), text);
        REPORT_ERROR("Помилка тесту принтера: " + text);
        resultOut = text;
        return false;
    }
    if (!ValidateProfile(profile, err)) {
        SetError(CodeToInt("BAD_INPUT"), err);
        REPORT_ERROR("Помилка тесту принтера: " + err);
        resultOut = err;
        return false;
    }
    // Уже підключений принтер перевіряємо НА НЬОМУ САМОМУ: окрема сесія на :9100
    // типовим ZPL-принтером НЕ приймається, і адміністратор побачив би
    // TRANSPORT_ERROR на справному пристрої. Драйвер коректно розрізняє «канал уже
    // відкритий» -> Ok. Окрема короткоживуча сесія лишається шляхом для випадку,
    // коли підключення ще немає, — тоді чіпати нічого.
    const bool reuseActive = !DeviceId().empty();
    const std::string probeId = reuseActive ? DeviceId() : driver_.Connect(profile);
    if (probeId.empty()) {
        SetError(CodeToInt("TRANSPORT_ERROR"), "Не вдалося створити канал до принтера");
        REPORT_ERROR("Не вдалося створити канал до принтера");
        resultOut = "Не вдалося створити канал до принтера";
        return false;
    }
    const ResultEnvelope env = driver_.Probe(probeId);
    if (!reuseActive) driver_.Disconnect(probeId);   // знімаємо ЛИШЕ свій тимчасовий пристрій

    const std::string target = DescribeTarget(profile);
    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        REPORT_ERROR("Принтер недоступний (" + target + "): " + env.description);
        resultOut = target + " — недоступно: " + env.description;
        return false;
    }
    resultOut = target + " — доступно";
    ClearError();
    return true;
}

// ============================ функціональні методи ============================

void AddinLabelPrinter::RegisterPrinterMethods() {

    // Ініціалізація принтера: IN ИДУстройства -> BOOL.
    AddFunction(u"InitializePrinter", u"ИнициализацияПринтера",
        Ret([this](VH deviceId) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!CheckDeviceId(id)) return false;
                return MapEnvToBool(driver_.InitializePrinter(id));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ИнициализацияПринтера: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", true, {} } });

    // Друк етикеток: IN ИДУстройства, IN ТаблицаЭтикеток(XML), IN СтатусПакета -> BOOL.
    // СтатусПакета — first/regular/last: first несе <Formatting> і скидає кеш формату,
    // last після друку його очищає.
    AddFunction(u"PrintLabels", u"ПечатьЭтикеток",
        Ret([this](VH deviceId, VH labelsTable, VH packageStatus) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!CheckDeviceId(id)) return false;
                const std::string xml = VariantToString(labelsTable);
                LabelBatch batch;
                std::string err;
                if (!LabelXml::ParseLabelsTable(xml, batch, err)) {
                    const std::string text = err.empty() ? "Некоректний пакет ТаблицаЭтикеток" : err;
                    SetError(CodeToInt("BAD_INPUT"), text);
                    REPORT_ERROR("Помилка розбору ТаблицаЭтикеток: " + text);
                    return false;
                }
                return MapEnvToBool(driver_.PrintLabels(id, batch, VariantToString(packageStatus)));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ПечатьЭтикеток: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID", u"ИДУстройства", true, {} },
            ParamSpec{ u"LabelsTable", u"ДанныеДляВыгрузки", true, {} },
            ParamSpec{ u"PackageStatus", u"СтатусПакета", true, {} } });

    REPORT_INFO("Реєстрація методів LabelPrinter завершена");
}
