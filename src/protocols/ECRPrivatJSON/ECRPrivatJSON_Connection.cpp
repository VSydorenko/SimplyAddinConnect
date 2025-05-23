#include "../../core/pch.h"
#include "ECRPrivatJSON.h"
#include "../../helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "../../transport/Transport_COM.h"
#include "../../transport/Transport_TCP.h"
#include "../../transport/Transport_WSClient.h"
#include "../../helpers/ServiceTools.h"
#include <iomanip>
#include <sstream>

namespace ECRPrivatJSON {

// ===========================
// Методы подключения через COM-порт
// ===========================

bool ECRPrivatJSONProtocol::ConnectCOM(const std::u16string& portName, int baudRate) {
    std::string portNameStr;
    try {
        portNameStr = ServiceTools::SafeWCHAR2MB(portName);
    } catch (const std::exception& e) {
        std::string errorMsg = "Ошибка конвертации имени порта: " + std::string(e.what());
        NEUTRAL_REPORT_WARN(componentName_, errorMsg);
        portNameStr = "COM порт";
    } catch (...) {
        NEUTRAL_REPORT_WARN(componentName_, "Неизвестная ошибка при конвертации имени порта");
        portNameStr = "COM порт";
    }
    
    std::string connectionInfo = "Подключение к терминалу через COM, параметры: порт=" + portNameStr + 
                               ", скорость=" + std::to_string(baudRate);
    NEUTRAL_REPORT_INFO(componentName_, connectionInfo);
    
    try {
        // Проверка корректности параметров
        if (portName.find(u"COM") == std::u16string::npos) {
            std::string errorMsg = "Неверный формат имени COM-порта: " + portNameStr;
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            return false;
        }
        if (baudRate <= 0) {
            std::string errorMsg = "Некорректная скорость порта: " + std::to_string(baudRate);
            NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем объект COM-порта
        auto comTransport = std::make_unique<COMTransport>();
        
        try {
            // Настраиваем параметры подключения
            comTransport->SetPortName(ServiceTools::U16StringToWString(portName));
            comTransport->SetBaudRate(baudRate);
            comTransport->SetDataBits(8);
            comTransport->SetParity(NOPARITY);
            comTransport->SetStopBits(ONESTOPBIT);
            comTransport->SetFlowControl(false);
            
            // Увеличиваем буферы для надежной работы с JSON
            comTransport->SetBufferSizes(4096, 4096);
            
            // Установка таймаутов для более надежной работы с терминалом Приватбанка
            // Согласно документации, терминал может требовать больше времени для первого ответа
            comTransport->SetTimeouts(5000, 10000);
            
            // Регистрация обработчиков
            comTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
                this->OnDataReceived(data);
            });
            
            comTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
                this->OnError(errorMessage, errorCode);
            });
            
            comTransport->SetConnectionStateCallback([this](bool connected) {
                this->OnConnectionStateChanged(connected);
            });
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка настройки COM-порта: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_INFO(componentName_, "Открытие COM-порта: " + portNameStr);
        
