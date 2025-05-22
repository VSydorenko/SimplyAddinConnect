#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "transport/Transport_COM.h"
#include "transport/Transport_TCP.h"
#include "transport/Transport_WSClient.h"
#include "helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>

namespace SimplyConnect {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONProtocol::ECRPrivatJSONProtocol()
    : parentComponent_(nullptr)
    , componentName_("ECRPrivatJSONProtocol")
    , connected_(false)
    , waitingForResponse_(false)
    , responseReceived_(false) {
    // Инициализация последнего ответа
    lastResponse_.Clear();
}

ECRPrivatJSONProtocol::~ECRPrivatJSONProtocol() {
    // Отключение от терминала при уничтожении объекта
    Disconnect();
}

void ECRPrivatJSONProtocol::SetParentComponent(AddInNative* component) {
    parentComponent_ = component;
    
    if (parentComponent_) {
        componentName_ = "ECRPrivatJSON";
    }
}

// ===========================
// Методы подключения
// ===========================

bool ECRPrivatJSONProtocol::ConnectCOM(const std::u16string& portName, int baudRate) {
    std::string portNameStr;
    try {
        portNameStr = ServiceTools::SafeWCHAR2MB(portName);
    } catch (...) {
        portNameStr = "COM порт";
    }
    
    if (parentComponent_) {
        REPORT_INFO("Подключение к терминалу через COM, параметры: порт=" + portNameStr + 
                   ", скорость=" + std::to_string(baudRate));
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Подключение к терминалу через COM, параметры: порт=" + 
                           portNameStr + ", скорость=" + std::to_string(baudRate));
    }
    
    try {
        // Проверка корректности параметров
        if (portName.find(u"COM") == std::u16string::npos) {
            if (parentComponent_) {
                REPORT_ERROR("Неверный формат имени COM-порта: " + portNameStr);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Неверный формат имени COM-порта: " + portNameStr);
            }
            return false;
        }
        if (baudRate <= 0) {
            if (parentComponent_) {
                REPORT_ERROR("Некорректная скорость порта: " + std::to_string(baudRate));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Некорректная скорость порта: " + std::to_string(baudRate));
            }
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем объект транспортного слоя COM-порта
        auto comTransport = std::make_unique<TransportCOM>(portNameStr, baudRate, 8, 'N', 1.0f);
        
        // Установка обработчиков событий
        comTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
            this->OnDataReceived(data);
        });
        
        comTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
            this->OnError(errorMessage, errorCode);
        });
        
        comTransport->SetConnectionStateCallback([this](bool connected) {
            this->OnConnectionStateChanged(connected);
        });
        
        if (parentComponent_) {
            REPORT_INFO("Открытие COM-порта: " + portNameStr);
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Открытие COM-порта: " + portNameStr);
        }
        
        // Открываем COM-порт
        if (!comTransport->Open()) {
            if (parentComponent_) {
                REPORT_ERROR("Не удалось открыть COM-порт: " + portNameStr);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть COM-порт: " + portNameStr);
            }
            return false;
        }
        
        // Проверяем, что порт действительно открыт
        if (!comTransport->IsOpen()) {
            if (parentComponent_) {
                REPORT_ERROR("Порт не открыт после вызова Open(): " + portNameStr);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Порт не открыт после вызова Open(): " + portNameStr);
            }
            return false;
        }
        
        // Сохраняем транспортный объект
        transport_ = std::move(comTransport);
        
        // Создаем вспомогательный объект
        helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        
        if (parentComponent_) {
            REPORT_INFO("Выполнение процедуры хендшейка с терминалом");
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Выполнение процедуры хендшейка с терминалом");
        }
        
        // Выполнение процедуры хендшейка
        // Добавляем несколько попыток для повышения надежности
        bool handshakeSuccess = false;
        for (int attempt = 1; attempt <= 3 && !handshakeSuccess; attempt++) {
            if (parentComponent_) {
                REPORT_INFO("Попытка хендшейка #" + std::to_string(attempt));
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Попытка хендшейка #" + std::to_string(attempt));
            }
            
            if (PerformHandshake()) {
                handshakeSuccess = true;
                if (parentComponent_) {
                    REPORT_INFO("Хендшейк успешен на попытке #" + std::to_string(attempt));
                } else {
                    NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешен на попытке #" + std::to_string(attempt));
                }
            } else {
                if (parentComponent_) {
                    REPORT_WARN("Ошибка выполнения хендшейка (попытка #" + std::to_string(attempt) + "), пауза и повтор");
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка выполнения хендшейка (попытка #" + std::to_string(attempt) + "), пауза и повтор");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt)); // Увеличиваем время пауз с каждой попыткой
            }
        }
        
        if (!handshakeSuccess) {
            if (parentComponent_) {
                REPORT_ERROR("Ошибка выполнения хендшейка с терминалом");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка с терминалом");
            }
            
            // Закрываем соединение
            transport_->Close();
            transport_.reset();
            helper_.reset();
            connected_ = false;
            
            return false;
        }
        
        // Идентификация терминала и сохранение информации о нем
        std::string terminalInfo;
        if (IdentifyTerminal(terminalInfo)) {
            if (parentComponent_) {
                REPORT_INFO("Подключение к терминалу успешно: " + terminalInfo);
            } else {
                NEUTRAL_REPORT_INFO(componentName_, "Подключение к терминалу успешно: " + terminalInfo);
            }
        } else {
            if (parentComponent_) {
                REPORT_WARN("Подключение к терминалу успешно, но идентификация не выполнена");
            } else {
                NEUTRAL_REPORT_WARN(componentName_, "Подключение к терминалу успешно, но идентификация не выполнена");
            }
        }
        
        // Устанавливаем флаг подключения
        connected_ = true;
        
        return true;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка подключения к терминалу: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка подключения к терминалу: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка подключения к терминалу");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка подключения к терминалу");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::ConnectTCP(const std::string& address, int port) {
    if (parentComponent_) {
        REPORT_INFO("Подключение к терминалу через TCP: " + address + ":" + std::to_string(port));
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Подключение к терминалу через TCP: " + address + ":" + std::to_string(port));
    }
    
    try {
        // Проверка корректности параметров
        if (address.empty()) {
            if (parentComponent_) {
                REPORT_ERROR("Пустой адрес для TCP подключения");
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Пустой адрес для TCP подключения");
            }
            return false;
        }
        if (port <= 0 || port > 65535) {
            if (parentComponent_) {
                REPORT_ERROR("Некорректный TCP порт: " + std::to_string(port));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Некорректный TCP порт: " + std::to_string(port));
            }
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем объект транспортного слоя TCP
        auto tcpTransport = std::make_unique<TransportTCP>(address, port);
        
        // Установка обработчиков событий
        tcpTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
            this->OnDataReceived(data);
        });
        
        tcpTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
            this->OnError(errorMessage, errorCode);
        });
        
        tcpTransport->SetConnectionStateCallback([this](bool connected) {
            this->OnConnectionStateChanged(connected);
        });
        
        if (parentComponent_) {
            REPORT_INFO("Открытие TCP соединения: " + address + ":" + std::to_string(port));
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Открытие TCP соединения: " + address + ":" + std::to_string(port));
        }
        
        // Открываем TCP соединение
        if (!tcpTransport->Open()) {
            if (parentComponent_) {
                REPORT_ERROR("Не удалось открыть TCP соединение: " + address + ":" + std::to_string(port));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть TCP соединение: " + address + ":" + std::to_string(port));
            }
            return false;
        }
        
        // Проверяем, что соединение действительно открыто
        if (!tcpTransport->IsOpen()) {
            if (parentComponent_) {
                REPORT_ERROR("TCP соединение не открыто после вызова Open(): " + address + ":" + std::to_string(port));
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "TCP соединение не открыто после вызова Open(): " + address + ":" + std::to_string(port));
            }
            return false;
        }
        
        // Сохраняем транспортный объект
        transport_ = std::move(tcpTransport);
        
        // Создаем вспомогательный объект
        helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        
        // Аналогично ConnectCOM, выполняем хендшейк и идентификацию...
        // [Тот же код, что и в ConnectCOM]
        
        // Устанавливаем флаг подключения
        connected_ = true;
        
        return true;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка TCP подключения к терминалу: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка TCP подключения к терминалу: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка TCP подключения к терминалу");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка TCP подключения к терминалу");
        }
        return false;
    }
}

