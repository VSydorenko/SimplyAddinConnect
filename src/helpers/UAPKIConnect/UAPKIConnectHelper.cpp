#include "../core/pch.h"
#include "UAPKIConnectHelper.h"
#include "../ServiceTools.h"
#include <stdexcept>

// Подключаем Windows.h для доступа к функциям Windows API
#ifdef _WINDOWS
#include <Windows.h>
#endif

// Подключение библиотеки UAPKI только если определен WITH_UAPKI
#ifdef WITH_UAPKI
// Объявляем функции из библиотеки UAPKI
#define UAPKI_EXPORT    // локальное отключение dllimport

extern "C" {
    char* process(const char* request);
    void json_free(char* json);
}
#endif

// Константы для кодов ответа
namespace UAPKIResponseCodes {
    const std::string SUCCESS = "0";
    const std::string SUCCESS_WITH_WARNING = "1";
    const std::string GENERAL_ERROR = "500";
    const std::string INVALID_PARAMETER = "400";
    const std::string LIBRARY_NOT_LOADED = "503";
}

bool UAPKIConnectHelper::ParseParamsString(const std::string& paramsString, nlohmann::json& paramsJson) {
    // Инициализируем пустой JSON-объект для параметров
    paramsJson = nlohmann::json::object();
    
    // Если строка пустая, возвращаем пустой объект
    if (paramsString.empty()) {
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Получена пустая строка параметров, возвращаем пустой JSON объект");
        return true;
    }
    
    try {
        std::string key, value;
        bool insideQuotes = false;
        size_t pos = 0;
        
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Начало разбора параметров: " + paramsString);
        
        while (pos < paramsString.size()) {
            // Читаем ключ до знака = или конца строки
            key.clear();
            while (pos < paramsString.size() && paramsString[pos] != '=') {
                key += paramsString[pos++];
            }
            
            // Проверяем, достигли ли мы знака =
            if (pos >= paramsString.size() || paramsString[pos] != '=') {
                // Если нет знака =, это некорректный формат
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Некорректный формат параметров: отсутствует символ '='");
                return false;
            }
            
            // Пропускаем символ =
            pos++;
            
            // Читаем значение до запятой или конца строки
            value.clear();
            insideQuotes = false;
            
            while (pos < paramsString.size()) {
                char c = paramsString[pos];
                
                // Обрабатываем кавычки
                if (c == '\'' || c == '\"') {
                    if (value.empty() && !insideQuotes) {
                        // Первая кавычка - начало строки в кавычках
                        insideQuotes = true;
                        pos++;
                        continue;
                    }
                    else if (insideQuotes) {
                        // Закрывающая кавычка
                        insideQuotes = false;
                        pos++;
                        
                        // Пропускаем пробелы после закрытия кавычек
                        while (pos < paramsString.size() && 
                              (paramsString[pos] == ' ' || paramsString[pos] == '\t')) {
                            pos++;
                        }
                        
                        // Проверяем, что после закрытия кавычек идет запятая или конец строки
                        if (pos < paramsString.size() && paramsString[pos] != ',') {
                            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Некорректный формат параметров: после закрывающей кавычки ожидалась запятая");
                            return false; // Некорректный формат
                        }
                        break;
                    }
                }
                
                // Если мы не в кавычках и встретили запятую, это разделитель
                if (!insideQuotes && c == ',') {
                    break;
                }
                
                // Добавляем символ к значению
                value += c;
                pos++;
            }
            
            // Проверяем, закрылись ли кавычки
            if (insideQuotes) {
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Некорректный формат параметров: незакрытые кавычки");
                return false; // Незакрытые кавычки
            }
            
            // Добавляем пару ключ-значение в JSON
            paramsJson[key] = value;
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Добавлен параметр: " + key + " = " + value);
            
            // Пропускаем запятую и пробелы
            if (pos < paramsString.size() && paramsString[pos] == ',') {
                pos++;
                
                // Пропускаем пробелы после запятой
                while (pos < paramsString.size() && 
                      (paramsString[pos] == ' ' || paramsString[pos] == '\t')) {
                    pos++;
                }
            }
        }
        
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Успешный разбор параметров, найдено параметров: " + std::to_string(paramsJson.size()));
        return true;
    }
    catch (const std::exception& e) {
        // При исключении логируем и возвращаем false
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Исключение при разборе параметров: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        // При любом неизвестном исключении логируем и возвращаем false
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Неизвестное исключение при разборе параметров");
        return false;
    }
}

