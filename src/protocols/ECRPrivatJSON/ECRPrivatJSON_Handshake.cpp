#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace SimplyConnect {

// ===========================
// Методы для хендшейка и идентификации
// ===========================

bool ECRPrivatJSONProtocol::PerformHandshake() {
    if (parentComponent_) {
        REPORT_INFO("Выполнение хендшейка с терминалом");
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение хендшейка с терминалом");
    }
    
    try {
        if (!transport_ || !transport_->IsOpen() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Транспортный слой не инициализирован или не открыт");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не инициализирован или не открыт");
            }
            return false;
        }
        
        // Формируем запрос PingDevice
        std::map<std::string, std::string> params;
        std::string methodName = "PingDevice";
        
        // Создаем JSON запрос
        json requestJson;
        requestJson["method"] = methodName;
        requestJson["step"] = 0;
        
        // Добавляем параметры, если они есть
        if (!params.empty()) {
            json paramsJson;
            for (const auto& param : params) {
                paramsJson[param.first] = param.second;
            }
            requestJson["params"] = paramsJson;
        }
        
        // Преобразуем JSON в строку
        std::string request = requestJson.dump();
        
        // Добавляем нулевые терминаторы согласно протоколу
        // Для хендшейка нужен дополнительный нулевой терминатор в начале
        std::vector<uint8_t> requestData;
        requestData.push_back(0); // Начальный нулевой терминатор для хендшейка
        requestData.insert(requestData.end(), request.begin(), request.end());
        requestData.push_back(0); // Конечный нулевой терминатор
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка хендшейк-запроса: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка хендшейк-запроса: " + request);
        }
        
        // Обнуляем флаги ожидания и получения ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Отправляем запрос
        if (transport_->Send(requestData) <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка отправки хендшейк-запроса");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки хендшейк-запроса");
            }
            return false;
        }
        
        // Ожидаем ответ от терминала
        if (helper_) {
            if (!helper_->WaitForResponse(5000)) { // 5 секунд таймаут для хендшейка
                if (parentComponent_) {
                    REPORT_ERROR("Таймаут ожидания ответа на хендшейк-запрос");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на хендшейк-запрос");
                }
                return false;
            }
            
            // Получаем ответ
            std::string response = helper_->GetResponse();
            
            if (parentComponent_) {
                REPORT_DEBUG("Получен ответ на хендшейк: " + response);
            } else {
                NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на хендшейк: " + response);
            }
            
            // Парсим ответ и проверяем успешность
            bool success = helper_->IsSuccess(response);
            
            if (success) {
                if (parentComponent_) {
                    REPORT_INFO("Хендшейк успешно выполнен");
                } else {
                    NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешно выполнен");
                }
            } else {
                if (parentComponent_) {
                    REPORT_ERROR("Ошибка выполнения хендшейка: неуспешный ответ");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка: неуспешный ответ");
                }
            }
            
            // Сбрасываем состояние ответа
            helper_->ResetResponseState();
            
            return success;
        }
        else {
            // Если хелпер не инициализирован, ожидаем ответ самостоятельно
            std::unique_lock<std::mutex> lock(bufferMutex_);
            bool result = dataCondition_.wait_for(lock, std::chrono::milliseconds(5000),
                [this] { return responseReceived_; });
            
            if (!result) {
                if (parentComponent_) {
                    REPORT_ERROR("Таймаут ожидания ответа на хендшейк-запрос");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на хендшейк-запрос");
                }
                waitingForResponse_ = false;
                return false;
            }
            
            // Проверяем ответ
            if (parentComponent_) {
                REPORT_DEBUG("Получен ответ на хендшейк: " + receivedResponse_);
            } else {
                NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на хендшейк: " + receivedResponse_);
            }
            
            // Анализируем полученный ответ
            try {
                json responseJson = json::parse(receivedResponse_);
                bool success = false;
                
                // Проверяем наличие полей в ответе
                if (responseJson.contains("error")) {
                    success = !responseJson["error"].get<bool>();
                }
                
                if (responseJson.contains("params") && responseJson["params"].is_object() &&
                    responseJson["params"].contains("responseCode")) {
                    std::string responseCode = responseJson["params"]["responseCode"];
                    success = (responseCode == "0000" || responseCode == "00");
                }
                
                // Сбрасываем состояние ответа
                waitingForResponse_ = false;
                responseReceived_ = false;
                receivedResponse_.clear();
                
                if (success) {
                    if (parentComponent_) {
                        REPORT_INFO("Хендшейк успешно выполнен");
                    } else {
                        NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешно выполнен");
                    }
                } else {
                    if (parentComponent_) {
                        REPORT_ERROR("Ошибка выполнения хендшейка: неуспешный ответ");
                    } else {
                        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка: неуспешный ответ");
                    }
                }
                
                return success;
            }
            catch (const std::exception& e) {
                if (parentComponent_) {
                    REPORT_ERROR("Ошибка парсинга ответа на хендшейк: " + std::string(e.what()));
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на хендшейк: " + std::string(e.what()));
                }
                waitingForResponse_ = false;
                responseReceived_ = false;
                receivedResponse_.clear();
                return false;
            }
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка выполнения хендшейка: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка выполнения хендшейка");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка выполнения хендшейка");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::IdentifyTerminal(std::string& terminalInfo) {
    if (parentComponent_) {
        REPORT_INFO("Идентификация терминала");
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Идентификация терминала");
    }
    
    try {
        if (!transport_ || !transport_->IsOpen() || !helper_) {
            if (parentComponent_) {
                REPORT_ERROR("Транспортный слой не инициализирован или не открыт");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не инициализирован или не открыт");
            }
            return false;
        }
        
        // Формируем запрос ServiceMessage с типом identify
        std::map<std::string, std::string> params;
        params["msgType"] = "identify";
        
        // Создаем JSON запрос
        json requestJson;
        requestJson["method"] = "ServiceMessage";
        requestJson["step"] = 0;
        
        // Добавляем параметры
        json paramsJson;
        for (const auto& param : params) {
            paramsJson[param.first] = param.second;
        }
        requestJson["params"] = paramsJson;
        
        // Преобразуем JSON в строку
        std::string request = requestJson.dump();
        
        // Добавляем нулевой терминатор в конец
        std::vector<uint8_t> requestData(request.begin(), request.end());
        requestData.push_back(0);
        
        if (parentComponent_) {
            REPORT_DEBUG("Отправка запроса идентификации: " + request);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса идентификации: " + request);
        }
        
        // Обнуляем флаги ожидания и получения ответа
        waitingForResponse_ = true;
        responseReceived_ = false;
        
        // Отправляем запрос
        if (transport_->Send(requestData) <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка отправки запроса идентификации");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки запроса идентификации");
            }
            return false;
        }
        
        // Ожидаем ответ от терминала через хелпер или напрямую
        std::string response;
        
        if (helper_) {
            if (!helper_->WaitForResponse(5000)) {
                if (parentComponent_) {
                    REPORT_ERROR("Таймаут ожидания ответа на запрос идентификации");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на запрос идентификации");
                }
                return false;
            }
            
            response = helper_->GetResponse();
        }
        else {
            // Если хелпер не инициализирован, ожидаем ответ самостоятельно
            std::unique_lock<std::mutex> lock(bufferMutex_);
            bool result = dataCondition_.wait_for(lock, std::chrono::milliseconds(5000),
                [this] { return responseReceived_; });
            
            if (!result) {
                if (parentComponent_) {
                    REPORT_ERROR("Таймаут ожидания ответа на запрос идентификации");
                } else {
                    NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа на запрос идентификации");
                }
                waitingForResponse_ = false;
                return false;
            }
            
            response = receivedResponse_;
            waitingForResponse_ = false;
            responseReceived_ = false;
            receivedResponse_.clear();
        }
        
        if (parentComponent_) {
            REPORT_DEBUG("Получен ответ на запрос идентификации: " + response);
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ на запрос идентификации: " + response);
        }
        
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
                    if (parentComponent_) {
                        REPORT_INFO("Идентификация успешно выполнена, терминал: " + terminalInfo);
                    } else {
                        NEUTRAL_REPORT_INFO(componentName_, "Идентификация успешно выполнена, терминал: " + terminalInfo);
                    }
                    
                    // Сбрасываем состояние ответа в хелпере
                    if (helper_) {
                        helper_->ResetResponseState();
                    }
                    
                    return true;
                }
            }
            
            // Если не удалось извлечь информацию
            if (parentComponent_) {
                REPORT_ERROR("Ошибка идентификации терминала: нет информации в ответе");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала: нет информации в ответе");
            }
            
            // Сбрасываем состояние ответа в хелпере
            if (helper_) {
                helper_->ResetResponseState();
            }
            
            return false;
        }
        catch (const std::exception& e) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка парсинга ответа на запрос идентификации: " + std::string(e.what()));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа на запрос идентификации: " + std::string(e.what()));
            }
            
            // Сбрасываем состояние ответа в хелпере
            if (helper_) {
                helper_->ResetResponseState();
            }
            
            return false;
        }
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка идентификации терминала: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка идентификации терминала");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка идентификации терминала");
        }
        return false;
    }
}

std::string ECRPrivatJSONProtocol::GetTerminalInfo() {
    if (parentComponent_) {
        REPORT_INFO("Запрос информации о терминале");
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Запрос информации о терминале");
    }
    
    try {
        if (!IsConnected()) {
            if (parentComponent_) {
                REPORT_ERROR("Не установлено соединение с терминалом");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не установлено соединение с терминалом");
            }
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
        if (parentComponent_) {
            REPORT_ERROR("Ошибка получения информации о терминале: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка получения информации о терминале: " + std::string(e.what()));
        }
        return "";
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка получения информации о терминале");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка получения информации о терминале");
        }
        return "";
    }
}

TerminalResponse ECRPrivatJSONProtocol::GetLastResponse() const {
    return lastResponse_;
}

} // namespace SimplyConnect
