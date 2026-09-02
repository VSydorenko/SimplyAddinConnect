#include "LocalKeys.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <windows.h>

using nlohmann::json;

bool LoadLocalKeys(const std::wstring& jsonPath, std::vector<LocalKey>& out, std::string& err) {
    out.clear();
    err.clear();

    const DWORD attr = GetFileAttributesW(jsonPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;   // немає файлу -> SKIP, не помилка

    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) { err = "Файл конфігу існує, але не читається"; return false; }

    json root;
    try { f >> root; }
    catch (const std::exception& e) { err = std::string("Некоректний JSON: ") + e.what(); return false; }

    if (!root.contains("keys") || !root["keys"].is_array()) {
        err = "У конфігу немає масиву keys";
        return false;
    }
    std::vector<LocalKey> parsed;   // накопичуємо окремо: out мусить лишитись порожнім при будь-якому return false
    for (const auto& k : root["keys"]) {
        if (!k.is_object()) {
            err = "Запис ключа не є об'єктом JSON";
            return false;
        }
        LocalKey lk;
        lk.id       = k.value("id", std::string());
        lk.path     = k.value("path", std::string());
        lk.password = k.value("password", std::string());
        if (k.contains("expect") && k["expect"].is_object()) {
            lk.expectContainer = k["expect"].value("container", std::string());
            lk.expectSignAlgo  = k["expect"].value("signAlgo", std::string());
        }
        if (lk.id.empty() || lk.path.empty()) {
            err = "Запис ключа без обов'язкових полів id/path";
            return false;
        }
        parsed.push_back(lk);
    }
    out = std::move(parsed);
    return true;
}

const LocalKey* FindLocalKey(const std::vector<LocalKey>& keys, const std::string& id) {
    for (const auto& k : keys) if (k.id == id) return &k;
    return nullptr;
}