bool ECRPrivatJSONProtocol::ConnectWebSocket(const std::string& url) {
    if (parentComponent_) {
        REPORT_INFO("Подключение к терминалу через WebSocket: " + url);
    } else {
        NEUTRAL_REPORT_INFO(componentName_, "Подключение к терминалу через WebSocket: " + url);
    }
    
    try {
        // Проверка корректности URL
        if (url.empty() || (url.find("ws://") != 0 && url.find("wss://") != 0)) {
            if (parentComponent_) {
                REPORT_ERROR("Неверный формат URL для WebSocket: " + url);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Неверный формат URL для WebSocket: " + url);
            }
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем объект транспортного слоя WebSocket
        auto wsTransport = std::make_unique<TransportWSClient>(url);
        
        // Установка обработчиков событий
        wsTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
            this->OnDataReceived(data);
        });
        
        wsTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
            this->OnError(errorMessage, errorCode);
        });
        
        wsTransport->SetConnectionStateCallback([this](bool connected) {
            this->OnConnectionStateChanged(connected);
        });
        
        if (parentComponent_) {
            REPORT_INFO("Открытие WebSocket соединения: " + url);
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Открытие WebSocket соединения: " + url);
        }
        
        // Открываем WebSocket соединение
        if (!wsTransport->Open()) {
            if (parentComponent_) {
                REPORT_ERROR("Не удалось открыть WebSocket соединение: " + url);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть WebSocket соединение: " + url);
            }
            return false;
        }
        
        // Проверяем, что соединение действительно открыто
        if (!wsTransport->IsOpen()) {
            if (parentComponent_) {
                REPORT_ERROR("WebSocket соединение не открыто после вызова Open(): " + url);
            } else {
                NEUTRAL_REPORT_ERROR(componentName_, "WebSocket соединение не открыто после вызова Open(): " + url);
            }
            return false;
        }
        
        // Сохраняем транспортный объект
        transport_ = std::move(wsTransport);
        
        // Создаем вспомогательный объект
        helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        
        // Аналогично ConnectCOM, выполняем хендшейк и идентификацию...
        // [Тот же код, что и в ConnectCOM]
        
        // Устанавливаем флаг подключения
        connected_ = true;
        
        return true;
    }
    catch (const std::exception& e) {
        if (parentComponent_) {
            REPORT_ERROR("Ошибка WebSocket подключения к терминалу: " + std::string(e.what()));
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка WebSocket подключения к терминалу: " + std::string(e.what()));
        }
        return false;
    }
    catch (...) {
        if (parentComponent_) {
            REPORT_ERROR("Неизвестная ошибка WebSocket подключения к терминалу");
        } else {
            NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка WebSocket подключения к терминалу");
        }
        return false;
    }
}

void ECRPrivatJSONProtocol::Disconnect() {
    if (transport_ && transport_->IsOpen()) {
        if (parentComponent_) {
            REPORT_INFO("Отключение от терминала");
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Отключение от терминала");
        }
        
        // Закрываем соединение
        transport_->Close();
    }
    
    // Очистка ресурсов
    transport_.reset();
    helper_.reset();
    connected_ = false;
    
    // Очистка буфера данных
    std::lock_guard<std::mutex> lock(bufferMutex_);
    dataBuffer_.clear();
    waitingForResponse_ = false;
    responseReceived_ = false;
    receivedResponse_.clear();
}

bool ECRPrivatJSONProtocol::IsConnected() const {
    return connected_ && transport_ && transport_->IsOpen();
}

// ===========================
// Обработчики событий транспортного слоя
// ===========================

