##
## @file dependencies.cmake
## @brief Настройка внешних зависимостей проекта
##

# Додаємо бібліотеку nlohmann/json для парсингу JSON
set(NLOHMANN_JSON_DIR ${CMAKE_SOURCE_DIR}/extern/nlohmann_json)
set(NLOHMANN_JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extern/nlohmann_json/single_include)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})

# Додаємо бібліотеку spdlog для логування
set(SPDLOG_DIR ${CMAKE_SOURCE_DIR}/extern/spdlog)
set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extern/spdlog/include)
# Отключаем сборку примеров, тестов и бенчмарков spdlog
option(SPDLOG_BUILD_EXAMPLE "" OFF)
option(SPDLOG_BUILD_TESTS "" OFF)
option(SPDLOG_BUILD_BENCH "" OFF)
option(SPDLOG_INSTALL "" OFF)
# Подключаем spdlog через add_subdirectory
add_subdirectory(${SPDLOG_DIR})