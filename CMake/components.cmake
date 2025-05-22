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
    src/transport/Transport_WSServer.h

    # Следующие файлы закомментированы, так как они еще не существуют
    # src/helpers/LoggerHelper.h
    # src/helpers/BPOS1Parser.h
    # src/helpers/UapkiHelper.h
    # src/helpers/ECRPrivatJSONHelper.h
    # src/components/AddinECRCommX.h
    # src/components/AddinPOSAPI.h
    # src/components/AddinUAPKI.h
    # src/components/AddinECRPrivatJSON.h
    # src/transport/ECRPrivatJSONTransport.h
    # src/protocols/ECRPrivatJSON.h
    # src/protocols/ECRPrivatJSON_Types.h
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
    src/transport/Transport_WSServer.cpp
    
    # Следующие файлы закомментированы, так как они еще не существуют
    # src/helpers/LoggerHelper.cpp
    # src/helpers/BPOS1Parser.cpp
    # src/helpers/UapkiHelper.cpp
    # src/helpers/ECRPrivatJSONHelper.cpp
    # src/helpers/ECRPrivatJSONHelper_Parsing.cpp
    # src/helpers/ECRPrivatJSONHelper_Request.cpp
    # src/helpers/ECRPrivatJSONHelper_Response.cpp
    # src/helpers/ECRPrivatJSONHelper_Service.cpp
    # src/helpers/ECRPrivatJSONHelper_Terminal.cpp
    # src/helpers/ECRPrivatJSONHelper_Utils.cpp
    # src/components/AddinECRCommX.cpp
    # src/components/AddinPOSAPI.cpp
    # src/components/AddinUAPKI.cpp
    # src/components/AddinECRPrivatJSON.cpp
    # src/transport/ECRPrivatJSONTransport.cpp
    # src/protocols/ECRPrivatJSON.cpp
    # src/protocols/ECRPrivatJSON_Connection.cpp
    # src/protocols/ECRPrivatJSON_Handshake.cpp
    # src/protocols/ECRPrivatJSON_Internal.cpp
    # src/protocols/ECRPrivatJSON_FinancialOperations.cpp
    # src/protocols/ECRPrivatJSON_ServiceOperations.cpp
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
#     src/helpers/LoggerHelper.h
#     src/helpers/LoggerHelper.cpp
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
    src/transport/Transport_WSServer.h
    src/transport/Transport_WSServer.cpp
)

## @var privat_json_helper_component
## @brief Компонент для работы с протоколом ПриватБанка на основе JSON
# add_library(privat_json_helper_component OBJECT
#     src/helpers/ECRPrivatJSONHelper.h
#     src/helpers/ECRPrivatJSONHelper.cpp
#     src/helpers/ECRPrivatJSONHelper_Parsing.cpp
#     src/helpers/ECRPrivatJSONHelper_Request.cpp
#     src/helpers/ECRPrivatJSONHelper_Response.cpp
#     src/helpers/ECRPrivatJSONHelper_Service.cpp
#     src/helpers/ECRPrivatJSONHelper_Terminal.cpp
#     src/helpers/ECRPrivatJSONHelper_Utils.cpp
# )

# Устанавливаем свойства для компонента JSON-обработки
# set_target_properties(privat_json_helper_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# Добавляем пути включения для компонента JSON-обработки
# target_include_directories(privat_json_helper_component PRIVATE 
#     include
#     ${SPDLOG_INCLUDE_DIR}
#     ${NLOHMANN_JSON_INCLUDE_DIR}
# )

## @var ecr_json_transport_component
## @brief Компонент для транспортного уровня JSON протокола
# add_library(ecr_json_transport_component OBJECT
#     src/transport/ECRPrivatJSONTransport.h
#     src/transport/ECRPrivatJSONTransport.cpp
# )

# Устанавливаем свойства для транспортного компонента
# set_target_properties(ecr_json_transport_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

# Добавляем пути включения для транспортного компонента
# target_include_directories(ecr_json_transport_component PRIVATE 
#     include
#     ${SPDLOG_INCLUDE_DIR}
# )

## @var ecr_privat_json_component
## @brief Компонент для работы с платежным терминалом ПриватБанка по JSON-протоколу
# add_library(ecr_privat_json_component OBJECT
#     src/components/AddinECRPrivatJSON.h
#     src/components/AddinECRPrivatJSON.cpp
# )

## @var protocols_component
## @brief Компонент с протоколами взаємодії з терміналами
# add_library(protocols_component OBJECT
#     src/protocols/ECRPrivatJSON.h
#     src/protocols/ECRPrivatJSON_Types.h
#     src/protocols/ECRPrivatJSON.cpp
#     src/protocols/ECRPrivatJSON_Connection.cpp
#     src/protocols/ECRPrivatJSON_Handshake.cpp
#     src/protocols/ECRPrivatJSON_Internal.cpp
#     src/protocols/ECRPrivatJSON_FinancialOperations.cpp
#     src/protocols/ECRPrivatJSON_ServiceOperations.cpp
# )

# set_target_properties(protocols_component PROPERTIES
#     POSITION_INDEPENDENT_CODE ON
#     CXX_STANDARD 17
#     CXX_STANDARD_REQUIRED ON
# )

