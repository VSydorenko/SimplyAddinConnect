##
## @file CMakeLists.txt
## @brief Конфігурація збірки зовнішньої компоненти 1С
## @version 1.0
## author SimplyAddinConnect Team
##

cmake_minimum_required(VERSION 3.16)

# Включаем модульный файл с опциями
include(CMake/options.cmake)

project(SimplyAddinConnect)
set(TARGET SimplyAddinConnect)

# Подключаем все модульные файлы
include(CMake/dependencies.cmake)
include(CMake/components.cmake)
include(CMake/compiler_settings.cmake)
include(CMake/output_settings.cmake)
include(CMake/platform_settings.cmake)

# Подключаем файл настройки UAPKI только если включена опция
if(BUILD_WITH_UAPKI)
    include(CMake/uapki_settings.cmake)
endif()

# Подключаем тесты, если включена соответствующая опция
if(BUILD_TESTS)
    add_subdirectory(tests)
    message(STATUS "Test suite enabled")
else()
    message(STATUS "Test suite disabled")
endif()
