#include "../../core/pch.h"
#include "UAPKIConnectHelper.h"
#include "../ServiceTools.h"
#include <stdexcept>
#include <algorithm>
#include <cctype>

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

#ifdef _WINDOWS
namespace {
    // Якорь-функция: используется исключительно как адрес внутри ТЕКУЩЕГО модуля (нашей DLL),
    // чтобы через GetModuleHandleExW(FROM_ADDRESS) получить дескриптор именно своей DLL,
    // а не хост-процесса 1cv8.exe. Не выполняет полезной работы.
    static void ModuleAnchor() {}
}
#endif

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

// Рекурсивно маскирует значения полей "password" на любом уровне JSON (для безопасного логирования)
void UAPKIConnectHelper::MaskPasswords(nlohmann::json& node) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "password") {
                it.value() = "***";
            } else {
                MaskPasswords(it.value());
            }
        }
    } else if (node.is_array()) {
        for (auto& element : node) {
            MaskPasswords(element);
        }
    }
}

// Определяет каталог собственной DLL (не хост-процесса 1cv8.exe) и возвращает его в UTF-8
bool UAPKIConnectHelper::GetOwnModuleDir(std::string& outUtf8Dir) {
#ifdef _WINDOWS
    HMODULE hMod = nullptr;

    // Получаем дескриптор именно СВОЕГО модуля через адрес функции внутри него.
    // GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT — не увеличиваем счётчик ссылок.
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleAnchor),
            &hMod) || hMod == nullptr) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось получить дескриптор собственного модуля (GetModuleHandleExW), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    // Буфер с запасом на случай длинных путей
    std::vector<wchar_t> pathBuf(32768, L'\0');
    DWORD len = GetModuleFileNameW(hMod, pathBuf.data(), static_cast<DWORD>(pathBuf.size()));
    if (len == 0 || len >= pathBuf.size()) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось получить путь к собственному модулю (GetModuleFileNameW), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    std::wstring fullPath(pathBuf.data(), len);

    // Отрезаем имя файла, оставляем каталог с завершающим слешем
    size_t slashPos = fullPath.find_last_of(L"\\/");
    if (slashPos == std::wstring::npos) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось выделить каталог из пути собственного модуля");
        return false;
    }
    std::wstring dirPath = fullPath.substr(0, slashPos + 1); // включая завершающий слеш

    // Проверяем путь на наличие не-ASCII символов.
    // CmLoader грузит провайдер через LoadLibraryA (ANSI), поэтому не-ASCII в пути (кириллица
    // в имени пользователя и т.п.) может привести к сбою загрузки провайдера.
    auto hasNonAscii = [](const std::wstring& s) {
        for (wchar_t wc : s) {
            if (static_cast<unsigned int>(wc) > 127u) {
                return true;
            }
        }
        return false;
    };

    if (hasNonAscii(dirPath)) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Каталог собственной DLL содержит не-ASCII символы; CmLoader использует LoadLibraryA (ANSI), возможны проблемы загрузки провайдера. Пробуем короткий (8.3) путь через GetShortPathNameW");

        std::vector<wchar_t> shortBuf(32768, L'\0');
        DWORD shortLen = GetShortPathNameW(dirPath.c_str(), shortBuf.data(), static_cast<DWORD>(shortBuf.size()));
        if (shortLen > 0 && shortLen < shortBuf.size()) {
            std::wstring shortPath(shortBuf.data(), shortLen);
            if (!hasNonAscii(shortPath)) {
                dirPath = shortPath;
                NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Использован короткий (ASCII-safe) путь к каталогу провайдеров");
            } else {
                NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Короткий путь также содержит не-ASCII символы; используем исходный путь");
            }
        } else {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось получить короткий путь (GetShortPathNameW), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())) + "; используем исходный путь");
        }
    }

    // Гарантируем завершающий слеш (GetShortPathNameW мог его отбросить)
    if (!dirPath.empty() && dirPath.back() != L'\\' && dirPath.back() != L'/') {
        dirPath += L'\\';
    }

    // Конвертируем wide (UTF-16) → UTF-8 через ServiceTools::SafeWCHAR2MB.
    // На Windows wchar_t 16-битный, поэтому корректно реинтерпретируем как char16_t.
    std::u16string u16dir(reinterpret_cast<const char16_t*>(dirPath.c_str()), dirPath.size());
    outUtf8Dir = ServiceTools::SafeWCHAR2MB(u16dir);
    return true;
