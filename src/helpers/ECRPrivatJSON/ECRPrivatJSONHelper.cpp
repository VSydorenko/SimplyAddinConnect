#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONHelper::ECRPrivatJSONHelper(ITransport* transport, const std::string& componentName)
    : transport_(transport)
    , componentName_(componentName)
    , responseReceived_(false) {
}

ECRPrivatJSONHelper::~ECRPrivatJSONHelper() {
    // Очистка ресурсов
    transport_ = nullptr;
}

// Установка транспортного объекта
void ECRPrivatJSONHelper::SetTransport(ITransport* transport) {
    transport_ = transport;
}

// Метод IsSuccess перенесен из ECRPrivatJSONHelper_Service.cpp
bool ECRPrivatJSONHelper::IsSuccess(const std::string& jsonResponse) const {
    try {
        json j = json::parse(jsonResponse);
        
        // Проверка успешности на основе кода ответа
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            
            if (params.contains("responseCode")) {
                std::string responseCode = params["responseCode"].get<std::string>();
                
                // Успешными считаем коды из документации
                bool isSuccess = (responseCode == ResponseCodes::SUCCESS || 
                                 responseCode == ResponseCodes::SUCCESS_SHORT || 
                                 responseCode == ResponseCodes::PARTIAL_APPROVAL);
                
                // Специальный случай для метода "GetReceiptInfo" и для частичного одобрения (10) 
                // согласно документации: "Виключенням є RC=10, який може бути при Partial approval 
                // або при відміні операції Cashback, але успішному проведені оплати"
                if (!isSuccess && responseCode == "10") {
                    // Для GetReceiptInfo код 10 считается успешным
                    if (j.contains("method") && j["method"].get<std::string>() == "GetReceiptInfo") {
                        isSuccess = true;
                    }
                    // Для других операций проверяем поле error
                    else if (j.contains("error") && j["error"].is_boolean()) {
                        isSuccess = !j["error"].get<bool>();
                    }
                }
                
                return isSuccess;
            }
        }
        
        // Если нет кода ответа, то проверяем поле error
        if (j.contains("error") && j["error"].is_boolean()) {
            return !j["error"].get<bool>();
        }
        
        // По умолчанию считаем неуспешным, если не удалось однозначно определить
        return false;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при проверке успешности операции: " + std::string(e.what()));
        return false;
    }
}

} // namespace ECRPrivatJSON
