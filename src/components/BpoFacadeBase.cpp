#include "../core/pch.h"
#include "BpoFacadeBase.h"
#include "../helpers/ServiceTools.h"
#include "../platform/MoneyFormat.h"

BpoFacadeBase::BpoFacadeBase() = default;

BpoFacadeBase::~BpoFacadeBase() {
    // Пристрій закриває НАЩАДОК у своєму деструкторі: CloseDevice() віртуальний,
    // а на момент роботи базового деструктора нащадка вже немає.
    ServiceTools::DisableComponentLogging(this);
}

// ============================ помилки ============================

void BpoFacadeBase::SetError(int code, const std::string& description) {
    lastErrorCode_ = code;
    lastErrorDesc_ = description;
}

void BpoFacadeBase::ClearError() {
    lastErrorCode_ = 0;
    lastErrorDesc_.clear();
}

// Числова таксономія для ПолучитьОшибку (LONG). Протокольний код термінала —
// числовий, проходить як є; рядкові коди драйверів мапляться в стабільні числа.
// ⚠️ Таблиця ЄДИНА на всю компоненту: числа 0..11 — наявна нумерація еквайрингу
// (ecr_native_host жорстко чекає 3 = UNSUPPORTED), 12..16 — коди принтера етикеток.
// Нові коди дописувати з 17, наявні НЕ РУХАТИ.
int BpoFacadeBase::CodeToInt(const std::string& code) {
    if (code.empty()) return -1;
    bool numeric = true;
    for (char c : code) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) { try { return std::stoi(code); } catch (...) { return -1; } }
    if (code == "OK")                  return 0;
    if (code == "NOT_CONNECTED")       return 1;
    if (code == "DEVICE_BUSY")         return 2;
    if (code == "UNSUPPORTED")         return 3;
    if (code == "TIMEOUT")             return 4;
    if (code == "DISCONNECTED")        return 5;
    if (code == "SEND_FAILED")         return 6;
    if (code == "STOPPED")             return 7;
    if (code == "CONCURRENT")          return 8;
    if (code == "DESYNC")              return 9;
    if (code == "BAD_RESPONSE")        return 10;
    if (code == "EXCEPTION")           return 11;
    if (code == "BAD_INPUT")           return 12;
    if (code == "TRANSPORT_ERROR")     return 13;
    if (code == "UNSUPPORTED_BARCODE") return 14;
    if (code == "BARCODE_TOO_WIDE")    return 15;
    if (code == "RENDER_ERROR")        return 16;
    return -1;   // невідомий нечисловий код
}

bool BpoFacadeBase::MapEnvToBool(const ResultEnvelope& env) {
    if (env.ok) { ClearError(); return true; }
    SetError(CodeToInt(env.code), env.description.empty() ? env.code : env.description);
    return false;
}

// ============================ хелпери ============================

std::string BpoFacadeBase::XmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;
        }
    }
    return out;
}

std::string BpoFacadeBase::ToUpperAscii(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string BpoFacadeBase::PayloadStr(const ResultEnvelope& env, const char* field) {
    try {
        if (!env.payload.is_object()) return {};
        auto it = env.payload.find(field);
        if (it == env.payload.end()) return {};
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
        if (it->is_number()) return MoneyToString(it->get<double>());
        return {};
    } catch (...) { return {}; }
}

std::string BpoFacadeBase::VariantToString(VH value) {
    try {
        switch (value.type()) {
        case VTYPE_PWSTR:
            return static_cast<std::string>(value);
        case VTYPE_EMPTY:
        case VTYPE_NULL:
            return {};
        case VTYPE_BOOL:
            return static_cast<bool>(value) ? "true" : "false";
        case VTYPE_R4:
        case VTYPE_R8: {
            const double d = value;
            // Ціле значення віддаємо без дробової частини: Port=2000, а не 2000.00.
            if (d == static_cast<double>(static_cast<long long>(d)))
                return std::to_string(static_cast<long long>(d));
            return MoneyToString(d);
        }
        default:
            return std::to_string(static_cast<int64_t>(value));
        }
    } catch (...) {
        return {};
    }
}

double BpoFacadeBase::VariantToDouble(VH value) {
    try { return static_cast<double>(value); }
    catch (...) { return 0.0; }
}

std::string BpoFacadeBase::Param(const char* name, const std::string& fallback) const {
    auto it = params_.find(name);
    return (it == params_.end() || it->second.empty()) ? fallback : it->second;
}

bool BpoFacadeBase::ParamBool(const char* name, bool fallback) const {
    const std::string raw = Param(name);
    if (raw.empty()) return fallback;
    const std::string v = ToUpperAscii(raw);
    if (v == "FALSE" || v == "0" || v == "НЕТ" || v == "НІ") return false;
    if (v == "TRUE"  || v == "1" || v == "ДА"  || v == "ТАК") return true;
    return fallback;
}

bool BpoFacadeBase::CheckDeviceId(const std::string& deviceId) {
    if (deviceId_.empty()) {
        SetError(1, "Обладнання не підключено");
        return false;
    }
    if (deviceId != deviceId_) {
        SetError(1, "Невідомий ідентифікатор пристрою: " + deviceId);
        return false;
    }
    return true;
}

// ======================= дефолти гачків =======================

std::string BpoFacadeBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Actions/>";
}

bool BpoFacadeBase::RunAction(const std::string& name) {
    SetError(2, "Невідома додаткова дія: " + name);
    return false;
}

// ============================ системні методи ============================

