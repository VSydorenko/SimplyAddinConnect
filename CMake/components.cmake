##
## @file components.cmake
## @brief Определение компонентов проекта
##

##
## @section global_files Визначення файлів проекту
## @brief Тут визначаються всі необхідні файли проекту, розділені за типами
##

## @var HEADER_FILES
## @brief Заголовочні файли проекту
## @details Тут потрібно додавати .h файли проекту.
## При додаванні нового компоненту, додайте його заголовочний файл сюди.
set(HEADER_FILES
    include/AddInDefBase.h
    include/com.h
    include/ComponentBase.h
    include/IMemoryManager.h
    include/types.h
    src/core/pch.h
    src/core/AddInNative.h
    src/TestComponent.h
    src/helpers/ServiceTools.h
    src/transport/Transport.h
    src/transport/Transport_COM.h
    src/transport/Transport_TCP.h
    src/transport/Transport_WSClient.h
    # src/helpers/BPOS1Parser.h
    src/helpers/UAPKIConnect/UAPKIConnectHelper.h
    # src/components/AddinECRCommX.h
    # src/components/AddinPOSAPI.h
    src/components/AddinUAPKIConnect.h
    src/components/AddinECRPrivatJSON.h
)

## @var SOURCE_FILES
## @brief Файли з реалізацією компонентів (.cpp)
## @details При додаванні нового компоненту, додайте файл реалізації компоненту сюди.
set(SOURCE_FILES
    src/core/AddInNative.cpp
    src/TestComponent.cpp
    src/TestComponent_COM.cpp
    src/helpers/ServiceTools.cpp
    src/helpers/ServiceTools_Conversion.cpp
    src/helpers/ServiceTools_Errors.cpp
    src/helpers/ServiceTools_Log.cpp
    src/transport/Transport_COM.cpp
    src/transport/Transport_TCP.cpp
    src/transport/Transport_WSClient.cpp
    # src/helpers/BPOS1Parser.cpp
    src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp
    # src/components/AddinECRCommX.cpp
    # src/components/AddinPOSAPI.cpp
    src/components/AddinUAPKIConnect.cpp
    src/components/AddinECRPrivatJSON.cpp
)

## @var RESOURCE_FILES
## @brief Ресурсні файли проекту (.def, .rc)
## @details Зазвичай ці файли не потрібно змінювати при додаванні нового компоненту.
set(RESOURCE_FILES
    src/core/AddInNative.def
    src/core/AddInNative.rc
)

##
## @section object_libraries Створення об'єктних бібліотек
## @brief Тут визначаються об'єктні бібліотеки для компонентів
##

## @var base_component
## @brief Базовий компонент, який містить базову функціональність AddInNative
## @note Цей компонент є основою для всіх інших компонентів
add_library(base_component OBJECT
    src/core/pch.h
    src/core/AddInNative.h
    src/core/AddInNative.cpp
)

## @var test_component
## @brief Тестовий компонент, який залежить від базового компоненту
## @details Це приклад компоненту, який наслідує функціональність базового
add_library(test_component OBJECT
    src/TestComponent.h
    src/TestComponent.cpp
    src/TestComponent_COM.cpp
)

## @var helpers_component
## @brief Компонент с вспомогательными классами
## @details Содержит классы помощники для логирования и работы с COM-портами
add_library(helpers_component OBJECT
#     src/helpers/BPOS1Parser.h
#     src/helpers/BPOS1Parser.cpp
    src/helpers/ServiceTools.h
    src/helpers/ServiceTools.cpp
    src/helpers/ServiceTools_Conversion.cpp
    src/helpers/ServiceTools_Errors.cpp
    src/helpers/ServiceTools_Log.cpp
)

# add_library(ecrcommx_component OBJECT
#     src/components/AddinECRCommX.h
#     src/components/AddinECRCommX.cpp
# )

# add_library(posapi_component OBJECT
#     src/components/AddinPOSAPI.h
#     src/components/AddinPOSAPI.cpp
# )

## Добавляем интерфейсы
## Изменяем OBJECT на INTERFACE, так как это только заголовочная библиотека
add_library(interfaces_component INTERFACE)
target_include_directories(interfaces_component INTERFACE include)

