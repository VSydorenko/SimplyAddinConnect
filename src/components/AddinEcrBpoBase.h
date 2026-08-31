#pragma once
#include "../core/AddInNative.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonDriver.h"
#include <map>
#include <string>
#include <vector>

/// Спільна половина БПО-фасадів еквайрингу над драйвером ECRPrivatJSON.
///
/// Контракт «Подключаемое оборудование» описано в docs/architecture/bpo-contract.md.
/// Системні методи, параметри підключення й методи можливостей однакові для всіх
/// ревізій — вони тут. Різниця між ревізіями лише в РОЗКЛАДЦІ параметрів платіжних
/// методів, тож похідний клас додає рівно їх плюс власну `ПолучитьРевизиюИнтерфейса`.
///
/// Чому не один клас на всі ревізії: у моделі компоненти одне ім'я методу — це одна
/// арність (`GetNParams` віддає одне число), а `ОплатитьПлатежнойКартой` має 7 або 9
/// параметрів залежно від ревізії, причому на позиції 1 різний ЗМІСТ. Див. §2.2 доку.
class AddinEcrBpoBase : public AddInNative {
public:
    virtual ~AddinEcrBpoBase();

protected:
    AddinEcrBpoBase();

    /// Ревізія інтерфейсу БПО, яку оголошує конкретний фасад (§2.3 доку).
    virtual int InterfaceRevision() const = 0;

    /// Реєструє системну половину контракту. Похідний клас кличе її у своєму
    /// конструкторі ПЕРЕД реєстрацією власних платіжних методів.
    void RegisterSystemMethods();

    // ---- Операції драйвера для платіжних методів похідних класів ----
    // Кожна вже мапить результат у lastError* і повертає ResultEnvelope.
    ResultEnvelope RunPurchase(double amount);
    ResultEnvelope RunRefund(double amount, const std::string& rrn);
    /// Скасування платежу. Власної операції void протокол ПриватБанку в нашій
    /// реалізації не має, тож за параметром підключення `VoidAsRefund` (дефолт —
    /// увімкнено) скасування виконується ПОВЕРНЕННЯМ за RRN. Вимкнений параметр →
    /// чесна відмова (§2.3.1 доку).
    /// УВАГА: void і refund мають різну банківську семантику — void скасовує до
    /// звірки й сліду не лишає, refund створює зворотну транзакцію. Тому це
    /// видиме налаштування, а не зашите рішення.
    ResultEnvelope RunVoid(double amount, const std::string& rrn);
    /// Аварійне скасування драйвер не реалізує — чесна відмова.
    ResultEnvelope RunEmergencyVoid();
    /// Підсумки дня по картах = Сверка (Verify) драйвера.
    ResultEnvelope RunDayTotals();

    /// Відмова «операція не підтримується обладнанням» у формі, якої вимагає ІТС §1.3.
    ResultEnvelope Unsupported(const std::string& method);

    /// Перевіряє, що ИДУстройства збігається з виданим при `Подключить`.
    /// При розбіжності ставить lastError і повертає false.
    bool CheckDeviceId(const std::string& deviceId);

    /// Загальний хвіст платіжного методу: ставить lastError із конверта і
    /// повертає значення для 1С (Булево).
    bool MapEnvToBool(const ResultEnvelope& env);

    /// Дістає рядкове поле з `payload` конверта; відсутнє поле → порожній рядок.
    static std::string PayloadStr(const ResultEnvelope& env, const char* field);

    /// Толерантне читання вхідного параметра в рядок.
    /// ⚠️ Пряме `static_cast<std::string>(VH)` КИДАЄ на всьому, крім `VTYPE_PWSTR`,
    /// а 1С передає параметри форми налаштувань їхніми оголошеними типами: `Port`
    /// і `Baud` приходять числами, `VoidAsRefund` — булевим. Тому всі вхідні
    /// значення читаємо лише через цей хелпер.
    static std::string VariantToString(VH value);

    /// Те саме для числа: порожній/незаповнений параметр → 0.0 замість винятку.
    static double VariantToDouble(VH value);

    /// Сума з 1С приходить як VTYPE_R8 (перевірено зондом на живій 1С), а драйвер
    /// чекає рядок «100.00». Конверсія ЛОКАЛЕНЕЗАЛЕЖНА — через цілі копійки, бо
    /// printf-подібне форматування дало б кому замість крапки в ru/uk-локалі.
    static std::string AmountToString(double amount);

private:
    /// Реєструє асинхронну трійцю ПОВЕРХ контракту БПО. Штатний обробник БПО цих
    /// методів не знає й ніколи не покличе — їх бере розширення 1С через
    /// `ПодключенноеУстройство.ОбъектДрайвера` з точки розширення РМК, щоб показати
    /// живий статус оплати. Живуть саме тут, а не в прямому класі `ECRPrivatJSON`,
    /// бо доступ до термінала монопольний: обладнання вже підключене цим об'єктом,
    /// і другий об'єкт до того самого термінала не під'єднається.
    void RegisterAsyncExtensions();

    /// Чи скасовувати платіж поверненням за RRN (параметр `VoidAsRefund`).
    bool VoidAsRefundEnabled() const;

    void SetError(int code, const std::string& description);
    void ClearError();
    /// Числовий код для `ПолучитьОшибку` з машинного коду ResultEnvelope.
    static int CodeToInt(const std::string& code);
    /// Рядок підключення драйвера з накопичених `УстановитьПараметр`.
    std::string BuildConnectionString() const;
    std::string Param(const char* name, const std::string& fallback = "") const;

    EcrPrivatJsonDriver driver_;
    std::map<std::string, std::string> params_;   ///< накопичене через УстановитьПараметр
    std::string deviceId_;                        ///< видане при Подключить; порожнє = не підключено
    int lastErrorCode_ = 0;
    std::string lastErrorDesc_;
};
