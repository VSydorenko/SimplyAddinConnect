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
target_compile_definitions(byte-array PUBLIC UAPKIC_STATIC NOCRYPT _CRT_SECURE_NO_WARNINGS BA_STATIC)

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
target_compile_definitions(uapkic PUBLIC UAPKIC_STATIC UAPKIC_SELF_TEST NOCRYPT _CRT_SECURE_NO_WARNINGS BA_STATIC)
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
target_compile_definitions(uapkif PRIVATE UAPKIF_STATIC NOCRYPT _CRT_SECURE_NO_WARNINGS)

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

# --- Збірка cm-pkcs12 як окремої самодостатньої SHARED-DLL (провайдер) ---
# Провайдер вантажиться в рантаймі через LoadLibrary; символьних залежностей
# від головної DLL немає. Статичні uapkic/uapkif/asn1/parson/ba-utils/byte-array
# лінкуються всередину провайдера. Спільні джерела (parson, ba-utils) беруться
# ЛІНКУВАННЯМ статичних цілей, а не компілюються вдруге (інакше LNK2005).
file(GLOB CM_PKCS12_SRC
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/*.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/crypto/*.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/storage/*.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/crypto/*.cpp"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/storage/*.cpp"
)
add_library(cm-pkcs12-provider SHARED ${CM_PKCS12_SRC}
    # common/pkix файли, яких немає в жодній статичній цілі
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/aid.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/dstu4145-params.c
    # ecdsa-params.c — з оновленням upstream private-key.c інклудить ecdsa-params.h
    # і викликає ecdsa_ecparams_get_ecid(); без цього файлу провайдер падає з
    # unresolved external. Ядро uapki підхоплює його через file(GLOB common/pkix/*.c).
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/ecdsa-params.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/iconv-utils.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/iso15946.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/key-wrap.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/oids.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/oid-utils.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/private-key.c
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/uapki-ns-util.cpp
)
target_include_directories(cm-pkcs12-provider PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/crypto
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/cm-pkcs12/src/storage
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/cm-api
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkif/include
)
# curl НЕ лінкувати — потрібен лише ядру uapki
target_link_libraries(cm-pkcs12-provider PRIVATE
    uapkic uapkif asn1 parson ba-utils byte-array stacktrace dirent-internal
    bcrypt crypt32 ws2_32
)
# CM_LIBRARY робить рівно 7 provider_* символів dllexport (усі позначені CM_EXPORT
# у main-cm-pkcs12.cpp); STATIC-дефайни вже глобальні через add_definitions вище.
# БЕЗ CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS і без .def — інакше витягнуло б тисячі символів.
target_compile_definitions(cm-pkcs12-provider PRIVATE CM_LIBRARY NOCRYPT _CRT_SECURE_NO_WARNINGS)
if(APPLE)
    target_link_libraries(cm-pkcs12-provider PRIVATE iconv)
endif()
# Ім'я з арх-суфіксом, вихід — поруч з головною DLL у bin/Release
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(cm-pkcs12-provider PROPERTIES OUTPUT_NAME "cm-pkcs12_x64")
else()
    set_target_properties(cm-pkcs12-provider PROPERTIES OUTPUT_NAME "cm-pkcs12_x86")
endif()
set_target_properties(cm-pkcs12-provider PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin/Release"
)

# --- dirent-internal уже объявлен ранее ---

# --- Збірка uapki ---
file(GLOB UAPKI_API_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/api/*.cpp")
file(GLOB UAPKI_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/src/*.cpp")
file(GLOB COMMON_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/*.cpp")
file(GLOB JSON_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json/*.cpp")
# common/pkix/*.c потрібні ядру uapki (oids, oid-utils, key-wrap, private-key, iconv-utils,
# aid, dstu4145-params, iso15946). Раніше приходили через стару cm-pkcs12. ba-utils.c та
# uapki-errors.c ВИКЛЮЧАЄМО — вони вже в статичній цілі ba-utils (уникаємо LNK2005).
file(GLOB UAPKI_PKIX_C_SOURCES "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/*.c")
list(REMOVE_ITEM UAPKI_PKIX_C_SOURCES
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/ba-utils.c"
    "${CMAKE_SOURCE_DIR}/extern/uapki/library/common/pkix/uapki-errors.c"
)
add_library(uapki STATIC
    ${UAPKI_API_SOURCES}
    ${UAPKI_SOURCES}
    ${COMMON_SOURCES}
    ${JSON_SOURCES}
    ${UAPKI_PKIX_C_SOURCES}
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
target_link_libraries(uapki PUBLIC uapkic stacktrace uapkif asn1 ba-utils parson dirent-internal)
target_compile_definitions(uapki PRIVATE UAPKI_STATIC NOCRYPT _CRT_SECURE_NO_WARNINGS)

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

# Прибудований libcurl 8.21.0 (upstream v2.0.16) посилається на __imp_if_nametoindex
# (iphlpapi) — без нього лінк падає з unresolved. `normaliz` (IDN) додано для паритету
# з власним library/uapki/CMakeLists.txt upstream, де перелічені обидві.
message(STATUS "[UAPKI] Using libcurl: ${CURL_LIBRARY}")

# --- volatileaccessu.lib: прихована залежність прибудованого libcurl 8.21.0 ---
# Кожен об'єктний файл цього libcurl несе /DEFAULTLIB:volatileaccessu.lib і посилається
# на RtlSetVolatileMemory: у Windows SDK 10.0.26100+ саме так реалізовано SecureZeroMemory.
# У старіших SDK (напр. 10.0.20348 — дефолт на Windows Server 2022) файлу немає, і фінальний
# лінк головної DLL падає з LNK1104. Тому шукаємо цей .lib у ВСІХ встановлених Windows Kits
# і лінкуємо повним шляхом (якщо поточний SDK уже свіжий — знайдеться той самий файл).
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_VA_ARCH "x64")
    else()
        set(_VA_ARCH "x86")
    endif()
    set(_VA_KITS_ROOTS
        "$ENV{WindowsSdkDir}"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10"
        "$ENV{ProgramFiles}/Windows Kits/10"
        "C:/Program Files (x86)/Windows Kits/10"
    )
    set(_VA_CANDIDATES "")
    foreach(_root IN LISTS _VA_KITS_ROOTS)
        if(_root)
            file(GLOB _found "${_root}/Lib/*/um/${_VA_ARCH}/volatileaccessu.lib")
            list(APPEND _VA_CANDIDATES ${_found})
        endif()
    endforeach()
    if(_VA_CANDIDATES)
        list(REMOVE_DUPLICATES _VA_CANDIDATES)
        list(SORT _VA_CANDIDATES COMPARE NATURAL)
        list(GET _VA_CANDIDATES -1 VOLATILEACCESS_LIBRARY)
        message(STATUS "[UAPKI] Using volatileaccessu: ${VOLATILEACCESS_LIBRARY}")
        target_link_libraries(uapki PUBLIC "${VOLATILEACCESS_LIBRARY}")
    else()
        message(FATAL_ERROR
            "[UAPKI] volatileaccessu.lib (${_VA_ARCH}) не знайдено в жодному Windows Kit. "
            "Прибудований libcurl 8.21.0 з UAPKI v2.0.16 посилається на RtlSetVolatileMemory — "
            "встановіть Windows SDK 10.0.26100 або новіший.")
    endif()
endif()

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
        ${CURL_LIBRARY}
        ${VOLATILEACCESS_LIBRARY}
        wldap32 crypt32 ws2_32 winmm bcrypt normaliz iphlpapi
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
    target_link_libraries(uapki_bundle INTERFACE wldap32 crypt32 ws2_32 winmm bcrypt normaliz iphlpapi)
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
    target_link_libraries(uapki PUBLIC crypt32 ws2_32 bcrypt normaliz iphlpapi)
    target_link_libraries(uapki_bundle INTERFACE crypt32 ws2_32 bcrypt normaliz iphlpapi)
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

# --- Вбудовуємо провайдер cm-pkcs12_<arch>.dll ресурсом RCDATA у головну DLL ---
# Щоб хелпер розгортав його з ресурсу в рантаймі (самодостатність для 1С).
# Файл include-иться з components.cmake ПІСЛЯ add_library(${TARGET} SHARED ...),
# тож ${TARGET} тут гарантовано існує, а ціль cm-pkcs12-provider визначена вище.
# $<TARGET_FILE:...> у file(GENERATE) віддає шлях з прямими слешами — RC приймає '/'.
# VS — мультиконфіг-генератор: шлях провайдера залежить від конфігурації, тому
# генеруємо окремий .rc на кожну конфігурацію ($<CONFIG> у імені й у вмісті).
set(_PROVIDER_RC "${CMAKE_BINARY_DIR}/cm_pkcs12_provider_resource_$<CONFIG>.rc")
file(GENERATE OUTPUT "${_PROVIDER_RC}"
     CONTENT "CM_PKCS12_PROVIDER RCDATA \"$<TARGET_FILE:cm-pkcs12-provider>\"\n")
target_sources(${TARGET} PRIVATE "${_PROVIDER_RC}")
# Провайдер має бути ЗІБРАНИЙ до компіляції ресурсу головної DLL.
add_dependencies(${TARGET} cm-pkcs12-provider)
