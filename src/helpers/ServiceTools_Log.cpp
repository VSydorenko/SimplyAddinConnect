#include "ServiceTools.h"
#include "../core/AddInNative.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <map>
#include <mutex>
#include <iostream>

/**
 * @file ServiceTools_Log.cpp
 * @brief Реализация инструментов для логирования
 * 
 * Этот файл содержит реализацию методов и функций логирования, объявленных в ServiceTools.h.
 * Обеспечивает потокобезопасную работу с логгерами.
 */

namespace ServiceTools {

/**
 * @brief Карта зарегистрированных логгеров для компонентов
 * 
 * Карта для хранения логгеров всех компонентов системы. Ключом является название компонента,
 * значением - указатель на объект логгера.
 */
static std::map<std::string, std::shared_ptr<spdlog::logger>> loggers;

/**
 * @brief Преобразует строку уровня логирования в перечисление LogLevel
 * 
 * @param levelStr Строка с названием уровня логирования
 * @return LogLevel Соответствующее значение перечисления
 */
LogLevel StringToLogLevel(const std::string& levelStr) {
    if (levelStr == "trace") return LogLevel::Trace;
    if (levelStr == "debug") return LogLevel::Debug;
    if (levelStr == "info") return LogLevel::Info;
    if (levelStr == "warn") return LogLevel::Warn;
    if (levelStr == "error") return LogLevel::Error;
    if (levelStr == "off") return LogLevel::Off;
    
    // По умолчанию возвращаем Info
    return LogLevel::Info;
}

/**
 * @brief Конвертирует уровень логирования ServiceTools::LogLevel в соответствующий уровень spdlog
 * 
 * @param level Уровень логирования ServiceTools
 * @return spdlog::level::level_enum Соответствующий уровень spdlog
 */
static spdlog::level::level_enum ConvertLogLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Off:    return spdlog::level::off;
    case LogLevel::Error:  return spdlog::level::err;
    case LogLevel::Warn:   return spdlog::level::warn;
    case LogLevel::Info:   return spdlog::level::info;
    case LogLevel::Debug:  return spdlog::level::debug;
    case LogLevel::Trace:  return spdlog::level::trace;
    default:               return spdlog::level::info;
    }
}

/**
 * @brief Получает логгер для компонента
 * 
 * @param componentName Название компонента
 * @return std::shared_ptr<spdlog::logger> Указатель на логгер компонента или дефолтный логгер
 */
static std::shared_ptr<spdlog::logger> GetLogger(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(loggersMutex);
    
    auto it = loggers.find(componentName);
    if (it != loggers.end()) {
        return it->second;
    }
    
    // Если логгер для компонента не найден, возвращаем дефолтный логгер
    return spdlog::default_logger();
}

/**
 * @brief Инициализирует логирование для компонента
 * 
 * @param componentName Название компонента-владельца логгера
 * @param level Уровень логирования согласно LogLevel
 * @param filePath Путь к файлу лога
 * @return true, если логирование успешно инициализировано
 * @return false, если произошла ошибка
 */
bool InitLogging(const std::string& componentName, LogLevel level, const std::string& filePath) {
    try {
        std::lock_guard<std::mutex> lock(loggersMutex);
        
        // Проверяем, существует ли уже логгер для данного компонента
        auto it = loggers.find(componentName);
        if (it != loggers.end()) {
            // Если логгер уже существует, просто обновляем уровень логирования
            it->second->set_level(ConvertLogLevel(level));
            
            // Также обновляем настройки компонента
            ComponentLogSettings settings(filePath, level, true);
            componentLogSettings[componentName] = settings;
            
            return true;
        }
        
        // Создаем новый логгер с ротацией файлов (5 файлов по 10 МБ)
        auto logger = spdlog::rotating_logger_mt(
            componentName, 
            filePath, 
            10 * 1024 * 1024, // 10 МБ
            5                // 5 файлов
        );
        
        // Устанавливаем уровень логирования
        logger->set_level(ConvertLogLevel(level));
        
        // Сохраняем логгер в карту
        loggers[componentName] = logger;
        
        // Обновляем настройки компонента
        ComponentLogSettings settings(filePath, level, true);
        componentLogSettings[componentName] = settings;
        
        // Логируем информацию об инициализации логирования
        logger->info("Логирование инициализировано для компонента {}, уровень: {}, путь: {}", 
             componentName, static_cast<int>(level), filePath);
        
        return true;
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Ошибка инициализации логирования для " << componentName 
                 << ": " << ex.what() << std::endl;
        return false;
    }
}

/**
 * @brief Завершает работу логгера для компонента
 * 
 * @param componentName Название компонента, логгер которого нужно закрыть
 */
