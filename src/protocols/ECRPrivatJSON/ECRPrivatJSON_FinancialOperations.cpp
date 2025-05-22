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
    if (parentComponent_) {
        REPORT_INFO("Выполнение операции оплаты на сумму " + std::to_string(amount));
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции оплаты на сумму " + std::to_string(amount));
    }
    
    try {
        if (!IsConnected() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            }
            return false;
        }
        
        // Проверка корректности суммы
        if (amount <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Некорректная сумма оплаты: " + std::to_string(amount));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Некорректная сумма оплаты: " + std::to_string(amount));
            }
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
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
        
        // Формируем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::Purchase),
            params
        );
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса оплаты: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса оплаты: " + request);
        }
        
        // Отправляем запрос и получаем ответ
        std::string response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для финансовых операций
        
        if (response.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Не получен ответ на запрос оплаты");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос оплаты");
            }
            return false;
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос оплаты: " + response);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос оплаты: " + response);
        }
        
        // Парсим ответ
        helper_->ParseTerminalResponse(response, lastResponse_);
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            if (parentComponent_) {
                REPORT_INFO("Операция оплаты успешно выполнена");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Операция оплаты успешно выполнена");
            }
        } else {
            std::string errorMsg = "Ошибка выполнения операции оплаты: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            
            if (parentComponent_) {
                REPORT_ERROR(errorMsg);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            }
        }
        
        return success;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Исключение при выполнении операции оплаты: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции оплаты: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестное исключение при выполнении операции оплаты");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции оплаты");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::Refund(double amount, const std::string& rrn, const std::string& discount, 
                                 const std::string& merchantId, const std::string& subMerchant) {
    if (parentComponent_) {
        REPORT_INFO("Выполнение операции возврата на сумму " + std::to_string(amount));
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции возврата на сумму " + std::to_string(amount));
    }
    
    try {
        if (!IsConnected() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            }
            return false;
        }
        
        // Проверка корректности суммы
        if (amount <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Некорректная сумма возврата: " + std::to_string(amount));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Некорректная сумма возврата: " + std::to_string(amount));
            }
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
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
        
        // Формируем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::Refund),
            params
        );
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса возврата: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса возврата: " + request);
        }
        
        // Отправляем запрос и получаем ответ
        std::string response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для финансовых операций
        
        if (response.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Не получен ответ на запрос возврата");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос возврата");
            }
            return false;
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос возврата: " + response);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос возврата: " + response);
        }
        
        // Парсим ответ
        helper_->ParseTerminalResponse(response, lastResponse_);
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            if (parentComponent_) {
                REPORT_INFO("Операция возврата успешно выполнена");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Операция возврата успешно выполнена");
            }
        } else {
            std::string errorMsg = "Ошибка выполнения операции возврата: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            
            if (parentComponent_) {
                REPORT_ERROR(errorMsg);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            }
        }
        
        return success;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Исключение при выполнении операции возврата: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции возврата: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестное исключение при выполнении операции возврата");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции возврата");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::Settlement(const std::string& merchantId) {
    if (parentComponent_) {
        REPORT_INFO("Выполнение операции сверки итогов");
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение операции сверки итогов");
    }
    
    try {
        if (!IsConnected() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            }
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
        }
        
        // Формируем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::Verify),
            params
        );
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса сверки итогов: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса сверки итогов: " + request);
        }
        
        // Отправляем запрос и получаем ответ
        std::string response = helper_->SendReceive(request, 120000); // Увеличенный таймаут для сверки итогов
        
        if (response.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Не получен ответ на запрос сверки итогов");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос сверки итогов");
            }
            return false;
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос сверки итогов: " + response);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос сверки итогов: " + response);
        }
        
        // Парсим ответ
        helper_->ParseTerminalResponse(response, lastResponse_);
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            if (parentComponent_) {
                REPORT_INFO("Операция сверки итогов успешно выполнена");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Операция сверки итогов успешно выполнена");
            }
        } else {
            std::string errorMsg = "Ошибка выполнения операции сверки итогов: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            
            if (parentComponent_) {
                REPORT_ERROR(errorMsg);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            }
        }
        
        return success;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Исключение при выполнении операции сверки итогов: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при выполнении операции сверки итогов: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестное исключение при выполнении операции сверки итогов");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при выполнении операции сверки итогов");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetReceipt(const std::string& transactionId) {
    if (parentComponent_) {
        REPORT_INFO("Запрос копии чека для транзакции: " + transactionId);
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Запрос копии чека для транзакции: " + transactionId);
    }
    
    try {
        if (!IsConnected() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом или не инициализирован вспомогательный класс");
            }
            return false;
        }
        
        if (transactionId.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Не указан идентификатор транзакции для получения чека");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не указан идентификатор транзакции для получения чека");
            }
            return false;
        }
        
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        params["transactionId"] = transactionId;
        
        // Формируем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::PrintReceiptNum),
            params
        );
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса чека: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса чека: " + request);
        }
        
        // Отправляем запрос и получаем ответ
        std::string response = helper_->SendReceive(request, 60000);
        
        if (response.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Не получен ответ на запрос чека");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не получен ответ на запрос чека");
            }
            return false;
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос чека: " + response);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос чека: " + response);
        }
        
        // Парсим ответ
        helper_->ParseTerminalResponse(response, lastResponse_);
        
        // Проверяем успешность операции
        bool success = lastResponse_.isSuccess;
        
        if (success) {
            if (parentComponent_) {
                REPORT_INFO("Получен чек для транзакции: " + transactionId);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Получен чек для транзакции: " + transactionId);
            }
        } else {
            std::string errorMsg = "Ошибка получения чека: " + lastResponse_.responseCode;
            if (!lastResponse_.responseDescription.empty()) {
                errorMsg += " - " + lastResponse_.responseDescription;
            }
            
            if (parentComponent_) {
                REPORT_ERROR(errorMsg);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            }
        }
        
        return success;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Исключение при получении чека: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при получении чека: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестное исключение при получении чека");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при получении чека");
        }
        return false;
    }
}

} // namespace ECRPrivatJSON
