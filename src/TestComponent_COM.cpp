#include "core/pch.h"
#include "TestComponent.h"
#include "helpers/ServiceTools.h"
#include "transport/Transport_COM.h"

// Получение списка доступных COM-портов
std::vector<std::u16string> TestComponent::GetAvailablePorts() {
    REPORT_DEBUG("Получение списка доступных COM-портов");
    std::vector<std::u16string> availablePorts;
    
    // Используем функцию Windows API для получения списка устройств
    
    for (int i = 1; i <= 25; i++) {
        std::string portName = "COM" + std::to_string(i);
        std::string fullPortName = "\\\\.\\" + portName;
        
        // Пытаемся создать временный объект TransportCOM для проверки порта
        std::unique_ptr<TransportCOM> tempTransport = std::make_unique<TransportCOM>(portName);
        if (tempTransport) {
            // Пытаемся открыть порт без фактического подключения к нему
            HANDLE portHandle = CreateFileA(
                fullPortName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,                // Не разделять
                NULL,             // Атрибуты безопасности по умолчанию
                OPEN_EXISTING,    // Открыть существующий порт
                FILE_FLAG_OVERLAPPED, // Асинхронный режим
                NULL              // Шаблон не используется
            );
            
            if (portHandle != INVALID_HANDLE_VALUE) {
                // Порт существует, закрываем хендл
                CloseHandle(portHandle);
                // Преобразуем имя порта в UTF-16
                std::u16string portNameU16 = ServiceTools::SafeMB2WCHAR(portName.c_str());
                availablePorts.push_back(portNameU16);
                
                REPORT_DEBUG("Найден доступный порт: " + portName);
            }
        }
    }
    
    std::string msg = "Найдено портов: " + std::to_string(availablePorts.size());
    REPORT_INFO(msg);
    return availablePorts;
}

// Проверка существования порта с использованием транспортного контура
bool TestComponent::CheckPortExists(const std::u16string &portName) {
    std::string portNameMB = ServiceTools::SafeWCHAR2MB(portName);
    
    std::string msg = "Проверка существования порта: " + portNameMB;
    REPORT_DEBUG(msg);
    
    // Создаем временный объект TransportCOM для проверки порта
    std::unique_ptr<TransportCOM> tempTransport = std::make_unique<TransportCOM>(portNameMB);
    if (!tempTransport) {
        REPORT_ERROR("Не удалось создать объект TransportCOM");
        return false;
    }
    
    // Пытаемся открыть порт без фактического подключения
    std::string fullPortName = "\\\\.\\" + portNameMB;
    HANDLE portHandle = CreateFileA(
        fullPortName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
    );
    
    bool exists = (portHandle != INVALID_HANDLE_VALUE);
    
    if (exists) {
        CloseHandle(portHandle);
        REPORT_INFO("Порт " + portNameMB + " существует");
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            REPORT_INFO("Порт " + portNameMB + " не существует");
        } else {
            REPORT_INFO("Порт " + portNameMB + " существует, но не доступен. Ошибка: " + std::to_string(error));
            // Порт существует, но не может быть открыт по другой причине
            exists = true;
        }
    }
    
    return exists;
}

// Проверка доступности порта с использованием транспортного контура
bool TestComponent::IsPortAvailable(const std::u16string &portName) {
    std::string portNameMB = ServiceTools::SafeWCHAR2MB(portName);
    
    std::string msg = "Проверка доступности порта: " + portNameMB;
    REPORT_DEBUG(msg);
    
    // Проверяем, не открыт ли уже порт в нашем компоненте
    if (comTransport && comTransport->IsOpen()) {
        std::string currentPort = comTransport->GetPortName();
        if (currentPort == portNameMB) {
            REPORT_INFO("Порт " + portNameMB + " уже открыт в текущем компоненте");
            return false;
        }
    }
    
    // Создаем временный объект TransportCOM для проверки порта
    std::unique_ptr<TransportCOM> tempTransport = std::make_unique<TransportCOM>(portNameMB);
    if (!tempTransport) {
        REPORT_ERROR("Не удалось создать объект TransportCOM");
        return false;
    }
    
    // Пробуем открыть порт временно для проверки
    bool available = tempTransport->Open();
    
    // Если удалось открыть, закрываем
    if (available) {
        tempTransport->Close();
        REPORT_INFO("Порт " + portNameMB + " доступен");
    } else {
        REPORT_WARN("Порт " + portNameMB + " недоступен");
    }
    
    return available;
}