#else
    (void)outUtf8Dir;
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Определение каталога собственного модуля поддерживается только на платформе Windows");
    return false;
#endif
}

// Автоинъекция конфигурации провайдеров НКИ в параметры метода INIT
bool UAPKIConnectHelper::InjectProviderConfig(nlohmann::json& parameters) {
    // Определяем каталог собственной DLL (там же лежат провайдеры cm-pkcs12_x86/_x64)
    std::string dllDirUtf8;
    if (!GetOwnModuleDir(dllDirUtf8)) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось определить каталог собственной DLL для автоинъекции провайдеров");
        return false;
    }

    // Архитектурный суффикс имени файла провайдера
#ifdef _WIN64
    const std::string archSuffix = "_x64";
#else
    const std::string archSuffix = "_x86";
#endif

    // parameters должен быть объектом
    if (!parameters.is_object()) {
        parameters = nlohmann::json::object();
    }

    if (!parameters.contains("cmProviders")) {
        // Провайдеры не заданы — формируем конфиг по умолчанию (cm-pkcs12 рядом с DLL)
        nlohmann::json provider;
        provider["lib"] = std::string("cm-pkcs12") + archSuffix;

        nlohmann::json cmProviders;
        cmProviders["dir"] = dllDirUtf8;
        cmProviders["allowedProviders"] = nlohmann::json::array({ provider });

        parameters["cmProviders"] = cmProviders;
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Поле cmProviders отсутствовало — добавлен провайдер по умолчанию cm-pkcs12" + archSuffix);
    } else {
        nlohmann::json& cmProviders = parameters["cmProviders"];
        if (!cmProviders.is_object()) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Поле cmProviders имеет некорректный тип (ожидался объект) — пересоздаём");
            cmProviders = nlohmann::json::object();
        }

        // Подставляем каталог DLL, если dir отсутствует или пустой
        bool needDir = true;
        if (cmProviders.contains("dir") && cmProviders["dir"].is_string()) {
            needDir = cmProviders["dir"].get<std::string>().empty();
        }
        if (needDir) {
            cmProviders["dir"] = dllDirUtf8;
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Поле cmProviders.dir отсутствовало/пустое — подставлен каталог собственной DLL");
        }

        // Дописываем арх-суффикс к именам провайдеров, у которых его ещё нет
        if (cmProviders.contains("allowedProviders") && cmProviders["allowedProviders"].is_array()) {
            for (auto& prov : cmProviders["allowedProviders"]) {
                if (prov.is_object() && prov.contains("lib") && prov["lib"].is_string()) {
                    std::string lib = prov["lib"].get<std::string>();
                    bool hasSuffix = (lib.size() >= 4) &&
                        (lib.compare(lib.size() - 4, 4, "_x86") == 0 ||
                         lib.compare(lib.size() - 4, 4, "_x64") == 0);
                    if (!hasSuffix) {
                        prov["lib"] = lib + archSuffix;
                        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "К имени провайдера добавлен арх-суффикс: " + lib + " -> " + lib + archSuffix);
                    }
                }
            }
        }
    }

    // Логируем итоговый конфиг, замаскировав пароли в копии для лога (не в реальном запросе)
    try {
        nlohmann::json masked = parameters;
        MaskPasswords(masked);
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Итоговый инъектированный конфиг INIT: " + masked.dump());
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось сформировать лог инъектированного конфига: " + std::string(e.what()));
    }

    return true;
}

