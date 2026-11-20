#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include "../Cascade.h"
#include "../FakeExplorer.h"
#include "../SayonaraDelete.h"
#include "../SongSync.h"
#include "../Wobble.h"
#include "../Flags.h"
#include "../CrashLog.h"
#include "../Toast.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern void PreloadIcon();
extern void SpawnError();
bool IsChildMode();

static const DWORD P6_TOAST_DELAYS[] = {
    864,   872,   824,   904,   897,
    864,   816,   872,   905,   911,
    841,   816,   888,   872,   856,
    880,   856,   937,   839,   905,
    817,   872,   895,   848,   904,
    873,   832,   855,   912,   784,
    921,   887,   848,   889,   815,
    937,   833,   904,   880,  1144,
    577,   871,   841,   904,   840,
    920,   848,   809,   895,   864,
    897,   864,   840,   847,   912,
    832,   880,   849,   927,   825,
    864,   903,   904,   825
};

// DVD-style explorer. Same physics as payload 3's bouncer but biased:
// noticeably faster along X, only a bit faster along Y.
static volatile LONG g_bounceStop = 0;

// Navigation timeline for that same bouncer window: one folder per beat gap.
static const DWORD P6_NAV_DELAYS[] = {
    480,   424,   400,   424,   464,
    480,   377,   256,   232,   441,
    456,   392,   495,   368,   424,
    432,   208,   232,   416,   456,
    433,   424,   448,   432,   432,
    168,   311,   433,   399,   433,
    416,   441,   456,   224,   168,
    160,   248,   472,   464,   440,
    400,   496,   423,   416,   200,
    261,   403,   440,   433,   423,
    457,   424,   392,   208,   264,
    424,   449,   400,   480,   424,
    400,   416,   248,   232,   440,
    480,   433,   416,   472,   336,
    480,   424,   408,   193,   199,
    465,   271,   201,   216,   192,
    216,   224,   432,   223,   216,
    233,   208,   200,   248,   480,
    272,   168,   200,   208,   201,
    216,   505,   184,   208,   215,
    225,   192,   191,   464,   264,
    192,   208,   208,   208,   208,
    489,   200,   208,   193,   248,
    216,   256,   425,   200,   192,
    241,   215,   217,   207,   432,
    216,   232,   208,   232,   392,
    457,   416,   465,   432,   408,
    455,   392,   440,   432,   248,
    248,   192,   184,   208,   216,
    456,   208,   224,   224,   216,
    208,   224,   513,   288,   248,
    232,   216,   208,   473,   248,
    207,   232,   192,   184,   240,
    417,   223,   216,   232,   232,
    192,   216,   408,   448
};

static const wchar_t* P6_NAV_PATHS[] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\SysWOW64",
    L"C:\\Windows\\Temp",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\Users\\Public",
    L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",   // This PC
    L"::{26EE0668-A00A-44D7-9371-BEB064C98683}",   // Control Panel
    L"::{59031A47-3F72-44A7-89C5-5595FE6B30EE}",   // User's Files
    L"::{645FF040-5081-101B-9F08-00AA002F954E}",   // Recycle Bin
    L"::{374DE290-123F-4565-9164-39C4925E467B}",   // Downloads
};
static const int P6_NAV_PATH_COUNT = (int)(sizeof(P6_NAV_PATHS) / sizeof(P6_NAV_PATHS[0]));

static DWORD WINAPI BounceExplorerThread(LPVOID p) {
    (void)p;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;

    HWND hwnd = CreateFakeExplorer(L"C:\\Users", 160, 140, 0, 0);
    if (hwnd && !g_bounceStop) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        RECT rc;
        GetWindowRect(hwnd, &rc);
        double w = (double)(rc.right - rc.left);
        double h = (double)(rc.bottom - rc.top);
        if (w < 64.0) w = 1024.0;
        if (h < 64.0) h = 576.0;

        double maxX = (double)sw - w;
        double maxY = (double)sh - h;
        if (maxX < 0.0) maxX = 0.0;
        if (maxY < 0.0) maxY = 0.0;

        double x = maxX * 0.25 + ((double)rand() / RAND_MAX) * maxX * 0.5;
        double y = maxY * 0.25 + ((double)rand() / RAND_MAX) * maxY * 0.5;
        double vx = 7.0;
        double vy = 3.5;
        if (rand() % 2) vx = -vx;
        if (rand() % 2) vy = -vy;

        ExplorerMove(hwnd, (int)x, (int)y);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);

        MSG m;
        const int navCount = (int)(sizeof(P6_NAV_DELAYS) / sizeof(P6_NAV_DELAYS[0]));
        int navIdx = 0;
        DWORD navT = GetTickCount();
        while (!g_bounceStop) {
            x += vx;
            y += vy;
            if (x <= 0.0) { x = 0.0; vx = -vx; }
            else if (x >= maxX) { x = maxX; vx = -vx; }
            if (y <= 0.0) { y = 0.0; vy = -vy; }
            else if (y >= maxY) { y = maxY; vy = -vy; }

            // Already topmost from startup - move only, so the Z-order is not
            // churned (and every other top-level window repainted) per frame.
            // Posted to the explorer's own thread so the shared pump never
            // cross-thread blocks while that thread is in BrowseToIDList.
            ExplorerMove(hwnd, (int)x, (int)y);

            if (navIdx < navCount && (LONG)(GetTickCount() - navT) >= 0) {
                NavigateFakeExplorer(hwnd, P6_NAV_PATHS[navIdx % P6_NAV_PATH_COUNT]);
                navT += P6_NAV_DELAYS[navIdx];
                navIdx++;
            }

            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(15);
        }

        DestroyFakeExplorer(hwnd);
    }
    CoUninitialize();
    return 0;
}

