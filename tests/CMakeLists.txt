##
## @file tests/CMakeLists.txt
## @brief Тестові консольні exe для крипто-ядра UAPKI (L1-харнес).
##        Це НЕ головна DLL: тести — окремі виконувані файли, які статично
##        лінкують крипто-ядро (uapki_bundle) і НЕ підключають src/core/pch.h.
##

# Каталог тестів доступний на Windows завжди; окремі цілі мають власні умови.
# core_selftest (L1-харнес ядра) не залежить від UAPKI й будується при -DBUILD_TESTS=ON;
# uapki_selftest/native_host потребують крипто-ядра (BUILD_WITH_UAPKI) — див. гейт нижче.
if(NOT WIN32)
    message(STATUS "[tests] пропущено (потрібен WIN32)")
    return()
endif()

# Арх-суфікс імен exe (як у головних DLL, output_settings.cmake): обидві
# архітектури кладуться в один bin/Release, тож без суфікса x86 затирав би x64.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_TEST_ARCH_SUFFIX "_x64")
else()
    set(_TEST_ARCH_SUFFIX "_x86")
endif()

# ---------------------------------------------------------------------------
# core_selftest — L1-харнес ядра AddInNative (без 1С, без UAPKI).
# Лінкує OBJECT-бібліотеки ядра напряму; платформа 1С емулюється моками.
# ---------------------------------------------------------------------------
add_executable(core_selftest core_selftest.cpp
    $<TARGET_OBJECTS:base_component>
    $<TARGET_OBJECTS:helpers_component>
)
set_target_properties(core_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "core_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(core_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR}
)
# _WINDOWS — критично: types.h саме за ним вмикає WCHAR_T=wchar_t і
# ADDIN_API=__stdcall; об'єкти ядра зібрані з _WINDOWS — ABI мусить збігатися.
target_compile_definitions(core_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
target_link_libraries(core_selftest PRIVATE spdlog::spdlog)
if(MSVC)
    target_compile_options(core_selftest PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# wire_selftest — L0.6-харнес device-facing ядра (framer/classifier/session).
# Не залежить від UAPKI; лінкує wire_component+helpers+base напряму.
# ---------------------------------------------------------------------------
add_executable(wire_selftest wire_selftest.cpp
    $<TARGET_OBJECTS:wire_component>
    $<TARGET_OBJECTS:transport_component>   # реальні TransportTCP/COM/WSClient для смоук-тестів
    $<TARGET_OBJECTS:helpers_component>
    $<TARGET_OBJECTS:base_component>
)
set_target_properties(wire_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "wire_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(wire_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}                   # корінь: Transport_WSClient.h тягне "extern/ixwebsocket/..."
    ${SPDLOG_INCLUDE_DIR}
    ${IXWEBSOCKET_INCLUDE_DIR}            # transport_component тягне ixwebsocket-заголовки
)
target_compile_definitions(wire_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
# ixwebsocket+ws2_32 — символи, які тягне transport_component (WSClient, Winsock).
target_link_libraries(wire_selftest PRIVATE spdlog::spdlog ixwebsocket ws2_32)
if(MSVC)
    target_compile_options(wire_selftest PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# ecr_privatjson_selftest — харнес пілотного драйвера ECRPrivatJSON (без UAPKI).
# Кодек/класифікатор/емулятор/e2e поверх DeviceSession+TransportTCP проти власного
# протокол-обізнаного TCP-емулятора термінала (support/TerminalEmulator).
# ---------------------------------------------------------------------------
add_executable(ecr_privatjson_selftest ecr_privatjson_selftest.cpp
    support/TerminalEmulator.cpp
    $<TARGET_OBJECTS:ecr_facade_component>       # 1С-фасад AddinECRPrivatJSON (смоук через CreateObject)
    $<TARGET_OBJECTS:driver_ecr_privatjson_component>
    $<TARGET_OBJECTS:platform_component>
    $<TARGET_OBJECTS:transport_component>   # реальний TransportTCP/COM для e2e
    $<TARGET_OBJECTS:wire_component>        # DeviceSession/NullTerminatedFramer/IFrameClassifier
    $<TARGET_OBJECTS:helpers_component>
    $<TARGET_OBJECTS:base_component>
)
set_target_properties(ecr_privatjson_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ecr_privatjson_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(ecr_privatjson_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}                   # корінь: Transport_WSClient.h тягне "extern/ixwebsocket/..."
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${IXWEBSOCKET_INCLUDE_DIR}            # transport_component тягне ixwebsocket-заголовки
)
target_compile_definitions(ecr_privatjson_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
# ixwebsocket+ws2_32 — символи transport_component (WSClient, Winsock); емулятор — Winsock.
target_link_libraries(ecr_privatjson_selftest PRIVATE spdlog::spdlog ixwebsocket ws2_32)
if(MSVC)
    target_compile_options(ecr_privatjson_selftest PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# Далі — цілі, що потребують крипто-ядра UAPKI (uapki_bundle існує лише при
# BUILD_WITH_UAPKI). Без нього тихо пропускаємо решту файлу.
# ---------------------------------------------------------------------------
if(NOT BUILD_WITH_UAPKI)
    message(STATUS "[tests] uapki_selftest/native_host пропущено (потрібен BUILD_WITH_UAPKI)")
    return()
endif()

# ---------------------------------------------------------------------------
# uapki_selftest — L1-харнес крипто-ядра.
# Проганяє JSON-сценарій (масив "tasks") напряму через process()/json_free()
# статично злінкованого ядра (без завантаження DLL, без 1С).
# ---------------------------------------------------------------------------
add_executable(uapki_selftest uapki_selftest.cpp)

set_target_properties(uapki_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "uapki_selftest${_TEST_ARCH_SUFFIX}"
)

# uapki_bundle тягне ядро (process/json_free та всю статику); parson дає
# ParsonHelper/parson.h для розбору сценарію й відповіді.
target_link_libraries(uapki_selftest PRIVATE uapki_bundle parson)

target_include_directories(uapki_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json      # parson-helper.h, parson.h
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include
)

# /utf-8 — вихідник у UTF-8 з кириличними коментарями (як і решта проєкту),
# інакше MSVC читає його в системному кодуванні й ламає літерали/коментарі.
if(MSVC)
    target_compile_options(uapki_selftest PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# native_host — L2-e2e + L3-крос-валідаційний харнес поверх ГОЛОВНОЇ DLL.
# Емулює платформу 1С: вантажить головну DLL через LoadLibraryW у рантаймі й
# викликає компоненту через IComponentBase (метод CallUapki). Крипто-ядро НЕ
# лінкується — усе йде через головну DLL. Тому спец-лінкування тут не потрібне.
# ---------------------------------------------------------------------------
add_executable(native_host native_host.cpp)

set_target_properties(native_host PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "native_host${_TEST_ARCH_SUFFIX}"
)

target_include_directories(native_host PRIVATE
    ${CMAKE_SOURCE_DIR}/include            # SDK 1С: ComponentBase.h, AddInDefBase.h, ...
    ${NLOHMANN_JSON_INCLUDE_DIR}           # nlohmann/json.hpp (header-only) — розбір відповідей
    ${CMAKE_SOURCE_DIR}
)

# _WINDOWS — КРИТИЧНО: types.h саме за ним вмикає WCHAR_T=wchar_t та
# ADDIN_API=__stdcall; головна DLL зібрана з _WINDOWS, тож ABI (в т.ч. угода
# виклику vtable) мусить збігатися. UNICODE/_UNICODE — узгодженість W-API.
# HOST_DATA_DIR = tests/data (КОПІЯ), а НЕ сабмодуль: UAPKI CerStore пише в
# certCache.path (перейменовує/кешує серти), тож указувати на read-only оригінали
# сабмодуля не можна — інакше його робоче дерево бруднилося б. run_tests.ps1 усе одно
# передає dataDir аргументом; цей дефолт — для standalone-запусків native_host.
target_compile_definitions(native_host PRIVATE _WINDOWS UNICODE _UNICODE
    HOST_BIN_DIR="${CMAKE_SOURCE_DIR}/bin/Release"
    HOST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
)

# /utf-8 — вихідник у UTF-8 з кириличними коментарями (як і решта проєкту),
# інакше MSVC читає його в системному кодуванні й ламає літерали/коментарі.
if(MSVC)
    target_compile_options(native_host PRIVATE /utf-8)
endif()

# Головний раннер сценаріїв — зовнішній (run_tests.ps1), тому add_test тут
# не оголошуємо: достатньо, щоб ціль збиралася. За потреби ctest-обгортку
# додасть агент сценаріїв, коли з'являться самі .json-файли.
