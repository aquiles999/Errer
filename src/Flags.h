#pragma once
#include <windows.h>
#include <wchar.h>

static bool EnvFlag(const wchar_t* name) {
    wchar_t buf[8];
    DWORD n = GetEnvironmentVariableW(name, buf, 8);
    return n > 0 && n <= 8 && _wcsicmp(buf, L"1") == 0;
}

static bool NoBsodFlag()       { return EnvFlag(L"ERRER_NOBSOD"); }
static bool NoDateChangeFlag() { return EnvFlag(L"ERRER_NODATECHANGE"); }
static bool NoGdiFlag()        { return EnvFlag(L"ERRER_NOGDI"); }