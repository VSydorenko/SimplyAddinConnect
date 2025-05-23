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
    NEUTRAL_REPORT_DEBUG(componentName_, "Инициализация ECRPrivatJSONHelper");
}

ECRPrivatJSONHelper::~ECRPrivatJSONHelper() {
    // Очистка ресурсов
    NEUTRAL_REPORT_DEBUG(componentName_, "Деструктор ECRPrivatJSONHelper");
    transport_ = nullptr;
}

// Установка транспортного объекта
void ECRPrivatJSONHelper::SetTransport(ITransport* transport) {
    NEUTRAL_REPORT_INFO(componentName_, "Установка нового транспортного объекта");
    transport_ = transport;
}

// Метод IsSuccess перенесен из ECRPrivatJSONHelper_Service.cpp
bool ECRPrivatJSONHelper::IsSuccess(const std::string& jsonResponse) const {
    try {
        NEUTRAL_REPORT_DEBUG(componentName_, "Проверка успешности выполнения операции");
        
        // Проверка на пустой ответ
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN(componentName_, "Получен пустой JSON ответ");
            return false;
        }
        
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
                
                if (!isSuccess) {
                    NEUTRAL_REPORT_DEBUG(componentName_, "Код ответа не соответствует успешным кодам: " + responseCode);
                }
                
                // Специальный случай для метода "GetReceiptInfo" и для частичного одобрения (10) 
                // согласно документации: "Виключенням є RC=10, який може бути при Partial approval 
                // або при відміні операції Cashback, але успішному проведені оплати"
                if (!isSuccess && responseCode == "10") {
                    // Для GetReceiptInfo код 10 считается успешным
                    if (j.contains("method") && j["method"].get<std::string>() == "GetReceiptInfo") {
                        NEUTRAL_REPORT_INFO(componentName_, "Код 10 для метода GetReceiptInfo считается успешным");
                        isSuccess = true;
                    }
                    // Для других операций проверяем поле error
                    else if (j.contains("error") && j["error"].is_boolean()) {
                        isSuccess = !j["error"].get<bool>();
                        if (isSuccess) {
                            NEUTRAL_REPORT_INFO(componentName_, "Код 10 с отсутствием ошибки считается успешным");
                        }
                    }
                }
                
                return isSuccess;
            } else {
                NEUTRAL_REPORT_WARN(componentName_, "В ответе отсутствует поле responseCode");
            }
        }
        
        // Если нет кода ответа, то проверяем поле error
        if (j.contains("error") && j["error"].is_boolean()) {
            bool hasError = j["error"].get<bool>();
            if (hasError) {
                NEUTRAL_REPORT_WARN(componentName_, "В ответе указано наличие ошибки");
            }
            return !hasError;
        }
        
        // По умолчанию считаем неуспешным, если не удалось однозначно определить
        NEUTRAL_REPORT_WARN(componentName_, "Невозможно определить успешность операции, считаем неуспешной");
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
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при проверке успешности операции");
        return false;
    }
}

} // namespace ECRPrivatJSON
