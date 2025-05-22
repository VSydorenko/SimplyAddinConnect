#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <stdexcept>

namespace SimplyConnect {

// Парсинг JSON-строки в объект json
json ECRPrivatJSONHelper::ParseJSON(const std::string& jsonString) {
    try {
        // Нормализуем JSON-строку перед обработкой
        std::string normalizedJson = NormalizeResponseJson(jsonString);
        
        // Проверяем валидность JSON
        if (!IsJsonValid(normalizedJson)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Невалидный JSON");
            throw std::runtime_error("Невалидный JSON");
        }
        
        // Используем nlohmann/json для парсинга
        return json::parse(normalizedJson);
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        throw std::runtime_error(std::string("Ошибка парсинга JSON: ") + e.what());
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке JSON: " + std::string(e.what()));
        throw std::runtime_error(std::string("Ошибка при обработке JSON: ") + e.what());
    }
}

// Проверка корректности формата JSON
bool ECRPrivatJSONHelper::IsJsonValid(const std::string& jsonString) const {
    if (jsonString.empty()) {
        NEUTRAL_REPORT_DEBUG(componentName_, "Пустая JSON-строка");
        return false;
    }
    
    // Используем nlohmann/json для проверки корректности JSON
    try {
        json j = json::parse(jsonString);
        return j.is_object();
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_DEBUG(componentName_, "JSON не валиден: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_DEBUG(componentName_, "Ошибка при проверке JSON: " + std::string(e.what()));
        return false;
    }
}

// Приведение JSON-ответа к нормальной форме
std::string ECRPrivatJSONHelper::NormalizeResponseJson(const std::string& jsonString) const {
    if (jsonString.empty()) {
        return jsonString;
    }
    
    // Удаляем непечатаемые символы и нулевые байты с конца
    std::string result = jsonString;
    
    // Удаляем все нули в начале (обычно это один нуль, но на всякий случай проверяем все)
    size_t startPos = 0;
    while (startPos < result.length() && result[startPos] == '\0') {
        startPos++;
    }
    
    if (startPos > 0) {
        result = result.substr(startPos);
    }
    
    // Удаляем все непечатаемые символы и нули в конце
    size_t endPos = result.length();
    while (endPos > 0 && (result[endPos-1] == '\0' || result[endPos-1] < 32)) {
        endPos--;
    }
    
    if (endPos < result.length()) {
        result = result.substr(0, endPos);
    }
    
    // Проверяем чистоту JSON-строки внутри
    for (size_t i = 0; i < result.length(); i++) {
        if (result[i] == '\0') {
            // Заменяем внутренние нулевые байты на пробел
            result[i] = ' ';
        }
    }
    
    return result;
}

bool ECRPrivatJSONHelper::ParseTerminalResponse(const std::string& jsonResponse, TerminalResponse& response) const {
    try {
        // Нормализация JSON-строки
        std::string normalizedJson = NormalizeResponseJson(jsonResponse);
        
        // Проверка валидности JSON
        if (!IsJsonValid(normalizedJson)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Невалидный JSON-ответ от терминала: " + jsonResponse);
            return false;
        }
        
        // Парсинг JSON
        json j = json::parse(normalizedJson);
        
        // Очистка существующей информации в структуре ответа
        response.Clear();
        
        // Сохранение исходной JSON-строки
        response.jsonResponse = normalizedJson;
        
        // Определение успешности операции с использованием унифицированного метода IsSuccess
        response.success = IsSuccess(normalizedJson);
        
        // Заполнение основной информации
        if (j.contains("method")) {
            response.method = j["method"].get<std::string>();
        }
        
        if (j.contains("params")) {
            auto params = j["params"];
            
            if (params.contains("responseCode")) {
                response.responseCode = params["responseCode"].get<std::string>();
            }
            
            if (params.contains("errorMessage")) {
                response.errorMessage = params["errorMessage"].get<std::string>();
                // Если есть сообщение об ошибке, операция не успешна
                response.success = false;
            }
            
            if (params.contains("receiptText")) {
                response.receiptText = params["receiptText"].get<std::string>();
            }
            
            if (params.contains("operationStatus")) {
                response.operationStatus = params["operationStatus"].get<std::string>();
            }
            
            if (params.contains("totalAmount")) {
                std::string amountStr = params["totalAmount"].get<std::string>();
                response.totalAmount = std::stod(amountStr);
            }
            
            if (params.contains("currency")) {
                response.currency = params["currency"].get<std::string>();
            }
            
            if (params.contains("transactionId")) {
                response.transactionId = params["transactionId"].get<std::string>();
            }
            
            if (params.contains("terminalId")) {
                response.terminalId = params["terminalId"].get<std::string>();
            }
            
            if (params.contains("approvalCode")) {
                response.approvalCode = params["approvalCode"].get<std::string>();
            }
            
            if (params.contains("rrn")) {
                response.rrn = params["rrn"].get<std::string>();
            }
            
            if (params.contains("cardPAN")) {
                response.cardPAN = params["cardPAN"].get<std::string>();
            }
            
            if (params.contains("cardExpDate")) {
                response.cardExpDate = params["cardExpDate"].get<std::string>();
            }
            
            if (params.contains("cardHolder")) {
                response.cardHolder = params["cardHolder"].get<std::string>();
            }
            
            if (params.contains("merchantId")) {
                response.merchantId = params["merchantId"].get<std::string>();
            }
            
            if (params.contains("AID")) {
                response.aid = params["AID"].get<std::string>();
            }
            
            if (params.contains("paymentStatus")) {
                response.paymentStatus = params["paymentStatus"].get<std::string>();
            }
            
            if (params.contains("bankName")) {
                response.bankName = params["bankName"].get<std::string>();
            }
            
            if (params.contains("refundNDSPerc")) {
                response.refundNDSPerc = params["refundNDSPerc"].get<std::string>();
            }
            
            if (params.contains("refundNDSAmount")) {
                response.refundNDSAmount = params["refundNDSAmount"].get<std::string>();
            }
        }
        
        return true;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке ответа терминала: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при обработке ответа терминала");
        return false;
    }
}

} // namespace SimplyConnect
