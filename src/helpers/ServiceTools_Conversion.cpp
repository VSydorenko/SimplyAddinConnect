#include "../core/pch.h"

#include "ServiceTools.h"
#include "../core/AddInNative.h"

/**
 * @file ServiceTools_Conversion.cpp
 * @brief Реализация функций для безопасной конвертации строк между различными кодировками
 * 
 * Этот файл содержит набор функций для безопасной конвертации строк между кодировками UTF-8, UTF-16 и wchar_t.
 * Все функции используют публичные методы класса AddInNative для конвертации, обеспечивая
 * правильное управление памятью и безопасное преобразование строк.
 */

namespace ServiceTools {

/**
 * @brief Безопасно конвертирует строку UTF-8 в строку UTF-16
 * 
 * @param str Входная строка в формате UTF-8
 * @return std::u16string Строка в формате UTF-16
 * 
 * @details Использует статический метод AddInNative::MB2WCHAR для конвертации,
 * обеспечивая правильное управление памятью через std::u16string.
 */
std::u16string SafeMB2WCHAR(const char* str) {
    if (!str) {
        return std::u16string();
    }
    
    return AddInNative::MB2WCHAR(str);
}

/**
 * @brief Безопасно конвертирует строку UTF-16 в строку UTF-8
 * 
 * @param str Входная строка в формате UTF-16
 * @return std::string Строка в формате UTF-8
 * 
 * @details Использует статический метод AddInNative::WCHAR2MB для конвертации,
 * обеспечивая правильное управление памятью через std::string.
 */
std::string SafeWCHAR2MB(const std::u16string& str) {
    if (str.empty()) {
        return std::string();
    }
    
    // Используем string_view для избежания дополнительных копирований
    std::basic_string_view<WCHAR_T> view(reinterpret_cast<const WCHAR_T*>(str.data()), str.size());
    return AddInNative::WCHAR2MB(view);
}

/**
 * @brief Конвертирует строку UTF-16 (std::u16string) в широкую строку (std::wstring)
 * 
 * @param str Входная строка в формате UTF-16
 * @return std::wstring Строка в формате wstring для использования с Windows API
 * 
 * @details Преобразует U16String в WString для работы с Windows API функциями.
 * Рассчитывает на одинаковое внутреннее представление символов в обоих типах строк.
 */
std::wstring U16StringToWString(const std::u16string& str) {
    return std::wstring(reinterpret_cast<const wchar_t*>(str.c_str()), str.length());
}

/**
 * @brief Получает имя компонента по его указателю
 * 
 * @param component Указатель на компонент
 * @return std::string Имя компонента, готовое для использования в логах
 * 
 * @details Автоматически получает имя типа компонента с помощью RTTI и возвращает
 * его в формате, готовом для использования в логах. Если компонент недействителен или
 * тип не удалось определить, возвращает дефолтное имя.
 */

} // namespace ServiceTools
