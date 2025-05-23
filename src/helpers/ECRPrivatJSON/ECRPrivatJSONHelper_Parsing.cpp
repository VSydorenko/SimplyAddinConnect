#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <stdexcept>

namespace ECRPrivatJSON {

// Парсинг JSON-строки в объект json
json ECRPrivatJSONHelper::ParseJSON(const std::string& jsonString) {
    try {
        // Нормализуем JSON-строку перед обработкой
        std::string normalizedJson = NormalizeResponseJson(jsonString);
        
        // Проверяем валидность JSON
        if (!IsJsonValid(normalizedJson)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Невалидный JSON");
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Парсинг JSON строки");
        // Используем nlohmann/json для парсинга
        return json::parse(normalizedJson);
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке JSON: " + std::string(e.what()));
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при парсинге JSON");
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
    try {
        if (jsonString.empty()) {
            NEUTRAL_REPORT_DEBUG(componentName_, "Нормализация: получена пустая строка");
            return jsonString;
        }
        
        NEUTRAL_REPORT_TRACE(componentName_, "Нормализация JSON-ответа");
        
        // Удаляем непечатаемые символы и нулевые байты с конца
        std::string result = jsonString;
        
        // Удаляем все нули в начале (обычно это один нуль, но на всякий случай проверяем все)
        size_t startPos = 0;
        while (startPos < result.length() && result[startPos] == '\0') {
            startPos++;
        }
        
        if (startPos > 0) {
            NEUTRAL_REPORT_DEBUG(componentName_, "Удалены нулевые байты в начале: " + std::to_string(startPos));
            result = result.substr(startPos);
        }
        
        // Удаляем все непечатаемые символы и нули в конце
        size_t endPos = result.length();
        while (endPos > 0 && (result[endPos-1] == '\0' || result[endPos-1] < 32)) {
            endPos--;
        }
        
        if (endPos < result.length()) {
            NEUTRAL_REPORT_DEBUG(componentName_, "Удалены непечатаемые символы в конце: " + std::to_string(result.length() - endPos));
            result = result.substr(0, endPos);
        }
        
        // Проверяем чистоту JSON-строки внутри
        size_t replacedChars = 0;
        for (size_t i = 0; i < result.length(); i++) {
            if (result[i] == '\0') {
                // Заменяем внутренние нулевые байты на пробел
                result[i] = ' ';
                replacedChars++;
            }
        }
        
        if (replacedChars > 0) {
            NEUTRAL_REPORT_DEBUG(componentName_, "Заменены внутренние нулевые байты: " + std::to_string(replacedChars));
        }
        
        return result;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при нормализации JSON: " + std::string(e.what()));
        return jsonString; // В случае ошибки возвращаем исходную строку
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при нормализации JSON");
        return jsonString;
    }
}

// Парсинг ответа терминала в структуру TerminalResponse
bool ECRPrivatJSONHelper::ParseTerminalResponse(const std::string& jsonResponse, TerminalResponse& response) const {
    try {
        NEUTRAL_REPORT_INFO(componentName_, "Начало парсинга ответа терминала");
        
        // Нормализация JSON-строки
        std::string normalizedJson = NormalizeResponseJson(jsonResponse);
        
        // Проверка валидности JSON
        if (!IsJsonValid(normalizedJson)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Невалидный JSON-ответ от терминала");
            return false;
        }
        
        // Парсинг JSON
        NEUTRAL_REPORT_DEBUG(componentName_, "Парсинг нормализованного JSON");
        json j = json::parse(normalizedJson);
        
        // Очистка существующей информации в структуре ответа
        response.Clear();
        
        // Сохранение исходной JSON-строки
        response.jsonResponse = normalizedJson;
        
        // Определение успешности операции с использованием унифицированного метода IsSuccess
        response.success = IsSuccess(normalizedJson);
          // Заполнение основной информации
        NEUTRAL_REPORT_DEBUG(componentName_, "Заполнение полей структуры TerminalResponse");
        
        if (j.contains("method")) {
            response.method = j["method"].get<std::string>();
            NEUTRAL_REPORT_DEBUG(componentName_, "Метод: " + response.method);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Поле 'method' отсутствует в ответе");
        }
        
        if (j.contains("params")) {
            auto params = j["params"];
            
            if (params.contains("responseCode")) {
                response.responseCode = params["responseCode"].get<std::string>();
                NEUTRAL_REPORT_DEBUG(componentName_, "Код ответа: " + response.responseCode);
            }
            
            if (params.contains("errorMessage")) {
                response.errorMessage = params["errorMessage"].get<std::string>();
                // Если есть сообщение об ошибке, операция не успешна
                response.success = false;
                NEUTRAL_REPORT_WARN(componentName_, "Сообщение об ошибке: " + response.errorMessage);
            }
            
            if (params.contains("receiptText")) {
                response.receiptText = params["receiptText"].get<std::string>();
            }
            
            if (params.contains("operationStatus")) {
                response.operationStatus = params["operationStatus"].get<std::string>();
            }
              if (params.contains("totalAmount")) {
                try {
                    std::string amountStr = params["totalAmount"].get<std::string>();
                    response.totalAmount = std::stod(amountStr);
                    NEUTRAL_REPORT_DEBUG(componentName_, "Сумма: " + amountStr);
                }
                catch (const std::invalid_argument& e) {
                    NEUTRAL_REPORT_ERROR(componentName_, "Ошибка преобразования суммы: " + std::string(e.what()));
                }
                catch (const std::out_of_range& e) {
                    NEUTRAL_REPORT_ERROR(componentName_, "Сумма вне допустимого диапазона: " + std::string(e.what()));
                }
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
          NEUTRAL_REPORT_INFO(componentName_, "Парсинг ответа терминала успешно завершен");
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

} // namespace ECRPrivatJSON
