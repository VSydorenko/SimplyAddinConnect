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

# ixwebsocket з проєкту ВИДАЛЕНО (сабмодуль теж). Він тягнувся в головну DLL
# заради TransportWSClient, якого не створював жоден драйвер, а останнім його
# споживачем лишався ручний tests/uapki_fiscal_emulator — тому HTTP-сервер там
# замінено власним мінімальним (tests/support/MiniHttpServer.*, ~300 рядків).
# Тримати мережеву бібліотеку й умовний механізм її підключення заради одного
# тестового HTTP-сервера — дорожче, ніж мати цей сервер своїм.

# Статичні бібліотеки для всього проєкту (spdlog, об'єктні цілі компонент).
# Глобальна змінна CMake, а не примха окремої залежності.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# --- Статичний CRT (/MT) — ПОЛІТИКА ПРОЄКТУ, а не налаштування залежності -----
# Раніше цей блок стояв під заголовком «для ixwebsocket» і виглядав як частина
# налаштування тієї бібліотеки. Насправді він задає CRT для ВСЬОГО проєкту:
# зовнішня компонента 1С мусить лінкуватися статично, інакше на машині клієнта
# вона вимагатиме встановленого VC++ Redistributable і 1С просто не завантажить
# її («не вдалося підключити зовнішню компоненту»).
#
# ПОРЯДОК ВАЖЛИВИЙ: блок стоїть ПІСЛЯ add_subdirectory(spdlog) — саме так було
# й раніше, коли він ховався під заголовком «для ixwebsocket». Не переставляти:
# зміна порядку змінює, які цілі підхоплять прапорці, і ламається це МОВЧКИ —
# лише на машині без Redistributable.
if(MSVC)
    # Статична багатопотокова бібліотека часу виконання
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Підміна /MD -> /MT у прапорцях компілятора
    foreach(flag_var
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO)
        if(${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
    
    # Добавляем определения для статической компоновки
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

# Історична примітка (актуальна й без ixwebsocket): рядок
#   set(BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
# колись стояв тут «для ixwebsocket», хоча той цю змінну не читає. Насправді він
# FORCE-затирав ОДНОЙМЕННУ опцію проєкту (CMake/options.cmake) і ламав
# -DBUILD_TESTS=ON: add_subdirectory(tests) у кореневому CMakeLists ніколи не
# виконувався. Не повертати.