## @var uapki_helper_component
## @brief Вспомогательный компонент для работы с библиотекой UAPKI
if(BUILD_WITH_UAPKI)
    add_library(uapki_helper_component OBJECT
        src/helpers/UapkiHelper.h
        src/helpers/UapkiHelper.cpp
    )
    target_include_directories(uapki_helper_component PRIVATE 
        include
        extern/uapki/library/uapki/include
        extern/uapki/library/uapkic/include
        extern/uapki/library/uapkif/include
    )
    target_compile_definitions(uapki_helper_component PRIVATE WITH_UAPKI)
    set_target_properties(uapki_helper_component PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    add_library(uapki_component OBJECT
        src/components/AddinUAPKI.h
        src/components/AddinUAPKI.cpp
    )
    target_include_directories(uapki_component PRIVATE 
        include
        extern/uapki/library/uapki/include
        extern/uapki/library/uapkic/include
        extern/uapki/library/uapkif/include
    )
    target_compile_definitions(uapki_component PRIVATE WITH_UAPKI)
    set_target_properties(uapki_component PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

# Встановлюємо явну залежність
add_dependencies(test_component base_component spdlog)
add_dependencies(helpers_component base_component spdlog)
add_dependencies(transport_component base_component spdlog ixwebsocket)

# add_dependencies(ecrcommx_component base_component spdlog)
# add_dependencies(ecrcommx_component helpers_component)
# add_dependencies(ecrcommx_component mscomm_component)
# add_dependencies(ecrcommx_component test_component)
# add_dependencies(posapi_component base_component spdlog)
# add_dependencies(posapi_component helpers_component)
# add_dependencies(posapi_component test_component)

# Зависимости для компонентов UAPKI
if(BUILD_WITH_UAPKI)
    add_dependencies(uapki_helper_component base_component spdlog)
    add_dependencies(uapki_helper_component helpers_component)
    add_dependencies(uapki_helper_component nlohmann_json)
    add_dependencies(uapki_component base_component spdlog)
    add_dependencies(uapki_component helpers_component)
    add_dependencies(uapki_component uapki_helper_component)
    target_include_directories(uapki_helper_component PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    target_include_directories(uapki_component PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    target_include_directories(uapki_helper_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
    target_include_directories(uapki_component PRIVATE include ${SPDLOG_INCLUDE_DIR})
endif()

# Зависимости для компонентов Privat JSON
# add_dependencies(ecr_json_transport_component base_component spdlog)
# target_link_libraries(ecr_json_transport_component PRIVATE interfaces_component spdlog::spdlog)
# add_dependencies(privat_json_helper_component base_component spdlog)
# add_dependencies(privat_json_helper_component helpers_component)
# add_dependencies(privat_json_helper_component ecr_json_transport_component)
# add_dependencies(privat_json_helper_component nlohmann_json)
# add_dependencies(ecr_privat_json_component base_component spdlog)
# add_dependencies(ecr_privat_json_component helpers_component)
# add_dependencies(ecr_privat_json_component privat_json_helper_component)
# target_link_libraries(ecr_json_transport_component PRIVATE interfaces_component)
# target_link_libraries(privat_json_helper_component PRIVATE interfaces_component)
# target_link_libraries(ecr_privat_json_component PRIVATE interfaces_component)

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
    # $<TARGET_OBJECTS:ecrcommx_component>
    # $<TARGET_OBJECTS:posapi_component>
    # $<TARGET_OBJECTS:ecr_json_transport_component>
    # $<TARGET_OBJECTS:privat_json_helper_component>
    # $<TARGET_OBJECTS:ecr_privat_json_component>
    # $<TARGET_OBJECTS:protocols_component>

    # Включаем компоненты UAPKI только если включена опция
    $<$<BOOL:${BUILD_WITH_UAPKI}>:$<TARGET_OBJECTS:uapki_helper_component>>
    $<$<BOOL:${BUILD_WITH_UAPKI}>:$<TARGET_OBJECTS:uapki_component>>
)

# Додаємо шляхи включення та визначення компілятора для фінальної DLL
target_include_directories(${TARGET} PRIVATE include ${NLOHMANN_JSON_INCLUDE_DIR} ${SPDLOG_INCLUDE_DIR} ${IXWEBSOCKET_INCLUDE_DIR})
target_compile_definitions(${TARGET} PRIVATE UNICODE _UNICODE)
target_link_libraries(${TARGET} PRIVATE nlohmann_json spdlog::spdlog ixwebsocket)

# Настройка транспортного компонента
set_target_properties(transport_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Пути включения для транспортного компонента
target_include_directories(transport_component PRIVATE 
    include
    ${CMAKE_SOURCE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${IXWEBSOCKET_INCLUDE_DIR}
)

# Зависимости для транспортного компонента
add_dependencies(transport_component base_component spdlog)
add_dependencies(transport_component helpers_component)

# Зависимость от библиотеки ixwebsocket
add_dependencies(transport_component ixwebsocket)
target_link_libraries(transport_component PRIVATE interfaces_component spdlog::spdlog ixwebsocket)