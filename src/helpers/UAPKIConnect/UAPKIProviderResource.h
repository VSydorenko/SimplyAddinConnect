#pragma once

#ifdef _WINDOWS
// Ім'я RCDATA-ресурсу, у який вбудовано провайдер cm-pkcs12_<arch>.dll.
// Той самий літерал використовує згенерований .rc у CMake/uapki_full_static.cmake — тримати синхронно.
#define UAPKI_PROVIDER_RESOURCE_NAME  L"CM_PKCS12_PROVIDER"
#endif
