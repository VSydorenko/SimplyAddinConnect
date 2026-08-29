#include "../core/pch.h"

#include "ServiceTools.h"
#include "../core/AddInNative.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

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
    // Преобразование входной строки к нижнему регистру для регистронезависимого сравнения
    std::string lowerLevelStr = levelStr;
    std::transform(lowerLevelStr.begin(), lowerLevelStr.end(), lowerLevelStr.begin(), 
                   [](unsigned char c) { return std::tolower(c); });
    
    if (lowerLevelStr == "trace") return LogLevel::Trace;
    if (lowerLevelStr == "debug") return LogLevel::Debug;
    if (lowerLevelStr == "info") return LogLevel::Info;
    if (lowerLevelStr == "warn" || lowerLevelStr == "warning") return LogLevel::Warn;
    if (lowerLevelStr == "error" || lowerLevelStr == "err") return LogLevel::Error;
    if (lowerLevelStr == "off" || lowerLevelStr == "none") return LogLevel::Off;
    
    // Проверяем числовые значения
    if (lowerLevelStr == "5" || lowerLevelStr == "all") return LogLevel::Trace;  // 5 = Trace
    if (lowerLevelStr == "4") return LogLevel::Debug;                           // 4 = Debug
    if (lowerLevelStr == "3") return LogLevel::Info;                            // 3 = Info
    if (lowerLevelStr == "2") return LogLevel::Warn;                            // 2 = Warn
    if (lowerLevelStr == "1") return LogLevel::Error;                           // 1 = Error
    if (lowerLevelStr == "0") return LogLevel::Off;                             // 0 = Off
    
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
// Fallback-логер до першого EnableLogging: OutputDebugString, рівень warn — щоб
// діагностика старту DLL не губилась мовчки. msvc_sink_mt(false): БЕЗ перевірки
// IsDebuggerPresent — інакше DebugView (який читає буфер DBWIN, але відладчиком
// не є) не бачив жодного рядка, і зібрати діагностику без Visual Studio було
// неможливо (інцидент 2026-08-29).
static std::shared_ptr<spdlog::logger> GetFallbackLogger() {
    static std::shared_ptr<spdlog::logger> fallback = [] {
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>(false);
        auto logger = std::make_shared<spdlog::logger>("SimplyAddinConnect", sink);
        logger->set_level(spdlog::level::warn);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        return logger;
    }();
    return fallback;
}

// Прив'язує логер "General" до sink'ів логера-донора (спільний файл, той самий
// рівень). "General" — канал NEUTRAL_REPORT_* (транспорти, драйвери, статичні
// методи): окремо він ніде не реєструється, тож без прив'язки вся діагностика
// Connect/TransportTCP ішла у fallback і НЕ потрапляла у файл, увімкнений через
// ИспользоватьЛогирование. Семантика: останній EnableLogging виграє.
// Викликати ЛИШЕ під loggersMutex. Логер створюємо вручну (НЕ через фабрику
// spdlog) — щоб ім'я "General" не потрапляло в глобальний реєстр spdlog і не
// конфліктувало при повторних прив'язках.
static void BindGeneralToLocked(const std::shared_ptr<spdlog::logger>& donor) {
    auto general = std::make_shared<spdlog::logger>(
        "General", donor->sinks().begin(), donor->sinks().end());
    general->set_level(donor->level());
    // info+ — одразу на диск (див. коментар у InitLogging)
    general->flush_on(spdlog::level::info);
    loggers["General"] = general;
}

