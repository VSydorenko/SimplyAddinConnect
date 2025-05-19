#include "ServiceTools.h"
#include "../core/AddInNative.h"
#include <iostream>

/**
 * @file ServiceTools.cpp
 * @brief Базовая реализация сервисных инструментов
 * 
 * Этот файл содержит общие определения и вспомогательные функции
 * для модулей логирования и обработки ошибок.
 */

namespace ServiceTools {

// Определение внешних переменных
std::mutex loggersMutex;
std::map<std::string, ComponentLogSettings> componentLogSettings;

/**
 * @brief Получает имя компонента по его указателю
 * 
 * @param component Указатель на компонент
 * @return std::string Имя компонента, готовое для использования в логах
 */
std::string GetComponentName(const AddInNative* component) {
    if (!component) {
        return "General";
    }
    
    try {
        // Получаем имя типа через RTTI
        const std::type_info& ti = typeid(*component);
        std::string fullName = ti.name();
        
        // Находим имя класса без пространства имен и декораторов
        size_t colonPos = fullName.find_last_of(':');
        size_t startPos = (colonPos == std::string::npos) ? 0 : colonPos + 1;
        
        // Находим позицию пробела после имени класса (если есть)
        size_t spacePos = fullName.find(' ', startPos);
        size_t endPos = (spacePos == std::string::npos) ? fullName.length() : spacePos;
        
        // Вырезаем имя класса
        std::string className = fullName.substr(startPos, endPos - startPos);
        
        // Если имя начинается с "class " или "struct ", убираем это
        if (className.find("class ") == 0) {
            className = className.substr(6);
        } else if (className.find("struct ") == 0) {
            className = className.substr(7);
        }
        
        return className;
    }
    catch (...) {
        // В случае ошибки возвращаем дефолтное имя
        return "UnknownComponent";
    }
}

/**
 * @brief Записывает событие в лог компонента с указанным уровнем и именем метода
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @param methodName Имя метода, в котором произошло событие
 * @param message Сообщение о событии
 */
void ReportComponentEvent(AddInNative* component, const std::string& level, const std::string& methodName, const std::string& message) {
    if (!component) {
        return;
    }
    
    std::string formattedMessage = "[" + methodName + "] " + message;
    
    // Для ошибок вызываем специальную обработку
    if (level == "error") {
        std::u16string u16Msg = SafeMB2WCHAR(formattedMessage.c_str());
        AddComponentError(component, u16Msg);
        return;
    }
    
    // Для других уровней - проверяем, включен ли данный уровень логирования
    if (IsComponentLoggingEnabled(component, level)) {
        AddComponentLog(component, level, formattedMessage);
    }
}

/**
 * @brief Реализация логирования без привязки к компоненту
 * 
 * @param level Уровень логирования
 * @param method Имя метода
 * @param tag Тег для группировки сообщений
 * @param message Сообщение
 */
void NeutralReportImpl(const std::string& level, const std::string& method, const std::string& tag, const std::string& message) {
    std::string componentName = "General";
    std::string formattedMessage = "[" + tag + "::" + method + "] " + message;
    
    if (level == "trace") {
        Trace(componentName, formattedMessage);
    } else if (level == "debug") {
        Debug(componentName, formattedMessage);
    } else if (level == "info") {
        Info(componentName, formattedMessage);
    } else if (level == "warn") {
        Warn(componentName, formattedMessage);
    } else if (level == "error") {
        Error(componentName, formattedMessage);
    }
}

/**
 * @brief Перегруженная реализация логирования без привязки к компоненту и без тега
 * 
 * @param level Уровень логирования
 * @param method Имя метода
 * @param message Сообщение
 */
void NeutralReportImpl(const std::string& level, const std::string& method, const std::string& message) {
    NeutralReportImpl(level, method, "General", message);
}

} // namespace ServiceTools
