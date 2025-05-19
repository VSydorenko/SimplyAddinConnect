#pragma once

#include <string>
#include <memory>
#include <functional>
#include <cstdint>
#include <typeinfo>
#include <map>
#include <mutex>
#include <winerror.h> // Для константы E_FAIL
#include "../core/AddInNative.h" // Для типов из AddInNative

// Предварительное объявление класса для использования в параметрах
class AddInNative;

/**
 * @namespace ServiceTools
 * @brief Пространство имен для централизованных сервисных инструментов
 * 
 * Содержит функции для работы с логированием и обработкой ошибок, которые могут использоваться
 * различными компонентами проекта. Предоставляет единый интерфейс для логирования с разными уровнями
 * детализации и обработки ошибок Windows.
 */
namespace ServiceTools {

/**
 * @enum LogLevel
 * @brief Уровни логирования для системы логирования
 * 
 * Определяет различные уровни детализации для логирования, от полного выключения (Off) 
 * до наиболее детального уровня (Trace).
 */
enum class LogLevel {
    Off,    ///< Логирование выключено
    Error,  ///< Только критические ошибки
    Warn,   ///< Предупреждения и ошибки
    Info,   ///< Информационные сообщения, предупреждения и ошибки
    Debug,  ///< Отладочные сообщения и все вышеупомянутое
    Trace   ///< Наиболее детальный уровень логирования
};

/**
 * @struct ComponentLogSettings
 * @brief Структура для хранения настроек логирования компонентов
 * 
 * Содержит информацию о пути к файлу логов и текущем уровне логирования для компонента
 */
struct ComponentLogSettings {
    std::string logFilePath;  ///< Путь к файлу логирования компонента
    LogLevel logLevel;        ///< Уровень логирования компонента
    bool isEnabled;           ///< Флаг включения/выключения логирования
    
    /**
     * @brief Конструктор с параметрами по умолчанию
     */
    ComponentLogSettings() : logFilePath(""), logLevel(LogLevel::Off), isEnabled(false) {}
    
