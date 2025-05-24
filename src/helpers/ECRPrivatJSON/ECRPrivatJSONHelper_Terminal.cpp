#include "../../core/pch.h"
#include "ECRPrivatJSONHelper.h"
#include "../ServiceTools.h"

namespace ECRPrivatJSON {

// Terminal-specific функции для работы с терминалом ПриватБанка

// Извлечение текста чека из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractReceiptText(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало извлечения текста чека из ответа");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая строка JSON для извлечения текста чека");
            return "";
        }
        
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("receiptText") && params["receiptText"].is_string()) {
                std::string receiptText = params["receiptText"].get<std::string>();
                NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Успешно извлечен текст чека");
                return receiptText;
            }
        }
        
        NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Текст чека не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при извлечении текста чека: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при извлечении текста чека: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при извлечении текста чека");
        return "";
    }
}

// Извлечение кода ответа из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractResponseCode(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало извлечения кода ответа");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая строка JSON для извлечения кода ответа");
            return "";
        }
        
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("responseCode") && params["responseCode"].is_string()) {
                std::string responseCode = params["responseCode"].get<std::string>();
                NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Извлечен код ответа: " + responseCode);
                return responseCode;
            }
        }
        
        NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Код ответа не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при извлечении кода ответа: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при извлечении кода ответа: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при извлечении кода ответа");
        return "";
    }
}

// Извлечение сообщения об ошибке из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractErrorMessage(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало извлечения сообщения об ошибке");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Получена пустая строка JSON при извлечении сообщения об ошибке");
            return "";
        }
        
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("errorMessage") && params["errorMessage"].is_string()) {
                std::string errorMessage = params["errorMessage"].get<std::string>();
                if (!errorMessage.empty()) {
                    NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Извлечено сообщение об ошибке: " + errorMessage);
                }
                return errorMessage;
            }
        }
        
        NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Сообщение об ошибке не найдено в ответе");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при извлечении сообщения об ошибке: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при извлечении сообщения об ошибке: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при извлечении сообщения об ошибке");
        return "";
    }
}

// Извлечение идентификатора транзакции из JSON-ответа
std::string ECRPrivatJSONHelper::ExtractTransactionId(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало извлечения идентификатора транзакции");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая строка JSON для извлечения идентификатора транзакции");
            return "";
        }
        
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains("transactionId") && params["transactionId"].is_string()) {
                std::string transactionId = params["transactionId"].get<std::string>();
                NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Извлечен идентификатор транзакции: " + transactionId);
                return transactionId;
            }
        }
        
        NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Идентификатор транзакции не найден в ответе терминала");
        return "";
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при извлечении идентификатора транзакции: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при извлечении идентификатора транзакции: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при извлечении идентификатора транзакции");
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
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало извлечения информации о терминале");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая строка JSON для извлечения информации о терминале");
            return false;
        }
        
        json j = ParseJSON(jsonResponse);
        
        if (j.contains("method") && j["method"] == "GetTerminalInfo" &&
            j.contains("params") && j["params"].is_object()) {
            
            auto params = j["params"];
            vendor = params.value("vendor", "Unknown");
            model = params.value("model", "Unknown");
            serialNumber = params.value("serialNumber", "Unknown");
            firmware = params.value("firmware", "Unknown");
            
            NEUTRAL_REPORT_INFO("ECRPrivatJSONHelper", "Извлечена информация о терминале: вендор=" + vendor + 
                              ", модель=" + model + ", серийный номер=" + serialNumber);
            NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Версия прошивки терминала: " + firmware);
            
            return true;
        }
        
        NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Информация о терминале не найдена в ответе");
        return false;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при извлечении информации о терминале: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при извлечении информации о терминале: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при извлечении информации о терминале");
        return false;
    }
}

// Проверка результата хендшейка
bool ECRPrivatJSONHelper::CheckHandshakeResult(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Начало проверки результата хендшейка");
        
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая строка JSON при проверке хендшейка");
            return false;
        }
        
        json j = ParseJSON(jsonResponse);
        
        bool result = (j.contains("method") && j["method"] == "PingDevice");
        
        if (result) {
            NEUTRAL_REPORT_INFO("ECRPrivatJSONHelper", "Хендшейк успешен, получен корректный ответ PingDevice");
        } else {
            if (j.contains("method")) {
                NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Хендшейк не удался: неверный метод " + j["method"].get<std::string>());
            } else {
                NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Хендшейк не удался: отсутствует поле method");
            }
        }
        
        return result;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON при проверке результата хендшейка: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при проверке результата хендшейка: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при проверке результата хендшейка");
        return false;
    }
}

} // namespace ECRPrivatJSON