static const int P6_DELAYS[218] = {
    224,   216,   216,   240,   447,
    201,   216,   471,   417,   216,
    207,   216,   192,   216,   216,
    249,   216,   416,   216,   233,
    183,   225,   224,   209,   231,
    216,   225,   224,   209,   231,
    216,   208,   408,   207,   216,
    417,   456,   231,   225,   231,
    216,   225,   207,   233,   216,
    408,   209,   231,   441,   432,
    447,   432,   233,   200,   216,
    216,   432,   225,   224,   416,
    441,   224,   209,   240,   216,
    207,   225,   200,   231,   408,
    209,   224,   233,   216,   224,
    216,   192,   225,   207,   240,
    209,   224,   209,   240,   416,
    216,   225,   456,   408,   216,
    224,   240,   192,   224,   240,
    201,   224,   408,   224,   209,
    416,   456,   449,   432,
    199,   216,   208,   224,   432,
    201,   207,   224,   209,   224,
    233,   401,   231,   216,   216,
    233,   224,   209,   440,   216,
    209,   240,   209,   248,   200,
    441,   192,   216,   207,   201,
    240,   224,   408,   209,   224,
    209,   240,   248,   209,   416,
    224,   216,   216,   225,   216,
    215,   449,   208,   231,   222,
    203,   209,   231,   449,   216,
    209,   224,   216,   209,   216,
    440,   240,   192,   216,   216,
    209,   216,   416,   224,   233,
    209,   224,   224,   208,   449,
    216,   231,   209,   224,   216,
    233,   416,   233,   216,   207,
    201,   240,   192,   416,   216,
    200,   216,   240,   201,   216,
    464,   224,   209,   209,   231,
    208,   224,   449,   224,   209,
    209,   216,   231,   225
};

// Payload 6 delete boxes: first at 6937 ms, then in three bursts separated by
// the 6961 / 6937 / 6112 ms pauses.
static const DWORD P6_DELETE_DELAYS[] = {
    6937,  6105,   129,   120,   135,
    153,   152,   128,  6961,  5281,
    392,   432,   441,   440,  6937,
   6112,   152,   153,   152,   144,
    128,   120,   449,   480,   401,
    423,   456,   401,   432,   440,
    441,   447,   440,   432,   456,
    425,   392,   449,   440,   432,
    432,   425,   440,   417,   440,
    432,   384,   497,   447,   432,
    401,   488,   192,   144,   144,
    161
};

void runPayload6() {
    if (SongSyncWaitHostFlag()) SongSyncWaitHost();

    bool anchored = false;
    DWORD base = SongSyncBase(126834, &anchored);
    SongSyncWait(base);

    if (!NoGdiFlag() && !IsChildMode()) {
        DWORD* wobMs = new DWORD(54781);
        HANDLE hWobble = CreateThread(NULL, 0, WobbleAmbientProc, wobMs, 0, NULL);
        if (hWobble) CloseHandle(hWobble);
    }

    PreloadIcon();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    CascadeStart();

    srand((unsigned)GetTickCount() ^ 0x6B61);

    StartToastTimeline(P6_TOAST_DELAYS,
                       (int)(sizeof(P6_TOAST_DELAYS) / sizeof(P6_TOAST_DELAYS[0])));
    SayonaraDeleteScheduleFrom(P6_DELETE_DELAYS,
                           (int)(sizeof(P6_DELETE_DELAYS) / sizeof(P6_DELETE_DELAYS[0])),
                           base ? base : GetTickCount());

    HANDLE hBounce = CreateThread(NULL, 0, BounceExplorerThread, NULL, 0, NULL);
    if (hBounce) CloseHandle(hBounce);

const int beatCount = (int)(sizeof(P6_DELAYS) / sizeof(P6_DELAYS[0]));
    bool haveClock = SongSyncAvailable();
    DWORD acc = 0;
    for (int i = 0; i < beatCount; i++) {
        if (haveClock) {
            SongSyncWaitBeat(126834 + acc);
        } else if (base) {
            DWORD target = base + acc;
            while ((LONG)(GetTickCount() - target) < 0) Sleep(5);
        } else if (i > 0) {
            Sleep((DWORD)P6_DELAYS[i - 1]);
        }
        SpawnError();
        if (!NoGdiFlag()) WobbleRetintNow();
        if (i + 1 < beatCount) acc += (DWORD)P6_DELAYS[i];
    }

    ToastShutdown();
    SayonaraDeleteCloseAll();
    CrashLog("end");
    TerminateProcess(GetCurrentProcess(), 0);
}