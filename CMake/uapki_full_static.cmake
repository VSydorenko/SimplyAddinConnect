##
## @file uapki_full_static.cmake
## @brief Централізована статична збірка UAPKI та всіх залежностей для інтеграції в основну DLL
##

message(STATUS "[UAPKI] Static full bundle integration enabled")

# --- Дефайни для статичної лінковки ---
add_definitions(-DUAPKI_STATIC -DUAPKIF_STATIC -DUAPKIC_STATIC -DBA_STATIC -DASN1_STATIC)

# --- Збірка asn1 ---
file(GLOB ASN1_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/asn1/*.c")
add_library(asn1 STATIC ${ASN1_SOURCES})
target_include_directories(asn1 PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/asn1
)
add_definitions(-DASN1_STATIC)

# --- Збірка byte-array ---
file(GLOB BYTE_ARRAY_SOURCES
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array-internal.c"
)
add_library(byte-array STATIC ${BYTE_ARRAY_SOURCES})
target_include_directories(byte-array PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
)
if(WIN32)
    target_link_libraries(byte-array PUBLIC bcrypt)
endif()
# Добавляем явные экспортируемые дефайны для byte-array
target_compile_definitions(byte-array PUBLIC UAPKIC_STATIC UAPKIC_LIBRARY NOCRYPT _CRT_SECURE_NO_WARNINGS BA_STATIC)

# --- Збірка ba-utils ---
file(GLOB BA_UTILS_SOURCES 
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/ba-utils.c"
)
# Добавляем uapki-errors.c для реализации error_code_to_str
list(APPEND BA_UTILS_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/uapki-errors.c")
add_library(ba-utils STATIC ${BA_UTILS_SOURCES})
target_include_directories(ba-utils PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/asn1
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src
)
target_link_libraries(ba-utils PUBLIC byte-array stacktrace)
target_compile_definitions(ba-utils PUBLIC UAPKIC_STATIC BA_STATIC)

# --- Збірка stacktrace ---
file(GLOB STACKTRACE_SOURCES 
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/stacktrace.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros/stacktrace.h"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/pthread-impl.c"
)
add_library(stacktrace STATIC ${STACKTRACE_SOURCES})
target_include_directories(stacktrace PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
)
target_link_libraries(stacktrace PUBLIC byte-array)
target_compile_definitions(stacktrace PUBLIC NOCRYPT _CRT_SECURE_NO_WARNINGS UAPKIC_STATIC)

# --- Добавляем dirent-internal для поддержки директорий ---
file(GLOB DIRENT_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/dirent-internal.c")
add_library(dirent-internal STATIC ${DIRENT_SOURCES})
target_include_directories(dirent-internal PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
)
target_link_libraries(dirent-internal PUBLIC stacktrace ba-utils)

# --- Збірка uapkic ---
file(GLOB UAPKIC_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/*.c")
# Исключаем файлы ByteArray, так как они уже включены в отдельную библиотеку
list(REMOVE_ITEM UAPKIC_SOURCES 
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array-internal.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/stacktrace.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/pthread-impl.c"
)
add_library(uapkic STATIC ${UAPKIC_SOURCES})
target_include_directories(uapkic PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
)
target_compile_definitions(uapkic PUBLIC UAPKIC_STATIC UAPKIC_LIBRARY UAPKIC_SELF_TEST NOCRYPT _CRT_SECURE_NO_WARNINGS BA_STATIC)
if(WIN32)
    target_link_libraries(uapkic PUBLIC stacktrace byte-array bcrypt)
else()
    target_link_libraries(uapkic PUBLIC stacktrace byte-array)
endif()

# --- Збірка uapkif ---
file(GLOB UAPKIF_ASN1_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/asn1/*.c")
file(GLOB UAPKIF_STRUCT_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/struct/*.c")
add_library(uapkif STATIC ${UAPKIF_ASN1_SOURCES} ${UAPKIF_STRUCT_SOURCES})
target_include_directories(uapkif PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/src/asn1
)
target_link_libraries(uapkif PUBLIC uapkic asn1 ba-utils)
target_compile_definitions(uapkif PRIVATE UAPKIF_STATIC UAPKIF_LIBRARY NOCRYPT _CRT_SECURE_NO_WARNINGS)

# --- Збірка parson (JSON) ---
file(GLOB PARSON_SOURCES
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/parson.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/parson-helper.cpp"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/parson-ba-utils.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/strtod-no-locale.c"
)
add_library(parson STATIC ${PARSON_SOURCES})
target_include_directories(parson PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
)
target_link_libraries(parson PUBLIC ba-utils byte-array)

# --- Збірка cm-pkcs12 ---
file(GLOB CM_PKCS12_SRC
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/*.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/crypto/*.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/storage/*.c"
)
add_library(cm-pkcs12 STATIC ${CM_PKCS12_SRC}
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/aid.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/ba-utils.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/dstu4145-params.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/iconv-utils.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/iso15946.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/key-wrap.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/oids.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/oid-utils.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/private-key.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/uapki-ns-util.cpp
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/parson.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/parson-helper.cpp
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/strtod-no-locale.c
)
target_include_directories(cm-pkcs12 PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/crypto
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/storage
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/cm-api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
)
target_link_libraries(cm-pkcs12 PUBLIC uapkic uapkif parson stacktrace)
target_compile_definitions(cm-pkcs12 PRIVATE CM_LIBRARY)
if(WIN32)
    target_compile_definitions(cm-pkcs12 PRIVATE NOCRYPT _CRT_SECURE_NO_WARNINGS)
endif()
if(APPLE)
    target_link_libraries(cm-pkcs12 PRIVATE iconv)
endif()

# --- dirent-internal уже объявлен ранее ---

# --- Збірка uapki ---
file(GLOB UAPKI_API_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/api/*.cpp")
file(GLOB UAPKI_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/*.cpp")
file(GLOB COMMON_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/*.cpp")
file(GLOB JSON_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/*.cpp")
add_library(uapki STATIC
    ${UAPKI_API_SOURCES}
    ${UAPKI_SOURCES}
    ${COMMON_SOURCES}
    ${JSON_SOURCES}
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/loaders/cm-loader.cpp
)
target_include_directories(uapki PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/cm-api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/loaders
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/curl/include
)
target_link_libraries(uapki PUBLIC uapkic stacktrace uapkif asn1 ba-utils parson cm-pkcs12 dirent-internal)
target_compile_definitions(uapki PRIVATE UAPKI_STATIC UAPKI_LIBRARY NOCRYPT _CRT_SECURE_NO_WARNINGS)

# --- Додаємо curl ---
set(CURL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/curl/include")

# Выбираем библиотеку libcurl в зависимости от целевой архитектуры
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64-bit архитектура (x64)
    set(CURL_LIBRARY "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/curl/builds/windows_x86-64/libcurl.lib")
else()
    # 32-bit архитектура (x86)
    set(CURL_LIBRARY "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/curl/builds/windows_x86/libcurl.lib")
endif()

message(STATUS "[UAPKI] Using libcurl: ${CURL_LIBRARY}")
target_link_libraries(uapki PUBLIC ${CURL_LIBRARY})

# --- Создаем объединенную библиотеку всех компонентов UAPKI ---
if(TARGET uapki_full_static)
    return()
endif()

# Создаем объединенную статическую библиотеку, которая включает все символы
add_library(uapki_full_static STATIC 
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/byte-array-internal.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/src/stacktrace.c"
)

# Линкуем все библиотеки с WHOLE_ARCHIVE опцией, чтобы включить все символы
if(WIN32)
    # Для Windows используем специальный синтаксис MSVC
    target_link_libraries(uapki_full_static PUBLIC 
        uapki
        uapkic
        byte-array
        ba-utils
        stacktrace
        dirent-internal
        uapkif
        asn1
        parson
        cm-pkcs12
        ${CURL_LIBRARY}
        wldap32 crypt32 ws2_32 winmm bcrypt
    )
else()
    # Для Linux и других платформ
    target_link_libraries(uapki_full_static PUBLIC
        -Wl,--whole-archive
        uapki
        uapkic
        byte-array
        ba-utils
        stacktrace
        dirent-internal
        uapkif
        asn1
        parson
        cm-pkcs12
        -Wl,--no-whole-archive
        ${CURL_LIBRARY}
    )
endif()

# --- Создаём uapki_bundle (интерфейсная библиотека) ---
if(TARGET uapki_bundle)
    return()
endif()
add_library(uapki_bundle INTERFACE)
target_link_libraries(uapki_bundle INTERFACE uapki_full_static)
# Системные библиотеки для Windows
if(WIN32)
    target_link_libraries(uapki_bundle INTERFACE wldap32 crypt32 ws2_32 winmm bcrypt)
endif()
target_include_directories(uapki_bundle INTERFACE
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/cm-api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/loaders
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/curl/include
)
target_compile_definitions(uapki_bundle INTERFACE WITH_UAPKI CURL_STATICLIB CURL_DISABLE_SSH CURL_DISABLE_LDAP)

# Этот блок перемещен выше

# --- Системные бібліотеки ---
if(WIN32)
    target_link_libraries(uapki PUBLIC crypt32 ws2_32 bcrypt)
    target_link_libraries(uapki_bundle INTERFACE crypt32 ws2_32 bcrypt)
endif()
if(UNIX)
    target_link_libraries(uapki PUBLIC pthread dl)
    target_link_libraries(uapki_bundle INTERFACE pthread dl)
endif()
if(APPLE)
    target_link_libraries(uapki PUBLIC iconv)
    target_link_libraries(uapki_bundle INTERFACE iconv)
endif()

message(STATUS "[UAPKI] All static dependencies added. Link them to your DLL target as needed.")
