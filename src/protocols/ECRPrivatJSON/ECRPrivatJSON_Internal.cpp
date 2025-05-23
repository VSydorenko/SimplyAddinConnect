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
            if (parentComponent_) {
                REPORT_INFO("Получено неожиданное сообщение от терминала: " + jsonResponse);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Получено неожиданное сообщение от терминала: " + jsonResponse);
            }
            
            // TODO: Обработка инициативных сообщений терминала
        }
        
        // Удаляем обработанное сообщение из буфера (включая нулевой терминатор)
        dataBuffer_.erase(dataBuffer_.begin(), dataBuffer_.begin() + jsonSize + 1);
        
        // Ищем следующий нулевой терминатор
        nullTerminator = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
    }
}

void ECRPrivatJSONProtocol::OnError(const std::string& errorMessage, int errorCode) {
    if (parentComponent_) {
        REPORT_ERROR("Ошибка транспортного уровня: " + errorMessage + " (код: " + std::to_string(errorCode) + ")");
    } else {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка транспортного уровня: " + errorMessage + " (код: " + std::to_string(errorCode) + ")");
    }
}

void ECRPrivatJSONProtocol::OnConnectionStateChanged(bool connected) {
    if (connected != connected_) {
        connected_ = connected;
        
        if (parentComponent_) {
            if (connected) {
                REPORT_INFO("Соединение с терминалом установлено");
            } else {
                REPORT_INFO("Соединение с терминалом разорвано");
            }
        } else {
            if (connected) {
                NEUTRAL_REPORT_INFO(componentName_, "Соединение с терминалом установлено");
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Соединение с терминалом разорвано");
            }
        }
    }
}

bool ECRPrivatJSONProtocol::PerformHandshake() {
    if (!transport_ || !transport_->IsOpen()) {
        if (parentComponent_) {
            REPORT_ERROR("Невозможно выполнить хендшейк: транспорт не инициализирован или не открыт");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Невозможно выполнить хендшейк: транспорт не инициализирован или не открыт");
        }
        return false;
    }
    
    try {
        // Формирование запроса хендшейка с дополнительным нулевым байтом в начале
        std::string request = helper_->BuildRequest("PingDevice", {}, true); // true для обозначения хендшейка
        
        // Отправка с ожиданием ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Очищаем буфер
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            dataBuffer_.clear();
            receivedResponse_.clear();
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса хендшейка: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса хендшейка");
        }
        
        // Отправляем запрос
        int bytesSent = transport_->Send(std::vector<uint8_t>(request.begin(), request.end()));
        
        if (bytesSent <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Не удалось отправить запрос хендшейка");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось отправить запрос хендшейка");
            }
            return false;
        }
        
        // Ждем ответа
        {
            std::unique_lock<std::mutex> lock(bufferMutex_);
            bool received = dataCondition_.wait_for(lock, std::chrono::seconds(5), [this] {
                return responseReceived_;
            });
            
            if (!received) {
                if (parentComponent_) {
                    REPORT_ERROR("Timeout ожидания ответа на хендшейк");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Timeout ожидания ответа на хендшейк");
                }
                waitingForResponse_ = false;
                return false;
            }
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на хендшейк: " + receivedResponse_);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на хендшейк: " + receivedResponse_);
        }
        
        // Проверяем, что ответ корректный
        try {
            json response = helper_->ParseJSON(receivedResponse_);
            
            if (response.contains("method") && response["method"] == "PingDevice") {
                return true;
            } else {
                if (parentComponent_) {
                    REPORT_ERROR("Некорректный ответ на хендшейк");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Некорректный ответ на хендшейк");
                }
                return false;
            }
        }
        catch (const std::exception& e) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка при обработке ответа на хендшейк: " + std::string(e.what()));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке ответа на хендшейк: " + std::string(e.what()));
            }
            return false;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при выполнении хендшейка: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при выполнении хендшейка: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при выполнении хендшейка");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при выполнении хендшейка");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::IdentifyTerminal(std::string& terminalInfo) {
    if (!transport_ || !transport_->IsOpen()) {
        if (parentComponent_) {
            REPORT_ERROR("Невозможно выполнить идентификацию: транспорт не инициализирован или не открыт");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Невозможно выполнить идентификацию: транспорт не инициализирован или не открыт");
        }
        return false;
    }
    
    try {
        // Формирование запроса идентификации
        std::string request = helper_->BuildRequest("GetTerminalInfo", {}, false);
        
        // Отправка с ожиданием ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Очищаем буфер
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            dataBuffer_.clear();
            receivedResponse_.clear();
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса идентификации: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса идентификации");
        }
        
        // Отправляем запрос
        int bytesSent = transport_->Send(std::vector<uint8_t>(request.begin(), request.end()));
        
        if (bytesSent <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Не удалось отправить запрос идентификации");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось отправить запрос идентификации");
            }
            return false;
        }
        
        // Ждем ответа
        {
            std::unique_lock<std::mutex> lock(bufferMutex_);
            bool received = dataCondition_.wait_for(lock, std::chrono::seconds(5), [this] {
                return responseReceived_;
            });
            
            if (!received) {
                if (parentComponent_) {
                    REPORT_ERROR("Timeout ожидания ответа на запрос идентификации");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Timeout ожидания ответа на запрос идентификации");
                }
                waitingForResponse_ = false;
                return false;
            }
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос идентификации: " + receivedResponse_);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос идентификации: " + receivedResponse_);
        }
        
        // Проверяем, что ответ корректный
        try {
            json response = helper_->ParseJSON(receivedResponse_);
            
            if (response.contains("method") && response["method"] == "GetTerminalInfo") {
                if (response.contains("params")) {
                    auto params = response["params"];
                    
                    std::string vendor = params.value("vendor", "Unknown");
                    std::string model = params.value("model", "Unknown");
                    std::string serialNumber = params.value("serialNumber", "Unknown");
                    std::string firmware = params.value("firmware", "Unknown");
                    
                    terminalInfo = "Vendor: " + vendor + ", Model: " + model + 
                                  ", S/N: " + serialNumber + ", Firmware: " + firmware;
                    
                    return true;
                }
            }
            
            if (parentComponent_) {
                REPORT_ERROR("Некорректный ответ на запрос идентификации");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Некорректный ответ на запрос идентификации");
            }
            return false;
        }
        catch (const std::exception& e) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка при обработке ответа на запрос идентификации: " + std::string(e.what()));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке ответа на запрос идентификации: " + std::string(e.what()));
            }
            return false;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка при выполнении идентификации: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при выполнении идентификации: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка при выполнении идентификации");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при выполнении идентификации");
        }
        return false;
    }
}

} // namespace ECRPrivatJSON
