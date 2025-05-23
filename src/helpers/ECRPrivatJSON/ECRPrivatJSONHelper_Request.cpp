#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// Формирование JSON-запроса
std::string ECRPrivatJSONHelper::BuildRequest(const std::string& method, const std::map<std::string, std::string>& params) {
    // Создаём JSON объект с использованием nlohmann/json
    NEUTRAL_REPORT_DEBUG(componentName_, "Формирование JSON-запроса для метода: " + method);
    
    try {
        json j;
        
        // Добавляем основные поля
        j["method"] = method;
        j["step"] = 0;
    
        // Если есть параметры, добавляем их в объект params
        if (!params.empty()) {
            // Создаём вложенный объект params
            json params_obj;
            
            // Добавляем все параметры
            for (const auto& param : params) {
                params_obj[param.first] = param.second;
            }
            
            // Добавляем объект params в корневой объект
            j["params"] = params_obj;
        }    
        // Сериализуем JSON в строку
        return j.dump();
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка формирования JSON-запроса: " + std::string(e.what()));
        return "";
    }
}

// Добавление нулевых разделителей к JSON согласно протоколу
std::string ECRPrivatJSONHelper::AddNullTerminator(const std::string& json, bool addLeadingNull) {
    try {
        NEUTRAL_REPORT_DEBUG(componentName_, "Добавление нулевых разделителей к JSON");
        
        std::string result;
        
        // Добавляем начальный нулевой байт, если требуется (для хендшейка)
        if (addLeadingNull) {
            result.push_back('\0');
        }
        
        // Добавляем JSON-строку
        result.append(json);
        
        // Добавляем конечный нулевой байт
        result.push_back('\0');
        
        return result;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при добавлении нулевых разделителей: " + std::string(e.what()));
        // Возвращаем исходную строку в случае ошибки
        return json;
    }
}

// Отправка запроса и получение ответа
std::string ECRPrivatJSONHelper::SendReceive(const std::string& request, int timeout) {
    if (!transport_) {
        NEUTRAL_REPORT_ERROR(componentName_, "Не установлен транспортный объект");
        throw std::runtime_error("Транспортный объект не установлен");
    }
    
    if (!transport_->IsOpen()) {
        NEUTRAL_REPORT_ERROR(componentName_, "Транспортное соединение не открыто");
        throw std::runtime_error("Транспортное соединение не открыто");
    }
    
    NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса: " + request);
    
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
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки данных");
            throw std::runtime_error("Ошибка отправки данных");
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Запрос отправлен, ожидание ответа (таймаут: " + std::to_string(timeout) + " мс)");
        
        // Ждем ответа
        bool received = WaitForResponse(timeout);
        
        if (!received) {
            NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа");
            throw std::runtime_error("Таймаут ожидания ответа");
        }
        
        // Получаем накопленный ответ
        std::string response = GetResponse();
        
        // Сбрасываем состояние ожидания ответа
        ResetResponseState();
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ: " + response);
        
        return response;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при выполнении запроса: " + std::string(e.what()));
        throw;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при выполнении запроса");
        throw std::runtime_error("Неизвестная ошибка при выполнении запроса");
    }
}

} // namespace ECRPrivatJSON
