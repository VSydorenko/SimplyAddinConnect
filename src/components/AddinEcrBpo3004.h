#pragma once
#include "AddinEcrBpoBase.h"
#include <string>
#include <vector>

/// БПО-фасад еквайрингу, ревізія інтерфейсу **3004**.
///
/// Сімейство сигнатур із семи параметрів. Ця ревізія — єдина, яку BAS УНФ ua кличе
/// БЕЗУМОВНО (без перевірки ревізії взагалі), і водночас гілка `Иначе` у УНФ ru.
/// Тобто фасад працює в обох продуктах і в старіших збірках, де гілки 4000 ще немає.
///
/// Повна звірка сигнатур і розкладки OUT — docs/architecture/bpo-contract.md §2.4.
class AddinEcrBpo3004 : public AddinEcrBpoBase {
public:
    static std::vector<std::u16string> names;
    AddinEcrBpo3004();

protected:
    int InterfaceRevision() const override { return 3004; }

private:
    void RegisterPaymentMethods();
};
