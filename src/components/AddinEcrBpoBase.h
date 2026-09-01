#pragma once
#include "BpoFacadeBase.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonDriver.h"
#include <string>
#include <vector>

/// Спільна ЕКВАЙРИНГОВА половина БПО-фасадів над драйвером ECRPrivatJSON.
///
/// Контрактна (не залежна від типу обладнання) половина живе в `BpoFacadeBase`;
/// тут — семантика еквайрингу: паспорт драйвера, форма налаштувань термінала,
/// підключення до драйвера, методи можливостей і платіжні операції.
///
/// Контракт «Подключаемое оборудование» описано в docs/architecture/bpo-contract.md.
/// Параметри підключення й методи можливостей однакові для всіх ревізій — вони тут.
/// Різниця між ревізіями лише в РОЗКЛАДЦІ параметрів платіжних методів, тож похідний
/// клас додає рівно їх плюс власну `ПолучитьРевизиюИнтерфейса`.
///
/// Чому не один клас на всі ревізії: у моделі компоненти одне ім'я методу — це одна
/// арність (`GetNParams` віддає одне число), а `ОплатитьПлатежнойКартой` має 7 або 9
/// параметрів залежно від ревізії, причому на позиції 1 різний ЗМІСТ. Див. §2.2 доку.
class AddinEcrBpoBase : public BpoFacadeBase {
public:
    virtual ~AddinEcrBpoBase();

protected:
    AddinEcrBpoBase();

    // ---- гачки BpoFacadeBase ----
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    std::string BuildActionsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    bool RunAction(const std::string& name) override;

    /// Реєструє еквайрингову половину: методи можливостей, ИтогиДняПоКартам,
    /// АварийнаяОтменаОперации, асинхронне розширення. Похідний клас кличе її
    /// ПІСЛЯ RegisterSystemMethods() і ПЕРЕД своїми платіжними методами.
    void RegisterAcquiringMethods();

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

    /// Рядок підключення драйвера з накопичених `УстановитьПараметр`.
    std::string BuildConnectionString() const;

    EcrPrivatJsonDriver driver_;
};
