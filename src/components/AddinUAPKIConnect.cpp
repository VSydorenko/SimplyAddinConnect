#include "../core/pch.h"
#include "AddinUAPKIConnect.h"
#include "../helpers/UAPKIConnect/UAPKIConnectHelper.h"
#include "../helpers/ServiceTools.h"

// Регистрация компонента через статический член класса
std::vector<std::u16string> AddinUAPKIConnect::names = {
    AddComponent(u"AddinUAPKIConnect", []() { return new AddinUAPKIConnect; })
};
namespace { auto& _forceAddinUAPKIConnectNames = AddinUAPKIConnect::names; }

// Конструктор класса
AddinUAPKIConnect::AddinUAPKIConnect() {
    // Регистрируем методы компонента
    RegisterMethods();
}

// Деструктор класса
AddinUAPKIConnect::~AddinUAPKIConnect() {
    // Логируем завершение работы компонента
    REPORT_INFO("Завершение работы компонента UAPKI");
    ServiceTools::DisableComponentLogging(this);
}

// Регистрация методов компонента
void AddinUAPKIConnect::RegisterMethods() {
    // Добавляем метод для включения логирования из 1С
    AddFunction(u"EnableLogging", u"ИспользоватьЛогирование",
        [&](VH logLevel, VH logFilePath) {
            // Преобразуем параметры в строки
            std::string level = logLevel;
            std::string path = logFilePath;
            
            // Вызываем метод EnableLogging
            return this->EnableLogging(level, path);
        },
        { {0, DefaultHelper(u"info")}, {1, DefaultHelper(u"")} }  // Значения по умолчанию
    );
    
    // Регистрируем метод CallUapki/ВызватьUAPKI, который будет точкой входа
    // для всех вызовов библиотеки UAPKI
    AddFunction(u"CallUapki", u"ВызватьUAPKI",
        [&](VH method, VH jsonParams) {
            try {
                // Преобразуем параметр method из VariantHelper в строку
                std::string methodStr = method;
                
                // Получаем строку параметров из второго аргумента
                std::string paramsStr = (std::string)jsonParams;
                
                // Логирование вызова метода UAPKI
                std::string logMsg = "Вызов метода UAPKI: " + methodStr;
                REPORT_INFO(logMsg);
                
                std::string jsonResponse;
                
                // Вызываем метод CallUapki для выполнения команды UAPKI
                bool success = this->CallUapki(methodStr, paramsStr, jsonResponse);
                
                // Если вызов не успешен, логируем ошибку
                if (!success) {
                    std::string errorMsg = "Ошибка выполнения метода " + methodStr + ": " + jsonResponse;
                    REPORT_ERROR(errorMsg);
                }
                
                // Устанавливаем результат (успешный или с ошибкой)
                this->result = jsonResponse;
                
                return success;
            }
            catch (const std::exception& e) {
                // Обработка исключений - возвращаем ошибку в формате JSON
                std::string errorMessage = e.what();
                std::string jsonError = "{\"status\":\"error\",\"error\":\"" + errorMessage + "\"}";
                this->result = jsonError;
                
                // Логирование ошибки
                std::string errorMsg = "Исключение C++: " + errorMessage;
                REPORT_ERROR(errorMsg);
                
                return false;
            }
            catch (...) {
                // Обработка неизвестных исключений
                std::string jsonError = "{\"status\":\"error\",\"error\":\"Unknown error\"}";
                this->result = jsonError;
                
                // Логирование неизвестной ошибки
                REPORT_ERROR("Неизвестная ошибка при вызове метода UAPKI");
                
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
    // Логирование вызова с ограничением длины параметров
    std::string logParams = paramsString;
    if (logParams.length() > 500) {
        logParams = logParams.substr(0, 500) + "...";
    }
    
    std::string debugMsg = "Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + 
                         method + ", параметры=" + logParams;
    REPORT_DEBUG(debugMsg);
    
    // Вызываем метод UAPKIConnectHelper для выполнения команды
    bool result = UAPKIConnectHelper::ExecuteUapkiCommand(method, paramsString, jsonResponse);
    
    // Если результат выполнения с ошибкой - логируем
    if (!result) {
        std::string errorMsg = "Ошибка выполнения ExecuteUapkiCommand: " + jsonResponse;
        REPORT_ERROR(errorMsg);
    }
    
    return result;
}

// Реализация метода EnableLogging
bool AddinUAPKIConnect::EnableLogging(const std::string& logLevel, const std::string& logFilePath) {
    // Делегирование вызова к ServiceTools
    return ServiceTools::EnableComponentLogging(this, logLevel, logFilePath);
}