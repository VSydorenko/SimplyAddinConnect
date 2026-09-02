#pragma once
#include "AcquiringBpo4000.h"
#include <memory>
#include <string>
#include <vector>

/// БПО-фасад еквайрингу ПриватБанк, ревізія інтерфейсу 4000 (дев'ятка/десятки параметрів).
/// Клас у 1С: "AddIn.<символьне ім'я>.ECRPrivatBPO4000" — адміністратор обирає його
/// записом довідника драйверів; автовизначення ревізії в БПО немає (§2.3 контракту).
///
/// ⚠️ Обидва записи довідника драйверів мають їхати в обидва продукти, а вибирає
/// адміністратор: у клієнтських базах різні версії УНФ, і драйвер з оголошеною 4000
/// у старішій збірці тихо впаде в гілку 3005 — вісім параметрів замість дев'яти.
class EcrPrivatBpo4000 : public AcquiringBpo4000 {
public:
    static std::vector<std::u16string> names;
    EcrPrivatBpo4000();

protected:
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override;
};
