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
    /// Скасування платежу драйвер не реалізує — чесна відмова (§2.3.1 доку).
    ResultEnvelope RunVoid();
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

    /// Сума з 1С приходить як VTYPE_R8 (перевірено зондом на живій 1С), а драйвер
    /// чекає рядок «100.00». Конверсія ЛОКАЛЕНЕЗАЛЕЖНА — через цілі копійки, бо
    /// printf-подібне форматування дало б кому замість крапки в ru/uk-локалі.
    static std::string AmountToString(double amount);

private:
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