static std::shared_ptr<spdlog::logger> GetLogger(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(loggersMutex);

    auto it = loggers.find(componentName);
    if (it != loggers.end()) {
        return it->second;
    }

    // Если логгер для компонента не найден, возвращаем fallback-логгер (OutputDebugString)
    return GetFallbackLogger();
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
            
            // Логируем информацию об обновлении уровня логирования
            it->second->info("Уровень логирования обновлен для компонента {}, новый уровень: {}, путь: {}",
                          componentName, static_cast<int>(level), filePath);

            // Оновлюємо прив'язку neutral-каналу "General" (рівень міг змінитись)
            BindGeneralToLocked(it->second);
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

        // info+ (Connect/Disconnect/помилки) — скидати на диск ОДРАЗУ: інакше хвіст
        // лога сидить у буфері spdlog, і файл, скопійований при живому процесі 1С,
        // бреше про останні події (інцидент 2026-08-29 — «Отключить без сліду»).
        // trace/debug (wire-дамп, байти) лишаються буферизованими до наступного
        // info+ або Shutdown — спільний sink скидає їх разом.
        logger->flush_on(spdlog::level::info);

        // Настраиваем форматирование сообщений
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        
        // Сохраняем логгер в карту
        loggers[componentName] = logger;
        
        // Обновляем настройки компонента
        ComponentLogSettings settings(filePath, level, true);
        componentLogSettings[componentName] = settings;
        
        // Логируем информацию об инициализации логирования
        logger->info("Логирование инициализировано для компонента {}, уровень: {}, путь: {}",
                  componentName, static_cast<int>(level), filePath);

        // Neutral-репорти (NEUTRAL_REPORT_*) — у той самий файл
        BindGeneralToLocked(logger);
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
            // Логируем напрямую через объект логгера (НЕ через Info(): тот снова
            // берёт loggersMutex внутри GetLogger — это был дедлок)
            it->second->info("Завершение работы логгера для компонента " + componentName);
            it->second->flush();
            loggers.erase(it);
            // Прибираємо ім'я і з глобального реєстру spdlog: інакше повторний
            // InitLogging того ж імені (нова інстанція того ж типу компоненти в
            // одній сесії 1С) кине spdlog_ex "already exists" і файлове логування
            // зламається назавжди. spdlog::drop — no-op, якщо імені вже немає.
            spdlog::drop(componentName);

            // Также удаляем настройки компонента
            auto settingsIt = componentLogSettings.find(componentName);
            if (settingsIt != componentLogSettings.end()) {
                componentLogSettings.erase(settingsIt);
            }

            // "General" міг ділити sink із логером, що закривається: переприв'язуємо
            // до будь-якого живого компонентного логера, а якщо їх не лишилось —
            // прибираємо (інакше він тримав би файл відкритим після Shutdown).
            std::shared_ptr<spdlog::logger> donor;
            for (const auto& kv : loggers) {
                if (kv.first != "General") { donor = kv.second; break; }
            }
            if (donor) {
                BindGeneralToLocked(donor);
            } else {
                loggers.erase("General");
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
    
    try {
        // Определяем название компонента
        std::string componentName = GetComponentName(component);
        
        // Преобразуем строковый уровень логирования в перечисление
        LogLevel level = StringToLogLevel(logLevel);
        
        // Проверяем корректность настроек логирования
        if (level != LogLevel::Off && logFilePath.empty()) {
            // Если уровень не Off, а путь к файлу пуст - это ошибка
            std::cerr << "Ошибка включения логирования: уровень не Off, но путь к файлу пуст" << std::endl;
            return false;
        }
        
        // Инициализируем логирование - метод сам заботится о блокировках
        bool result = InitLogging(componentName, level, logFilePath);
        
        return result;
    }
    catch (const std::exception& ex) {
        // Выводим информацию об ошибке без использования логирования
        // чтобы избежать рекурсии в случае проблем с логированием
        std::cerr << "Исключение при активации логирования: " << ex.what() << std::endl;
        return false;
    }
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
    
    // Получаем имя компонента без блокировки мьютекса
    std::string componentName = GetComponentName(component);
    
    // Создаем локальную копию настроек под блокировкой для минимизации времени блокировки
    ComponentLogSettings settings;
    bool hasSettings = false;
    
    {
        std::lock_guard<std::mutex> lock(loggersMutex);
        
        // Проверяем, есть ли настройки для данного компонента
        auto it = componentLogSettings.find(componentName);
        if (it != componentLogSettings.end()) {
            settings = it->second;
            hasSettings = true;
        }
    }
    
    if (!hasSettings || !settings.isEnabled) {
        return false;
    }
    
    // Преобразуем строковый уровень логирования в перечисление
    // Эту операцию выполняем без блокировки
    LogLevel requestedLevel = StringToLogLevel(level);
    LogLevel configuredLevel = settings.logLevel;
    
    // Логика сравнения: 
    // Trace(5) > Debug(4) > Info(3) > Warn(2) > Error(1) > Off(0)
    // Если настроенный уровень >= запрашиваемого, то разрешаем логирование
    return static_cast<int>(configuredLevel) >= static_cast<int>(requestedLevel);
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
    
    // Преобразуем строковый уровень в нижний регистр для сравнения
    std::string lowerLevel = level;
    std::transform(lowerLevel.begin(), lowerLevel.end(), lowerLevel.begin(), 
                  [](unsigned char c) { return std::tolower(c); });
    
    // Форматируем сообщение с добавлением имени компонента в квадратных скобках
    std::string formattedMsg = message;
    
    // Записываем сообщение с соответствующим уровнем
    if (lowerLevel == "trace") {
        logger->trace(formattedMsg);
    } else if (lowerLevel == "debug") {
        logger->debug(formattedMsg);
    } else if (lowerLevel == "info") {
        logger->info(formattedMsg);
    } else if (lowerLevel == "warn" || lowerLevel == "warning") {
        logger->warn(formattedMsg);
    } else if (lowerLevel == "error" || lowerLevel == "err") {
        logger->error(formattedMsg);
    } else {
        // Неизвестный уровень, используем info по умолчанию
        logger->info(formattedMsg);
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
