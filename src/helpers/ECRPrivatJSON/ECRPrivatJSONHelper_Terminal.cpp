#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace SimplyConnect {

// Terminal-specific функции для работы с терминалом ПриватБанка

// Извлечение текста чека из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractReceiptText(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("receiptText") && params["receiptText"].is_string()) {
                return params["receiptText"].get<std::string>();
            }
        }
        
        NEUTRAL_REPORT_WARN(componentName_, "Текст чека не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при извлечении текста чека: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении текста чека: " + std::string(e.what()));
        return "";
    }
}

// Извлечение кода ответа из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractResponseCode(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("responseCode") && params["responseCode"].is_string()) {
                return params["responseCode"].get<std::string>();
            }
        }
        
        NEUTRAL_REPORT_WARN(componentName_, "Код ответа не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при извлечении кода ответа: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении кода ответа: " + std::string(e.what()));
        return "";
    }
}

// Извлечение сообщения об ошибке из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractErrorMessage(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("errorMessage") && params["errorMessage"].is_string()) {
                return params["errorMessage"].get<std::string>();
            }
        }
        
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при извлечении сообщения об ошибке: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении сообщения об ошибке: " + std::string(e.what()));
        return "";
    }
}

// Извлечение идентификатора транзакции из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractTransactionId(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("transactionId") && params["transactionId"].is_string()) {
                return params["transactionId"].get<std::string>();
            }
        }
        
        NEUTRAL_REPORT_WARN(componentName_, "Идентификатор транзакции не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при извлечении идентификатора транзакции: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении идентификатора транзакции: " + std::string(e.what()));
        return "";
    }
}

// Извлечение информации о терминале из JSON-ответа
bool ECRPrivatJSONHelper::ExtractTerminalInfo(const std::string& jsonResponse, 
                                            std::string& vendor,
                                            std::string& model,
                                            std::string& serialNumber,
                                            std::string& firmware) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("method") && j["method"] == "GetTerminalInfo" &&
            j.contains("params") && j["params"].is_object()) {
            
            auto params = j["params"];
            vendor = params.value("vendor", "Unknown");
            model = params.value("model", "Unknown");
            serialNumber = params.value("serialNumber", "Unknown");
            firmware = params.value("firmware", "Unknown");
            
            return true;
        }
        
        NEUTRAL_REPORT_WARN(componentName_, "Информация о терминале не найдена в ответе");
        return false;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при извлечении информации о терминале: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении информации о терминале: " + std::string(e.what()));
        return false;
    }
}

// Проверка результата хендшейка
bool ECRPrivatJSONHelper::CheckHandshakeResult(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        return (j.contains("method") && j["method"] == "PingDevice");
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON при проверке результата хендшейка: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при проверке результата хендшейка: " + std::string(e.what()));
        return false;
    }
}

} // namespace SimplyConnect
