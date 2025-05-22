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

# Додаємо бібліотеку ixwebsocket для роботи з WebSocket
set(IXWEBSOCKET_DIR ${CMAKE_SOURCE_DIR}/extern/ixwebsocket)
set(IXWEBSOCKET_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extern/ixwebsocket)

# Підготовлюємо функцію для відключення попереджень в ixwebsocket
function(disable_warnings_for_target target_name)
  if(MSVC)
    # Параметри для компілятора MSVC (Visual Studio)
    target_compile_options(${target_name} PRIVATE 
      /wd4244   # преобразование "тип1" в "тип2", возможна потеря данных
      /wd4267   # преобразование из "size_t" в "тип", возможна потеря данных
      /wd4305   # усечение константы
      /wd4996   # устаревшая функция
    )
  else()
    # Параметри для GCC/Clang
    target_compile_options(${target_name} PRIVATE 
      -Wno-conversion
      -Wno-sign-conversion
      -Wno-unused-variable
    )
  endif()
endfunction()

# Отключаем сборку демонстрационных примеров ixwebsocket и настраиваем статическую линковку
option(BUILD_DEMO "" OFF)
option(USE_TLS "" OFF)
option(USE_OPEN_SSL "" OFF)
option(USE_MBED_TLS "" OFF)
option(USE_ZLIB "" OFF)

# Принудительно отключаем сборку примеров и тестов
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

# Настраиваем статическую библиотеку
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE) 
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Enable install target" FORCE)

# Устанавливаем статическую многопоточную библиотеку времени выполнения для ixwebsocket
if(MSVC)
    # Принудительно отключаем использование динамических библиотек для ixwebsocket
    set(USE_STATIC_CRT ON CACHE BOOL "Use static C runtime" FORCE)
    
    # Принудительно устанавливаем статическую многопоточную библиотеку
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # Заменяем флаги компилятора для ixwebsocket, чтобы использовать /MT вместо /MD
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

# Добавляем библиотеку ixwebsocket как подпроект
add_subdirectory(${IXWEBSOCKET_DIR})

# Відключаємо попередження для всіх цілей ixwebsocket після їх додавання
disable_warnings_for_target(ixwebsocket)

# Перевіряємо існування інших таргетів з бібліотеки ixwebsocket та відключаємо попередження і для них
if(TARGET ws)
  disable_warnings_for_target(ws)
endif()

# Якщо в проекті використовуються інші цілі з ixwebsocket,
# їх також можна додати тут, наприклад:
if(TARGET ixsnake)
  disable_warnings_for_target(ixsnake)
endif()
if(TARGET ixcrypto)
  disable_warnings_for_target(ixcrypto)
endif()
