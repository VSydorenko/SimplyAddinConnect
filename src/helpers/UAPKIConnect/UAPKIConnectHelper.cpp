#include "../../core/pch.h"
#include "UAPKIConnectHelper.h"
#include "../ServiceTools.h"
#include <stdexcept>
#include <algorithm>
#include <cctype>

// Подключаем Windows.h для доступа к функциям Windows API
#ifdef _WINDOWS
#include <Windows.h>
#include "version.h"                 // VERSION_FULL для каталога развёртывания провайдера
#include "UAPKIProviderResource.h"   // UAPKI_PROVIDER_RESOURCE_NAME (имя встроенного ресурса)

// Стрингификация неквотированного токена VERSION_FULL (напр. 3.0.2.97) в narrow-литерал "3.0.2.97"
#define UAPKI_STR2(x)   #x
#define UAPKI_STR(x)    UAPKI_STR2(x)
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

    // Провайдер грузится ядром UAPKI через LoadLibraryW (патч сабмодуля, шаг A), поэтому
    // ANSI-совместимость пути больше не требуется — возвращаем каталог как есть.
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

// Разворачивает встроенный ресурс провайдера в каталог %LOCALAPPDATA% / SimplyAddinConnect / providers / <VERSION_FULL>
bool UAPKIConnectHelper::EnsureProviderDeployed(std::string& outUtf8Dir) {
#ifdef _WINDOWS
    // Имя файла провайдера с архитектурным суффиксом
#ifdef _WIN64
    const std::wstring providerFile = L"cm-pkcs12_x64.dll";
#else
    const std::wstring providerFile = L"cm-pkcs12_x86.dll";
#endif
    // Версия (VERSION_FULL из version.h) как компонент каталога — изоляция по версиям DLL.
    // VERSION_FULL — неквотированный токен (напр. 3.0.2.98); стрингифицируем в narrow и конвертируем в wide.
    const std::wstring versionW =
        ServiceTools::U16StringToWString(ServiceTools::SafeMB2WCHAR(UAPKI_STR(VERSION_FULL)));

    // Базовый каталог: %LOCALAPPDATA% (GetEnvironmentVariableW — без новых зависимостей от Shell32)
    std::vector<wchar_t> envBuf(32768, L'\0');
    DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", envBuf.data(), static_cast<DWORD>(envBuf.size()));
    if (envLen == 0 || envLen >= envBuf.size()) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось получить переменную окружения LOCALAPPDATA, код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }
    std::wstring baseDir(envBuf.data(), envLen);
    if (!baseDir.empty() && baseDir.back() != L'\\' && baseDir.back() != L'/') {
        baseDir += L'\\';
    }

    // Каталоги (поэтапно): %LOCALAPPDATA% / SimplyAddinConnect / providers / <VERSION_FULL>
    const std::wstring dirApp       = baseDir + L"SimplyAddinConnect";
    const std::wstring dirProviders = dirApp + L"\\providers";
    const std::wstring dirVersion   = dirProviders + L"\\" + versionW;
    const std::wstring targetDir    = dirVersion + L"\\";
    const std::wstring targetPath   = targetDir + providerFile;

    // Конвертация целевого каталога (UTF-16 → UTF-8) — общая для веток успеха
    auto targetDirToUtf8 = [&targetDir]() {
        std::u16string u16(reinterpret_cast<const char16_t*>(targetDir.c_str()), targetDir.size());
        return ServiceTools::SafeWCHAR2MB(u16);
    };

    // Уже развёрнут ранее (этот процесс или другой) — не перезаписываем
    if (GetFileAttributesW(targetPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        outUtf8Dir = targetDirToUtf8();
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Провайдер уже развёрнут в целевом каталоге, повторное развёртывание не требуется: " + outUtf8Dir);
        return true;
    }

    // Дескриптор собственного модуля (тот же якорь ModuleAnchor, что и в GetOwnModuleDir)
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleAnchor),
            &hMod) || hMod == nullptr) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось получить дескриптор собственного модуля для извлечения ресурса провайдера, код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    // Извлекаем встроенный RCDATA-ресурс провайдера.
    // 10 == RT_RCDATA; объектная цель собирается без UNICODE, поэтому макрос RT_RCDATA
    // раскрылся бы в ANSI-форму (LPSTR) и не подошёл к FindResourceW — используем MAKEINTRESOURCEW.
    HRSRC hRes = FindResourceW(hMod, UAPKI_PROVIDER_RESOURCE_NAME, MAKEINTRESOURCEW(10));
    if (hRes == nullptr) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Встроенный ресурс провайдера не найден (FindResourceW), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }
    HGLOBAL hData = LoadResource(hMod, hRes);
    if (hData == nullptr) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось загрузить ресурс провайдера (LoadResource), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }
    const void* pData = LockResource(hData);
    DWORD resSize = SizeofResource(hMod, hRes);
    if (pData == nullptr || resSize == 0) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Ресурс провайдера пуст или недоступен (LockResource/SizeofResource), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    // Поэтапно создаём каталоги (игнорируя ERROR_ALREADY_EXISTS)
    auto ensureDir = [](const std::wstring& d) -> bool {
        if (CreateDirectoryW(d.c_str(), nullptr)) {
            return true;
        }
        return (GetLastError() == ERROR_ALREADY_EXISTS);
    };
    if (!ensureDir(dirApp) || !ensureDir(dirProviders) || !ensureDir(dirVersion)) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось создать каталог для развёртывания провайдера, код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    // Пишем во временное имя (уникальное по PID) — для атомарной замены против гонки процессов 1С
    const std::wstring tmpPath = targetPath + L".tmp_" + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось создать временный файл провайдера (CreateFileW), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    bool writeOk = true;
    const BYTE* p = static_cast<const BYTE*>(pData);
    DWORD remaining = resSize;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(hFile, p, remaining, &written, nullptr) || written == 0) {
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Ошибка записи файла провайдера (WriteFile), код ошибки: " + std::to_string(static_cast<unsigned long>(GetLastError())));
            writeOk = false;
            break;
        }
        p += written;
        remaining -= written;
    }
    CloseHandle(hFile);

    if (!writeOk) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    // Атомарное перемещение временного файла в целевой
    if (!MoveFileExW(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD moveErr = GetLastError();
        // Могли проиграть гонку другому процессу — но если целевой файл уже есть, это успех
        if (GetFileAttributesW(targetPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(tmpPath.c_str());
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Целевой файл провайдера развёрнут другим процессом — используем существующий");
        } else {
            DeleteFileW(tmpPath.c_str());
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось переместить временный файл провайдера в целевой (MoveFileExW), код ошибки: " + std::to_string(static_cast<unsigned long>(moveErr)));
            return false;
        }
    }

    outUtf8Dir = targetDirToUtf8();
    NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Провайдер cm-pkcs12 развёрнут из встроенного ресурса в каталог: " + outUtf8Dir);
    return true;
#else
    (void)outUtf8Dir;
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Развёртывание встроенного провайдера поддерживается только на платформе Windows");
    return false;
#endif
}

// Определяет каталог провайдера: сначала рядом с собственной DLL, иначе — развёртывание ресурса
bool UAPKIConnectHelper::ResolveProviderDir(std::string& outUtf8Dir) {
#ifdef _WINDOWS
    // Имя файла провайдера с архитектурным суффиксом
#ifdef _WIN64
    const std::wstring providerFile = L"cm-pkcs12_x64.dll";
#else
    const std::wstring providerFile = L"cm-pkcs12_x86.dll";
#endif

    // Шаг 2: провайдер лежит рядом с собственной DLL (тесты, ручные/не-1С развёртывания)
    std::string dllDirUtf8;
    if (GetOwnModuleDir(dllDirUtf8)) {
        std::u16string u16dir = ServiceTools::SafeMB2WCHAR(dllDirUtf8.c_str());
        std::wstring candidate = ServiceTools::U16StringToWString(u16dir);
        if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/') {
            candidate += L'\\';
        }
        candidate += providerFile;

        DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            outUtf8Dir = dllDirUtf8;
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Провайдер найден рядом с собственной DLL, используем каталог: " + dllDirUtf8);
            return true;
        }
    } else {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось определить каталог собственной DLL — переходим к развёртыванию встроенного ресурса");
    }

    // Шаг 3: разворачиваем встроенный ресурс в %LOCALAPPDATA%
    return EnsureProviderDeployed(outUtf8Dir);
