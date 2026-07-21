#pragma once
#include "../../transport/IFrameClassifier.h"

/// Класифікатор кадрів ECR ПриватБанк (спека §5): кореляція за method/msgType,
/// deviceBusy→RejectPrimary, interrupt/correctTransaction відповіді→ServiceResponse,
/// methodNotImplemented→RejectBoth/single. Уся Privat-специфіка ізольована тут.
class EcrPrivatJsonClassifier : public IFrameClassifier {
public:
    Classification Classify(const PendingView& pending,
                            const std::vector<uint8_t>& frame) override;
};
