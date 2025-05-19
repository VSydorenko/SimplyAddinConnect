// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//
#pragma once

#ifdef _WINDOWS
#include <windows.h>
#endif //_WINDOWS

#include <string>

std::wstring MB2WC(const std::string& source);
std::string WC2MB(const std::wstring& source);
