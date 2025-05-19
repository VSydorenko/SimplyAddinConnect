##
## @file options.cmake
## @brief Настройки и опции сборки
##

# ---- UAPKI: збираємо все статично ------------------------------------
set(UAPKI_LIBS_TYPE STATIC CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF  CACHE BOOL   "" FORCE)

# вимикаємо dllimport
add_compile_definitions(
    BA_STATIC ASN1_STATIC DRBG_STATIC
    UAPKIC_STATIC UAPKIF_STATIC UAPKI_STATIC
    CURL_STATICLIB              # libcurl теж статична
)

# Опція для зборки тестів (по умолчанию выключено)
option(BUILD_TESTS "Build test suite" OFF)
# Опція для зборки з UAPKI (по умолчанию выключено)
option(BUILD_WITH_UAPKI "Build with UAPKI integration" OFF)