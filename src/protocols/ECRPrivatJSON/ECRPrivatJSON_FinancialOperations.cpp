#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>

namespace ECRPrivatJSON {

// ===========================
// Финансовые операции
// ===========================

bool ECRPrivatJSONProtocol::Payment(double amount, const std::string& discount, 
                                  const std::string& merchantId, const std::string& subMerchant) {
    NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции оплаты на сумму " + std::to_string(amount));
    
    try {
        if (!IsConnected() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            return false;
        }
        
        // Проверка корректности суммы
        if (amount <= 0) {
            NEUTRAL_REPORT_ERROR(componentName_, "Некорректная сумма оплаты: " + std::to_string(amount));
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        try {
            params["amount"] = helper_->FormatAmount(amount);
            
            if (!discount.empty()) {
                params["discount"] = discount;
            }
            
            if (!merchantId.empty() && merchantId != "0") {
                params["merchantId"] = merchantId;
            }
            
            if (!subMerchant.empty()) {
                params["subMerchant"] = subMerchant;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании параметров запроса: " + std::string(e.what()));
            return false;
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::Purchase),
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса оплаты: " + request);
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для финансовых операций
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса оплаты: " + std::string(e.what()));
            return false;
        }
        
        if (response.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос оплаты");
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос оплаты: " + response);
        
        // Парсим ответ
        try {
            helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            NEUTRAL_REPORT_INFO(componentName_, "Операция оплаты успешно выполнена");
        } else {
            std::string errorMsg = "Ошибка выполнения операции оплаты: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
        }
        
        return success;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции оплаты: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции оплаты");
        return false;
    }
}

bool ECRPrivatJSONProtocol::Refund(double amount, const std::string& rrn, const std::string& discount, 
                                 const std::string& merchantId, const std::string& subMerchant) {
    NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции возврата на сумму " + std::to_string(amount));
    
    try {
        if (!IsConnected() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            return false;
        }
        
        // Проверка корректности суммы
        if (amount <= 0) {
            NEUTRAL_REPORT_ERROR(componentName_, "Некорректная сумма возврата: " + std::to_string(amount));
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        try {
            params["amount"] = helper_->FormatAmount(amount);
            
            if (!rrn.empty()) {
                params["rrn"] = rrn;
            }
            
            if (!discount.empty()) {
                params["discount"] = discount;
            }
            
            if (!merchantId.empty() && merchantId != "0") {
                params["merchantId"] = merchantId;
            }
            
            if (!subMerchant.empty()) {
                params["subMerchant"] = subMerchant;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании параметров запроса возврата: " + std::string(e.what()));
            return false;
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::Refund),
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса возврата: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса возврата: " + request);
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для финансовых операций
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса возврата: " + std::string(e.what()));
            return false;
        }
        
        if (response.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос возврата");
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос возврата: " + response);
        
        // Парсим ответ
        try {
            helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос возврата: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            NEUTRAL_REPORT_INFO(componentName_, "Операция возврата успешно выполнена");
        } else {
            std::string errorMsg = "Ошибка выполнения операции возврата: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
        }
        
        return success;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции возврата: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции возврата");
        return false;
    }
}

bool ECRPrivatJSONProtocol::Settlement(const std::string& merchantId) {
    NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции сверки итогов");
    
    try {
        if (!IsConnected() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        try {
            if (!merchantId.empty() && merchantId != "0") {
                params["merchantId"] = merchantId;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании параметров запроса сверки итогов: " + std::string(e.what()));
            return false;
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::Verify),
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса сверки итогов: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса сверки итогов: " + request);
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для сверки итогов
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса сверки итогов: " + std::string(e.what()));
            return false;
        }
        
        if (response.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос сверки итогов");
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос сверки итогов: " + response);
        
        // Парсим ответ
        try {
            helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос сверки итогов: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            NEUTRAL_REPORT_INFO(componentName_, "Операция сверки итогов успешно выполнена");
        } else {
            std::string errorMsg = "Ошибка выполнения операции сверки итогов: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
        }
        
        return success;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции сверки итогов: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции сверки итогов");
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetReceipt(const std::string& transactionId) {
    NEUTRAL_REPORT_INFO(componentName_, "Запрос копии чека для транзакции: " + transactionId);
    
    try {
        if (!IsConnected() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            return false;
        }
        
        if (transactionId.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не указан идентификатор транзакции для получения чека");
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        try {
            params["transactionId"] = transactionId;
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании параметров запроса чека: " + std::string(e.what()));
            return false;
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::PrintReceiptNum),
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса чека: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса чека: " + request);
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request, 60000);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса чека: " + std::string(e.what()));
            return false;
        }
        
        if (response.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос чека");
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос чека: " + response);
        
        // Парсим ответ
        try {
            helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос чека: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            NEUTRAL_REPORT_INFO(componentName_, "Получен чек для транзакции: " + transactionId);
        } else {
            std::string errorMsg = "Ошибка получения чека: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
        }
        
        return success;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при получении чека: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при получении чека");
        return false;
    }
}

} // namespace ECRPrivatJSON
