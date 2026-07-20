#pragma once

#include "../core/AddInNative.h"
#include "../protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "../protocols/ECRPrivatJSON/ECRPrivatJSON_Types.h"
#include <string>
#include <vector>
#include <memory>

/**
 * @class AddinECRPrivatJSON
 * @brief Компонент для работы с эквайринговыми терминалами по протоколу ПриватБанка (JSON based)
 * 
 * Класс обеспечивает интеграцию с терминалами по протоколу ECR PrivatBank JSON based v1.0.3.1
 */
class AddinECRPrivatJSON : public AddInNative {
public:
    static std::vector<std::u16string> names;
    AddinECRPrivatJSON();
    virtual ~AddinECRPrivatJSON();
    
    /**
     * @brief Получение структуры ответа последней операции
     * @return Структура с данными ответа
     */
    ECRPrivatJSON::TerminalResponse GetLastTerminalResponse() const;

    /**
     * @brief Проверяет успешность кода ответа терминала
     * 
     * @param responseCode Код ответа терминала
     * @return bool Результат проверки (true - успешный код)
     */
    bool IsSuccessCode(const std::string& responseCode) const;

private:
    /**
     * @brief Определение типа транспорта на основе строки подключения
     * @param connectionString Строка подключения (COM порт, TCP адрес, WebSocket URL)
     * @return Тип транспорта
     */
    std::string DetermineTransportType(const std::string& connectionString) const;
    
    // Метод для регистрации методов компонента
    void RegisterMethods();
    
    // Протокол ECR Privat JSON
    std::unique_ptr<ECRPrivatJSON::ECRPrivatJSONProtocol> protocol_;
    
    // Последний ответ терминала в виде структуры
    ECRPrivatJSON::TerminalResponse lastTerminalResponse_;
};