void BpoFacadeBase::RegisterSystemMethods() {

    AddFunction(u"GetInterfaceRevision", u"ПолучитьРевизиюИнтерфейса",
        Ret([this]() -> int { return InterfaceRevision(); }));

    AddFunction(u"GetVersionNumber", u"ПолучитьНомерВерсии",
        Ret([]() -> std::string { return AddInNative::version(); }));

    // Паспорт драйвера. EquipmentType тут ІНФОРМАЦІЙНИЙ (конфігурація його з переліком
    // не звіряє) — на відміну від EquipmentType, що ПРИХОДИТЬ в УстановитьПараметр.
    // Булеві конфігурація читає як ВРег(...) = "TRUE", тож "1" не спрацював би.
    AddFunction(u"GetDescription", u"ПолучитьОписание",
        Ret([this](VH out) -> bool {
            try {
                const DriverInfo info = BuildDriverInfo();
                const std::string ver = AddInNative::version();
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<DriverDescription "
                    "Name=\"" + XmlEscape(info.name) + "\" "
                    "Description=\"" + XmlEscape(info.description) + "\" "
                    "EquipmentType=\"" + XmlEscape(info.equipmentType) + "\" "
                    "IntegrationComponent=\"false\" "
                    "MainDriverInstalled=\"true\" "
                    "DriverVersion=\"" + ver + "\" "
                    "IntegrationComponentVersion=\"" + ver + "\" "
                    "IsEmulator=\"false\" "
                    "LocalizationSupported=\"false\" "
                    "LogIsEnabled=\"" + (info.logEnabled ? "true" : "false") + "\" "
                    "LogPath=\"" + XmlEscape(Param("LogPath")) + "\"/>");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ПолучитьОписание: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverDescription", u"ОписаниеДрайвера", false, {} } });

    // Опис форми налаштувань. Формат СУВОРИЙ: корінь Settings, тип у TypeValue —
    // інакше форма БПО мовчки лишається без жодного поля (§3.1 контракту).
    AddFunction(u"GetParameters", u"ПолучитьПараметры",
        Ret([this](VH out) -> bool {
            try {
                out = BuildSettingsXml();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverParameters", u"ПараметрыДрайвера", false, {} } });

    // Параметри приходять ПООДИНЦІ до Подключить, і першим — EquipmentType.
    AddFunction(u"SetParameter", u"УстановитьПараметр",
        Ret([this](VH name, VH value) -> bool {
            try {
                const std::string n = VariantToString(name);
                const std::string v = VariantToString(value);
                if (n == "EquipmentType" && !AcceptEquipmentType(v)) {
                    SetError(2, "Непідтримуваний тип обладнання: " + v);
                    return false;
                }
                params_[n] = v;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Name", u"ИмяПараметра", true, {} },
                                ParamSpec{ u"Value", u"ЗначениеПараметра", false, DefaultHelper(u"") } });

    // Підключення: параметрів не приймає (вони вже накопичені), ИДУстройства — OUT.
    AddFunction(u"Connect", u"Подключить",
        Ret([this](VH deviceIdOut) -> bool {
            try {
                // Журнал вмикаємо ДО OpenDevice — щоб діагностика самого підключення
                // потрапила у файл. Рівень і шлях — з параметрів підключення.
                const std::string logPath = Param("LogPath");
                if (!logPath.empty())
                    ServiceTools::EnableComponentLogging(this, Param("LogLevel", "Info"), logPath);

                std::string id;
                if (!OpenDevice(id)) return false;   // OpenDevice сам поставив lastError
                deviceId_ = id;
                deviceIdOut = deviceId_;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка Подключить: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    AddFunction(u"Disconnect", u"Отключить",
        Ret([this](VH deviceId) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!deviceId_.empty() && id != deviceId_) {
                    SetError(1, "Невідомий ідентифікатор пристрою: " + id);
                    return false;
                }
                CloseDevice();
                deviceId_.clear();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    // Перевірка параметрів підключення з форми налаштувань. Підключеним бути НЕ
    // зобов'язана: форма кличе ТестУстройства одразу після УстановитьПараметр.
    AddFunction(u"EquipmentTest", u"ТестУстройства",
        Ret([this](VH resultOut, VH demoOut) -> bool {
            std::string text;
            bool demo = false;
            bool ok = false;
            try {
                ok = ProbeDevice(text, demo);
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                text = std::string("Помилка тесту: ") + e.what();
                ok = false;
            }
            demoOut = demo;
            resultOut = text;
            return ok;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"TestResult", u"РезультатТеста", false, {} },
                                ParamSpec{ u"DemoMode", u"АктивированДемоРежим", false, {} } });

    AddFunction(u"GetLastError", u"ПолучитьОшибку",
        Ret([this](VH descriptionOut) -> int {
            descriptionOut = lastErrorDesc_;
            return lastErrorCode_;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ErrorDescription", u"ОписаниеОшибки", false, {} } });

    // Додаткові дії — пункти меню «Функції» форми налаштувань обладнання
    // (форма АДМІНІСТРАТОРА, не робоче місце касира).
    AddFunction(u"GetAdditionalActions", u"ПолучитьДополнительныеДействия",
        Ret([this](VH out) -> bool {
            try {
                out = BuildActionsXml();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Actions", u"ДополнительныеДействия", false, {} } });

    AddFunction(u"DoAdditionalAction", u"ВыполнитьДополнительноеДействие",
        Ret([this](VH actionName) -> bool {
            try {
                return RunAction(VariantToString(actionName));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ActionName", u"ИмяДействия", true, {} } });

    REPORT_INFO("Реєстрація системних методів БПО-фасаду завершена");
}
