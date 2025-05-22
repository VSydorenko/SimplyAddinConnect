#pragma once

#include "../protocols/ECRPrivatJSON/ECRPrivatJSON_Types.h"
#include "../transport/Transport.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <mutex>
#include <condition_variable>

// Алиас для упрощения использования nlohmann::json
using json = nlohmann::json;

namespace SimplyConnect {

/**
 * @class ECRPrivatJSONHelper
 * @brief Вспомогательный класс для работы с протоколом ПриватБанк JSON based
 * 
 * Данный класс реализует низкоуровневую логику работы с JSON-форматом протокола ПриватБанк
 */
class ECRPrivatJSONHelper {
public:
    /**
     * @brief Конструктор
     * @param transport Указатель на объект транспортного слоя
     * @param componentName Имя компонента для логирования
     */
    explicit ECRPrivatJSONHelper(ITransport* transport, const std::string& componentName);
    
    /**
     * @brief Деструктор
     */
    ~ECRPrivatJSONHelper();

    /**
     * @brief Парсинг JSON-строки в объект json
     * @param jsonString JSON-строка для парсинга
     * @return Объект json с распарсенными данными
     */
    json ParseJSON(const std::string& jsonString);

    /**
     * @brief Формирование JSON-запроса
     * @param method Название метода
     * @param params Карта параметров метода
     * @return Строка с JSON-запросом
     */
    std::string BuildRequest(const std::string& method, const std::map<std::string, std::string>& params = {});
    
    /**
     * @brief Отправка запроса и получение ответа
     * @param request Строка с запросом
     * @param timeout Таймаут ожидания ответа в миллисекундах
     * @return Ответ от терминала
     */
    std::string SendReceive(const std::string& request, int timeout = 30000);
    
    /**
     * @brief Добавление нулевого терминатора к JSON
     * @param json Строка с JSON
     * @param addLeadingNull Добавлять ли начальный нулевой терминатор (для хендшейка)
     * @return Строка с добавленным нулевым терминатором
     */
    std::string AddNullTerminator(const std::string& json, bool addLeadingNull = false);
    
    /**
     * @brief Обработка полученных данных
     * @param data Полученные данные
     */
    void ProcessReceivedData(const std::vector<uint8_t>& data);
    
    /**
     * @brief Ожидание получения ответа
     * @param timeout Таймаут ожидания в миллисекундах
     * @return true, если ответ получен до истечения таймаута
     */
    bool WaitForResponse(int timeout);
    
    /**
     * @brief Получение накопленного ответа
     * @return Строка с ответом
     */
    std::string GetResponse();
    
    /**
     * @brief Сброс состояния ожидания ответа
     */
    void ResetResponseState();

    /**
     * @brief Преобразование типа операции в строку
     * @param opType Тип операции
     * @return Строковое представление типа операции
     */
    std::string OperationTypeToString(OperationType opType) const;

    /**
     * @brief Преобразование типа служебного сообщения в строку
     * @param msgType Тип служебного сообщения
     * @return Строковое представление типа служебного сообщения
     */
    std::string ServiceMessageTypeToString(ServiceMessageType msgType) const;

    /**
     * @brief Форматирование денежной суммы
     * @param amount Сумма
     * @param precision Количество знаков после запятой
     * @return Отформатированная строка с суммой
     */
    std::string FormatAmount(double amount, int precision = 2) const;

    /**
     * @brief Извлечение значения по ключу из JSON
     * @param jsonResponse JSON-строка с ответом
     * @param key Ключ для извлечения
     * @param defaultValue Значение по умолчанию
     * @return Извлеченное значение или значение по умолчанию
     */
    std::string ExtractValueByKey(const std::string& jsonResponse, const std::string& key, const std::string& defaultValue = "") const;

    /**
     * @brief Проверка успешности выполнения операции
     * @param jsonResponse JSON-строка с ответом
     * @return true, если операция выполнена успешно
     */
    bool IsSuccess(const std::string& jsonResponse) const;

    /**
     * @brief Парсинг ответа терминала в структуру TerminalResponse
     * @param jsonResponse JSON-строка с ответом
     * @param response Структура для заполнения
     * @return true, если парсинг успешен
     */
    bool ParseTerminalResponse(const std::string& jsonResponse, TerminalResponse& response) const;

    /**
     * @brief Установка транспортного объекта
     * @param transport Указатель на транспортный объект
     */
    void SetTransport(ITransport* transport);

private:
    /**
     * @brief Поиск нулевого терминатора в буфере
     * @return Индекс нулевого терминатора или -1, если не найден
     */
    int FindNullTerminator() const;
    
    // Указатель на транспортный слой
    ITransport* transport_;
    
    // Имя компонента для логирования
    std::string componentName_;
    
    // Буфер для накопления данных
    std::vector<uint8_t> dataBuffer_;
    
    // Мьютекс для синхронизации доступа к буферу данных
    std::mutex bufferMutex_;
    
    // Условная переменная для уведомления о получении данных
    std::condition_variable dataCondition_;
    
    // Флаг, указывающий, что ответ получен
    bool responseReceived_;
    
    // Полученный ответ
    std::string response_;
};

} // namespace SimplyConnect
