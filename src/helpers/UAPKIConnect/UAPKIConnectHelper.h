#pragma once

#include <string>
#include <nlohmann/json.hpp>

/**
 * @class UAPKIConnectHelper
 * @brief Вспомогательный класс для работы с библиотекой UAPKI
 *
 * Класс предоставляет интерфейс для выполнения команд UAPKI через JSON-запросы.
 *
 * Параметры принимаются в двух форматах:
 *  - "сырой" JSON-объект (строка начинается с '{') — используется напрямую как объект
 *    parameters запроса (поддерживает вложенные структуры: cmProviders, сложные SIGN и т.п.);
 *  - плоский формат "ключ=значение,ключ=значение" — разбирается через ParseParamsString
 *    (только для простых плоских параметров, вложенность не поддерживается).
 *
 * Для метода INIT выполняется автоинъекция конфигурации провайдеров НКИ через InjectProviderConfig
 * (тройной порядок поиска каталога провайдера: явный dir вызывающего → провайдер рядом с DLL →
 * развёртывание встроенного ресурса в %LOCALAPPDATA%) + арх-суффикс имени провайдера. После INIT
 * проверяется число реально загруженных провайдеров (ProvidersLoadedOrFail): ноль при непустом
 * allowedProviders — ошибка 502 NO_CM_PROVIDERS_LOADED.
 *
 * Успешность операции определяется по полю errorCode ответа (0 = успех) согласно
 * протоколу UAPKI (extern/uapki/doc/UAPKI-PM-2.0.16.md).
 */
class UAPKIConnectHelper {
public:
    /**
     * @brief Выполняет команду UAPKI
     * 
     * @param method Название метода UAPKI
     * @param paramsString Параметры в формате "ключ=значение,ключ=значение"
     * @param responseJson Строка для записи ответа в формате JSON
     * @return true если команда выполнена успешно
     * @return false в случае ошибки
     */
    static bool ExecuteUapkiCommand(const std::string& method, const std::string& paramsString, std::string& responseJson);

private:
    /**
     * @brief Проверяет успешность выполнения операции по JSON-ответу
     * 
     * @param jsonResponse Строка с JSON-ответом от UAPKI
     * @return true если операция выполнена успешно
     * @return false если операция завершилась с ошибкой
     */
    static bool IsOperationSuccess(const std::string& jsonResponse);

    /**
     * @brief Разбирает строку параметров в формате "ключ=значение,ключ=значение" в JSON объект
     * 
     * @param paramsString Строка параметров в формате "ключ=значение,ключ=значение"
     * @param paramsJson Объект JSON для записи параметров
     * @return true если разбор выполнен успешно
     * @return false в случае ошибки формата
     */
    static bool ParseParamsString(const std::string& paramsString, nlohmann::json& paramsJson);

    /**
     * @brief Автоинъекция конфигурации провайдеров НКИ в параметры метода INIT
     *
     * Тройной порядок определения каталога провайдера, когда вызывающий НЕ задал cmProviders.dir:
     *  1) явный непустой cmProviders.dir — уважается как есть (ничего не разворачивается);
     *  2) провайдер cm-pkcs12_<arch>.dll рядом с собственной DLL — используется каталог DLL;
     *  3) иначе — развёртывание встроенного ресурса в %LOCALAPPDATA% (см. ResolveProviderDir).
     * Дополнительно дописывает архитектурный суффикс (_x86/_x64) к именам провайдеров. Если
     * cmProviders отсутствует — формирует конфиг по умолчанию с провайдером cm-pkcs12.
     *
     * @param parameters JSON-объект параметров INIT (модифицируется на месте)
     * @return true при успехе; false если не удалось определить каталог провайдера
     */
    static bool InjectProviderConfig(nlohmann::json& parameters);