        // Открываем COM-порт
        try {
            if (!comTransport->Open()) {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть COM-порт: " + portNameStr);
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при открытии COM-порта: " + std::string(e.what()));
            return false;
        }
        
        // После открытия порта сначала настроим соединение
        NEUTRAL_REPORT_INFO(componentName_, "Настройка линий управления COM-порта для работы с терминалом");
        
        // Добавляем небольшую паузу после открытия порта для стабилизации
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        try {
            // Согласно протоколу, включаем линии DTR и RTS для правильного хендшейка
            comTransport->SetDTR(true);
            comTransport->SetRTS(true);
            
            // Добавляем паузу после установки линий для их стабилизации
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Проверяем, что порт действительно открыт
            if (!comTransport->IsOpen()) {
                NEUTRAL_REPORT_ERROR(componentName_, "Порт не открыт после вызова Open(): " + portNameStr);
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при установке параметров порта: " + std::string(e.what()));
            return false;
        }
        
        try {
            // Сохраняем транспортный объект
            transport_ = std::move(comTransport);
            
            // Создаем хелпер для работы с протоколом
            helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при инициализации протокола: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение процедуры хендшейка с терминалом");
        
        // Выполнение процедуры хендшейка
        // Добавляем несколько попыток для повышения надежности
        bool handshakeSuccess = false;
        for (int attempt = 1; attempt <= 3 && !handshakeSuccess; attempt++) {
            NEUTRAL_REPORT_INFO(componentName_, "Попытка хендшейка #" + std::to_string(attempt));
            
            try {
                if (PerformHandshake()) {
                    handshakeSuccess = true;
                    NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешен на попытке #" + std::to_string(attempt));
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка выполнения хендшейка (попытка #" + std::to_string(attempt) + "), пауза и повтор");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt)); // Увеличиваем время пауз с каждой попыткой
                }
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_WARN(componentName_, "Исключение при выполнении хендшейка (попытка #" + std::to_string(attempt) + "): " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
            }
        }
        
        if (!handshakeSuccess) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка с терминалом");
            Disconnect();
            return false;
        }
        
        // Выполняем идентификацию терминала
        std::string terminalInfo;
        if (!IdentifyTerminal(terminalInfo)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала");
            Disconnect();
            return false;
        }
        
        connected_ = true;
        
        NEUTRAL_REPORT_INFO(componentName_, "Успешное подключение к терминалу через COM-порт: " + portNameStr);
        NEUTRAL_REPORT_INFO(componentName_, "Информация о терминале: " + terminalInfo);
        
        return true;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при подключении к терминалу: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при подключении к терминалу");
        return false;
    }
}

// ===========================
// Методы подключения через TCP
// ===========================

bool ECRPrivatJSONProtocol::ConnectTCP(const std::string& address, int port) {
    std::string connectionInfo = "Подключение к терминалу через TCP, параметры: адрес=" + address + 
                              ", порт=" + std::to_string(port);
    NEUTRAL_REPORT_INFO(componentName_, connectionInfo);
    
    try {
        // Проверка корректности параметров
        if (address.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Пустой адрес TCP");
            return false;
        }
        if (port <= 0 || port > 65535) {
            NEUTRAL_REPORT_ERROR(componentName_, "Некорректный порт TCP: " + std::to_string(port));
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем TCP транспорт
        auto tcpTransport = std::make_unique<TCPTransport>();
        
        try {
            // Настраиваем параметры подключения
            tcpTransport->SetAddress(address);
            tcpTransport->SetPort(port);
            
            // Установка таймаута соединения 
            tcpTransport->SetConnectTimeout(5000); // 5 секунд
            
            // Регистрация обработчиков
            tcpTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
                this->OnDataReceived(data);
            });
            
            tcpTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
                this->OnError(errorMessage, errorCode);
            });
            
            tcpTransport->SetConnectionStateCallback([this](bool connected) {
                this->OnConnectionStateChanged(connected);
            });
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка настройки TCP-соединения: " + std::string(e.what()));
            return false;
        }
        
        std::string openConnectionMsg = "Открытие TCP-соединения: " + address + ":" + std::to_string(port);
        NEUTRAL_REPORT_INFO(componentName_, openConnectionMsg);
        
        // Открываем TCP-соединение
        try {
            if (!tcpTransport->Open()) {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть TCP-соединение: " + address + ":" + std::to_string(port));
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при открытии TCP-соединения: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем, что соединение действительно открыто
        if (!tcpTransport->IsOpen()) {
            NEUTRAL_REPORT_ERROR(componentName_, "TCP-соединение не открыто после вызова Open(): " + address + ":" + std::to_string(port));
            return false;
        }
        
        try {
            // Сохраняем транспортный объект
            transport_ = std::move(tcpTransport);
            
            // Создаем хелпер для работы с протоколом
            helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при инициализации протокола: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение процедуры хендшейка с терминалом");
        
        // Выполнение процедуры хендшейка
        // Добавляем несколько попыток для повышения надежности
        bool handshakeSuccess = false;
        for (int attempt = 1; attempt <= 3 && !handshakeSuccess; attempt++) {
            NEUTRAL_REPORT_INFO(componentName_, "Попытка хендшейка #" + std::to_string(attempt));
            
            try {
                if (PerformHandshake()) {
                    handshakeSuccess = true;
                    std::string successMsg = "Хендшейк успешен на попытке #" + std::to_string(attempt);
                    NEUTRAL_REPORT_INFO(componentName_, successMsg);
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка выполнения хендшейка (попытка #" + std::to_string(attempt) + "), пауза и повтор");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt)); // Увеличиваем время пауз с каждой попыткой
                }
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_WARN(componentName_, "Исключение при выполнении хендшейка (попытка #" + std::to_string(attempt) + "): " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
            }
        }
        
        if (!handshakeSuccess) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка с терминалом");
            Disconnect();
            return false;
        }
        
        // Выполняем идентификацию терминала
        std::string terminalInfo;
        if (!IdentifyTerminal(terminalInfo)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала");
            Disconnect();
            return false;
        }
        
        connected_ = true;
        
        NEUTRAL_REPORT_INFO(componentName_, "Успешное подключение к терминалу через TCP: " + address + ":" + std::to_string(port));
        NEUTRAL_REPORT_INFO(componentName_, "Информация о терминале: " + terminalInfo);
        
        return true;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при подключении к терминалу: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при подключении к терминалу");
        return false;
    }
}

// ===========================
// Методы подключения через WebSocket
// ===========================

bool ECRPrivatJSONProtocol::ConnectWebSocket(const std::string& url) {
    NEUTRAL_REPORT_INFO(componentName_, "Подключение к терминалу через WebSocket, URL: " + url);
    
    try {
        // Проверка корректности параметров
        if (url.empty()) {
            NEUTRAL_REPORT_ERROR(componentName_, "Пустой URL WebSocket");
            return false;
        }
        
        // Закрываем предыдущее соединение, если оно было
        Disconnect();
        
        // Создаем WebSocket транспорт
        auto wsTransport = std::make_unique<WSClientTransport>();
        
        try {
            // Настраиваем параметры подключения
            wsTransport->SetURL(url);
            
            // Регистрация обработчиков
            wsTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
                this->OnDataReceived(data);
            });
            
            wsTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
                this->OnError(errorMessage, errorCode);
            });
            
            wsTransport->SetConnectionStateCallback([this](bool connected) {
                this->OnConnectionStateChanged(connected);
            });
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка настройки WebSocket-соединения: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_INFO(componentName_, "Открытие WebSocket-соединения: " + url);
        
        // Открываем WebSocket-соединение
        try {
            if (!wsTransport->Open()) {
                NEUTRAL_REPORT_ERROR(componentName_, "Не удалось открыть WebSocket-соединение: " + url);
                return false;
            }
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Исключение при открытии WebSocket-соединения: " + std::string(e.what()));
            return false;
        }
        
        // Проверяем, что соединение действительно открыто
        if (!wsTransport->IsOpen()) {
            NEUTRAL_REPORT_ERROR(componentName_, "WebSocket-соединение не открыто после вызова Open(): " + url);
            return false;
        }
        
        try {
            // Сохраняем транспортный объект
            transport_ = std::move(wsTransport);
            
            // Создаем хелпер для работы с протоколом
            helper_ = std::make_unique<ECRPrivatJSONHelper>(transport_.get(), componentName_);
        } catch (const std::exception& e) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при инициализации протокола: " + std::string(e.what()));
            return false;
        }
        
        NEUTRAL_REPORT_INFO(componentName_, "Выполнение процедуры хендшейка с терминалом");
        
        // Выполнение процедуры хендшейка
        // Добавляем несколько попыток для повышения надежности
        bool handshakeSuccess = false;
        for (int attempt = 1; attempt <= 3 && !handshakeSuccess; attempt++) {
            NEUTRAL_REPORT_INFO(componentName_, "Попытка хендшейка #" + std::to_string(attempt));
            
            try {
                if (PerformHandshake()) {
                    handshakeSuccess = true;
                    NEUTRAL_REPORT_INFO(componentName_, "Хендшейк успешен на попытке #" + std::to_string(attempt));
                } else {
                    NEUTRAL_REPORT_WARN(componentName_, "Ошибка выполнения хендшейка (попытка #" + std::to_string(attempt) + "), пауза и повтор");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt)); // Увеличиваем время пауз с каждой попыткой
                }
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_WARN(componentName_, "Исключение при выполнении хендшейка (попытка #" + std::to_string(attempt) + "): " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
            }
        }
        
