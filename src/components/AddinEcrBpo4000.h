#pragma once
#include "AcquiringFacadeBase.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h"
#include <memory>
#include <string>
#include <vector>

/// БПО-фасад еквайрингу, ревізія інтерфейсу **4000**.
///
/// Сімейство сигнатур із дев'яти (оплата/повернення) і десяти (скасування, видача
/// готівки) параметрів: попереду з'являються `НомерМерчанта` і `РеквизитыКартыQR`.
/// Кличе його СУЧАСНИЙ УНФ ru. BAS УНФ ua цю гілку не має — там працює фасад 3004.
///
/// ⚠️ Обидва записи довідника драйверів мають їхати в обидва продукти, а вибирає
/// адміністратор: у клієнтських базах різні версії УНФ, і драйвер з оголошеною 4000
/// у старішій збірці тихо впаде в гілку 3005 — вісім параметрів замість дев'яти.
///
/// Повна звірка сигнатур і розкладки OUT — docs/architecture/bpo-contract.md §2.5.
class AddinEcrBpo4000 : public AcquiringFacadeBase {
public:
    static std::vector<std::u16string> names;
    AddinEcrBpo4000();

protected:
    int InterfaceRevision() const override { return 4000; }
    /// Єдиний протокол сьогодні — ПриватБанк JSON. Другий протокол додасть свій
    /// клас фасаду, а не гілку тут.
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override {
        return std::make_unique<EcrPrivatJsonAcquiring>();
    }

private:
    void RegisterPaymentMethods();
};