    /**
     * @brief Конструктор с указанием параметров
     * 
     * @param path Путь к файлу логирования
     * @param level Уровень логирования
     * @param enabled Флаг включения/выключения логирования
     */
    ComponentLogSettings(const std::string& path, LogLevel level, bool enabled = false) 
        : logFilePath(path), logLevel(level), isEnabled(enabled) {}
};

/**
 * @brief Преобразование строкового представления уровня логирования в перечисление LogLevel
 * 
 * @param levelStr Строковое представление уровня логирования
 * @return LogLevel Соответствующий уровень логирования
 * 
 * @details Преобразует строковое представление уровня логирования в соответствующее 
 * значение перечисления LogLevel. Допустимые значения: "off", "error", "warn", "info", "debug", "trace".
 * Если строка не соответствует ни одному из известных уровней, возвращает LogLevel::Info.
 */
LogLevel StringToLogLevel(const std::string& levelStr);

// Объявления внешних переменных
extern std::mutex loggersMutex;
extern std::map<std::string, ComponentLogSettings> componentLogSettings;

//=================================================================================================
// Logging
//=================================================================================================

/**
 * @brief Инициализирует логирование для компонента
 * 
 * @param componentName Название компонента-владельца логгера
 * @param level Уровень логирования согласно LogLevel
 * @param filePath Путь к файлу лога
 * @return true, если логирование успешно инициализировано
 * @return false, если произошла ошибка
 * 
 * @details Создает логгер для компонента с указанными параметрами. Если логгер для этого
 * компонента уже существует, просто обновляет уровень логирования. Логирование осуществляется в файлы
 * с автоматической ротацией (5 файлов по 10 МБ).
 */
bool InitLogging(const std::string& componentName, LogLevel level, const std::string& filePath);

/**
 * @brief Завершает работу логгера для компонента
 * 
 * @param componentName Название компонента, логгер которого нужно закрыть
 * 
 * @details Завершает работу логгера и удаляет его из реестра активных логгеров.
 * Рекомендуется вызывать этот метод при завершении работы компонента.
 */
void ShutdownLogging(const std::string& componentName);

/**
 * @brief Включает логирование для компонента
 * 
 * @param component Указатель на компонент
 * @param logLevel Уровень логирования (строковое значение)
 * @param logFilePath Путь к файлу логов
 * @return true, если логирование успешно включено
 * @return false, если произошла ошибка
 * 
 * @details Включает и настраивает логирование для указанного компонента.
 * Автоматически определяет имя компонента и создает соответствующий логгер.
 * Если уровень логирования не "off", путь к файлу должен быть указан.
 */
bool EnableComponentLogging(AddInNative* component, const std::string& logLevel, const std::string& logFilePath);

/**
 * @brief Выключает логирование для компонента
 * 
 * @param component Указатель на компонент
 * @return true, если логирование успешно отключено
 * @return false, если произошла ошибка
 * 
 * @details Выключает логирование для указанного компонента и освобождает
 * связанные ресурсы. Имя компонента определяется автоматически.
 */
bool DisableComponentLogging(AddInNative* component);

/**
 * @brief Добавляет запись в лог компонента
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @param message Сообщение для записи в лог
 * @return true, если сообщение успешно записано
 * @return false, если произошла ошибка
 * 
 * @details Добавляет сообщение в лог компонента с указанным уровнем.
 * Автоматически добавляет имя компонента в квадратных скобках в начало сообщения.
 * Если логирование для компонента выключено, сообщение не будет записано.
 */
bool AddComponentLog(AddInNative* component, const std::string& level, const std::string& message);

/**
 * @brief Логирует информационное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 * 
 * @details Записывает в лог сообщение уровня Info.
 * Выводится, если текущий уровень логирования компонента >= Info.
 */
void Info(const std::string& componentName, const std::string& message);

/**
 * @brief Логирует сообщение об ошибке
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 * 
 * @details Записывает в лог сообщение уровня Error.
 * Выводится, если текущий уровень логирования компонента >= Error.
 */
void Error(const std::string& componentName, const std::string& message);

/**
 * @brief Логирует отладочное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 * 
 * @details Записывает в лог сообщение уровня Debug.
 * Выводится, если текущий уровень логирования компонента >= Debug.
 */
void Debug(const std::string& componentName, const std::string& message);

/**
 * @brief Логирует предупреждение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 * 
 * @details Записывает в лог сообщение уровня Warn.
 * Выводится, если текущий уровень логирования компонента >= Warn.
 */
void Warn(const std::string& componentName, const std::string& message);

/**
 * @brief Логирует трассировочное сообщение
 * 
 * @param componentName Название компонента
 * @param message Сообщение для записи в лог
 * 
 * @details Записывает в лог сообщение уровня Trace.
 * Выводится, если текущий уровень логирования компонента >= Trace.
 * Наиболее детальный уровень логирования для отслеживания всех операций.
 */
void Trace(const std::string& componentName, const std::string& message);

//=================================================================================================
// Error
//=================================================================================================

/**
 * @brief Добавляет ошибку в компонент AddInNative
 * 
 * @param component Указатель на объект компонента
 * @param description Описание ошибки в формате u16string
 * @param code Код ошибки (по умолчанию 0)
 * @return true, если ошибка успешно добавлена
 * @return false, если произошла ошибка при добавлении
 * 
 * @details Добавляет ошибку в компонент для дальнейшего отображения в 1С.
 * Кроме того, записывает ошибку в логи.
 */
bool AddComponentError(AddInNative* component, const std::u16string& description, int32_t code = 0);

/**
 * @brief Возвращает код последней ошибки Windows
 * 
 * @return uint32_t Код ошибки Windows
 * 
 * @details Обертка для функции Windows::GetLastError(), которая возвращает
 * код последней ошибки, произошедшей в текущем потоке.
 */
uint32_t GetLastError();

/**
 * @brief Возвращает текстовое описание ошибки по ее коду
 * 
 * @param errorCode Код ошибки Windows
 * @return std::wstring Текстовое описание ошибки
 * 
 * @details Конвертирует числовой код ошибки Windows в читаемое сообщение.
 * Убирает из сообщения символы завершения строки.
 */
std::wstring GetErrorDescription(uint32_t errorCode);

//=================================================================================================
// Conversion
//=================================================================================================

/**
 * @brief Безопасно конвертирует строку UTF-8 в строку UTF-16
 * 
 * @param str Входная строка в формате UTF-8
 * @return std::u16string Строка в формате UTF-16
 * 
 * @details Использует статический метод AddInNative::MB2WCHAR для конвертации,
 * обеспечивая правильное управление памятью через std::u16string.
 */
std::u16string SafeMB2WCHAR(const char* str);

/**
 * @brief Безопасно конвертирует строку UTF-16 в строку UTF-8
 * 
 * @param str Входная строка в формате UTF-16
 * @return std::string Строка в формате UTF-8
 * 
 * @details Использует статический метод AddInNative::WCHAR2MB для конвертации,
 * обеспечивая правильное управление памятью через std::string.
 */
std::string SafeWCHAR2MB(const std::u16string& str);

/**
 * @brief Конвертирует строку UTF-16 (std::u16string) в широкую строку (std::wstring)
 * 
 * @param str Входная строка в формате UTF-16
 * @return std::wstring Строка в формате wstring для использования с Windows API
 */
std::wstring U16StringToWString(const std::u16string& str);

/**
 * @brief Получает имя компонента по его указателю
 * 
 * @param component Указатель на компонент
 * @return std::string Имя компонента, готовое для использования в логах
 * 
 * @details Автоматически получает имя типа компонента с помощью RTTI и возвращает
 * его в формате, готовом для использования в логах. Если компонент недействителен или
 * тип не удалось определить, возвращает дефолтное имя.
 */
std::string GetComponentName(const AddInNative* component);

/**
 * @brief Проверяет, включено ли логирование указанного уровня для компонента
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @return true, если логирование данного уровня включено
 * @return false, если логирование данного уровня выключено или произошла ошибка
 * 
 * @details Проверяет, включено ли логирование указанного уровня для компонента.
 * Используется для предварительной проверки перед вызовом функций логирования.
 */
bool IsComponentLoggingEnabled(AddInNative* component, const std::string& level);

/**
 * @brief Записывает событие в лог компонента с указанным уровнем и именем метода
 * 
 * @param component Указатель на компонент
 * @param level Уровень логирования (строковое значение)
 * @param methodName Имя метода, в котором произошло событие
 * @param message Сообщение о событии
 * 
 * @details Записывает сообщение в лог компонента. Если уровень "error",
 * вызывает AddComponentError, в других случаях проверяет, разрешено ли
 * логирование данного уровня через IsComponentLoggingEnabled, и если разрешено,
 * вызывает AddComponentLog.
 */
void ReportComponentEvent(AddInNative* component, const std::string& level, const std::string& methodName, const std::string& message);

void NeutralReportImpl(const std::string& level, const std::string& method, const std::string& tag, const std::string& message);
void NeutralReportImpl(const std::string& level, const std::string& method, const std::string& message); // перегрузка без tag

} // namespace ServiceTools

