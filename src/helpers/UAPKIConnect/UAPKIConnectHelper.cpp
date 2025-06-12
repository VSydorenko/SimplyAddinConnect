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

bool UAPKIConnectHelper::ParseParamsString(const std::string& paramsString, nlohmann::json& paramsJson) {
    // Инициализируем пустой JSON-объект для параметров
    paramsJson = nlohmann::json::object();
    
    // Если строка пустая, возвращаем пустой объект
    if (paramsString.empty()) {
        return true;
    }
    
    try {
        std::string key, value;
        bool insideQuotes = false;
        size_t pos = 0;
        
        while (pos < paramsString.size()) {
            // Читаем ключ до знака = или конца строки
            key.clear();
            while (pos < paramsString.size() && paramsString[pos] != '=') {
                key += paramsString[pos++];
            }
            
            // Проверяем, достигли ли мы знака =
            if (pos >= paramsString.size() || paramsString[pos] != '=') {
                // Если нет знака =, это некорректный формат
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
                return false; // Незакрытые кавычки
            }
            
            // Добавляем пару ключ-значение в JSON
            paramsJson[key] = value;
            
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
        
        return true;
    }
    catch (const std::exception&) {
        // При любом исключении возвращаем false
        return false;
    }
}

bool UAPKIConnectHelper::ExecuteUapkiCommand(const std::string& method, const std::string& paramsString, std::string& responseJson) {
#ifdef WITH_UAPKI
    try {
        // Формируем JSON-запрос с использованием nlohmann/json
        nlohmann::json requestJson;
        requestJson["method"] = method;
        
        // Разбираем строку параметров
        nlohmann::json paramsJson;
        if (!ParseParamsString(paramsString, paramsJson)) {
            // Если не удалось разобрать строку параметров, возвращаем ошибку
            responseJson = R"({"status":"error","error":"Invalid parameters format"})";
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: Invalid parameters format");
            return false;
        }
        
        // Добавляем разобранные параметры в запрос
        requestJson["parameters"] = paramsJson;
        
        // Преобразуем JSON в строку
        std::string requestStr = requestJson.dump();
        
        // Логирование запроса 
        std::string infoMsg = "UAPKI Request: " + requestStr;
        NEUTRAL_REPORT_INFO("UAPKIConnectHelper", infoMsg);
        
        // Выполнение запроса через UAPKI API с использованием функций из библиотеки
        char* response = ::process(requestStr.c_str());
        
        // Если получили ответ, обрабатываем его
        if (response) {
            // Копируем результат
            responseJson = std::string(response);
            
            // Логируем ответ
            std::string responseMsg = "UAPKI Response: " + responseJson;
            NEUTRAL_REPORT_INFO("UAPKIConnectHelper", responseMsg);
            
            // Освобождаем память, выделенную функцией process
            ::json_free(response);
            
            // Проверяем успешность выполнения из содержимого ответа
            // По умолчанию считаем операцию успешной, если получили непустой ответ
            return !responseJson.empty();
        }
        else {
            // Если ответ пустой, формируем JSON с ошибкой
            responseJson = R"({"status":"error","error":"No response from UAPKI library"})";
            
            // Логируем ошибку
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: No response from library");
            
            return false;
        }
    }
    catch (const std::exception& ex) {
        // В случае исключения формируем JSON-ответ с сообщением об ошибке
        responseJson = R"({"status":"error","error":")" + std::string(ex.what()) + R"("})";
        
        // Логируем ошибку
        std::string errorMsg = "UAPKI Exception: " + std::string(ex.what());
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", errorMsg);

        return false;
    }
#else
    // Версия метода для сборки без UAPKI
    responseJson = R"({"status":"error","error":"UAPKI functionality is not available in this build"})";
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: UAPKI functionality is not available in this build");
    return false;
#endif
}