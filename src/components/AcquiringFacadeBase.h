#pragma once
#include "BpoFacadeBase.h"
#include "../drivers/IAcquiringDriver.h"
#include <memory>
#include <string>

/// Семантика еквайрингу поверх контракту БПО. Протоколу НЕ знає: працює виключно
/// через IAcquiringDriver&. Ревізії теж не знає — розкладку параметрів платіжних
/// методів додає ревізійний шар (AcquiringBpo3004 / AcquiringBpo4000).
///
/// Тут: методи можливостей (ПараметрыТерминала для ru, ПечатьКвитанцийНаТерминале
/// для ua), ИтогиДняПоКартам, АварийнаяОтменаОперации, асинхронне розширення
/// і логіка VoidAsRefund.
class AcquiringFacadeBase : public BpoFacadeBase {
public:
    ~AcquiringFacadeBase() override;

protected:
    AcquiringFacadeBase();

    /// Фабрика драйвера протоколу — ЄДИНЕ, що перевизначає конкретний фасад.
    virtual std::unique_ptr<IAcquiringDriver> MakeDriver() const = 0;

    /// Драйвер створюється ЛІНИВО: віртуальний виклик у конструкторі базового класу
    /// пішов би не в нащадка. Перший доступ — уже з методу, покликаного 1С.
    IAcquiringDriver& Driver() const;
    AcquiringCapabilities Capabilities() const { return Driver().Capabilities(); }

    /// Реєструє еквайрингову половину. Ревізійний шар кличе її ПІСЛЯ
    /// RegisterSystemMethods() і ПЕРЕД своїми платіжними методами.
    void RegisterAcquiringMethods();

    // ---- операції для платіжних методів ревізійного шару ----
    ResultEnvelope RunPurchase(double amount);
    ResultEnvelope RunRefund(double amount, const std::string& rrn);
    /// Скасування. Якщо протокол має ВЛАСНУ операцію void — вона й правильна.
    /// Немає — за параметром `VoidAsRefund` (дефолт увімкнено) скасування виконується
    /// ПОВЕРНЕННЯМ за RRN; вимкнений параметр → чесна відмова (§2.3.1 контракту).
    /// ⚠️ void і refund мають різну банківську семантику: void скасовує до звірки й
    /// сліду не лишає, refund створює зворотну транзакцію. Тому це видиме
    /// налаштування, а не зашите рішення.
    ResultEnvelope RunVoid(double amount, const std::string& rrn);
    ResultEnvelope RunEmergencyVoid();
    ResultEnvelope RunDayTotals();
    /// Відмова «операція не підтримується обладнанням» у формі, якої вимагає ІТС §1.3.
    ResultEnvelope Unsupported(const std::string& method);

    // ---- гачки BpoFacadeBase ----
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    std::string BuildActionsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    bool RunAction(const std::string& name) override;

private:
    void RegisterAsyncExtensions();
    bool VoidAsRefundEnabled() const { return ParamBool("VoidAsRefund", true); }

    mutable std::unique_ptr<IAcquiringDriver> driver_;   ///< лінива ініціалізація, див. Driver()
};
