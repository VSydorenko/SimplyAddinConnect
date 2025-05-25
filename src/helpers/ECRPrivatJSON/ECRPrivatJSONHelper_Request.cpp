#include "../../core/pch.h"
#include "ECRPrivatJSONHelper.h"
#include "../ServiceTools.h"

namespace ECRPrivatJSON {

// Формирование JSON-запроса
std::string ECRPrivatJSONHelper::BuildRequest(const std::string& method, const std::map<std::string, std::string>& params, bool isHandshake) {
    // Создаём JSON объект с использованием nlohmann/json
    NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", isHandshake ? 
        "Формирование JSON-запроса для метода: " + method + " (хендшейк)" : 
        "Формирование JSON-запроса для метода: " + method);
    
    try {
        json requestJson;
        
        // Добавляем основные поля
        requestJson["method"] = method;
        requestJson["step"] = 0;
    
        // Если есть параметры, добавляем их в объект params
        if (!params.empty()) {
            // Создаём вложенный объект params
            json params_obj;
            
            // Добавляем все параметры
            for (const auto& param : params) {
                params_obj[param.first] = param.second;
                NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Добавлен параметр: " + param.first);
            }
            
            // Добавляем объект params в корневой объект
            requestJson["params"] = params_obj;
        } else {
            NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Запрос без дополнительных параметров");
        }
        
        // Сериализуем JSON в строку
        std::string jsonString = requestJson.dump();
        NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "JSON сформирован, размер: " + std::to_string(jsonString.length()) + " байт");
        
        // Добавляем нулевые терминаторы в соответствии с протоколом
        return AddNullTerminator(jsonString, isHandshake);
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка парсинга JSON: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка формирования JSON-запроса: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при формировании JSON-запроса");
        return "";
    }
}

// Добавление нулевых разделителей к JSON согласно протоколу
std::string ECRPrivatJSONHelper::AddNullTerminator(const std::string& json, bool isHandshake) {
    try {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", isHandshake ? 
            "Добавление нулевых разделителей к JSON (для хендшейка)" : 
            "Добавление нулевых разделителей к JSON");
        
        if (json.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получена пустая JSON строка для обработки");
            return json;
        }
        
        // Создаем вектор для результата
        std::vector<uint8_t> result;
        
        // Для хендшейка добавляем начальный нулевой байт
        if (isHandshake) {
            result.push_back(0);
            NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Добавлен начальный нулевой байт для хендшейка");
        }
        
        // Добавляем JSON-данные
        result.insert(result.end(), json.begin(), json.end());
        
        // Добавляем завершающий нулевой байт
        result.push_back(0);
        
        NEUTRAL_REPORT_TRACE("ECRPrivatJSONHelper", "Завершающий нулевой байт добавлен");
        
        return std::string(result.begin(), result.end());
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при добавлении нулевых разделителей: " + std::string(e.what()));
        // Возвращаем исходную строку в случае ошибки
        return json;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при добавлении нулевых разделителей");
        return json;
    }
}

// Отправка запроса и получение ответа
std::string ECRPrivatJSONHelper::SendReceive(const std::string& request, int timeout) {
    if (!transport_) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Не установлен транспортный объект");
        return "";
    }
    
    if (!transport_->IsOpen()) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Транспортное соединение не открыто");
        return "";
    }
    
    NEUTRAL_REPORT_INFO("ECRPrivatJSONHelper", "Начинается отправка запроса (размер: " + std::to_string(request.length()) + " байт)");
    NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Отправка запроса: " + request);
    
    try {
        // Преобразуем строку запроса в вектор байтов
        std::vector<uint8_t> requestData(request.begin(), request.end());
        
        // Очищаем буфер данных перед отправкой
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            dataBuffer_.clear();
            responseReceived_ = false;
        }
        // Отправляем запрос
        int bytesSent = transport_->Send(requestData);
        
        if (bytesSent <= 0) {
            NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка отправки данных (отправлено байт: " + std::to_string(bytesSent) + ")");
            return "";
        }
        
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Запрос отправлен успешно (" + std::to_string(bytesSent) + " байт), ожидание ответа (таймаут: " + std::to_string(timeout) + " мс)");
        
        // Ждем ответа
        bool received = WaitForResponse(timeout);
        
        if (!received) {
            NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Таймаут ожидания ответа (истекло " + std::to_string(timeout) + " мс)");
            return "";
        }
        
        NEUTRAL_REPORT_INFO("ECRPrivatJSONHelper", "Ответ получен вовремя");
          // Получаем накопленный ответ
        std::string response = GetResponse();
        
        if (response.empty()) {
            NEUTRAL_REPORT_WARN("ECRPrivatJSONHelper", "Получен пустой ответ");
        }
        
        // Сбрасываем состояние ожидания ответа
        ResetResponseState();
        
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSONHelper", "Получен ответ: " + response);
        NEUTRAL_REPORT_INFO("ECRPrivatJSONHelper", "Запрос-ответ завершён успешно");
        
        return response;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Ошибка при выполнении запроса: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSONHelper", "Неизвестная ошибка при выполнении запроса");
        return "";
    }
}

} // namespace ECRPrivatJSON