// Макросы для упрощения логирования
#define REPORT_TRACE(msg) ServiceTools::ReportComponentEvent(this, "trace", __func__, msg)
#define REPORT_DEBUG(msg) ServiceTools::ReportComponentEvent(this, "debug", __func__, msg)
#define REPORT_INFO(msg)  ServiceTools::ReportComponentEvent(this, "info",  __func__, msg)
#define REPORT_WARN(msg)  ServiceTools::ReportComponentEvent(this, "warn",  __func__, msg)
#define REPORT_ERROR(msg) ServiceTools::ReportComponentEvent(this, "error", __func__, msg)

// Макросы для нейтрального логирования (без компонента)
// Если тег не указан — используется "General"
#define NEUTRAL_REPORT_TRACE(...)  ServiceTools::NeutralReportImpl("trace",  __func__, __VA_ARGS__)
#define NEUTRAL_REPORT_DEBUG(...)  ServiceTools::NeutralReportImpl("debug",  __func__, __VA_ARGS__)
#define NEUTRAL_REPORT_INFO(...)   ServiceTools::NeutralReportImpl("info",   __func__, __VA_ARGS__)
#define NEUTRAL_REPORT_WARN(...)   ServiceTools::NeutralReportImpl("warn",   __func__, __VA_ARGS__)
#define NEUTRAL_REPORT_ERROR(...)  ServiceTools::NeutralReportImpl("error",  __func__, __VA_ARGS__)
