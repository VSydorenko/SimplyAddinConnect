#pragma once
#include <string>
#include <vector>

struct LocalKey {
    std::string id;
    std::string path;
    std::string password;
    std::string expectContainer;
    std::string expectSignAlgo;
};

// Файлу немає            -> true,  out порожній, err порожній  (SKIP на боці кейса)
// Файл є, але зіпсований -> false, err із причиною             (FAIL: наміри заявлені)
bool LoadLocalKeys(const std::wstring& jsonPath, std::vector<LocalKey>& out, std::string& err);
const LocalKey* FindLocalKey(const std::vector<LocalKey>& keys, const std::string& id);