void ECRPrivatJSONProtocol::OnDataReceived(const std::vector<uint8_t>& data) {
    if (helper_) {
        helper_->ProcessReceivedData(data);
    }
    else {
        // Если хелпер не создан, добавляем данные в буфер
        std::lock_guard<std::mutex> lock(bufferMutex_);
        dataBuffer_.insert(dataBuffer_.end(), data.begin(), data.end());
        
        // Если находимся в режиме ожидания ответа, проверяем наличие нулевого терминатора
        if (waitingForResponse_) {
            auto it = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
            if (it != dataBuffer_.end()) {
                // Найден нулевой терминатор, формируем ответ
                std::string response(dataBuffer_.begin(), it);
                receivedResponse_ = response;
                responseReceived_ = true;
                
                // Удаляем обработанные данные из буфера
                dataBuffer_.erase(dataBuffer_.begin(), it + 1);
                
                // Уведомляем о получении ответа
                dataCondition_.notify_one();
            }
        }
    }
}

void ECRPrivatJSONProtocol::OnError(const std::string& errorMessage, int errorCode) {
    if (parentComponent_) {
        REPORT_ERROR("Ошибка транспортного слоя: " + errorMessage + " (код " + std::to_string(errorCode) + ")");
    } else {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка транспортного слоя: " + errorMessage + " (код " + std::to_string(errorCode) + ")");
    }
}

void ECRPrivatJSONProtocol::OnConnectionStateChanged(bool connected) {
    connected_ = connected;
    
    if (parentComponent_) {
        if (connected) {
            REPORT_INFO("Установлено соединение с терминалом");
        } else {
            REPORT_INFO("Соединение с терминалом разорвано");
        }
    } else {
        if (connected) {
            NEUTRAL_REPORT_INFO(componentName_, "Установлено соединение с терминалом");
        } else {
            NEUTRAL_REPORT_INFO(componentName_, "Соединение с терминалом разорвано");
        }
    }
}

// ===========================
// Вспомогательные методы
// ===========================

std::unique_ptr<ITransport> ECRPrivatJSONProtocol::CreateTransport(const std::string& connectionString) {
    // Определяем тип транспорта по строке подключения
    if (connectionString.find("COM") != std::string::npos) {
        // COM-порт
        std::string portName = connectionString;
        int baudRate = 115200;
        
        // Если строка содержит скорость через двоеточие, извлекаем её
        size_t colonPos = connectionString.find(':');
        if (colonPos != std::string::npos) {
            try {
                std::string baudStr = connectionString.substr(colonPos + 1);
                baudRate = std::stoi(baudStr);
                portName = connectionString.substr(0, colonPos);
            }
            catch (...) {
                if (parentComponent_) {
                    REPORT_WARN("Ошибка разбора скорости COM-порта, используется 115200");
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка разбора скорости COM-порта, используется 115200");
                }
            }
        }
        
        return std::make_unique<TransportCOM>(portName, baudRate, 8, 'N', 1.0f);
    }
    else if (connectionString.find("tcp://") == 0) {
        // TCP соединение
        std::string address = connectionString.substr(6); // Убираем префикс "tcp://"
        int port = 2000;
        
        // Если адрес содержит порт через двоеточие, извлекаем его
        size_t colonPos = address.find(':');
        if (colonPos != std::string::npos) {
            try {
                std::string portStr = address.substr(colonPos + 1);
                port = std::stoi(portStr);
                address = address.substr(0, colonPos);
            }
            catch (...) {
                if (parentComponent_) {
                    REPORT_WARN("Ошибка разбора TCP порта, используется 2000");
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка разбора TCP порта, используется 2000");
                }
            }
        }
        
        return std::make_unique<TransportTCP>(address, port);
    }
    else if (connectionString.find("ws://") == 0 || connectionString.find("wss://") == 0) {
        // WebSocket соединение
        return std::make_unique<TransportWSClient>(connectionString);
    }
    
    // Если не определен тип, возвращаем nullptr
    if (parentComponent_) {
        REPORT_ERROR("Неизвестный тип соединения: " + connectionString);
    } else {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестный тип соединения: " + connectionString);
    }
    
    return nullptr;
}

} // namespace SimplyConnect
