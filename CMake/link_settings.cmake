##
## @file link_settings.cmake
## @brief Настройки линковки проекта
##

# Добавляем необходимые библиотеки для решения проблем линковки
if(MSVC)
    # Для функций strerror и strncpy требуется legacy_stdio_definitions
    target_link_libraries(${PROJECT_NAME} PRIVATE legacy_stdio_definitions.lib)
    
    # Опция для предотвращения конфликтов библиотек
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:MSVCRT")
endif()
