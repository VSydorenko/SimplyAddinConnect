#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>

namespace ECRPrivatJSON {

// Форматирование денежной суммы
std::string ECRPrivatJSONHelper::FormatAmount(double amount, int precision) const {
    // Проверка диапазона суммы
    if (amount < 0) {
        NEUTRAL_REPORT_WARN(componentName_, "Отрицательная сумма: " + std::to_string(amount) + ", будет заменена на 0");
        amount = 0;
    }
    
    if (amount > 999999.99) {
        NEUTRAL_REPORT_WARN(componentName_, "Слишком большая сумма: " + std::to_string(amount) + ", будет ограничена до 999999.99");
        amount = 999999.99;
    }
    
    // Форматируем сумму с нужной точностью
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << amount;
    std::string result = ss.str();
    
    // Проверяем, не получилось ли представление в научной нотации
    if (result.find('e') != std::string::npos || result.find('E') != std::string::npos) {
        // Если да, то форматируем вручную
        int64_t intPart = static_cast<int64_t>(amount);
        double fracPart = amount - intPart;
        
        // Фракционную часть умножаем на 10^precision и округляем
        int64_t fracDigits = static_cast<int64_t>(fracPart * std::pow(10, precision) + 0.5);
        
        // Собираем строку
        result = std::to_string(intPart) + ".";
        std::string fracStr = std::to_string(fracDigits);
        
        // Добавляем ведущие нули, если нужно
        while (fracStr.length() < static_cast<size_t>(precision)) {
            fracStr = "0" + fracStr;
        }
        
        // Обрезаем до нужной точности
        result += fracStr.substr(0, precision);
    }
    
    return result;
}

// Извлечение значения по ключу из JSON
std::string ECRPrivatJSONHelper::ExtractValueByKey(const std::string& jsonResponse, 
                                                  const std::string& key, 
                                                  const std::string& defaultValue) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        // Прямой доступ к корневым полям
        if (j.contains(key)) {
            if (j[key].is_string()) {
                return j[key].get<std::string>();
            } else {
                return j[key].dump();
            }
        }
        
        // Проверка в поле params
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            if (params.contains(key)) {
                if (params[key].is_string()) {
                    return params[key].get<std::string>();
                } else {
                    return params[key].dump();
                }
            }
        }
        
        // Если не найдено, возвращаем значение по умолчанию
        return defaultValue;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        return defaultValue;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при извлечении значения по ключу: " + std::string(e.what()));
        return defaultValue;
    }
}

} // namespace ECRPrivatJSON
