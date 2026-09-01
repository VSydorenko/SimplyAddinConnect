#pragma once
#include "AcquiringBpo3004.h"
#include <memory>
#include <string>
#include <vector>

/// БПО-фасад еквайрингу ПриватБанк, ревізія інтерфейсу 3004 (сімка параметрів).
/// Клас у 1С: "AddIn.<символьне ім'я>.ECRPrivatBPO3004" — адміністратор обирає його
/// записом довідника драйверів; автовизначення ревізії в БПО немає (§2.3 контракту).
class EcrPrivatBpo3004 : public AcquiringBpo3004 {
public:
    static std::vector<std::u16string> names;
    EcrPrivatBpo3004();

protected:
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override;
};
