#pragma once
// Локаленезалежне форматування грошової суми.
//
// Через ЦІЛІ КОПІЙКИ, а не printf: `%.2f` у ru/uk-локалі дає КОМУ замість крапки,
// а протокол термінала й payload драйвера чекають саме крапку. Помилки при цьому
// немає — термінал просто відхиляє суму, тож баг знайшовся б аж на живому обладнанні.
#include <cmath>
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
