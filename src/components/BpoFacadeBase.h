#pragma once
#include "../core/AddInNative.h"
#include "../platform/ResultEnvelope.h"
#include <map>
#include <string>

/// Спільна КОНТРАКТНА половина БПО-фасадів — усе, що однакове для будь-якого типу
/// обладнання. Типу обладнання, протоколу й драйвера цей клас не знає.
///
/// Контракт «Подключаемое оборудование» — docs/architecture/bpo-contract.md §2.
/// Послідовність підключення жорстка: УстановитьПараметр("EquipmentType", …) →
/// УстановитьПараметр(…) × N → Подключить(ИДУстройства[OUT]). Тому параметри
/// накопичуються НА ОБ'ЄКТІ, і один об'єкт обслуговує рівно ОДИН пристрій:
/// параметри другого затерли б перший.
///
/// Нащадок реалізує гачки нижче й кличе RegisterSystemMethods() у своєму конструкторі.
class BpoFacadeBase : public AddInNative {
public:
    virtual ~BpoFacadeBase();

protected:
    BpoFacadeBase();

    /// ЗМІННА частина паспорта драйвера. Решту XML (версії, IntegrationComponent,
    /// MainDriverInstalled, IsEmulator, LocalizationSupported, LogPath) складає база —
    /// це контрактні поля, однакові для всіх фасадів (bpo-contract.md §4.2).
    struct DriverInfo {
        std::string name;            ///< DriverDescription@Name
        std::string description;     ///< @Description
        std::string equipmentType;   ///< @EquipmentType — рядок ІТС (POSTerminal / LabelPrinter)
        bool logEnabled = false;     ///< @LogIsEnabled (ІТС вимагає true для ККТ і POSTerminal)
    };

    // ======================= гачки нащадка =======================

    /// Число ревізії інтерфейсу = ВИБІР РОЗКЛАДКИ ПАРАМЕТРІВ, а не «наскільки ми сучасні».
    virtual int InterfaceRevision() const = 0;
    virtual DriverInfo BuildDriverInfo() const = 0;
    /// XML форми налаштувань: корінь Settings → Page → Group → Parameter@TypeValue.
    virtual std::string BuildSettingsXml() const = 0;
    /// XML додаткових дій. Дефолт — порожній набір.
    virtual std::string BuildActionsXml() const;
    /// ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал / ПринтерЭтикеток),
    /// а не англійський рядок ІТС. Порівнювати толерантно (§4.1 контракту).
    virtual bool AcceptEquipmentType(const std::string& value) const = 0;
    /// Фактичне підключення з накопичених параметрів. Нащадок сам вирішує, який
    /// ИДУстройства віддати: синтезований чи справжній id від драйвера.
    /// При невдачі САМ ставить lastError через SetError і повертає false.
    virtual bool OpenDevice(std::string& deviceIdOut) = 0;
    virtual void CloseDevice() = 0;
    /// Тіло ТестУстройства: перевірка ПАРАМЕТРІВ + досяжності обладнання.
    /// При невдачі сам ставить lastError; resultOut — текст для адміністратора.
    virtual bool ProbeDevice(std::string& resultOut, bool& demoOut) = 0;
    /// Виконання додаткової дії з BuildActionsXml. Дефолт — «невідома дія».
    virtual bool RunAction(const std::string& name);

    /// Реєструє системну половину контракту. Нащадок кличе її у своєму конструкторі
    /// ПЕРЕД реєстрацією власних методів.
    void RegisterSystemMethods();

    // ======================= сервіси для нащадків =======================

    /// Виданий при Подключить. Порожній = не підключено.
    const std::string& DeviceId() const { return deviceId_; }
    /// Перевіряє, що ИДУстройства збігається з виданим. При розбіжності ставить
    /// lastError і повертає false.
    bool CheckDeviceId(const std::string& deviceId);

    /// Накопичене через УстановитьПараметр.
    std::string Param(const char* name, const std::string& fallback = "") const;
    /// Булевий параметр форми налаштувань. Приймає true/false/1/0/да/нет/так/ні
    /// незалежно від регістру — 1С передає Boolean-параметр БУЛЕВИМ, а не рядком,
    /// і VariantToString зводить його до "true"/"false".
    bool ParamBool(const char* name, bool fallback) const;
    const std::map<std::string, std::string>& Params() const { return params_; }

    void SetError(int code, const std::string& description);
    void ClearError();
    /// Хвіст методу: ставить lastError із конверта і повертає значення для 1С.
    bool MapEnvToBool(const ResultEnvelope& env);
    /// ЄДИНА числова таксономія на всю компоненту (див. план, Global Constraints).
    /// Не робити віртуальною: кожен новий драйвер завів би свою нумерацію.
    static int CodeToInt(const std::string& code);
    /// Рядкове поле з payload конверта; відсутнє поле → порожній рядок.
    static std::string PayloadStr(const ResultEnvelope& env, const char* field);

    /// ⚠️ Толерантне читання вхідного параметра в рядок. Пряме
    /// static_cast<std::string>(VH) КИДАЄ на всьому, крім VTYPE_PWSTR, а 1С передає
    /// параметри форми налаштувань їхніми ОГОЛОШЕНИМИ типами: Port/Baud/DotsPerMm —
    /// числами, VoidAsRefund — булевим.
    static std::string VariantToString(VH value);
    /// Те саме для числа: порожній/незаповнений параметр → 0.0 замість винятку.
    static double VariantToDouble(VH value);

    static std::string XmlEscape(const std::string& s);
    static std::string ToUpperAscii(std::string s);

private:
    std::map<std::string, std::string> params_;   ///< накопичене через УстановитьПараметр
    std::string deviceId_;                        ///< видане при Подключить
    int lastErrorCode_ = 0;
    std::string lastErrorDesc_;
};
