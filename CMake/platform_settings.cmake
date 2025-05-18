##
## @file platform_settings.cmake
## @brief Платформо-зависимые настройки
##

##
## @section platform_settings Платформо-залежні налаштування
## @brief Специфічні налаштування для різних платформ
##

if (UNIX)
    if (CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        set(CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
        set(CMAKE_CXX_FLAGS "-m32 ${CMAKE_CXX_FLAGS}")
    else()
        set(CMAKE_C_FLAGS "-m64 ${CMAKE_C_FLAGS}")
        set(CMAKE_CXX_FLAGS "-m64 ${CMAKE_CXX_FLAGS}")
    endif ()
    target_link_libraries(${PROJECT_NAME} -static-libstdc++)
else()
    add_definitions(/MT)
    set(CMAKE_SUPPRESS_REGENERATION 1)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
    target_compile_definitions(${TARGET} PRIVATE _WINDOWS
            _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_options(${TARGET} PRIVATE /utf-8)
    target_link_libraries(${TARGET} PRIVATE ws2_32)
endif()