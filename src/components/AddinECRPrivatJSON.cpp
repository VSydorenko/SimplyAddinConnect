#include "core/pch.h"
#include "AddinECRPrivatJSON.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON_Types.h"
#include "helpers/ServiceTools.h"

// Используем пространство имен для типов и констант
using namespace SimplyConnect;

// Инициализация статического поля names для регистрации компонента
std::vector<std::u16string> AddinECRPrivatJSON::names = {
    AddComponent(u"AddinECRPrivatJSON", []() { return new AddinECRPrivatJSON; })
};
namespace { auto& _forceAddinECRPrivatJSONNames = AddinECRPrivatJSON::names; }

// Конструктор
AddinECRPrivatJSON::AddinECRPrivatJSON() {    
    // Создаем объект протокола
    protocol_ = std::make_unique<SimplyConnect::ECRPrivatJSONProtocol>();
    
    // Передаем указатель на компонент протоколу для организации логирования
    protocol_->SetParentComponent(this);
    
    // Регистрация методов
    RegisterMethods();
    
    REPORT_INFO("Компонент AddinECRPrivatJSON создан");
}

// Деструктор
AddinECRPrivatJSON::~AddinECRPrivatJSON() {
    REPORT_INFO("Завершение работы компонента AddinECRPrivatJSON");
    
    // Отключаемся от терминала при уничтожении объекта
    if (protocol_) {
        protocol_->Disconnect();
    }
    
    // Отключаем логирование при уничтожении компонента
    ServiceTools::DisableComponentLogging(this);
}

// Получение структуры ответа последней операции
SimplyConnect::TerminalResponse AddinECRPrivatJSON::GetLastTerminalResponse() const {
    return lastTerminalResponse_;
}

// Проверка кода ответа
bool AddinECRPrivatJSON::IsSuccessCode(const std::string& responseCode) const {
    return (
        responseCode == SimplyConnect::ResponseCodes::SUCCESS ||
        responseCode == SimplyConnect::ResponseCodes::SUCCESS_SHORT ||
        responseCode == SimplyConnect::ResponseCodes::PARTIAL_APPROVAL
    );
}

// Определение типа транспорта на основе строки подключения
std::string AddinECRPrivatJSON::DetermineTransportType(const std::string& connectionString) const {
    // Проверяем префикс для WebSocket
    if (connectionString.find("ws://") == 0 || connectionString.find("wss://") == 0) {
        return "WebSocket";
    }
    
    // Проверяем префикс для TCP
    if (connectionString.find("tcp://") == 0) {
        return "TCP";
    }
    
    // Проверяем, является ли это COM-портом
    if (connectionString.find("COM") != std::string::npos) {
        return "COM";
    }
    
    // По умолчанию возвращаем неизвестный тип
    return "Unknown";
}

/**
 * @brief Включает логирование для компонента
 * 
 * @param logLevel Уровень логирования (error, warn, info, debug, trace, off)
 * @param logFilePath Путь к файлу логов
 * @return bool Результат операции
 */
bool AddinECRPrivatJSON::EnableLogging(const std::string& logLevel, const std::string& logFilePath) {
    // Вызываем метод из ServiceTools, передавая параметры
    return ServiceTools::EnableComponentLogging(this, logLevel, logFilePath);
}

