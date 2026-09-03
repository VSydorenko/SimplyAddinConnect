#include "../core/pch.h"
#include "AddinUAPKIConnect.h"
#include "../helpers/UAPKIConnect/UAPKIConnectHelper.h"
#include "../helpers/ServiceTools.h"
#include <nlohmann/json.hpp>

// Регистрация компонента через статический член класса
REGISTER_COMPONENT(u"AddinUAPKIConnect", AddinUAPKIConnect)

// Конструктор класса
AddinUAPKIConnect::AddinUAPKIConnect() {
    REPORT_INFO("Инициализация компонента AddinUAPKIConnect");
    
    // Регистрируем методы компонента
    RegisterMethods();
}

// Деструктор класса
AddinUAPKIConnect::~AddinUAPKIConnect() {
    // Логируем завершение работы компонента
    REPORT_INFO("Завершение работы компонента AddinUAPKIConnect");
    ServiceTools::DisableComponentLogging(this);
}

// Регистрация методов компонента
void AddinUAPKIConnect::RegisterMethods() {
    // Регистрируем метод CallUapki/ВызватьUAPKI, который будет точкой входа
    // для всех вызовов библиотеки UAPKI
    AddFunction(u"CallUapki", u"ВызватьUAPKI",
        [&](VH method, VH jsonParams) {
            try {
                // Преобразуем параметр method из VariantHelper в строку
                std::string methodStr = static_cast<std::string>(method);
                
                // Получаем строку параметров из второго аргумента
                std::string paramsStr = static_cast<std::string>(jsonParams);
                
                // Логирование вызова метода UAPKI
                REPORT_INFO("Вызов метода UAPKI: " + methodStr);
                
                std::string jsonResponse;
                
                // Вызываем метод CallUapki для выполнения команды UAPKI
                bool success = this->CallUapki(methodStr, paramsStr, jsonResponse);
                
                // Устанавливаем результат (успешный или с ошибкой)
                this->result = jsonResponse;
                
                // Логируем результат операции
                if (success) {
                    REPORT_DEBUG("Успешное выполнение метода UAPKI: " + methodStr);
                } else {
                    REPORT_DEBUG("Метод UAPKI завершился с ошибкой: " + methodStr);
                }
                
                return success;
            }
            catch (const std::exception& e) {
                // Обработка исключений - возвращаем ошибку в формате JSON.
                // Через nlohmann::json + dump(), а не конкатенацией строк: текст исключения
                // может содержать '"' и '\', и без экранирования JSON ломается на стороне 1С.
                std::string errorMessage = e.what();
                this->result = nlohmann::json{ {"errorCode", 500}, {"error", errorMessage} }.dump();

                // Логирование ошибки
                REPORT_ERROR("Исключение C++ при вызове UAPKI: " + errorMessage);
                
                return false;
            }
            catch (...) {
                // Обработка неизвестных исключений
                this->result = "{\"errorCode\":500,\"error\":\"Unknown error\"}";
                
                // Логирование неизвестной ошибки
                REPORT_ERROR("Неизвестное исключение при вызове метода UAPKI");
                
                return false;
            }
        },
        { {0, DefaultHelper(u"sign")}, {1, DefaultHelper(u"")} }  // Значения по умолчанию
    );
    
    // Логирование успешной регистрации методов
    REPORT_INFO("Регистрация методов компонента UAPKI завершена");
}

// Метод для вызова команды UAPKI
bool AddinUAPKIConnect::CallUapki(const std::string& method, const std::string& paramsString, std::string& jsonResponse) {
    try {
        // Параметры НЕ логируем: они содержат password для OPEN. Ниже хелпер
        // залогирует уже замаскированный запрос целиком.
        REPORT_DEBUG("Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + method);

        // Вызываем метод UAPKIConnectHelper для выполнения команды
        bool result = UAPKIConnectHelper::ExecuteUapkiCommand(method, paramsString, jsonResponse);
        
        // Логируем результат выполнения
        if (result) {
            // Логируем успех с ограниченным размером ответа
            if (jsonResponse.length() > 500) {
                REPORT_DEBUG("Успешное выполнение команды UAPKI: " + method + ", ответ (сокращен)=" + jsonResponse.substr(0, 500) + "...");
            } else {
                REPORT_DEBUG("Успешное выполнение команды UAPKI: " + method + ", ответ=" + jsonResponse);
            }
        } else {
            // Если результат с ошибкой - логируем
            REPORT_ERROR("Ошибка выполнения ExecuteUapkiCommand: " + jsonResponse);
        }
        
        return result;
    }
    catch (const std::exception& e) {
        std::string errorMessage = e.what();
        REPORT_ERROR("Исключение при вызове метода UAPKI " + method + ": " + errorMessage);
        // Через nlohmann::json + dump() — конкатенация ломает JSON, если errorMessage
        // содержит '"' или '\' (см. аналогичный фикс в лямбде RegisterMethods выше).
        jsonResponse = nlohmann::json{ {"errorCode", 500}, {"error", "Exception: " + errorMessage} }.dump();
        return false;
    }
    catch (...) {
        REPORT_ERROR("Неизвестное исключение при вызове метода UAPKI " + method);
        jsonResponse = "{\"errorCode\":500,\"error\":\"Unknown exception\"}";
        return false;
    }
}