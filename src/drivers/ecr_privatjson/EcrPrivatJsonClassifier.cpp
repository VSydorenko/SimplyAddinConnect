#include "../../core/pch.h"
#include "EcrPrivatJsonClassifier.h"
#include "EcrJsonCodec.h"

namespace {
// Очікуваний msgType ВІДПОВІДІ на service-запит із заданим msgType-ЗАПИТУ.
// Більшість — тотожні; interrupt/correctTransaction мають окремий ack.
std::string ExpectedResponseMsgType(const std::string& requestMsgType) {
    if (requestMsgType == "interrupt")         return "interruptTransmitted";
    if (requestMsgType == "correctTransaction") return "correctionTransmitted";
    return requestMsgType;
}
} // namespace

Classification EcrPrivatJsonClassifier::Classify(const PendingView& pending,
                                                 const std::vector<uint8_t>& frame) {
    std::string method, msgType;
    if (!EcrJsonCodec::PeekMethod(frame, method, msgType))
        return { FrameClass::Unsolicited, RejectReason::Busy };   // не розібрано → нейтрально

    std::string pm, pmt, sm, smt;
    const bool hasP = pending.primary && EcrJsonCodec::PeekMethod(*pending.primary, pm, pmt);
    const bool hasS = pending.service && EcrJsonCodec::PeekMethod(*pending.service, sm, smt);

    if (method != "ServiceMessage") {
        // Несервісна відповідь корелює лише з primary за method.
        if (hasP && method == pm) return { FrameClass::PrimaryResponse, RejectReason::Busy };
        return { FrameClass::Unsolicited, RejectReason::Busy };
    }

    // method == "ServiceMessage": розводимо за msgType.
    if (msgType == "deviceBusy")
        return hasP ? Classification{ FrameClass::RejectPrimary, RejectReason::Busy }
                    : Classification{ FrameClass::Unsolicited, RejectReason::Busy };

    if (msgType == "methodNotImplemented") {
        // Кадр не називає відхилений метод.
        if (hasP && hasS) return { FrameClass::RejectBoth, RejectReason::Unsupported };
        if (hasS)         return { FrameClass::RejectService, RejectReason::Unsupported };
        if (hasP)         return { FrameClass::RejectPrimary, RejectReason::Unsupported };
        return { FrameClass::Unsolicited, RejectReason::Unsupported };
    }

    // Відповідь на активний service-запит (identify/getLastStatMsgCode/getDiscountName/
    // interrupt→interruptTransmitted/correctTransaction→correctionTransmitted).
    if (hasS && !smt.empty() && msgType == ExpectedResponseMsgType(smt))
        return { FrameClass::ServiceResponse, RejectReason::Busy };

    return { FrameClass::Unsolicited, RejectReason::Busy };
}