// Проверка успешности выполнения операции на основе JSON-ответа UAPKI.
// Согласно протоколу (docs/UAPKI_Protokol.md) успех определяется полем errorCode (0 = успех).
bool UAPKIConnectHelper::IsOperationSuccess(const std::string& jsonResponse) {
    try {
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Проверка успешности выполнения операции UAPKI");

        // Проверка на пустой ответ
        if (jsonResponse.empty()) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Получен пустой JSON ответ от UAPKI");
            return false;
        }

        nlohmann::json responseJson = nlohmann::json::parse(jsonResponse);

        // Поле errorCode обязательно по протоколу
        if (!responseJson.contains("errorCode")) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Ответ UAPKI не содержит обязательного поля errorCode — несоответствие формату протокола");
            return false;
        }

        if (!responseJson["errorCode"].is_number_integer()) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Поле errorCode имеет некорректный тип (ожидалось целое число)");
            return false;
        }

        int ec = responseJson["errorCode"].get<int>();

        if (ec == 0) {
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Операция UAPKI выполнена успешно (errorCode=0)");
            return true;
        }

        // errorCode != 0 — ошибка: читаем error и method для диагностики
        std::string errorMsg;
        if (responseJson.contains("error") && responseJson["error"].is_string()) {
            errorMsg = responseJson["error"].get<std::string>();
        }
        std::string methodName;
        if (responseJson.contains("method") && responseJson["method"].is_string()) {
            methodName = responseJson["method"].get<std::string>();
        }

        std::string logMsg = "Ошибка UAPKI, errorCode=" + std::to_string(ec);
        if (!methodName.empty()) {
            logMsg += ", method=" + methodName;
        }
        if (!errorMsg.empty()) {
            logMsg += ", " + errorMsg;
        }
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", logMsg);
        return false;
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

        // Разбор параметров: поддерживаем как "сырой" JSON-объект, так и плоский формат "ключ=значение"
        nlohmann::json paramsJson;

        // Определяем первый непробельный символ (обрезаем ведущие пробелы/табы/CR/LF)
        size_t firstNonWs = paramsString.find_first_not_of(" \t\r\n");
        bool looksLikeJson = (firstNonWs != std::string::npos && paramsString[firstNonWs] == '{');

        if (looksLikeJson) {
            // Строка уже является готовым объектом parameters — парсим напрямую,
            // не прогоняя её через ParseParamsString (разблокирует вложенный cmProviders и т.п.)
            try {
                paramsJson = nlohmann::json::parse(paramsString);
                NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Параметры распознаны как готовый JSON-объект");
            }
            catch (const std::exception& e) {
                responseJson = R"({"errorCode":400,"error":"Invalid JSON parameters"})";
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось разобрать параметры как JSON-объект: " + std::string(e.what()));
                return false;
            }
        }
        else {
            // Плоский формат "ключ=значение,ключ=значение" — разбираем через ParseParamsString
            if (!ParseParamsString(paramsString, paramsJson)) {
                responseJson = R"({"errorCode":400,"error":"Invalid parameters format"})";
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось разобрать параметры команды UAPKI");
                return false;
            }
        }

        // Автоинъекция конфигурации провайдеров НКИ для метода INIT (регистронезависимо)
        std::string methodUpper = method;
        std::transform(methodUpper.begin(), methodUpper.end(), methodUpper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (methodUpper == "INIT") {
            if (!InjectProviderConfig(paramsJson)) {
                responseJson = R"({"errorCode":500,"error":"Failed to inject provider configuration"})";
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось выполнить автоинъекцию конфигурации провайдеров для INIT");
                return false;
            }
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

            // Освобождаем память, выделенную функцией process (до любого анализа копии).
            // Порядок гарантирует отсутствие утечек на всех ветках ниже.
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Освобождение памяти ответа с помощью json_free");
            ::json_free(response);
            response = nullptr;

            // Логируем ответ с ограничением длины для больших ответов
            if (responseJson.length() > 2000) {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Response (сокращенный): " + responseJson.substr(0, 2000) + "...");
            } else {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Response: " + responseJson);
            }

            // Проверяем успешность операции по полю errorCode
            bool isSuccess = IsOperationSuccess(responseJson);

            if (isSuccess) {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Команда UAPKI " + method + " выполнена успешно");
            } else {
                NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Команда UAPKI " + method + " завершилась с ошибкой");
            }

            return isSuccess;
        }
        else {
            // Если ответ пустой, формируем JSON с ошибкой в формате протокола
            responseJson = R"({"errorCode":503,"error":"No response from UAPKI library"})";

            // Логируем ошибку
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: No response from library for method " + method);

            return false;
        }
    }
    catch (const std::exception& e) {
        // В случае исключения формируем JSON-ответ с сообщением об ошибке
        responseJson = R"({"errorCode":500,"error":")" + std::string(e.what()) + R"("})";

        // Логируем ошибку
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Exception при выполнении метода " + method + ": " + std::string(e.what()));

        return false;
    }
    catch (...) {
        // В случае неизвестного исключения формируем JSON с ошибкой
        responseJson = R"({"errorCode":500,"error":"Unknown error"})";

        // Логируем неизвестное исключение
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Неизвестное исключение при выполнении метода UAPKI " + method);

        return false;
    }
#else
    // Версия метода для сборки без UAPKI
    (void)paramsString;
    responseJson = R"({"errorCode":503,"error":"UAPKI functionality is not available in this build"})";
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "UAPKI Error: UAPKI functionality is not available in this build. Method: " + method);
    return false;
#endif
}
