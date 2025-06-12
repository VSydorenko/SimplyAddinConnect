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
    // Здесь при необходимости можно выполнять дополнительные действия при уничтожении компонента
}

// Регистрация методов компонента
void AddinUAPKIConnect::RegisterMethods() {
    // Регистрируем метод CallUapki/ВызватьUAPKI, который будет точкой входа
    // для всех вызовов библиотеки UAPKI
    AddFunction(u"CallUapki", u"ВызватьUAPKI",
        [&](VH method, VH jsonParams) {
            try {
                // Преобразуем параметр method из VariantHelper в строку
                std::string methodStr = method;
                
                // Получаем строку параметров из второго аргумента
                std::string paramsStr = (std::string)jsonParams;
                
                std::string jsonResponse;
                
                // Логирование вызова метода UAPKI с использованием ServiceTools
                ServiceTools::LogInfo(u"[AddinUAPKIConnect] Вызов метода UAPKI: " + 
                                     std::u16string(methodStr.begin(), methodStr.end()));
                
                // Вызываем метод CallUapki для выполнения команды UAPKI
                bool success = this->CallUapki(methodStr, paramsStr, jsonResponse);
                
                // Если вызов не успешен, логируем ошибку
                if (!success) {
                    ServiceTools::LogError(u"[AddinUAPKIConnect] Ошибка выполнения метода " + 
                                         std::u16string(methodStr.begin(), methodStr.end()) + 
                                         u": " + std::u16string(jsonResponse.begin(), jsonResponse.end()));
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
                
                // Логирование ошибки с использованием ServiceTools
                ServiceTools::LogError(AddInNative::MB2WCHAR(errorMessage));
                
                // Добавляем ошибку в лог компоненты
                AddError(AddInNative::MB2WCHAR(errorMessage), E_FAIL);
                
                return false;
            }
            catch (...) {
                // Обработка неизвестных исключений
                std::string jsonError = "{\"status\":\"error\",\"error\":\"Unknown error\"}";
                this->result = jsonError;
                
                // Логирование неизвестной ошибки
                ServiceTools::LogError(u"[AddinUAPKIConnect] Неизвестная ошибка при вызове метода");
                
                // Добавляем ошибку в лог компоненты
                AddError(u"Неизвестная ошибка при вызове UAPKI", E_FAIL);
                
                return false;
            }
        },
        { {0, DefaultHelper(u"sign")}, {1, DefaultHelper(u"")} }  // Значения по умолчанию
    );
    
    // Логирование успешной регистрации методов
    ServiceTools::LogInfo(u"[AddinUAPKIConnect] Регистрация методов компонента завершена");
}

// Метод для вызова команды UAPKI
bool AddinUAPKIConnect::CallUapki(const std::string& method, const std::string& paramsString, std::string& jsonResponse) {
    // Логирование вызова с ограничением длины параметров
    std::string logParams = paramsString;
    if (logParams.length() > 500) {
        logParams = logParams.substr(0, 500) + "...";
    }
    ServiceTools::LogDebug(u"[AddinUAPKIConnect] Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + 
                          std::u16string(method.begin(), method.end()) + 
                          u", параметры=" + std::u16string(logParams.begin(), logParams.end()));
    
    // Вызываем метод UAPKIConnectHelper для выполнения команды
    bool result = UAPKIConnectHelper::ExecuteUapkiCommand(method, paramsString, jsonResponse);
    
    // Если результат выполнения с ошибкой - логируем
    if (!result) {
        ServiceTools::LogError(u"[AddinUAPKIConnect] Ошибка выполнения ExecuteUapkiCommand: " + 
                              std::u16string(jsonResponse.begin(), jsonResponse.end()));
    }
    
    return result;
}