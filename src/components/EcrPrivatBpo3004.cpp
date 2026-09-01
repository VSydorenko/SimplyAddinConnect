#include "../core/pch.h"
#include "EcrPrivatBpo3004.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h"
#include "../helpers/ServiceTools.h"

REGISTER_COMPONENT(u"ECRPrivatBPO3004", EcrPrivatBpo3004)

EcrPrivatBpo3004::EcrPrivatBpo3004() {
    REPORT_INFO("Ініціалізація БПО-фасаду еквайрингу ПриватБанк, ревізія 3004");
}

std::unique_ptr<IAcquiringDriver> EcrPrivatBpo3004::MakeDriver() const {
    return std::make_unique<EcrPrivatJsonAcquiring>();
}
