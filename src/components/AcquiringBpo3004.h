#pragma once
#include "AcquiringFacadeBase.h"
#include <memory>
#include <string>
#include <vector>

/// Ревізійний шар БПО-фасаду еквайрингу — ревізія інтерфейсу **3004**.
///
/// Тут — рівно РОЗКЛАДКА параметрів платіжних методів для цієї ревізії. Клас
/// абстрактний і протоколу не знає: конкретний протокол додає нащадок-фасад,
/// що обирає драйвер.
///
/// Сімейство сигнатур із семи параметрів. Ця ревізія — єдина, яку BAS УНФ ua кличе
/// БЕЗУМОВНО (без перевірки ревізії взагалі), і водночас гілка `Иначе` у УНФ ru.
/// Тобто фасад працює в обох продуктах і в старіших збірках, де гілки 4000 ще немає.
///
/// Повна звірка сигнатур і розкладки OUT — docs/architecture/bpo-contract.md §2.4.
class AcquiringBpo3004 : public AcquiringFacadeBase {
protected:
    AcquiringBpo3004();

    int InterfaceRevision() const override { return 3004; }

private:
    void RegisterPaymentMethods();
};
