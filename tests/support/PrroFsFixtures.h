#pragma once
// Завантаження шаблонів фікстур tests/data/prro_fs/<name>: {N} -> ORDERNUM,
// {REG} -> CASHREGISTERNUM. Плейсхолдери ASCII, тож заміна на байтах не чіпає
// кодування файлу (windows-1251 або UTF-8). Порожній рядок — файл не прочитано.
#include <fstream>
#include <sstream>
#include <string>

namespace prrofs {

inline std::string LoadFixture(const std::string& dir, const std::string& name,
                               long long orderNum, const std::string& registrar) {
    std::ifstream f(dir + "/" + name, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    auto replaceAll = [&s](const std::string& from, const std::string& to) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    };
    replaceAll("{N}", std::to_string(orderNum));
    replaceAll("{REG}", registrar);
    return s;
}

// Останнє число в тексті (для «…повинен дорівнювати N»); -1 — чисел немає.
// Тестовий хелпер, не фікстура — тримається тут (не статичною функцією в
// tests/prro_fs_state_selftest.cpp), щоб Task 5 могла взяти його з цього
// заголовка, а не дублювати (архітекторське рішення, Task 3).
inline long long LastNumber(const std::string& s) {
    const size_t e = s.find_last_of("0123456789");
    if (e == std::string::npos) return -1;
    size_t b = e;
    while (b > 0 && s[b - 1] >= '0' && s[b - 1] <= '9') --b;
    return std::stoll(s.substr(b, e - b + 1));
}

}  // namespace prrofs
