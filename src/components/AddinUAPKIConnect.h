// filepath: r:\github\SimplyAddinConnect\src\components\AddinUAPKIConnect.h
#pragma once

#include "../core/AddInNative.h"
#include <string>
#include <vector>

/**
 * @class AddinUAPKIConnect
 * @brief Компонент для работы с библиотекой UAPKI из 1С
 * 
 * Класс предоставляет интерфейс для вызова методов UAPKI через единую точку входа.
 * Обрабатывает параметры в формате ключ=значение и преобразует их в JSON-запросы к библиотеке UAPKI.
 */
class AddinUAPKIConnect : public AddInNative {
public:
    static std::vector<std::u16string> names;
    AddinUAPKIConnect();
    virtual ~AddinUAPKIConnect();
    
    /**
     * @brief Вызывает метод UAPKI и обрабатывает результат
     * 
     * @param method Название метода UAPKI
     * @param paramsString Параметры в формате "ключ=значение,ключ=значение"
     * @param jsonResponse Строка для записи ответа в формате JSON
     * @return true если команда выполнена успешно
     * @return false в случае ошибки
     */
    bool CallUapki(const std::string& method, const std::string& paramsString, std::string& jsonResponse);
    
    /**
     * @brief Включить логирование для компонента
     * 
     * @param logLevel Уровень логирования
     * @param logFilePath Путь к файлу лога
     * @return true если логирование успешно включено
     * @return false в случае ошибки
     */
    bool EnableLogging(const std::string& logLevel, const std::string& logFilePath);

private:
    // Метод для регистрации методов компонента
    void RegisterMethods();
};