    /**
     * @brief Определяет каталог провайдера (шаги 2→3 тройного поиска)
     *
     * Сначала проверяет наличие cm-pkcs12_<arch>.dll рядом с собственной DLL (GetOwnModuleDir);
     * если найден — возвращает каталог DLL. Иначе разворачивает встроенный ресурс через
     * EnsureProviderDeployed и возвращает каталог развёртывания.
     *
     * @param outUtf8Dir Выходной параметр: каталог провайдера в UTF-8 (со слешем на конце)
     * @return true при успехе; false при ошибке или на не-Windows платформе
     */
    static bool ResolveProviderDir(std::string& outUtf8Dir);

    /**
     * @brief Разворачивает встроенный ресурс провайдера cm-pkcs12_<arch>.dll на диск
     *
     * Целевой каталог: %LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\. Если файл
     * уже развёрнут — возвращает каталог без перезаписи. Иначе извлекает RCDATA-ресурс из
     * собственного модуля, поэтапно создаёт каталоги и пишет файл через временное имя с
     * последующим атомарным MoveFileExW (устойчивость к гонке параллельных процессов 1С).
     *
     * @param outUtf8Dir Выходной параметр: каталог развёртывания в UTF-8 (со слешем на конце)
     * @return true при успехе; false при ошибке Windows API/записи или на не-Windows платформе
     */
    static bool EnsureProviderDeployed(std::string& outUtf8Dir);

    /**
     * @brief Определяет каталог собственной DLL (не хост-процесса 1cv8.exe)
     *
     * Использует GetModuleHandleExW(FROM_ADDRESS) с адресом внутри текущего модуля.
     * Возвращает каталог с завершающим слешем в кодировке UTF-8.
     *
     * @param outUtf8Dir Выходной параметр: каталог DLL в UTF-8 (со слешем на конце)
     * @return true при успехе; false при ошибке Windows API или на не-Windows платформе
     */
    static bool GetOwnModuleDir(std::string& outUtf8Dir);

    /**
     * @brief Перевіряє, що провайдери НКІ справді піднялись, і формує вердикт.
     *
     * Правило: просили провайдерів (cmProviders.allowedProviders не порожній), а
     * завантажилось НУЛЬ — INIT є помилкою для 1С. Недобір (1 з 2) помилкою не є:
     * перелік законно може містити відсутній на машині провайдер.
     *
     * Відповідь бібліотеки зберігається цілою у полі uapkiResponse — прозорість
     * не втрачається, втрачається брехня про успіх.
     *
     * @param injectedParams параметри INIT після автоінʼєкції
     * @param responseJson   [in,out] відповідь; при нулі провайдерів ПЕРЕЗАПИСУЄТЬСЯ
     * @return true — усе гаразд; false — провайдерів нуль, відповідь замінено
     */
    static bool ProvidersLoadedOrFail(const nlohmann::json& injectedParams, std::string& responseJson);

    /**
     * @brief Робить INIT ідемпотентним для 1С.
     *
     * UAPKI на повторний INIT у тому самому екземплярі віддає 4106
     * ALREADY_INITIALIZED із порожнім результатом, хоча бібліотека жива й
     * придатна. Постумова «бібліотеку ініціалізовано» досягнута, тож для 1С це
     * успіх. Стан провайдерів НЕ вгадується й НЕ кешується — він МІРЯЄТЬСЯ
     * методом PROVIDERS, який віддає живий CmProviders::count().
     *
     * Порівнює параметри повторного INIT (без skipSelfTest) з параметрами справжньої
     * ініціалізації: та сама конфігурація — успіх; інша — 4106 з result.configMismatch.
     *
     * @param params       параметри INIT після автоінʼєкції
     * @param responseJson [in,out] відповідь INIT; при спрацюванні замінюється
     * @return true — відповідь замінено на успішну; false — залишити як є
     */
    static bool HandleAlreadyInitialized(const nlohmann::json& params, std::string& responseJson);

    /**
     * @brief Рекурсивно маскирует значения полей "password" на любом уровне JSON
     *
     * Используется для безопасного логирования конфигурации (маскирует копию, не оригинал).
     *
     * @param node Узел JSON (модифицируется на месте)
     */
    static void MaskPasswords(nlohmann::json& node);
};