// Регистрация методов компонента для вызова из 1С
void AddinECRPrivatJSON::RegisterMethods() {
    // Метод для включения логирования
    AddFunction(u"EnableLogging", u"ИспользоватьЛогирование",
        [&](VH logLevel, VH logFilePath) {
            std::string logLevelStr = static_cast<std::string>(logLevel);
            std::string logFilePathStr = static_cast<std::string>(logFilePath);
            return EnableLogging(logLevelStr, logFilePathStr);
        }
    );

    // Методы подключения
    AddFunction(u"ConnectCOM", u"ПодключитьCOM",
        [&](VH portName, VH baudRate) {
            std::u16string port = portName;
            // Устанавливаем значение скорости по умолчанию
            int baud = 115200; 
            
            // Обработка параметра скорости порта
            try {
                // Попытка преобразовать в число напрямую
                std::string strBaudRate = static_cast<std::string>(baudRate);
                // Удаляем все нецифровые символы
                strBaudRate.erase(
                    std::remove_if(strBaudRate.begin(), strBaudRate.end(), 
                        [](unsigned char c) { return !std::isdigit(c); }), 
                    strBaudRate.end()
                );
                
                if (!strBaudRate.empty()) {
                    baud = std::stoi(strBaudRate);
                }
            }
            catch (...) {
                // Если ошибка преобразования, используем значение по умолчанию
                REPORT_WARN("Не удалось преобразовать значение скорости передачи, используется 115200");
            }
            
            // Проверка на корректность имени порта
            if (port.find(u"COM") == std::u16string::npos) {
                std::string portUtf8 = ServiceTools::SafeWCHAR2MB(port);
                REPORT_ERROR("Некорректное имя COM-порта: " + portUtf8);
                return false;
            }
            
            // Получаем UTF-8 представление имени порта для логирования
            std::string portUtf8 = ServiceTools::SafeWCHAR2MB(port);
            REPORT_INFO("Подключение к COM-порту: " + portUtf8 + " со скоростью " + std::to_string(baud));
            
            // Вызываем метод подключения к COM-порту
            bool result = protocol_->ConnectCOM(port, baud);
            
            // Обновляем структуру ответа
            if (result) {
                lastTerminalResponse_ = protocol_->GetLastResponse();
            }
            
            return result;
        }
    );
    
    AddFunction(u"ConnectTCP", u"ПодключитьTCP",
        [&](VH address, VH port) {
            std::string addr = static_cast<std::string>(address);
            
            // Получаем порт как число
            int portNum = 2000; // Порт по умолчанию
            try {
                std::string portStr = static_cast<std::string>(port);
                portNum = std::stoi(portStr);
            }
            catch (...) {
                REPORT_WARN("Не удалось преобразовать порт, используется значение по умолчанию (2000)");
            }
            
            REPORT_INFO("Подключение по TCP: " + addr + ":" + std::to_string(portNum));
            
            // Вызываем метод подключения TCP
            bool result = protocol_->ConnectTCP(addr, portNum);
            
            // Обновляем структуру ответа
            if (result) {
                lastTerminalResponse_ = protocol_->GetLastResponse();
            }
            
            return result;
        }
    );
    
    AddFunction(u"ConnectWebSocket", u"ПодключитьWebSocket",
        [&](VH url) {
            std::string wsUrl = static_cast<std::string>(url);
            
            // Проверяем, что URL начинается с ws:// или wss://
            if (wsUrl.find("ws://") != 0 && wsUrl.find("wss://") != 0) {
                wsUrl = "ws://" + wsUrl;
            }
            
            REPORT_INFO("Подключение по WebSocket: " + wsUrl);
            
            // Вызываем метод подключения WebSocket
            bool result = protocol_->ConnectWebSocket(wsUrl);
            
            // Обновляем структуру ответа
            if (result) {
                lastTerminalResponse_ = protocol_->GetLastResponse();
            }
            
            return result;
        }
    );
    
    AddFunction(u"Connect", u"Подключить",
        [&](VH connectionString) {
            std::string connStr = static_cast<std::string>(connectionString);
            
            // Определяем тип соединения и вызываем соответствующий метод
            std::string transportType = DetermineTransportType(connStr);
            
            REPORT_INFO("Определен тип соединения: " + transportType + ", строка соединения: " + connStr);
            
            bool result = false;
            
            if (transportType == "COM") {
                // Для COM-порта извлекаем имя порта и скорость
                std::u16string portName = ServiceTools::SafeMB2WCHAR(connStr);
                int baudRate = 115200;
                
                // Если указана скорость через двоеточие, извлекаем её
                size_t colonPos = connStr.find(':');
                if (colonPos != std::string::npos) {
                    try {
                        std::string baudStr = connStr.substr(colonPos + 1);
                        baudRate = std::stoi(baudStr);
                    }
                    catch (...) {
                        REPORT_WARN("Ошибка разбора скорости порта, используется 115200");
                    }
                }
                
                result = protocol_->ConnectCOM(portName, baudRate);
            }
            else if (transportType == "TCP") {
                // Для TCP извлекаем адрес и порт
                std::string address = connStr.substr(6); // Убираем префикс "tcp://"
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
                        REPORT_WARN("Ошибка разбора TCP порта, используется 2000");
                    }
                }
                
                result = protocol_->ConnectTCP(address, port);
            }
            else if (transportType == "WebSocket") {
                // Для WebSocket сразу используем URL
                result = protocol_->ConnectWebSocket(connStr);
            }
            else {
                REPORT_ERROR("Неизвестный тип соединения: " + connStr);
                result = false;
            }
            
            // Обновляем структуру ответа
            if (result) {
                lastTerminalResponse_ = protocol_->GetLastResponse();
            }
            
            return result;
        }
    );
    
    AddFunction(u"Disconnect", u"Отключить",
        [&]() {
            REPORT_INFO("Отключение от терминала");
            protocol_->Disconnect();
            return true;
        }
    );
    
    AddFunction(u"IsConnected", u"ПроверитьПодключение",
        [&]() {
            bool result = protocol_->IsConnected();
            REPORT_DEBUG("Проверка подключения: " + std::string(result ? "подключено" : "не подключено"));
            return result;
        }
    );
    
    // Финансовые операции
    AddFunction(u"Payment", u"Оплата",
        [&](VH amount, VH discount, VH merchantId, VH subMerchant) {
            // Преобразование параметров
            double paymentAmount = 0.0;
            try {
                std::string amountStr = static_cast<std::string>(amount);
                paymentAmount = std::stod(amountStr);
            }
            catch (...) {
                REPORT_ERROR("Некорректная сумма оплаты");
                return false;
            }
            
            std::string discountStr = static_cast<std::string>(discount);
            std::string merchantIdStr = static_cast<std::string>(merchantId);
            std::string subMerchantStr = static_cast<std::string>(subMerchant);
            
            REPORT_INFO("Выполнение операции оплаты на сумму " + std::to_string(paymentAmount));
            
            bool result = protocol_->Payment(
                paymentAmount, discountStr, merchantIdStr, subMerchantStr
            );
            
            // Обновляем структуру ответа
            lastTerminalResponse_ = protocol_->GetLastResponse();
            
            return result;
        }
    );
    
    AddFunction(u"Refund", u"Возврат",
        [&](VH amount, VH rrn, VH discount, VH merchantId, VH subMerchant) {
            // Преобразование параметров
            double refundAmount = 0.0;
            try {
                std::string amountStr = static_cast<std::string>(amount);
                refundAmount = std::stod(amountStr);
            }
            catch (...) {
                REPORT_ERROR("Некорректная сумма возврата");
                return false;
            }
            
            std::string rrnStr = static_cast<std::string>(rrn);
            std::string discountStr = static_cast<std::string>(discount);
            std::string merchantIdStr = static_cast<std::string>(merchantId);
            std::string subMerchantStr = static_cast<std::string>(subMerchant);
            
            REPORT_INFO("Выполнение операции возврата на сумму " + std::to_string(refundAmount));
            
            bool result = protocol_->Refund(
                refundAmount, rrnStr, discountStr, merchantIdStr, subMerchantStr
            );
            
            // Обновляем структуру ответа
            lastTerminalResponse_ = protocol_->GetLastResponse();
            
            return result;
        }
    );
    
    AddFunction(u"Settlement", u"СверкаИтогов",
        [&](VH merchantId) {
            std::string merchantIdStr = static_cast<std::string>(merchantId);
            
            REPORT_INFO("Выполнение операции сверки итогов");
            
            bool result = protocol_->Settlement(merchantIdStr);
            
            // Обновляем структуру ответа
            lastTerminalResponse_ = protocol_->GetLastResponse();
            
            return result;
        }
    );
    
    // Дополнительные методы
    AddFunction(u"GetTerminalInfo", u"ИнформацияОТерминале",
        [&]() {
            REPORT_INFO("Запрос информации о терминале");
            
            std::string result = protocol_->GetTerminalInfo();
            
            // Обновляем структуру ответа
            lastTerminalResponse_ = protocol_->GetLastResponse();
            
            return result;
        }
    );
    
    // Методы для работы с данными последней операции
    AddFunction(u"GetLastResponseCode", u"ПолучитьКодОтвета",
        [&]() {
            return lastTerminalResponse_.responseCode;
        }
    );
    
    AddFunction(u"GetLastResponseDescription", u"ПолучитьОписаниеОтвета",
        [&]() {
            return lastTerminalResponse_.responseDescription;
        }
    );
    
    AddFunction(u"GetLastReceipt", u"ПолучитьТекстЧека",
        [&]() {
            return lastTerminalResponse_.receipt;
        }
    );
    
    AddFunction(u"GetLastRRN", u"ПолучитьRRN",
        [&]() {
            return lastTerminalResponse_.rrn;
        }
    );
    
    AddFunction(u"GetLastApprovalCode", u"ПолучитьКодАвторизации",
        [&]() {
            return lastTerminalResponse_.approvalCode;
        }
    );
    
    AddFunction(u"GetLastCardPAN", u"ПолучитьНомерКарты",
        [&]() {
            return lastTerminalResponse_.cardPAN;
        }
    );
    
    AddFunction(u"GetLastAmount", u"ПолучитьСуммуОперации",
        [&]() {
            return lastTerminalResponse_.amount;
        }
    );
    
    AddFunction(u"IsLastOperationSuccess", u"УспешнаЛиОперация",
        [&]() {
            return lastTerminalResponse_.isSuccess;
        }
    );
}