// Проверка успешности выполнения операции на основе JSON-ответа
bool UAPKIConnectHelper::IsOperationSuccess(const std::string& jsonResponse) {
    try {
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Проверка успешности выполнения операции UAPKI");
        
        // Проверка на пустой ответ
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Получен пустой JSON ответ от UAPKI");
            return false;
        }
        
        nlohmann::json responseJson = nlohmann::json::parse(jsonResponse);
        
        // Проверяем наличие поля status
        if (responseJson.contains("status")) {
            std::string status = responseJson["status"].get<std::string>();
            
            // Если статус "error", операция неуспешна
            if (status == "error") {
                if (responseJson.contains("error")) {
                    std::string errorMsg = responseJson["error"].get<std::string>();
                    NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Операция UAPKI завершилась с ошибкой: " + errorMsg);
                } else {
                    NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Операция UAPKI завершилась с ошибкой, но без деталей");
                }
                return false;
            }
            
            // Если статус "success", операция успешна
            if (status == "success") {
                NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Операция UAPKI выполнена успешно");
                return true;
            }
        }
        
        // Проверяем наличие поля code (некоторые ответы могут использовать это поле)
        if (responseJson.contains("code")) {
            std::string code = responseJson["code"].is_string() ? 
                              responseJson["code"].get<std::string>() : 
                              std::to_string(responseJson["code"].get<int>());
            
            // Проверяем успешность по коду
            bool isSuccess = (code == UAPKIResponseCodes::SUCCESS || 
                            code == UAPKIResponseCodes::SUCCESS_WITH_WARNING);
            
            if (isSuccess) {
                NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Операция UAPKI выполнена успешно, код: " + code);
            } else {
                NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Операция UAPKI завершилась с кодом ошибки: " + code);
            }
            
            return isSuccess;
        }
        
        // Если не удалось определить успешность, возвращаем true, если получен какой-то ответ
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Невозможно определить успешность операции UAPKI по формату ответа");
        return true;
    }
    catch (const nlohmann::json::exception& e) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Ошибка парсинга JSON при проверке успешности операции: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Исключение при проверке успешности операции: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Неизвестное исключение при проверке успешности операции");
        return false;
    }
}

bool UAPKIConnectHelper::ExecuteUapkiCommand(const std::string& method, const std::string& paramsString, std::string& responseJson) {
#ifdef WITH_UAPKI
    try {
        // Логируем начало выполнения операции
        NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Начало выполнения команды UAPKI: " + method);
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Параметры команды UAPKI: " + paramsString);
        
        // Формируем JSON-запрос с использованием nlohmann/json
        nlohmann::json requestJson;
        requestJson["method"] = method;
        
        // Разбираем строку параметров
        nlohmann::json paramsJson;
        if (!ParseParamsString(paramsString, paramsJson)) {
            // Если не удалось разобрать строку параметров, возвращаем ошибку
            responseJson = R"({"status":"error","error":"Invalid parameters format"})";
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось разобрать параметры команды UAPKI");
            return false;
        }
        
        // Добавляем разобранные параметры в запрос
        requestJson["parameters"] = paramsJson;
        
        // Преобразуем JSON в строку
        std::string requestStr = requestJson.dump();
        
        // Логирование запроса (с ограничением длины для больших запросов)
        if (requestStr.length() > 2000) {
            NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Request (сокращенный): " + requestStr.substr(0, 2000) + "...");
        } else {
            NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Request: " + requestStr);
        }
        
        // Выполнение запроса через UAPKI API с использованием функций из библиотеки
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Вызов функции process библиотеки UAPKI");
        char* response = ::process(requestStr.c_str());
        
        // Если получили ответ, обрабатываем его
        if (response) {
            // Копируем результат
            responseJson = std::string(response);
            
            // Логируем ответ с ограничением длины для больших ответов
            if (responseJson.length() > 2000) {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Response (сокращенный): " + responseJson.substr(0, 2000) + "...");
            } else {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Response: " + responseJson);
            }
            
            // Освобождаем память, выделенную функцией process
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Освобождение памяти ответа с помощью json_free");
            ::json_free(response);
            
            // Проверяем успешность операции
            bool isSuccess = IsOperationSuccess(responseJson);
            
            if (isSuccess) {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Команда UAPKI " + method + " выполнена успешно");
            } else {
                NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Команда UAPKI " + method + " завершилась с ошибкой");
            }
            
            return isSuccess;
        }
        else {
            // Если ответ пустой, формируем JSON с ошибкой
            responseJson = R"({"status":"error","error":"No response from UAPKI library"})";
            
            // Логируем ошибку
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: No response from library for method " + method);
            
            return false;
        }
    }
    catch (const std::exception& e) {
        // В случае исключения формируем JSON-ответ с сообщением об ошибке
        responseJson = R"({"status":"error","error":")" + std::string(e.what()) + R"("})";
        
        // Логируем ошибку
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Exception при выполнении метода " + method + ": " + std::string(e.what()));

        return false;
    }
    catch (...) {
        // В случае неизвестного исключения формируем JSON с ошибкой
        responseJson = R"({"status":"error","error":"Unknown error"})";
        
        // Логируем неизвестное исключение
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Неизвестное исключение при выполнении метода UAPKI " + method);
        
        return false;
    }
#else
    // Версия метода для сборки без UAPKI
    responseJson = R"({"status":"error","error":"UAPKI functionality is not available in this build"})";
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: UAPKI functionality is not available in this build. Method: " + method);
    return false;
#endif
}