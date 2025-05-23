#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <algorithm>
#include <vector>
#include <iomanip>
#include <sstream>

namespace ECRPrivatJSON {


std::string ECRPrivatJSONHelper::OperationTypeToString(OperationType opType) const {
    switch (opType) {
        case OperationType::Payment:
            return "Payment";
        case OperationType::Refund:
            return "Refund";
        case OperationType::Verify:
            return "Verify";
        case OperationType::Settlement:
            return "Settlement";
        case OperationType::XReport:
            return "XReport";
        case OperationType::ZReport:
            return "ZReport";
        case OperationType::Copy:
            return "Copy";
        case OperationType::ServiceMenu:
            return "ServiceMenu";
        case OperationType::Purchase:
            return "Purchase";
        case OperationType::Refund:
            return "Refund";
        case OperationType::Withdrawal:
            return "Withdrawal";
        case OperationType::WithdrawalPartly:
            return "WithdrawalPartly";
        case OperationType::Verify:
            return "Verify";
        case OperationType::CheckConnection:
            return "CheckConnection";
        case OperationType::PrintReceiptNum:
            return "PrintReceiptNum";
        case OperationType::ServiceMessage:
            return "ServiceMessage";
        case OperationType::ReadCardBank:
            return "ReadCardBank";
        case OperationType::ReadCardDiscount:
            return "ReadCardDiscount";
        case OperationType::GetTerminalInfo:
            return "GetTerminalInfo";
        case OperationType::PingDevice:
            return "PingDevice";
        case OperationType::GetBalance:
            return "GetBalance";
        case OperationType::ServiceRefund:
            return "ServiceRefund";
        case OperationType::ServicePbP:
            return "ServicePbP";
        case OperationType::ServiceRefPbP:
            return "ServiceRefPbP";
        case OperationType::ServicePartlyRefPbP:
            return "ServicePartlyRefPbP";
        case OperationType::ServicePbPperiod:
            return "ServicePbPperiod";
        case OperationType::ServiceRefPbPperiod:
            return "ServiceRefPbPperiod";
        case OperationType::ServicePartlyRefPbPperiod:
            return "ServicePartlyRefPbPperiod";
        case OperationType::ServiceInstantPbI:
            return "ServiceInstantPbI";
        case OperationType::ServiceRefPbI:
            return "ServiceRefPbI";
        case OperationType::ServicePartlyRefPbI:
            return "ServicePartlyRefPbI";
        case OperationType::ServicePbIAct:
            return "ServicePbIAct";
        case OperationType::ServiceRefPbIAct:
            return "ServiceRefPbIAct";
        case OperationType::ServicePartlyRefPbIAct:
            return "ServicePartlyRefPbIAct";
        case OperationType::ServicePbPAct:
            return "ServicePbPAct";
        case OperationType::ServiceRefPbPAct:
            return "ServiceRefPbPAct";
        case OperationType::ServicePartlyRefPbPAct:
            return "ServicePartlyRefPbPAct";
        case OperationType::ServiceGeneric:
            return "ServiceGeneric";
        case OperationType::Cashback:
            return "Cashback";
        case OperationType::Audit:
            return "Audit";
        case OperationType::VerifyCopy:
            return "VerifyCopy";
        case OperationType::PrintBatchJournal:
            return "PrintBatchJournal";
        case OperationType::ReadBonusCard:
            return "ReadBonusCard";
        case OperationType::GetPinBonusCard:
            return "GetPinBonusCard";
        case OperationType::PinChangeBonusCard:
            return "PinChangeBonusCard";
        case OperationType::GetPhoneNumber:
            return "GetPhoneNumber";
        case OperationType::Preauthorization:
            return "Preauthorization";
        case OperationType::SaleCompletion:
            return "SaleCompletion";
        case OperationType::GetOTPpassword:
            return "GetOTPpassword";
        case OperationType::GetReceiptInfo:
            return "GetReceiptInfo";
        default:
            return "Unknown";
    }
}

std::string ECRPrivatJSONHelper::ServiceMessageTypeToString(ServiceMessageType msgType) const {
    switch (msgType) {
        case ServiceMessageType::GetTerminalInfo:
            return "GetTerminalInfo";
        case ServiceMessageType::PingDevice:
            return "PingDevice";
        case ServiceMessageType::RunCommand:
            return "RunCommand";
        case ServiceMessageType::Identify:
            return "identify";
        case ServiceMessageType::DeviceBusy:
            return "deviceBusy";
        case ServiceMessageType::Interrupt:
            return "interrupt";
        case ServiceMessageType::InterruptTransmitted:
            return "interruptTransmitted";
        case ServiceMessageType::MethodNotImplemented:
            return "methodNotImplemented";
        case ServiceMessageType::GetMerchantList:
            return "getMerchantList";
        case ServiceMessageType::GetMaskList:
            return "getMaskList";
        case ServiceMessageType::Debug:
            return "debug";
        case ServiceMessageType::DebugOn:
            return "debugOn";
        case ServiceMessageType::DebugOff:
            return "debugOff";
        case ServiceMessageType::GetLastResult:
            return "getLastResult";
        case ServiceMessageType::GetLastStatMsgCode:
            return "getLastStatMsgCode";
        case ServiceMessageType::GetLastStatMsgDescription:
            return "getLastStatMsgDescription";
        case ServiceMessageType::GetDiscountName:
            return "getDiscountName";
        case ServiceMessageType::GetVersion:
            return "getVersion";
        default:
            return "unknown";
    }
}

} // namespace ECRPrivatJSON