## @var transport_component
## @brief Компонент для транспортного контура
add_library(transport_component OBJECT
    src/transport/Transport.h
    src/transport/Transport_COM.h
    src/transport/Transport_COM.cpp
    src/transport/Transport_TCP.h
    src/transport/Transport_TCP.cpp
    src/transport/Transport_WSClient.h
    src/transport/Transport_WSClient.cpp
)

## @var wire_component
## @brief Device-facing ядро драйверів (framer/classifier/session — §4 дизайну).
## @note Фундамент device-core для майбутніх драйверів; входить у фінальну DLL.
add_library(wire_component OBJECT
    src/transport/RequestTypes.h
    src/transport/IFramer.h
    src/transport/NullTerminatedFramer.h
    src/transport/NullTerminatedFramer.cpp
    src/transport/IFrameClassifier.h
    src/transport/DeviceSession.h
    src/transport/DeviceSession.cpp
)
set_target_properties(wire_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(wire_component PRIVATE
    include
    ${CMAKE_SOURCE_DIR}
    src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(wire_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(wire_component base_component spdlog)

## @var platform_component
## @brief Платформа-каркас драйверів обладнання (ResultEnvelope — уніфікований результат).
## @note Основа для драйверів; входить у фінальну DLL.
add_library(platform_component OBJECT
    src/platform/ResultEnvelope.h
    src/platform/ResultEnvelope.cpp
    src/platform/JobEngine.h
    src/platform/JobEngine.cpp
)
set_target_properties(platform_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(platform_component PRIVATE
    include
    ${CMAKE_SOURCE_DIR}
    src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(platform_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(platform_component base_component spdlog nlohmann_json)

## @var driver_ecr_privatjson_component
## @brief Пілотний драйвер ECRPrivatJSON: кодек JSON, класифікатор кадрів, життєвий цикл.
## @note Уся Privat-специфіка (кодек+класифікатор); входить у фінальну DLL.
add_library(driver_ecr_privatjson_component OBJECT
    src/drivers/ecr_privatjson/EcrJsonCodec.h
    src/drivers/ecr_privatjson/EcrJsonCodec.cpp
    src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h
    src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.cpp
    src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h
    src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp
)
set_target_properties(driver_ecr_privatjson_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(driver_ecr_privatjson_component PRIVATE
    include
    ${CMAKE_SOURCE_DIR}
    src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(driver_ecr_privatjson_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(driver_ecr_privatjson_component base_component spdlog nlohmann_json wire_component transport_component helpers_component)

## @var driver_label_printer_component
## @brief Драйвер принтера етикеток (ZPL): моделі даних, одиниці, кодери/класифікатори.
## @note Поки лише каркас (LabelModel/LabelUnits) — .cpp додаватимуться в наступних тасках;
##       НЕ входить у фінальну DLL до появи фасаду (Task 12).
add_library(driver_label_printer_component OBJECT
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/LabelModel.h
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/LabelUnits.h
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/GfEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/GfEncoder.h
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/BarcodeZpl.cpp
    ${CMAKE_SOURCE_DIR}/src/drivers/label_printer/BarcodeZpl.h
)
set_target_properties(driver_label_printer_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_include_directories(driver_label_printer_component PRIVATE
    include ${CMAKE_SOURCE_DIR} src ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(driver_label_printer_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(driver_label_printer_component base_component spdlog nlohmann_json)

## @var ecr_facade_component
## @brief 1С-фасад AddinECRPrivatJSON: реєстрація методів компоненти, делегування драйверу.
## @note Компонента-фасад над пілотним драйвером; входить у фінальну DLL.
add_library(ecr_facade_component OBJECT
    src/components/AddinECRPrivatJSON.h
    src/components/AddinECRPrivatJSON.cpp
)
set_target_properties(ecr_facade_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(ecr_facade_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(ecr_facade_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(ecr_facade_component base_component spdlog nlohmann_json helpers_component driver_ecr_privatjson_component platform_component)

## @var uapki_helper_component
## @brief Вспомогательный компонент для работы с библиотекой UAPKI
if(BUILD_WITH_UAPKI)
    ## Компонент для роботи з UAPKIConnect
    add_library(uapki_connect_helper_component OBJECT
        src/helpers/UAPKIConnect/UAPKIConnectHelper.h
        src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp
    )
    target_include_directories(uapki_connect_helper_component PRIVATE 
        include
        ${CMAKE_SOURCE_DIR}
        src
        extern/uapki/library/uapki/include
        extern/uapki/library/uapkic/include
        extern/uapki/library/uapkif/include
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
    )
    target_compile_definitions(uapki_connect_helper_component PRIVATE WITH_UAPKI)
    set_target_properties(uapki_connect_helper_component PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    ## Компонент для роботи з UAPKI з 1С
    add_library(uapki_connect_component OBJECT
        src/components/AddinUAPKIConnect.h
        src/components/AddinUAPKIConnect.cpp
    )
    target_include_directories(uapki_connect_component PRIVATE 
        include
        ${CMAKE_SOURCE_DIR}
        src
        extern/uapki/library/uapki/include
        extern/uapki/library/uapkic/include
        extern/uapki/library/uapkif/include
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
    )
    target_compile_definitions(uapki_connect_component PRIVATE WITH_UAPKI)
    set_target_properties(uapki_connect_component PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    
    # Устанавливаем зависимости для UAPKIConnect компонентов
    add_dependencies(uapki_connect_helper_component base_component spdlog)
    add_dependencies(uapki_connect_helper_component helpers_component)
    add_dependencies(uapki_connect_helper_component nlohmann_json)
    add_dependencies(uapki_connect_component base_component spdlog)
    add_dependencies(uapki_connect_component helpers_component)
    add_dependencies(uapki_connect_component uapki_connect_helper_component)
    target_link_libraries(uapki_connect_helper_component PRIVATE interfaces_component spdlog::spdlog nlohmann_json)
    target_link_libraries(uapki_connect_component PRIVATE interfaces_component spdlog::spdlog)
endif()

# Встановлюємо явну залежність
add_dependencies(test_component base_component spdlog)
add_dependencies(helpers_component base_component spdlog)
add_dependencies(transport_component base_component spdlog ixwebsocket)
# add_dependencies(ecrcommx_component base_component spdlog)
# add_dependencies(ecrcommx_component helpers_component)
# add_dependencies(posapi_component base_component spdlog)
# add_dependencies(posapi_component helpers_component)

# Настраиваем свойства для транспортного компонента
set_target_properties(transport_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Добавляем пути включения для транспортного компонента
target_include_directories(transport_component PRIVATE
    include
    ${CMAKE_SOURCE_DIR}
    src
    ${SPDLOG_INCLUDE_DIR}
    ${IXWEBSOCKET_INCLUDE_DIR}
)

# Подключаем зависимости к транспортному компоненту
target_link_libraries(transport_component PRIVATE interfaces_component spdlog::spdlog ixwebsocket)

## @brief Збірка DLL в правильному порядку
add_library(${TARGET} SHARED
    ${RESOURCE_FILES}
    $<TARGET_OBJECTS:base_component>
    $<TARGET_OBJECTS:test_component>
    $<TARGET_OBJECTS:helpers_component>
    $<TARGET_OBJECTS:transport_component>
    $<TARGET_OBJECTS:wire_component>
    $<TARGET_OBJECTS:platform_component>
    $<TARGET_OBJECTS:driver_ecr_privatjson_component>
    $<TARGET_OBJECTS:ecr_facade_component>
    # $<TARGET_OBJECTS:ecrcommx_component>
    # $<TARGET_OBJECTS:posapi_component>

    # Включаем компоненты UAPKI только если включена опция
    $<$<BOOL:${BUILD_WITH_UAPKI}>:$<TARGET_OBJECTS:uapki_connect_helper_component>>
    $<$<BOOL:${BUILD_WITH_UAPKI}>:$<TARGET_OBJECTS:uapki_connect_component>>
)

# Додаємо шляхи включення та визначення компілятора для фінальної DLL
target_include_directories(${TARGET} PRIVATE include ${NLOHMANN_JSON_INCLUDE_DIR} ${SPDLOG_INCLUDE_DIR} ${IXWEBSOCKET_INCLUDE_DIR})
target_compile_definitions(${TARGET} PRIVATE UNICODE _UNICODE)
target_link_libraries(${TARGET} PRIVATE nlohmann_json)
target_link_libraries(${TARGET} PRIVATE spdlog::spdlog)
target_link_libraries(${TARGET} PRIVATE ixwebsocket)

# Додаємо лінкування UAPKI до основної DLL
if(BUILD_WITH_UAPKI)
    include(CMake/uapki_full_static.cmake)
    target_link_libraries(${TARGET} PRIVATE uapki_bundle)
endif()