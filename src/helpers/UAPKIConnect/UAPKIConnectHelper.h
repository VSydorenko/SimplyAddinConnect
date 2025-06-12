#pragma once

#include <string>
#include <nlohmann/json.hpp>

/**
 * @class UAPKIConnectHelper
 * @brief Вспомогательный класс для работы с библиотекой UAPKI
 * 
 * Класс предоставляет интерфейс для выполнения команд UAPKI через JSON-запросы.
 * Обрабатывает параметры в формате "ключ=значение" и преобразует их в JSON для запросов к UAPKI.
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
     * @brief Разбирает строку параметров в формате "ключ=значение,ключ=значение" в JSON объект
     * 
     * @param paramsString Строка параметров в формате "ключ=значение,ключ=значение"
     * @param paramsJson Объект JSON для записи параметров
     * @return true если разбор выполнен успешно
     * @return false в случае ошибки формата
     */
    static bool ParseParamsString(const std::string& paramsString, nlohmann::json& paramsJson);
};