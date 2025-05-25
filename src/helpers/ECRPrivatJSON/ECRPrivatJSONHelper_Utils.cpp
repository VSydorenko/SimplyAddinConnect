#include "../../core/pch.h"
#include "ECRPrivatJSONHelper.h"
#include "../ServiceTools.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ECRPrivatJSON {


std::string ECRPrivatJSONHelper::OperationTypeToString(OperationType opType) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Преобразование типа операции в строку");
        
        std::string result;
        switch (opType) {
            case OperationType::Payment:
                result = "Payment";
                break;
            case OperationType::Refund:
                result = "Refund";
                break;
            case OperationType::Verify:
                result = "Verify";
                break;
            case OperationType::Settlement:
                result = "Settlement";
                break;
            case OperationType::XReport:
                result = "XReport";
                break;
            case OperationType::ZReport:
                result = "ZReport";
                break;
            case OperationType::Copy:
                result = "Copy";
                break;
            case OperationType::ServiceMenu:
                result = "ServiceMenu";
                break;
            case OperationType::Purchase:
                result = "Purchase";
                break;
            case OperationType::Withdrawal:
                result = "Withdrawal";
                break;
            case OperationType::WithdrawalPartly:
                result = "WithdrawalPartly";
                break;
            case OperationType::CheckConnection:
                result = "CheckConnection";
                break;
            case OperationType::PrintReceiptNum:
                result = "PrintReceiptNum";
                break;
            case OperationType::ServiceMessage:
                result = "ServiceMessage";
                break;
            case OperationType::ReadCardBank:
                result = "ReadCardBank";
                break;
            case OperationType::ReadCardDiscount:
                result = "ReadCardDiscount";
                break;
            case OperationType::GetTerminalInfo:
                result = "GetTerminalInfo";
                break;
            case OperationType::PingDevice:
                result = "PingDevice";
                break;
            case OperationType::GetBalance:
                result = "GetBalance";
                break;
            case OperationType::ServiceRefund:
                result = "ServiceRefund";
                break;
            case OperationType::ServicePbP:
                result = "ServicePbP";
                break;
            case OperationType::ServiceRefPbP:
                result = "ServiceRefPbP";
                break;
            case OperationType::ServicePartlyRefPbP:
                result = "ServicePartlyRefPbP";
                break;
            case OperationType::ServicePbPperiod:
                result = "ServicePbPperiod";
                break;
            case OperationType::ServiceRefPbPperiod:
                result = "ServiceRefPbPperiod";
                break;
            case OperationType::ServicePartlyRefPbPperiod:
                result = "ServicePartlyRefPbPperiod";
                break;
            case OperationType::ServiceInstantPbI:
                result = "ServiceInstantPbI";
                break;
            case OperationType::ServiceRefPbI:
                result = "ServiceRefPbI";
                break;
            case OperationType::ServicePartlyRefPbI:
                result = "ServicePartlyRefPbI";
                break;
            case OperationType::ServicePbIAct:
                result = "ServicePbIAct";
                break;
            case OperationType::ServiceRefPbIAct:
                result = "ServiceRefPbIAct";
                break;
            case OperationType::ServicePartlyRefPbIAct:
                result = "ServicePartlyRefPbIAct";
                break;
            case OperationType::ServicePbPAct:
                result = "ServicePbPAct";
                break;
            case OperationType::ServiceRefPbPAct:
                result = "ServiceRefPbPAct";
                break;
            case OperationType::ServicePartlyRefPbPAct:
                result = "ServicePartlyRefPbPAct";
                break;
            case OperationType::ServiceGeneric:
                result = "ServiceGeneric";
                break;
            case OperationType::Cashback:
                result = "Cashback";
                break;
            case OperationType::Audit:
                result = "Audit";
                break;
            case OperationType::VerifyCopy:
                result = "VerifyCopy";
                break;
            case OperationType::PrintBatchJournal:
                result = "PrintBatchJournal";
                break;
            case OperationType::ReadBonusCard:
                result = "ReadBonusCard";
                break;
            case OperationType::GetPinBonusCard:
                result = "GetPinBonusCard";
                break;
            case OperationType::PinChangeBonusCard:
                result = "PinChangeBonusCard";
                break;
            case OperationType::GetPhoneNumber:
                result = "GetPhoneNumber";
                break;
            case OperationType::Preauthorization:
                result = "Preauthorization";
                break;
            case OperationType::SaleCompletion:
                result = "SaleCompletion";
                break;
            case OperationType::GetOTPpassword:
                result = "GetOTPpassword";
                break;
            case OperationType::GetReceiptInfo:
                result = "GetReceiptInfo";
                break;
            default:
                NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Неизвестный тип операции: " + std::to_string(static_cast<int>(opType)));
                result = "Unknown";
                break;
        }
        
        NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Тип операции преобразован в строку: " + result);
        return result;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при преобразовании типа операции в строку: " + std::string(e.what()));
        return "Unknown";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при преобразовании типа операции в строку");
        return "Unknown";
    }
}

std::string ECRPrivatJSONHelper::ServiceMessageTypeToString(ServiceMessageType msgType) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Преобразование типа сервисного сообщения в строку");
        
        std::string result;
        switch (msgType) {
            case ServiceMessageType::GetTerminalInfo:
                result = "GetTerminalInfo";
                break;
            case ServiceMessageType::PingDevice:
                result = "PingDevice";
                break;
            case ServiceMessageType::RunCommand:
                result = "RunCommand";
                break;
            case ServiceMessageType::Identify:
                result = "identify";
                break;
            case ServiceMessageType::DeviceBusy:
                result = "deviceBusy";
                break;
            case ServiceMessageType::Interrupt:
                result = "interrupt";
                break;
            case ServiceMessageType::InterruptTransmitted:
                result = "interruptTransmitted";
                break;
            case ServiceMessageType::MethodNotImplemented:
                result = "methodNotImplemented";
                break;
            case ServiceMessageType::GetMerchantList:
                result = "getMerchantList";
                break;
            case ServiceMessageType::GetMaskList:
                result = "getMaskList";
                break;
            case ServiceMessageType::Debug:
                result = "debug";
                break;
            case ServiceMessageType::DebugOn:
                result = "debugOn";
                break;
            case ServiceMessageType::DebugOff:
                result = "debugOff";
                break;
            case ServiceMessageType::GetLastResult:
                result = "getLastResult";
                break;
            case ServiceMessageType::GetLastStatMsgCode:
                result = "getLastStatMsgCode";
                break;
            case ServiceMessageType::GetLastStatMsgDescription:
                result = "getLastStatMsgDescription";
                break;
            case ServiceMessageType::GetDiscountName:
                result = "getDiscountName";
                break;
            case ServiceMessageType::GetVersion:
                result = "getVersion";
                break;
            default:
                NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Неизвестный тип сервисного сообщения: " + std::to_string(static_cast<int>(msgType)));
                result = "unknown";
                break;
        }
        
        NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Тип сервисного сообщения преобразован в строку: " + result);
        return result;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при преобразовании типа сервисного сообщения в строку: " + std::string(e.what()));
        return "unknown";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при преобразовании типа сервисного сообщения в строку");
        return "unknown";
    }
}

} // namespace ECRPrivatJSON
