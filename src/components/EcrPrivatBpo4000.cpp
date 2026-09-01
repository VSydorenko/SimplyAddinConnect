#include "../core/pch.h"
#include "EcrPrivatBpo4000.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h"
#include "../helpers/ServiceTools.h"

REGISTER_COMPONENT(u"ECRPrivatBPO4000", EcrPrivatBpo4000)

EcrPrivatBpo4000::EcrPrivatBpo4000() {
    REPORT_INFO("Ініціалізація БПО-фасаду еквайрингу ПриватБанк, ревізія 4000");
}

std::unique_ptr<IAcquiringDriver> EcrPrivatBpo4000::MakeDriver() const {
    return std::make_unique<EcrPrivatJsonAcquiring>();
}