        if (!handshakeSuccess) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка выполнения хендшейка с терминалом");
            Disconnect();
            return false;
        }
        
        // Выполняем идентификацию терминала
        std::string terminalInfo;
        if (!IdentifyTerminal(terminalInfo)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка идентификации терминала");
            Disconnect();
            return false;
        }
        
        connected_ = true;
        
        NEUTRAL_REPORT_INFO(componentName_, "Успешное подключение к терминалу через WebSocket: " + url);
        NEUTRAL_REPORT_INFO(componentName_, "Информация о терминале: " + terminalInfo);
        
        return true;
    }
    catch (const std::exception& e) {
        std::string errorMsg = "Ошибка при подключении к терминалу: " + std::string(e.what());
        NEUTRAL_REPORT_ERROR(componentName_, errorMsg);
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при подключении к терминалу");
        return false;
    }
}

// ===========================
// Отключение
// ===========================

void ECRPrivatJSONProtocol::Disconnect() {
    try {
        if (transport_ && transport_->IsOpen()) {
            NEUTRAL_REPORT_INFO(componentName_, "Отключение от терминала");
            
            try {
                transport_->Close();
            } catch (const std::exception& e) {
                NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при закрытии соединения: " + std::string(e.what()));
            } catch (...) {
                NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при закрытии соединения");
            }
        }
        
        connected_ = false;
        helper_.reset();
        transport_.reset();
        
        NEUTRAL_REPORT_INFO(componentName_, "Отключение от терминала завершено");
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Критическая ошибка при отключении от терминала: " + std::string(e.what()));
    } catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная критическая ошибка при отключении от терминала");
    }
}

bool ECRPrivatJSONProtocol::IsConnected() const {
    bool currentStatus = (connected_ && transport_ && transport_->IsOpen());
    
    // Если флаг connected_ установлен, но фактически соединение закрыто
    if (connected_ && (!transport_ || !transport_->IsOpen())) {
        NEUTRAL_REPORT_WARN(componentName_, "Обнаружено несоответствие состояния соединения: флаг подключения установлен, но транспорт не активен");
    }
    
    return currentStatus;
}

} // namespace ECRPrivatJSON
