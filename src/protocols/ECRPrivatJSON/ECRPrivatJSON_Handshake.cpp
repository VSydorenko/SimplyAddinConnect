#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Методы для хендшейка и идентификации
// ===========================

bool ECRPrivatJSONProtocol::PerformHandshake() {
    NEUTRAL_REPORT_INFO(componentName_, "Выполнение хендшейка с терминалом");
    
    try {
        if (!transport_ || !transport_->IsOpen() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не инициализирован или не открыт");
            return false;
        }
        
        // Формируем запрос PingDevice с использованием хелпера
        std::map<std::string, std::string> params;
        
        std::string request;
        try {
            request = helper_->BuildRequest("PingDevice", params, true); // true для обозначения хендшейка
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка формирования хендшейк-запроса: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка хендшейк-запроса");
        
        // Преобразуем в вектор байтов для отправки
        std::vector<uint8_t> requestData(request.begin(), request.end());
        
        // Обнуляем флаги ожидания и получения ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Отправляем запрос
        try {
            if (transport_->Send(requestData) <= 0) {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки хендшейк-запроса");
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при отправке хендшейк-запроса: " + std::string(e.what()));
            return false;
        }
        
        // Ожидаем ответ от терминала
        if (helper_) {
            try {
                if (!helper_->WaitForResponse(5000)) { // 5 секунд таймаут для хендшейка
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на хендшейк-запрос");
                    return false;
                }
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_ERROR(componentName_, "Исключение при ожидании ответа на хендшейк: " + std::string(e.what()));
                return false;
            }
            
            // Получаем ответ
            std::string response = helper_->GetResponse();
            
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на хендшейк: " + response);
            
            // Парсим ответ и проверяем успешность
            bool success;
            try {
                success = helper_->IsSuccess(response);
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка проверки успешности хендшейка: " + std::string(e.what()));
                return false;
            }
            
            if (success) {
                NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешно выполнен");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка: неуспешный ответ");
            }
            
            // Сбрасываем состояние ответа
            try {
                helper_->ResetResponseState();
            } catch (...) {
                // Игнорируем ошибки при сбросе состояния
            }
            
            return success;
        }
        
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка выполнения хендшейка");
        return false;
    }
}

bool ECRPrivatJSONProtocol::IdentifyTerminal(std::string& terminalInfo) {
    NEUTRAL_REPORT_INFO(componentName_, "Идентификация терминала");
    
    try {
        if (!transport_ || !transport_->IsOpen() || !helper_) {
            NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не инициализирован или не открыт");
            return false;
        }
        
        // Формируем запрос ServiceMessage с типом identify
        std::map<std::string, std::string> params;
        params["msgType"] = "identify";
        
        // Использование хелпера для создания JSON запроса
        std::string request;
        try {
            request = helper_->BuildRequest("ServiceMessage", params);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при формировании запроса идентификации: " + std::string(e.what()));
            return false;
        }
        
        // Преобразуем в вектор байтов для отправки
        std::vector<uint8_t> requestData(request.begin(), request.end());
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса идентификации: " + request);
        
        // Обнуляем флаги ожидания и получения ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Отправляем запрос
        try {
            if (transport_->Send(requestData) <= 0) {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки запроса идентификации");
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при отправке запроса идентификации: " + std::string(e.what()));
            return false;
        }
        
        // Ожидаем ответ от терминала через хелпер или напрямую
        std::string response;
        
        if (helper_) {
            try {
                if (!helper_->WaitForResponse(5000)) {
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на запрос идентификации");
                    return false;
                }
                
                response = helper_->GetResponse();
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при получении ответа на запрос идентификации: " + std::string(e.what()));
                return false;
            }
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос идентификации: " + response);
        
        // Парсим ответ и извлекаем информацию о терминале
        try {
            json responseJson = json::parse(response);
            
            // Проверяем наличие информации о терминале
            if (responseJson.contains("params") && responseJson["params"].is_object()) {
                auto params = responseJson["params"];
                
                std::string terminalName;
                std::string terminalSerialNum;
                
                if (params.contains("terminalName")) {
                    terminalName = params["terminalName"].get<std::string>();
                }
                
                if (params.contains("terminalSerialNum")) {
                    terminalSerialNum = params["terminalSerialNum"].get<std::string>();
                }
                
                // Формируем строку с информацией о терминале
                terminalInfo = terminalName + " " + terminalSerialNum;
                
                if (!terminalInfo.empty()) {
                    NEUTRAL_REPORT_INFO(componentName_, "Идентификация успешно выполнена, терминал: " + terminalInfo);
                    
                    // Сбрасываем состояние ответа в хелпере
                    if (helper_) {
                        try {
                            helper_->ResetResponseState();
                        } catch (...) {
                            // Игнорируем ошибки при сбросе состояния
                        }
                    }
                    
                    return true;
                }
            }
            
            // Если не удалось извлечь информацию
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала: нет информации в ответе");
            
            // Сбрасываем состояние ответа в хелпере
            if (helper_) {
                try {
                    helper_->ResetResponseState();
                } catch (...) {
                    // Игнорируем ошибки при сбросе состояния
                }
            }
            
            return false;
        }
        catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос идентификации: " + std::string(e.what()));
            
            // Сбрасываем состояние ответа в хелпере
            if (helper_) {
                try {
                    helper_->ResetResponseState();
                } catch (...) {
                    // Игнорируем ошибки при сбросе состояния
                }
            }
            
            return false;
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка идентификации терминала");
        return false;
    }
}

std::string ECRPrivatJSONProtocol::GetTerminalInfo() {
    NEUTRAL_REPORT_INFO(componentName_, "Запрос информации о терминале");
    
    try {
        if (!IsConnected()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом");
            return "";
        }
        
        // Выполняем идентификацию терминала
        std::string terminalInfo;
        if (IdentifyTerminal(terminalInfo)) {
            return terminalInfo;
        } else {
            return "";
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения информации о терминале: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка получения информации о терминале");
        return "";
    }
}

} // namespace ECRPrivatJSON
