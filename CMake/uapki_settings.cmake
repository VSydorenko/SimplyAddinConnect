##
## @file uapki_settings.cmake
## @brief Настройки для интеграции с UAPKI
##

# Настройки для UAPKI, если включена соответствующая опция
message(STATUS "Building with UAPKI integration")

# Устанавливаем политику для корректной работы option() со змінними
if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

# Настройки для UAPKI - ОЧЕНЬ важно ставить эти настройки ДО add_subdirectory
set(UAPKI_DISABLE_COPY ON CACHE BOOL "Disable copying into out dir" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static libs" FORCE)

# Настройки CURL для UAPKI - отключаем неиспользуемые протоколы
if(EXISTS "C:/Program Files/curl-8.13.0_1-win64-mingw")
    set(CURL_INCLUDE_DIR "C:/Program Files/curl-8.13.0_1-win64-mingw/include" CACHE PATH "Path to CURL include directory" FORCE)
    set(CURL_LIBRARY "C:/Program Files/curl-8.13.0_1-win64-mingw/lib/libcurl.a" CACHE FILEPATH "Path to CURL library" FORCE)
    set(CURL_LIBRARIES ${CURL_LIBRARY} CACHE STRING "CURL libraries" FORCE)
endif()

# Создаем временный файл, чтобы заблокировать сборку CM-модулей при компиляции
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/skip_cm_modules.cmake" 
     "# Do not include CM modules\n"
     "if(IS_DIRECTORY \${PROJECT_SOURCE_DIR}/cm-pkcs12)\n"
     "  # Отключаем сборку cm-pkcs12\n"
     "  message(STATUS \"Skipping cm-pkcs12 module during main build\")\n"
     "  return()\n"
     "endif()\n")

# Создаем пустые файлы для перехвата add_subdirectory вызовов
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uapkic.cmake" 
    "add_subdirectory(uapkic)")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uapkif.cmake" 
    "add_subdirectory(uapkif)")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uapki.cmake"
    "add_subdirectory(uapki)")

# Создаем мишень для libcurl напрямую вместо find_package
add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES
    IMPORTED_LOCATION "${CURL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")

# Устанавливаем единые выходные директории для библиотек UAPKI
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release CACHE PATH "Archive output directory" FORCE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release CACHE PATH "Library output directory" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release CACHE PATH "Runtime output directory" FORCE)

# Патчим оригинальный CMakeLists.txt UAPKI библиотеки через включение файла
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_MODULE_PATH})

# Проверяем наличие директорий UAPKI
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic")
    message(FATAL_ERROR "UAPKI library not found. Please run: git submodule update --init --recursive")
endif()

# Добавляем подпроекты UAPKI
add_subdirectory(extern/uapki/library/uapkic)
add_subdirectory(extern/uapki/library/uapkif)
add_subdirectory(extern/uapki/library/uapki)

# Блокируем сборку cm-pkcs12 и других модулей перехватом вызова add_subdirectory
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cm-pkcs12.cmake" 
     "# Блокируем сборку cm-pkcs12\n"
     "message(STATUS \"Skipping cm-pkcs12 module\")\n")

# Добавляем директории с заголовочными файлами UAPKI и CURL
target_include_directories(${TARGET} PRIVATE 
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
    "${CURL_INCLUDE_DIR}"
)

# Добавляем директорию CURL для таргета uapki если она существует
if(EXISTS "C:/Program Files/curl-8.13.0_1-win64-mingw")
    target_include_directories(uapki PRIVATE "${CURL_INCLUDE_DIR}")
endif()

# Определение для отключения небезопасных предупреждений и SSL
add_definitions(-DCURL_STATICLIB -DCURL_DISABLE_LDAP -DCURL_DISABLE_TELNET -DCURL_DISABLE_DICT -DCURL_DISABLE_FILE -DCURL_DISABLE_TFTP -DCURL_DISABLE_LDAPS -DCURL_DISABLE_RTSP -DCURL_DISABLE_PROXY -DCURL_DISABLE_POP3 -DCURL_DISABLE_IMAP -DCURL_DISABLE_SMTP -DCURL_DISABLE_GOPHER -DCURL_DISABLE_SMB -DCURL_DISABLE_MQTT)

# Связываем библиотеки UAPKI с основной DLL
target_link_libraries(${TARGET} PRIVATE
    uapkic
    uapkif
    uapki
    curl
)

# Дополнительно добавляем зависимость WinCrypt и других необходимых библиотек для Windows
if(WIN32)
    target_link_libraries(${TARGET} PRIVATE 
        crypt32
        wldap32
        ws2_32
        winmm
        bcrypt # Для Windows 10+
    )
endif()

# Добавляем определение компилятора, чтобы код знав, що UAPKI включен
target_compile_definitions(${TARGET} PRIVATE 
    WITH_UAPKI 
    CURL_STATICLIB
    CURL_DISABLE_SSH
    CURL_DISABLE_LDAP
)