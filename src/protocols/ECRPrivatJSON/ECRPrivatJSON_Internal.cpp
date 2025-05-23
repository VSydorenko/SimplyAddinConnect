#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>

namespace ECRPrivatJSON {

// ===========================
// Обработчики событий и внутренние методы
// ===========================

void ECRPrivatJSONProtocol::OnDataReceived(const std::vector<uint8_t>& data) {
    try {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        
        // Добавляем полученные данные в буфер
        dataBuffer_.insert(dataBuffer_.end(), data.begin(), data.end());
        
        // Проверяем, есть ли нулевой терминатор
        auto nullTerminator = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
        
        while (nullTerminator != dataBuffer_.end()) {
            // Вычисляем размер JSON-сообщения
            size_t jsonSize = std::distance(dataBuffer_.begin(), nullTerminator);
            
            // Создаем строку с JSON-ответом
            std::string jsonResponse(dataBuffer_.begin(), dataBuffer_.begin() + jsonSize);
            
            // Проверяем, ожидалось ли сообщение
            if (waitingForResponse_) {
                // Сохраняем ответ
                receivedResponse_ = jsonResponse;
                responseReceived_ = true;
                waitingForResponse_ = false;
                
                // Уведомляем ожидающий поток
                dataCondition_.notify_all();
            } else {
                // Неожиданное сообщение (возможно, инициатива от терминала)
                NEUTRAL_REPORT_INFO(componentName_, "Получено неожиданное сообщение от терминала: " + jsonResponse);
                
                // TODO: Обработка инициативных сообщений терминала
            }
            
            // Удаляем обработанное сообщение из буфера (включая нулевой терминатор)
            dataBuffer_.erase(dataBuffer_.begin(), dataBuffer_.begin() + jsonSize + 1);
            
            // Ищем следующий нулевой терминатор
            nullTerminator = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
        }
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при обработке полученных данных: " + std::string(e.what()));
    } catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при обработке полученных данных");
    }
}

void ECRPrivatJSONProtocol::OnError(const std::string& errorMessage, int errorCode) {
    NEUTRAL_REPORT_ERROR(componentName_, "Ошибка транспортного уровня: " + errorMessage + " (код: " + std::to_string(errorCode) + ")");
}

void ECRPrivatJSONProtocol::OnConnectionStateChanged(bool connected) {
    try {
        if (connected != connected_) {
            connected_ = connected;
            
            if (connected) {
                NEUTRAL_REPORT_INFO(componentName_, "Соединение с терминалом установлено");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Соединение с терминалом разорвано");
            }
        }
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Исключение при обработке изменения состояния соединения: " + std::string(e.what()));
    } catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестное исключение при обработке изменения состояния соединения");
    }
}

} // namespace ECRPrivatJSON