void ShutdownLogging(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(loggersMutex);
    
    auto it = loggers.find(componentName);
    if (it != loggers.end()) {
        try {
            Info(componentName, "Завершение работы логгера для компонента " + componentName);
            loggers.erase(it);
            
            // Также удаляем настройки компонента
            auto settingsIt = componentLogSettings.find(componentName);
            if (settingsIt != componentLogSettings.end()) {
                componentLogSettings.erase(settingsIt);
            }
        }
        catch (...) {
            // Игнорируем исключения при закрытии логгера
        }
    }
}

/**
 * @brief Включает логирование для компонента
 * 
 * @param component Указатель на компонент
 * @param logLevel Уровень логирования (строковое значение)
 * @param logFilePath Путь к файлу логов
 * @return true, если логирование успешно включено
 * @return false, если произошла ошибка
 */
bool EnableComponentLogging(AddInNative* component, const std::string& logLevel, const std::string& logFilePath) {
    if (!component) {
        return false;
    }
    
    // Определяем название компонента
    std::string componentName = GetComponentName(component);
    
    // Преобразуем строковый уровень логирования в перечисление
    LogLevel level = StringToLogLevel(logLevel);
    
    // Если уровень не Off, а путь к файлу пуст - это ошибка
    if (level != LogLevel::Off && logFilePath.empty()) {
        return false;
    }
    
    // Инициализируем логирование
    bool result = InitLogging(componentName, level, logFilePath);
    
    return result;
}

/**
 * @brief Выключает логирование для компонента
 * 
 * @param component Указатель на компонент
 * @return true, если логирование успешно отключено
 * @return false, если произошла ошибка
 */
bool DisableComponentLogging(AddInNative* component) {
    if (!component) {
        return false;
    }
    
    // Определяем название компонента
    std::string componentName = GetComponentName(component);
    
    // Закрываем логгер
    ShutdownLogging(componentName);
    
    return true;
}

/**
 * @brief Проверяет, включено ли логирование указанного уровня для компонента
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @return true, если логирование данного уровня включено
 * @return false, если логирование данного уровня выключено или произошла ошибка
 */
bool IsComponentLoggingEnabled(AddInNative* component, const std::string& level) {
    if (!component) {
        return false;
    }
    
    std::string componentName = GetComponentName(component);
    
    std::lock_guard<std::mutex> lock(loggersMutex);
    
    // Проверяем, есть ли настройки для данного компонента
    auto it = componentLogSettings.find(componentName);
    if (it == componentLogSettings.end() || !it->second.isEnabled) {
        return false;
    }
    
    // Преобразуем строковый уровень логирования в перечисление
    LogLevel requestedLevel = StringToLogLevel(level);
    
    // Проверяем, позволяет ли текущий уровень логирования записывать сообщения данного уровня
    return it->second.logLevel >= requestedLevel;
}

/**
 * @brief Добавляет запись в лог компонента
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @param message Сообщение для записи в лог
 * @return true, если сообщение успешно записано
 * @return false, если произошла ошибка
 */
bool AddComponentLog(AddInNative* component, const std::string& level, const std::string& message) {
    if (!component) {
        return false;
    }
    
    // Определяем название компонента
    std::string componentName = GetComponentName(component);
    
    // Получаем логгер для компонента
    auto logger = GetLogger(componentName);
    
    // Форматируем сообщение с добавлением имени компонента в квадратных скобках
    std::string formattedMsg = "[" + componentName + "] " + message;
    
    // Записываем сообщение с соответствующим уровнем
    if (level == "trace") {
        logger->trace(formattedMsg);
    } else if (level == "debug") {
        logger->debug(formattedMsg);
    } else if (level == "info") {
        logger->info(formattedMsg);
    } else if (level == "warn") {
        logger->warn(formattedMsg);
    } else if (level == "error") {
        logger->error(formattedMsg);
    } else {
        // Неизвестный уровень
        return false;
    }
    
    return true;
}

/**
 * @brief Логирует информационное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 */
void Info(const std::string& componentName, const std::string& message) {
    auto logger = GetLogger(componentName);
    logger->info(message);
}

/**
 * @brief Логирует сообщение об ошибке
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 */
void Error(const std::string& componentName, const std::string& message) {
    auto logger = GetLogger(componentName);
    logger->error(message);
}

/**
 * @brief Логирует отладочное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 */
void Debug(const std::string& componentName, const std::string& message) {
    auto logger = GetLogger(componentName);
    logger->debug(message);
}

/**
 * @brief Логирует предупреждение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 */
void Warn(const std::string& componentName, const std::string& message) {
    auto logger = GetLogger(componentName);
    logger->warn(message);
}

/**
 * @brief Логирует трассировочное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 */
void Trace(const std::string& componentName, const std::string& message) {
    auto logger = GetLogger(componentName);
    logger->trace(message);
}

} // namespace ServiceTools
