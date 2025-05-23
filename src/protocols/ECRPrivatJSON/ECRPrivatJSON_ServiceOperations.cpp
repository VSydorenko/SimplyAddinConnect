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
    NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для мерчанта: " + merchantId);
    
    if (!IsConnected()) {
        NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для мерчанта: " + merchantId);
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Запрос дневного отчета для всех мерчантов");
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::Verify), 
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса дневного отчета: " + std::string(e.what()));
            return false;
        }
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса дневного отчета: " + std::string(e.what()));
            return false;
        }
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed;
        try {
            parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос дневного отчета: " + std::string(e.what()));
            return false;
        }
        
        if (!parsed) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос дневного отчета");
            return false;
        }
        
        if (!lastResponse_.success) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения дневного отчета: " + lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            return false;
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Успешное получение дневного отчета, код: " + lastResponse_.responseCode + ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            return true;
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе дневного отчета: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе дневного отчета");
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetXReport(const std::string& merchantId) {
    NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для мерчанта: " + merchantId);
    
    if (!IsConnected()) {
        NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для мерчанта: " + merchantId);
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Запрос X-отчета для всех мерчантов");
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::XReport), 
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса X-отчета: " + std::string(e.what()));
            return false;
        }
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса X-отчета: " + std::string(e.what()));
            return false;
        }
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed;
        try {
            parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос X-отчета: " + std::string(e.what()));
            return false;
        }
        
        if (!parsed) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос X-отчета");
            return false;
        }
        
        if (!lastResponse_.success) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения X-отчета: " + lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            return false;
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Успешное получение X-отчета, код: " + lastResponse_.responseCode + ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            return true;
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе X-отчета: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе X-отчета");
        return false;
    }
}

bool ECRPrivatJSONProtocol::GetZReport(const std::string& merchantId) {
    NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для мерчанта: " + merchantId);
    
    if (!IsConnected()) {
        NEUTRAL_REPORT_ERROR(componentName_, "Терминал не подключен");
        return false;
    }
    
    try {
        // Формируем параметры запроса
        std::map<std::string, std::string> params;
        
        if (!merchantId.empty() && merchantId != "0") {
            params["merchantId"] = merchantId;
            NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для мерчанта: " + merchantId);
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Запрос Z-отчета для всех мерчантов");
        }
        
        // Формируем запрос
        std::string request;
        try {
            request = helper_->BuildRequest(
                helper_->OperationTypeToString(OperationType::ZReport), 
                params
            );
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса Z-отчета: " + std::string(e.what()));
            return false;
        }
        
        // Отправляем запрос и получаем ответ
        std::string response;
        try {
            response = helper_->SendReceive(request);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при отправке запроса Z-отчета: " + std::string(e.what()));
            return false;
        }
        
        // Сохраняем полный ответ
        lastResponse_.jsonResponse = response;
        
        // Используем полученный ответ для создания структуры ответа
        bool parsed;
        try {
            parsed = helper_->ParseTerminalResponse(response, lastResponse_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при разборе ответа на запрос Z-отчета: " + std::string(e.what()));
            return false;
        }
        
        if (!parsed) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос Z-отчета");
            return false;
        }
        
        if (!lastResponse_.success) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения Z-отчета: " + lastResponse_.errorMessage + ", код: " + lastResponse_.responseCode);
            return false;
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Успешное получение Z-отчета, код: " + lastResponse_.responseCode + ", длина чека: " + std::to_string(lastResponse_.receiptText.length()));
            return true;
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при запросе Z-отчета: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при запросе Z-отчета");
        return false;
    }
}

TerminalResponse ECRPrivatJSONProtocol::GetLastResponse() const {
    return lastResponse_;
}

} // namespace ECRPrivatJSON
