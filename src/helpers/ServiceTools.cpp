#include "../core/pch.h"

#include "ServiceTools.h"
#include "../core/AddInNative.h"

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
        
        // Сначала удаляем все декораторы, которые могут быть добавлены компилятором
        // Например, для MSVC типичным форматом является "class ИмяКласса" или "struct ИмяКласса"
        static const std::string classPrefix = "class ";
        static const std::string structPrefix = "struct ";
        
        // Удаляем префикс "class " если есть
        if (fullName.find(classPrefix) == 0) {
            fullName.erase(0, classPrefix.length());
        }
        
        // Удаляем префикс "struct " если есть
        if (fullName.find(structPrefix) == 0) {
            fullName.erase(0, structPrefix.length());
        }
        
        // Находим имя класса без пространства имен (после последнего ::)
        size_t colonPos = fullName.find_last_of(':');
        size_t startPos = (colonPos == std::string::npos) ? 0 : colonPos + 1;
          // Находим позицию пробела после имени класса (если есть)
        size_t spacePos = fullName.find(' ', startPos);
        size_t endPos = (spacePos == std::string::npos) ? fullName.length() : spacePos;
        
        // Вырезаем имя класса
        std::string className = fullName.substr(startPos, endPos - startPos);
        
        // Удаляем возможные шаблонные параметры и прочие декораторы
        size_t templatePos = className.find('<');
        if (templatePos != std::string::npos) {
            className = className.substr(0, templatePos);
        }
        
        // Если после всех преобразований имя пусто или содержит только пробелы,
        // вернем информативное имя
        if (className.empty() || className.find_first_not_of(" \t\n\r") == std::string::npos) {
            return "Component_" + std::to_string(reinterpret_cast<uintptr_t>(component));
        }
        
        return className;
    }
    catch (...) {
        // В случае ошибки возвращаем уникальный идентификатор на основе адреса компонента
        return "UnknownComponent_" + std::to_string(reinterpret_cast<uintptr_t>(component));
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
    
    // Получаем имя компонента
    std::string componentName = GetComponentName(component);
    
    // Форматируем сообщение с добавлением имени метода
    std::string formattedMessage = "[" + methodName + "] " + message;
    
    // Для ошибок вызываем специальную обработку
    if (level == "error") {
        std::u16string u16Msg = SafeMB2WCHAR(formattedMessage.c_str());
        AddComponentError(component, u16Msg);
        return;
    }
    
    // Быстрая проверка без блокировок мьютексов
    if (IsComponentLoggingEnabled(component, level)) {
        // Используем напрямую функции логирования для записи сообщения с указанием пространства имен
        if (level == "trace") {
            ServiceTools::Trace(componentName, formattedMessage);
        } else if (level == "debug") {
            ServiceTools::Debug(componentName, formattedMessage);
        } else if (level == "info") {
            ServiceTools::Info(componentName, formattedMessage);
        } else if (level == "warn") {
            ServiceTools::Warn(componentName, formattedMessage);
        } else {
            // Неизвестный уровень, используем info
            ServiceTools::Info(componentName, "Неизвестный уровень логирования '" + level + "': " + formattedMessage);
        }
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
        ServiceTools::Trace(componentName, formattedMessage);
    } else if (level == "debug") {
        ServiceTools::Debug(componentName, formattedMessage);
    } else if (level == "info") {
        ServiceTools::Info(componentName, formattedMessage);
    } else if (level == "warn") {
        ServiceTools::Warn(componentName, formattedMessage);
    } else if (level == "error") {
        ServiceTools::Error(componentName, formattedMessage);
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