#else
    (void)outUtf8Dir;
    NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Определение каталога провайдера поддерживается только на платформе Windows");
    return false;
#endif
}

// Автоинъекция конфигурации провайдеров НКИ в параметры метода INIT
bool UAPKIConnectHelper::InjectProviderConfig(nlohmann::json& parameters) {
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
        // Провайдеры не заданы — определяем каталог тройным поиском (рядом с DLL или развёртывание ресурса)
        std::string providerDirUtf8;
        if (!ResolveProviderDir(providerDirUtf8)) {
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось определить каталог провайдера для автоинъекции конфигурации INIT");
            return false;
        }

        nlohmann::json provider;
        provider["lib"] = std::string("cm-pkcs12") + archSuffix;

        nlohmann::json cmProviders;
        cmProviders["dir"] = providerDirUtf8;
        cmProviders["allowedProviders"] = nlohmann::json::array({ provider });

        parameters["cmProviders"] = cmProviders;
        NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Поле cmProviders отсутствовало — добавлен провайдер по умолчанию cm-pkcs12" + archSuffix);
    } else {
        nlohmann::json& cmProviders = parameters["cmProviders"];
        if (!cmProviders.is_object()) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Поле cmProviders имеет некорректный тип (ожидался объект) — пересоздаём");
            cmProviders = nlohmann::json::object();
        }

        // Определяем, задал ли вызывающий каталог явно (шаг 1 — уважаем как есть)
        bool needDir = true;
        if (cmProviders.contains("dir") && cmProviders["dir"].is_string()) {
            needDir = cmProviders["dir"].get<std::string>().empty();
        }
        if (needDir) {
            // Каталог не задан — тройной поиск (рядом с DLL или развёртывание ресурса)
            std::string providerDirUtf8;
            if (!ResolveProviderDir(providerDirUtf8)) {
                NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Не удалось определить каталог провайдера для автоинъекции конфигурации INIT");
                return false;
            }
            cmProviders["dir"] = providerDirUtf8;
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Поле cmProviders.dir отсутствовало/пустое — подставлен каталог провайдера: " + providerDirUtf8);
        } else {
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "Поле cmProviders.dir задано вызывающим — используется как есть, развёртывание не выполняется");
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

