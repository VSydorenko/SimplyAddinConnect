#pragma once

#include <string>
#include <nlohmann/json.hpp>

/**
 * @class UAPKIConnectHelper
 * @brief Вспомогательный класс для работы с библиотекой UAPKI
 *
 * Класс предоставляет интерфейс для выполнения команд UAPKI через JSON-запросы.
 *
 * Параметры принимаются в двух форматах:
 *  - "сырой" JSON-объект (строка начинается с '{') — используется напрямую как объект
 *    parameters запроса (поддерживает вложенные структуры: cmProviders, сложные SIGN и т.п.);
 *  - плоский формат "ключ=значение,ключ=значение" — разбирается через ParseParamsString
 *    (только для простых плоских параметров, вложенность не поддерживается).
 *
 * Для метода INIT выполняется автоинъекция конфигурации провайдеров НКИ (каталог собственной
 * DLL + арх-суффикс имени провайдера) через InjectProviderConfig.
 *
 * Успешность операции определяется по полю errorCode ответа (0 = успех) согласно
 * протоколу UAPKI (docs/UAPKI_Protokol.md).
 */
class UAPKIConnectHelper {
public:
    /**
     * @brief Выполняет команду UAPKI
     * 
     * @param method Название метода UAPKI
     * @param paramsString Параметры в формате "ключ=значение,ключ=значение"
     * @param responseJson Строка для записи ответа в формате JSON
     * @return true если команда выполнена успешно
     * @return false в случае ошибки
     */
    static bool ExecuteUapkiCommand(const std::string& method, const std::string& paramsString, std::string& responseJson);

private:
    /**
     * @brief Проверяет успешность выполнения операции по JSON-ответу
     * 
     * @param jsonResponse Строка с JSON-ответом от UAPKI
     * @return true если операция выполнена успешно
     * @return false если операция завершилась с ошибкой
     */
    static bool IsOperationSuccess(const std::string& jsonResponse);

    /**
     * @brief Разбирает строку параметров в формате "ключ=значение,ключ=значение" в JSON объект
     * 
     * @param paramsString Строка параметров в формате "ключ=значение,ключ=значение"
     * @param paramsJson Объект JSON для записи параметров
     * @return true если разбор выполнен успешно
     * @return false в случае ошибки формата
     */
    static bool ParseParamsString(const std::string& paramsString, nlohmann::json& paramsJson);

    /**
     * @brief Автоинъекция конфигурации провайдеров НКИ в параметры метода INIT
     *
     * Подставляет каталог собственной DLL в cmProviders.dir (если он отсутствует/пустой)
     * и дописывает архитектурный суффикс (_x86/_x64) к именам провайдеров. Если cmProviders
     * отсутствует — формирует конфиг по умолчанию с провайдером cm-pkcs12. Пути кодируются
     * в UTF-8; при не-ASCII символах в пути используется короткий (8.3) путь.
     *
     * @param parameters JSON-объект параметров INIT (модифицируется на месте)
     * @return true при успехе; false если не удалось определить каталог DLL
     */
    static bool InjectProviderConfig(nlohmann::json& parameters);

    /**
     * @brief Определяет каталог собственной DLL (не хост-процесса 1cv8.exe)
     *
     * Использует GetModuleHandleExW(FROM_ADDRESS) с адресом внутри текущего модуля.
     * Возвращает каталог с завершающим слешем в кодировке UTF-8. При не-ASCII символах
     * в пути пытается получить ASCII-safe короткий путь через GetShortPathNameW.
     *
     * @param outUtf8Dir Выходной параметр: каталог DLL в UTF-8 (со слешем на конце)
     * @return true при успехе; false при ошибке Windows API или на не-Windows платформе
     */
    static bool GetOwnModuleDir(std::string& outUtf8Dir);

    /**
     * @brief Рекурсивно маскирует значения полей "password" на любом уровне JSON
     *
     * Используется для безопасного логирования конфигурации (маскирует копию, не оригинал).
     *
     * @param node Узел JSON (модифицируется на месте)
     */
    static void MaskPasswords(nlohmann::json& node);
};