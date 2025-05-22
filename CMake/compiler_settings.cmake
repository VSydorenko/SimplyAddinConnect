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
# target_include_directories(mscomm_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(mswinsock_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(ecrcommx_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(posapi_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(serial_port_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(tcp_connection_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(privat_json_helper_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(ecr_privat_json_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(protocols_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
# target_include_directories(protocols_component PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})

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
# target_precompile_headers(privat_json_helper_component PRIVATE src/core/pch.h)
# target_precompile_headers(ecr_json_transport_component PRIVATE src/core/pch.h)
# target_precompile_headers(ecr_privat_json_component PRIVATE src/core/pch.h)
# target_precompile_headers(protocols_component PRIVATE src/core/pch.h)

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

# set_target_properties(privat_json_helper_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# set_target_properties(ecr_privat_json_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# Додаємо потрібні визначення компілятора
target_compile_definitions(base_component PRIVATE UNICODE _UNICODE)
target_compile_definitions(test_component PRIVATE UNICODE _UNICODE)
target_compile_definitions(helpers_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(mscomm_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(mswinsock_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(ecrcommx_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(posapi_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(serial_port_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(tcp_connection_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(privat_json_helper_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(ecr_privat_json_component PRIVATE UNICODE _UNICODE)
# target_compile_definitions(protocols_component PRIVATE UNICODE _UNICODE)

if (NOT UNIX)
    target_compile_definitions(base_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_definitions(test_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_definitions(helpers_component PRIVATE _WINDOWS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(mscomm_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(mswinsock_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(ecrcommx_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(posapi_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(serial_port_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(tcp_connection_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(privat_json_helper_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(ecr_privat_json_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    # target_compile_definitions(protocols_component PRIVATE _WINDOWS
    #     _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    
    # Добавляем определения компилятора для UAPKI компонентов
    if(BUILD_WITH_UAPKI)
        target_compile_definitions(uapki_helper_component PRIVATE _WINDOWS
            _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
        target_compile_definitions(uapki_component PRIVATE _WINDOWS
            _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    endif()
      target_compile_options(base_component PRIVATE /utf-8)
    target_compile_options(test_component PRIVATE /utf-8)
    target_compile_options(helpers_component PRIVATE /utf-8)
    # target_compile_options(mscomm_component PRIVATE /utf-8)
    # target_compile_options(mswinsock_component PRIVATE /utf-8)
    # target_compile_options(ecrcommx_component PRIVATE /utf-8)
    # target_compile_options(posapi_component PRIVATE /utf-8)
    # target_compile_options(serial_port_component PRIVATE /utf-8)
    # target_compile_options(tcp_connection_component PRIVATE /utf-8)
    # target_compile_options(privat_json_helper_component PRIVATE /utf-8)
    # target_compile_options(ecr_privat_json_component PRIVATE /utf-8)
    # target_compile_options(protocols_component PRIVATE /utf-8)
    
    # Добавляем опцию компилятора /utf-8 для UAPKI компонентов
    if(BUILD_WITH_UAPKI)
        target_compile_options(uapki_helper_component PRIVATE /utf-8)
        target_compile_options(uapki_component PRIVATE /utf-8)
    endif()
endif()

# Добавляем зависимости для protocols_component
# add_dependencies(protocols_component base_component spdlog)
# add_dependencies(protocols_component helpers_component)
# add_dependencies(protocols_component nlohmann_json)
# add_dependencies(protocols_component serial_port_component)
# add_dependencies(protocols_component tcp_connection_component)
# add_dependencies(protocols_component privat_json_helper_component)

# Обновляем зависимости для ecr_privat_json_component
# add_dependencies(ecr_privat_json_component protocols_component)