// Проверка результата INIT: сравнивает число реально загруженных провайдеров (result.countCmProviders)
// с числом инъектированных (cmProviders.allowedProviders). При недоборе — WARN с деталями.
// JSON-ответ НЕ модифицируется (прозрачность для 1С), успешность операции не меняется.
void UAPKIConnectHelper::WarnIfProvidersNotLoaded(const nlohmann::json& injectedParams, const std::string& responseJson) {
    try {
        // Сколько провайдеров было инъектировано/передано + диагностические детали
        size_t expected = 0;
        std::string dirInfo;
        std::string libInfo;
        if (injectedParams.is_object() && injectedParams.contains("cmProviders") && injectedParams["cmProviders"].is_object()) {
            const nlohmann::json& cm = injectedParams["cmProviders"];
            if (cm.contains("dir") && cm["dir"].is_string()) {
                dirInfo = cm["dir"].get<std::string>();
            }
            if (cm.contains("allowedProviders") && cm["allowedProviders"].is_array()) {
                expected = cm["allowedProviders"].size();
                for (const auto& prov : cm["allowedProviders"]) {
                    if (prov.is_object() && prov.contains("lib") && prov["lib"].is_string()) {
                        if (!libInfo.empty()) {
                            libInfo += ", ";
                        }
                        libInfo += prov["lib"].get<std::string>();
                    }
                }
            }
        }

        // Если провайдеры не инъектировались — проверять нечего
        if (expected == 0) {
            return;
        }

        // Читаем result.countCmProviders из ответа
        nlohmann::json resp = nlohmann::json::parse(responseJson);
        long long loaded = -1;
        if (resp.contains("result") && resp["result"].is_object() &&
            resp["result"].contains("countCmProviders") && resp["result"]["countCmProviders"].is_number_integer()) {
            loaded = resp["result"]["countCmProviders"].get<long long>();
        }

        if (loaded < 0) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Ответ INIT не содержит result.countCmProviders — невозможно подтвердить загрузку провайдеров; ожидалось: " + std::to_string(expected) + ", dir: " + dirInfo + ", lib: " + libInfo);
            return;
        }

        if (static_cast<size_t>(loaded) < expected) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "UAPKI загрузил меньше провайдеров, чем ожидалось: загружено " + std::to_string(loaded) + " из " + std::to_string(expected) + "; dir: " + dirInfo + ", lib: " + libInfo + ". Проверьте наличие файла провайдера и права доступа");
        } else {
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "INIT: загружено провайдеров " + std::to_string(loaded) + " из " + std::to_string(expected));
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось проверить число загруженных провайдеров в ответе INIT: " + std::string(e.what()));
    }
    catch (...) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Неизвестная ошибка при проверке числа загруженных провайдеров в ответе INIT");
    }
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

            // Для INIT дополнительно сверяем число реально загруженных провайдеров с ожидаемым.
            // Диагностика молчаливого недобора провайдеров (ответ/успешность не меняем).
            if (methodUpper == "INIT") {
                WarnIfProvidersNotLoaded(paramsJson, responseJson);
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
