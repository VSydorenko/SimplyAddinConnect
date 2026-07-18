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
    // Добавляем метод для включения логирования из 1С
    AddFunction(u"EnableLogging", u"ИспользоватьЛогирование",
        [&](VH logLevel, VH logFilePath) {
            try {
                // Преобразуем параметры в строки
                std::string level = logLevel;
                std::string path = logFilePath;
                
                REPORT_INFO("Запрос на включение логирования с уровнем: " + level + ", путь: " + path);
                
                // Вызываем метод EnableLogging
                bool result = this->EnableLogging(level, path);
                
                if (result) {
                    REPORT_DEBUG("Логирование успешно настроено с уровнем: " + level);
                }
                
                return result;
            }
            catch (const std::exception& e) {
                REPORT_ERROR("Ошибка при включении логирования: " + std::string(e.what()));
                return false;
            }
            catch (...) {
                REPORT_ERROR("Неизвестная ошибка при включении логирования");
                return false;
            }
        },
        { {0, DefaultHelper(u"info")}, {1, DefaultHelper(u"")} }  // Значения по умолчанию
    );
    
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
                // Обработка исключений - возвращаем ошибку в формате JSON
                std::string errorMessage = e.what();
                this->result = "{\"errorCode\":500,\"error\":\"" + errorMessage + "\"}";

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
        // Логирование вызова с ограничением длины параметров
        std::string logParams;
        if (paramsString.length() > 500) {
            logParams = paramsString.substr(0, 500) + "...";
            REPORT_DEBUG("Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + method + ", сокращенные параметры=" + logParams);
        } else {
            REPORT_DEBUG("Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + method + ", параметры=" + paramsString);
        }
        
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
        jsonResponse = "{\"errorCode\":500,\"error\":\"Exception: " + errorMessage + "\"}";
        return false;
    }
    catch (...) {
        REPORT_ERROR("Неизвестное исключение при вызове метода UAPKI " + method);
        jsonResponse = "{\"errorCode\":500,\"error\":\"Unknown exception\"}";
        return false;
    }
}

// Реализация метода EnableLogging
bool AddinUAPKIConnect::EnableLogging(const std::string& logLevel, const std::string& logFilePath) {
    try {
        // Делегирование вызова к ServiceTools
        bool result = ServiceTools::EnableComponentLogging(this, logLevel, logFilePath);
        
        if (result) {
            REPORT_DEBUG("Логирование успешно настроено с уровнем: " + logLevel + ", путь: " + 
                         (logFilePath.empty() ? "стандартный" : logFilePath));
        } else {
            REPORT_ERROR("Не удалось настроить логирование с уровнем: " + logLevel);
        }
        
        return result;
    }
    catch (const std::exception& e) {
        // В случае ошибки не используем REPORT_ERROR, так как логирование могло не настроиться
        std::cout << "AddinUAPKIConnect: Ошибка при настройке логирования: " << e.what() << std::endl;
        return false;
    }
    catch (...) {
        std::cout << "AddinUAPKIConnect: Неизвестная ошибка при настройке логирования" << std::endl;
        return false;
    }
}