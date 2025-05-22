#pragma once

// Важное примечание: Windows заголовочные файлы должны включаться в определенном порядке
// winsock2.h необходимо включить перед windows.h, чтобы избежать конфликтов с winsock.h

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#endif

// Стандартные заголовочные файлы C++
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include <algorithm>
#include <variant>
#include <string_view>
#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <cstdint>

// Часто використовувані утиліти
std::wstring MB2WC(const std::string& source);
std::string WC2MB(const std::wstring& source);