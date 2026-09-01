#pragma once
// Локаленезалежне форматування грошової суми.
//
// Через ЦІЛІ КОПІЙКИ, а не printf: `%.2f` у ru/uk-локалі дає КОМУ замість крапки,
// а протокол термінала й payload драйвера чекають саме крапку. Помилки при цьому
// немає — термінал просто відхиляє суму, тож баг знайшовся б аж на живому обладнанні.
#include <cmath>
#include <cstdlib>
#include <string>

inline std::string MoneyToString(double amount) {
    const bool negative = amount < 0;
    const long long cents = std::llround(std::fabs(amount) * 100.0);
    std::string s = (negative ? "-" : "") + std::to_string(cents / 100) + ".";
    const long long frac = cents % 100;
    if (frac < 10) s += '0';
    s += std::to_string(frac);
    return s;
}

// Зворотний розбір: сума з payload драйвера в double. Пара до MoneyToString і
// живе поруч саме тому — обидві сторони мають однаково розуміти роздільник.
//
// Локаль процесу — "C", тож strtod чекає КРАПКУ: рівно те, що пише MoneyToString.
// Не вдалося розібрати (порожньо, сміття, хвіст після числа) → false, і вхідне
// значення НЕ перетирається: краще лишити суму, яку 1С запитала, ніж занулити її.
inline bool TryMoneyFromString(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || (end && *end != '\0')) return false;
    out = v;
    return true;
}
