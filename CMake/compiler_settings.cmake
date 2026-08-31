##
## @file compiler_settings.cmake
## @brief Настройки компилятора
##

##
## @section compiler_settings Налаштування компіляції
## @brief Тут налаштовуються параметри компіляції для всіх компонентів
##

# Створюємо спільні налаштування для всіх таргетів
target_include_directories(base_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
target_include_directories(test_component PRIVATE include src ${SPDLOG_INCLUDE_DIR})
target_include_directories(helpers_component PRIVATE include src ${SPDLOG_INCLUDE_DIR})

# Следующие строки закомментированы, так как компоненты пока не существуют
# target_include_directories(ecrcommx_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(posapi_component PRIVATE include ${SPDLOG_INCLUDE_DIR})

##
## @section precompiled_headers Налаштування предкомпільованих заголовків
## @brief Додаємо підтримку передзаголовочного файлу для прискорення компіляції
##

# Налаштування предкомпільованих заголовків для всіх компонентів
target_precompile_headers(base_component PRIVATE src/core/pch.h)
target_precompile_headers(test_component PRIVATE src/core/pch.h)
target_precompile_headers(helpers_component PRIVATE src/core/pch.h)
target_precompile_headers(transport_component PRIVATE src/core/pch.h)

# Закоментовані налаштування для компонентів, які ще не існують
# target_precompile_headers(ecrcommx_component PRIVATE src/core/pch.h)
# target_precompile_headers(posapi_component PRIVATE src/core/pch.h)

## @brief Налаштування властивостей об'єктних бібліотек
## @note При додаванні нового компоненту, скопіюйте ці налаштування для вашого компоненту
set_target_properties(base_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

set_target_properties(test_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

set_target_properties(helpers_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# set_target_properties(ecrcommx_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# set_target_properties(posapi_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# Додаємо потрібні визначення компілятора
target_compile_definitions(base_component PRIVATE UNICODE _UNICODE)
target_compile_definitions(test_component PRIVATE UNICODE _UNICODE)
target_compile_definitions(helpers_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(ecrcommx_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(posapi_component PRIVATE UNICODE _UNICODE)

if (NOT UNIX)
    target_compile_definitions(base_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_definitions(test_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_definitions(helpers_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(ecrcommx_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(posapi_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

    # Добавляем определения компилятора для UAPKI компонентов
    if(BUILD_WITH_UAPKI)
        target_compile_definitions(uapki_connect_helper_component PRIVATE _WINDOWS
            _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
        target_compile_definitions(uapki_connect_component PRIVATE _WINDOWS
            _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    endif()
      target_compile_options(base_component PRIVATE /utf-8)
    target_compile_options(test_component PRIVATE /utf-8)
    target_compile_options(helpers_component PRIVATE /utf-8)
    target_compile_options(wire_component PRIVATE /utf-8)
    target_compile_options(transport_component PRIVATE /utf-8)
    target_compile_options(platform_component PRIVATE /utf-8)
    target_compile_options(driver_ecr_privatjson_component PRIVATE /utf-8)
    target_compile_options(ecr_facade_component PRIVATE /utf-8)
    target_compile_options(driver_label_printer_component PRIVATE /utf-8)
    target_compile_options(label_facade_component PRIVATE /utf-8)
    # УВАГА: цей перелік ПОІМЕННИЙ. Нова ціль без /utf-8 компілюється в ANSI-кодуванні,
    # і кириличні u"..."-літерали мовчки спотворюються: англійські імена методів
    # потрапляють у DLL, російські — ні, а 1С каже «Метод объекта не обнаружен».
    # Помилки збірки при цьому НЕМАЄ. Додаючи компоненту — додай рядок і сюди.
    target_compile_options(ecr_bpo_facade_component PRIVATE /utf-8)
    # target_compile_options(ecrcommx_component PRIVATE /utf-8)
    # target_compile_options(posapi_component PRIVATE /utf-8)

    # Добавляем опцию компилятора /utf-8 для UAPKI компонентов
    if(BUILD_WITH_UAPKI)
        target_compile_options(uapki_connect_helper_component PRIVATE /utf-8)
        target_compile_options(uapki_connect_component PRIVATE /utf-8)
    endif()
endif()