// Открытие COM-порта с использованием транспортного контура
bool TestComponent::OpenPort(const std::u16string &portName, const std::u16string &baudRate) {
    std::string portNameMB = ServiceTools::SafeWCHAR2MB(portName);
    
    std::string baudRateMB = ServiceTools::SafeWCHAR2MB(baudRate);
    int baudRateInt = 9600; // значение по умолчанию
    
    try {
        baudRateInt = std::stoi(baudRateMB);
    } catch (const std::exception& e) {
        std::string errorMsg = "Некорректная скорость порта: " + baudRateMB + 
                              ". Используется значение по умолчанию 9600. Ошибка: " + e.what();
        REPORT_WARN(errorMsg);
    }
    
    std::string msg = "Открытие порта: " + portNameMB + " на скорости: " + std::to_string(baudRateInt);
    REPORT_DEBUG(msg);
    
    // Если порт уже открыт, закрываем его
    if (comTransport && comTransport->IsOpen()) {
        std::string currentPort = comTransport->GetPortName();
        REPORT_WARN("Порт " + currentPort + " уже был открыт. Закрытие текущего соединения");
        comTransport->Close();
    }
    
    // Создаем новый объект транспорта COM с использованием транспортного контура
    comTransport = std::make_unique<TransportCOM>(
        portNameMB,      // Имя порта
        baudRateInt,     // Скорость
        8,               // Биты данных (по умолчанию 8)
        'N',             // Четность (N - отсутствует)
        1.0f             // Стоповые биты (1)
    );
    
    if (!comTransport) {
        REPORT_ERROR("Не удалось создать объект TransportCOM");
        return false;
    }
    
    // Устанавливаем обработчики событий
    comTransport->SetErrorCallback([this](const std::string& errorMessage, int errorCode) {
        std::string msg = "Ошибка COM-порта: " + errorMessage + " (код: " + std::to_string(errorCode) + ")";
        REPORT_ERROR(msg);
    });
    
    comTransport->SetConnectionStateCallback([this](bool connected) {
        isPortOpen = connected;
        if (connected) {
            REPORT_INFO("COM-порт успешно подключен");
        } else {
            REPORT_INFO("COM-порт отключен");
        }
    });
    
    comTransport->SetDataReceivedCallback([this](const std::vector<uint8_t>& data) {
        std::string dataHex;
        for (const auto& byte : data) {
            char hex[3];
            sprintf_s(hex, "%02X", byte);
            dataHex += hex;
            dataHex += " ";
        }
        std::string msg = "Получены данные из COM-порта: " + dataHex;
        REPORT_DEBUG(msg);
        
        // Здесь можно добавить логику обработки полученных данных
    });
    
    // Открываем порт с использованием метода Open() из TransportCOM
    bool result = comTransport->Open();
    isPortOpen = result;
    
    if (result) {
        REPORT_INFO("COM-порт " + portNameMB + " успешно открыт");
    } else {
        REPORT_ERROR("Не удалось открыть COM-порт " + portNameMB);
    }
    
    return result;
}

// Закрытие COM-порта с использованием транспортного контура
bool TestComponent::ClosePort(const std::u16string &portName) {
    std::string portNameMB = ServiceTools::SafeWCHAR2MB(portName);
    
    std::string msg = "Закрытие порта: " + portNameMB;
    REPORT_DEBUG(msg);
    
    if (!comTransport) {
        REPORT_WARN("Невозможно закрыть порт: транспортный объект не создан");
        return false;
    }
    
    std::string currentPort = comTransport->GetPortName();
    if (currentPort != portNameMB) {
        REPORT_WARN("Попытка закрыть порт " + portNameMB + 
                   ", но текущий открытый порт - " + currentPort);
        return false;
    }
    
    if (!comTransport->IsOpen()) {
        REPORT_WARN("Порт " + portNameMB + " уже закрыт или не был открыт");
        return false;
    }
    
    // Закрываем порт с использованием метода Close() из TransportCOM
    bool result = comTransport->Close();
    isPortOpen = false;
    
    if (result) {
        REPORT_INFO("COM-порт " + portNameMB + " успешно закрыт");
    } else {
        REPORT_ERROR("Не удалось закрыть COM-порт " + portNameMB);
    }
    
    return result;
}