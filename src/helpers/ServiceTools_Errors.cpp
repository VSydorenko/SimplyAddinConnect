#include "ServiceTools.h"
#include "../core/AddInNative.h"
#include <string>
#include <Windows.h>

/**
 * @file ServiceTools_Errors.cpp
 * @brief Реализация функций для обработки ошибок
 * 
 * Этот файл содержит набор функций для обработки ошибок и добавления сообщений
 * об ошибках в компоненты 1С. Обеспечивает механизмы создания функций обратного
 * вызова для обработки ошибок.
 */

namespace ServiceTools {

/**
 * @brief Добавляет ошибку в компонент AddInNative
 * 
 * @param component Указатель на объект компонента
 * @param description Описание ошибки в формате u16string
 * @param code Код ошибки
 * @return true, если ошибка успешно добавлена
 * @return false, если произошла ошибка при добавлении
 */
bool AddComponentError(AddInNative* component, const std::u16string& description, int32_t code) {
    // Получаем имя компонента для логирования
    std::string componentName = component ? GetComponentName(component) : "General";
    
    // Логируем ошибку, используя функцию конвертации
    std::string descriptionStr = SafeWCHAR2MB(description);
    Error(componentName, "[" + componentName + "] Ошибка компонента: " + descriptionStr + ", код: " + std::to_string(code));
    
    // Если компонент не NULL, вызываем метод AddError компонента
    if (component) {
        return component->AddError(description, code);
    }
    
    // Для nullptr (нейтрального логирования) возвращаем true, поскольку ошибка уже залогирована
    return true;
}

/**
 * @brief Возвращает код последней ошибки Windows
 * 
 * @return uint32_t Код ошибки Windows
 */
uint32_t GetLastError() {
    return ::GetLastError();
}

/**
 * @brief Возвращает текстовое описание ошибки по ее коду
 * 
 * @param errorCode Код ошибки Windows
 * @return std::wstring Текстовое описание ошибки
 */
std::wstring GetErrorDescription(uint32_t errorCode) {
    LPWSTR messageBuffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
        (LPWSTR)&messageBuffer, 0, NULL);

    std::wstring message(messageBuffer, size);
    LocalFree(messageBuffer);

    // Удаляем переносы строки в конце сообщения, если они есть
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r')) {
        message.pop_back();
    }

    return message;
}

} // namespace ServiceTools
