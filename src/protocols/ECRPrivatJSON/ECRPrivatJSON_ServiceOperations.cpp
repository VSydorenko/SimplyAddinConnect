#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>

namespace ECRPrivatJSON {

// ===========================
// Сервисные операции
// ===========================

bool ECRPrivatJSONProtocol::GetDailyReport(const std::string& merchantId) {
    if (parentComponent_) {
        REPORT_INFO("Запрос дневного отчета для мерчанта: " + merchantId);
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для мерчанта: " + merchantId);
    }
    
    if (!IsConnected()) {
        if (parentComponent_) {
            REPORT_ERROR("Терминал не подключен");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        }
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            if (parentComponent_) {
                REPORT_INFO("Запрос дневного отчета для мерчанта: " + merchantId);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для мерчанта: " + merchantId);
            }
        } else {
            if (parentComponent_) {
                REPORT_INFO("Запрос дневного отчета для всех мерчантов");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для всех мерчантов");
            }
        }
        
        // Формируем и отправляем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::Verify), 
            params
        );
        request = helper_->AddNullTerminator(request);
        std::string response = helper_->SendReceive(request);
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        
        if (!parsed) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка парсинга ответа на запрос дневного отчета");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос дневного отчета");
            }
            return false;
        }
        
        if (!lastResponse_.success) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка получения дневного отчета: " + lastResponse_.errorMessage + 
                            ", код: " + lastResponse_.responseCode);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения дневного отчета: " + 
                                    lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            }
            return false;
        } else {
            if (parentComponent_) {
                REPORT_INFO("Успешное получение дневного отчета, код: " + lastResponse_.responseCode + 
                          ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Успешное получение дневного отчета, код: " + 
                                  lastResponse_.responseCode + ", длина чека: " + 
                                  std::to_string(lastResponse_.receiptText.length()));
            }
            return true;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при запросе дневного отчета: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе дневного отчета: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при запросе дневного отчета");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе дневного отчета");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetXReport(const std::string& merchantId) {
    if (parentComponent_) {
        REPORT_INFO("Запрос X-отчета для мерчанта: " + merchantId);
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для мерчанта: " + merchantId);
    }
    
    if (!IsConnected()) {
        if (parentComponent_) {
            REPORT_ERROR("Терминал не подключен");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        }
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            if (parentComponent_) {
                REPORT_INFO("Запрос X-отчета для мерчанта: " + merchantId);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для мерчанта: " + merchantId);
            }
        } else {
            if (parentComponent_) {
                REPORT_INFO("Запрос X-отчета для всех мерчантов");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для всех мерчантов");
            }
        }
        
        // Формируем и отправляем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::XReport), 
            params
        );
        request = helper_->AddNullTerminator(request);
        std::string response = helper_->SendReceive(request);
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        
        if (!parsed) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка парсинга ответа на запрос X-отчета");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос X-отчета");
            }
            return false;
        }
        
        if (!lastResponse_.success) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка получения X-отчета: " + lastResponse_.errorMessage + 
                            ", код: " + lastResponse_.responseCode);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения X-отчета: " + 
                                    lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            }
            return false;
        } else {
            if (parentComponent_) {
                REPORT_INFO("Успешное получение X-отчета, код: " + lastResponse_.responseCode + 
                          ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Успешное получение X-отчета, код: " + 
                                  lastResponse_.responseCode + ", длина чека: " + 
                                  std::to_string(lastResponse_.receiptText.length()));
            }
            return true;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при запросе X-отчета: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе X-отчета: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при запросе X-отчета");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе X-отчета");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetZReport(const std::string& merchantId) {
    if (parentComponent_) {
        REPORT_INFO("Запрос Z-отчета для мерчанта: " + merchantId);
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для мерчанта: " + merchantId);
    }
    
    if (!IsConnected()) {
        if (parentComponent_) {
            REPORT_ERROR("Терминал не подключен");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        }
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            if (parentComponent_) {
                REPORT_INFO("Запрос Z-отчета для мерчанта: " + merchantId);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для мерчанта: " + merchantId);
            }
        } else {
            if (parentComponent_) {
                REPORT_INFO("Запрос Z-отчета для всех мерчантов");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для всех мерчантов");
            }
        }
        
        // Формируем и отправляем запрос
        std::string request = helper_->BuildRequest(
            helper_->OperationTypeToString(OperationType::ZReport), 
            params
        );
        request = helper_->AddNullTerminator(request);
        std::string response = helper_->SendReceive(request);
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        
        if (!parsed) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка парсинга ответа на запрос Z-отчета");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос Z-отчета");
            }
            return false;
        }
        
        if (!lastResponse_.success) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка получения Z-отчета: " + lastResponse_.errorMessage + 
                            ", код: " + lastResponse_.responseCode);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения Z-отчета: " + 
                                    lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            }
            return false;
        } else {
            if (parentComponent_) {
                REPORT_INFO("Успешное получение Z-отчета, код: " + lastResponse_.responseCode + 
                          ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Успешное получение Z-отчета, код: " + 
                                  lastResponse_.responseCode + ", длина чека: " + 
                                  std::to_string(lastResponse_.receiptText.length()));
            }
            return true;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при запросе Z-отчета: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе Z-отчета: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при запросе Z-отчета");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе Z-отчета");
        }
        return false;
    }
}

std::string ECRPrivatJSONProtocol::GetTerminalInfo() {
    if (!IsConnected()) {
        if (parentComponent_) {
            REPORT_ERROR("Терминал не подключен");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        }
        return "";
    }
    
    try {
        std::string terminalInfo;
        if (!IdentifyTerminal(terminalInfo)) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка получения информации о терминале");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения информации о терминале");
            }
            return "";
        }
        
        return terminalInfo;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при получении информации о терминале: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при получении информации о терминале: " + std::string(e.what()));
        }
        return "";
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при получении информации о терминале");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при получении информации о терминале");
        }
        return "";
    }
}

TerminalResponse ECRPrivatJSONProtocol::GetLastResponse() const {
    return lastResponse_;
}

} // namespace ECRPrivatJSON
