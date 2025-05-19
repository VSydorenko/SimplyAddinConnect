##
## @file output_settings.cmake
## @brief Настройки выходных файлов
##

## 
## @section output_settings Налаштування виведення
## @brief Налаштування шляхів та імен вихідних файлів
##

# Налаштування директорій для вихідних файлів
set (LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)
set (EXECUTABLE_OUTPUT_PATH ${LIBRARY_OUTPUT_PATH})
set (CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})

# Явна перевірка архітектури
if (CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    set(MySuffix2 "_x86")
elseif (CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    set(MySuffix2 "_x64")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_GENERATOR_PLATFORM}")
endif()

if (UNIX)
    if (APPLE)
        set(MySuffix1 "Mac")
    else()
        set(MySuffix1 "Lin")
    endif()
else()
    set(MySuffix1 "Win")
endif()

# Налаштування імені вихідного файлу
set_target_properties( ${PROJECT_NAME} PROPERTIES
       OUTPUT_NAME ${PROJECT_NAME}${MySuffix1}${MySuffix2} 
       POSITION_INDEPENDENT_CODE ON 
       CXX_STANDARD_REQUIRED ON
       CXX_STANDARD 17
   )