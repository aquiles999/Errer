#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include <math.h>
#include <stdlib.h>
#include <shellapi.h>
#include <stdio.h>
#include "../FakeExplorer.h"
#include "../SayonaraDelete.h"
#include "../Cascade.h"
#include "../SongSync.h"
#include "../CrashLog.h"
#include "../Toast.h"
#include "../Flags.h"
#include "../WallpaperAnim.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

// rand() is unusable for "pick one of these windows": srand() only seeds the
// calling thread, and every thread here starts from the same default state, so
// the choice would repeat identically every run. Use the OS CSPRNG instead.
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID pbBuffer, ULONG cbBuffer);

static DWORD P3Rnd(void) {
    DWORD v = 0;
    if (!SystemFunction036(&v, sizeof(v))) {
        static volatile LONG seq = 0;
        v = GetTickCount() ^ (DWORD)InterlockedIncrement(&seq) ^
            (GetCurrentThreadId() * 2654435761u);
    }
    return v;
}

static int P3RndBelow(int n) {
    if (n <= 0) return 0;
    return (int)(P3Rnd() % (DWORD)n);
}

#define TRAIL 12
#define MAX_LIVE 43
#define WARP_COUNT 214
#define RENAME_AT 105
#define WAR_EXPLORERS_AT 106
// The two orbiting explorers come on screen one timed error later than the
// rename, so they are no longer tied to RENAME_AT.
#define WAR_EXPLORERS_AT 106
#define MOVE_MS 900
#define WAR_EXPLORERS_RESERVE 2
#define WAR_EXPLORERS_PARK_X 100000

extern int g_screenW;
extern int g_screenH;

static const int NAV_DELAYS[WARP_COUNT] = {
    200,   225,   208,   240,   391,
    224,   240,   425,   408,   232,
    223,   248,   217,   223,   224,
    215,   334,   323,   216,   232,
    225,   175,   249,   224,   224,
    199,   224,   208,   241,   208,
    209,   231,   360,   208,   240,
    488,   409,   208,   224,   239,
    208,   193,   247,   216,   281,
    400,   216,   224,   392,   424,
    432,   416,   240,   241,   258,
    210,   364,   224,   232,   456,
    440,   225,   200,   223,   209,
    223,   209,   239,   225,   391,
    217,   216,   209,   231,   232,
    207,   209,   232,   191,   224,
    249,   258,   166,   216,   464,
    200,   217,   423,   465,   208,
    208,   200,   240,   225,   209,
    200,   224,   424,   216,   224,
    464,   424,   440,   441,   200,
    216,   267,   158,   455,   216,
    200,   208,   216,   232,   201,
    487,   192,   192,   233,   207,
    224,   207,   448,   200,   216,
    224,   200,   216,   256,   432,
    216,   224,   208,   208,   232,
    201,   448,   224,   232,   215,
    184,   200,   232,   481,   192,
    224,   208,   208,   216,   225,
    432,   216,   224,   216,   200,
    232,   217,   456,   216,   250,
    173,   232,   208,   200,   433,
    191,   232,   240,   192,   224,
    224,   449,   208,   215,   240,
    192,   224,   216,   408,   240,
    208,   201,   232,   216,   224,
    448,   224,   216,   286,   212,
    159,   200,   409,   223,   216,
    233,   232,   224,   232,   408,
    200,   232,   207,   208,   200,
    264,   456,   200,   208,   225,
    215,   216,   233,   408
};

static const DWORD TOAST_DELAYS[70] = {
    904,   871,   913,   840,   848,
    888,   896,   881,   871,   857,
    832,   865,   888,   832,   855,
    872,   856,   864,   857,   889,
    927,   816,   881,   887,   840,
    824,   912,   832,   888,   921,
    864,   824,   904,   889,   872,
    881,   871,   849,   840,   873,
    880,   913,   856,   807,   944,
    872,   832,   856,   873,   911,
    769,   943,   896,   841,   887,
    857,   896,   848,   880,   840,
    888,   881,   888
};

static DWORD WINAPI ToastTimelineThread(LPVOID p) {
    (void)p;
    if (NoToastsFlag()) return 0;
    const int n = (int)(sizeof(TOAST_DELAYS) / sizeof(TOAST_DELAYS[0]));
    DWORD t = GetTickCount();
    for (int i = 0; i < n; i++) {
        t += TOAST_DELAYS[i];
        while ((LONG)(GetTickCount() - t) < 0) Sleep(4);
        // No fixed 700ms cap: this toast lives until the next one is due.
        DWORD shown = TOAST_DELAYS[i];
        ShowRandomErrorToast(shown);
        Sleep(shown);
        DismissToast();
    }
    return 0;
}

static const wchar_t* WAR_TITLES[16] = {
    L"Windows", L"System Error", L"Application Error", L"File Explorer", L"errer",
    L"Setup", L"DLL Host", L"Task Scheduler", L"Windows Security", L"Runtime Error!",
    L"svchost.exe - Application Error", L"Registry Editor", L"Microsoft Windows", L"Disc Utility",
    L"Error", L"Windows Explorer"
};

static const wchar_t* WAR_TEXTS[16] = {
    L"The instruction at 0x0000000000000000 referenced memory at 0x0000000000000000. The memory could not be read.",
    L"Windows cannot access the specified device, path, or file. You may not have the appropriate permissions to access the item.",
    L"The application failed to initialize properly (0xc0000005). Click on OK to terminate the application.",
    L"A fatal exception 0E has occurred at 0028:C0011E36 in VXD VMM(01) + 00010E36. The current application will be terminated.",
    L"The exception unknown software exception (0xc00000fd) occurred in the application at location 0x0000000140001A25.",
    L"Not enough storage is available to process this command.",
    L"A required DLL file could not be found.",
    L"The system cannot find the file specified.",
    L"The process cannot access the file because it is being used by another process.",
    L"Windows has detected a problem with one or more device drivers.",
    L"The instruction at 0x00000000FFC40123 referenced memory at 0x00000000FFC40123. The memory could not be written.",
    L"Cannot import file: The specified file is not a registry script.",
    L"Windows could not start because the following file is missing or corrupt: \\WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM.",
    L"The disc is not formatted. Do you want to format the disc now?",
    L"This application has requested the Runtime to terminate it in an unusual way.",
    L"Windows Explorer has stopped working. A problem caused the program to stop working correctly."
};

struct WarBox {
    HWND dlg;
    HWND anchor;
    bool alive;
    HANDLE thread;
    DWORD tid;
    int w, h;
    double x, y;
    double sx, sy;
    double fx, fy;
    DWORD moveStart;
    HBITMAP ghostBmp;
    HDC ghostDC;
    void* pix;
    int stride;
    double tx[TRAIL], ty[TRAIL];
};

struct BoxTask {
    TASKDIALOGCONFIG cfg;
};

static HRESULT CALLBACK BoxCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR ref) {
    (void)hwnd; (void)wParam; (void)lParam;
    if (msg == TDN_CREATED) {
        WarBox* b = (WarBox*)ref;
        if (b && !b->alive) SendMessageW(hwnd, TDM_CLICK_BUTTON, IDOK, 0);
    }
    return S_OK;
}

static DWORD WINAPI BoxThreadFn(LPVOID p) {
    BoxTask* t = (BoxTask*)p;
    TaskDialogIndirect(&t->cfg, NULL, NULL, NULL);
    return 0;
}

static BOOL CALLBACK EnumThrProc(HWND hwnd, LPARAM lp) {
    WarBox* b = (WarBox*)lp;
    wchar_t cls[16];
    if (GetClassNameW(hwnd, cls, 16) && _wcsicmp(cls, L"#32770") == 0) {
        b->dlg = hwnd;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK KillProc(HWND hwnd, LPARAM lp) {
    (void)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"CabinetWClass") == 0) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

static HWND g_warExplorers[WAR_EXPLORERS_RESERVE];
static volatile LONG g_warExplorersCount = 0;
static volatile LONG g_warExplorersStop = 0;
static volatile LONG g_warExplorersGo = 0;
static volatile LONG g_bounceStop = 0;
static HWND g_bounceHwnd = NULL;

struct CabEnum {
    HWND hwnd[8];
    int count;
};

static BOOL CALLBACK CabEnumProc(HWND hwnd, LPARAM lp) {
    CabEnum* e = (CabEnum*)lp;
    if (e->count >= 8) return FALSE;
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"CabinetWClass") == 0) {
        e->hwnd[e->count++] = hwnd;
    }
    return TRUE;
}

// The bouncing explorer also flips through these paths on its own clock, and
// drops the fake "Delete File" boxes on the payload 3 timeline.
static const int BOUNCE_NAV_DELAYS[] = {
    425,   416,   464,   417,   440,
    447,   201,   231,   233,   216,
    377,   456,   432,   399,   408,
    456,   249,   216,   423,   489,
    399,   456,   393,   471,   456,
    200,   209,   224,   208,   473,
    416,   425,   447,   377,   440,
    144,   153,   135,   136,   161,
    151,   464,   441,   416,   425,
    432,   440,   360,   281,   207,
    465,   423,   425,   464,   432,
    416,   209,   248,   393,   464,
    432,   425,   464,   416,   441,
    183,   297,   216,   176,   440,
    417,   456,   480,   399,   417,
    456,   440,   432,   456,   399,
    417,   432,   416,   216,   233,
    495,   401,   440,   432,   417,
    440,   440,   233,   209,   423,
    473,   432,   416,   473,   401,
    423,   456,   432,   432,   440,
    449,   425,   423,   417,   159,
    153,   128,   144,   137,   168,
    471,   408,   432,   408,   425,
    440,   432,   441,   440,   456,
    416,   432,   408,   432,   248,
    216,   425,   408,   456,   473,
    440,   449,   464,   425,   399,
    393,   480,   464,   408,   408,
    416,   153,   152,   144,   159,
    112
};

static const wchar_t* BOUNCE_NAV_PATHS[] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\Temp",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\Users\\Public",
    L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    L"::{645FF040-5081-101B-9F08-00AA002F954E}",
    L"::{374DE290-123F-4565-9164-39C4925E467B}"
};

static const DWORD P3_DELETE_DELAYS[] = {
    6945,  6112,   120,   161,   137,
    152,   137,   152,  6952,  5218,
    447,   432,   408,   464,  6922,
   6072,   153,   159,   153,   144,
    144,   144,   432,   440,   425,
    447,   425,   425,   447,   456,
    416,   417,   464,   440,   432,
    384,   216,   225,   447,   425,
    432,   456,   440,   417,   456,
    447,   441,   416,   464,   449,
    425,   440,   360,   159,   144,
    144,   136,   144,   136
};

// Bounce state. Only the move thread touches these after init, so no locking.
static double g_bx = 0, g_by = 0, g_bvx = 0, g_bvy = 0;
static double g_bmaxX = 0, g_bmaxY = 0;

// Nav timing, dumped by the --debug log so stutter can be diagnosed from
// numbers instead of guesses.
//
// These MUST use QueryPerformanceCounter, not GetTickCount64: the latter has
// ~15.6 ms granularity, so it cannot resolve a SetWindowPos at all - it just
// reports 0 or one whole tick, and averaging those produces numbers that look
// like measurements but are pure artefacts.
static inline ULONGLONG NowUs(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)((double)c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

static volatile LONG g_navCount = 0;
static volatile LONG64 g_navTotalUs = 0;
static volatile LONG64 g_navMaxUs = 0;
static volatile LONG64 g_moveTotalUs = 0;
static volatile LONG64 g_moveMaxUs = 0;
static volatile LONG g_moveCount = 0;
static volatile LONG64 g_lastFrameUs = 0;
static volatile LONG64 g_frameGapTotal = 0;
static volatile LONG64 g_frameGapMax = 0;
static volatile LONG64 g_frameGapOver50 = 0;
static volatile LONG64 g_pumpTotalUs = 0;
static volatile LONG64 g_pumpMaxUs = 0;
static volatile LONG g_pumpCount = 0;
static volatile LONG64 g_renderTotalUs = 0;
static volatile LONG64 g_renderMaxUs = 0;
static volatile LONG g_renderCount = 0;

static void DumpTiming() {
    LONG n = g_navCount;
    if (!n) return;
    // Two short lines: LogLine uses a 1024-char buffer, and the single
    // combined line was being truncated before pump_* made it into the log.
    char buf[512];
    snprintf(buf, sizeof(buf),
             "nav: count=%ld browse_avg=%.1fms browse_max=%.1fms setwinpos_avg=%.2fms "
             "setwinpos_max=%.2fms paints=%lu moves=%ld",
             n,
             (double)g_navTotalUs / n / 1000.0,
             (double)g_navMaxUs / 1000.0,
             (double)g_moveTotalUs / (g_moveCount ? g_moveCount : 1) / 1000.0,
             (double)g_moveMaxUs / 1000.0,
             FakeExplorerPaintCount(), g_moveCount);
    CrashLog(buf);

    snprintf(buf, sizeof(buf),
             "frame: avg=%.2fms max=%.2fms gaps>50ms=%lld "
             "pump_avg=%.2fms pump_max=%.2fms "
             "overlay_avg=%.2fms overlay_max=%.2fms renders=%ld",
             (double)g_frameGapTotal / (g_moveCount ? g_moveCount : 1) / 1000.0,
             (double)g_frameGapMax / 1000.0,
             (long long)g_frameGapOver50,
             (double)g_pumpTotalUs / (g_pumpCount ? g_pumpCount : 1) / 1000.0,
             (double)g_pumpMaxUs / 1000.0,
             (double)g_renderTotalUs / (g_renderCount ? g_renderCount : 1) / 1000.0,
             (double)g_renderMaxUs / 1000.0,
             g_renderCount);
    CrashLog(buf);
}

// Movement only, on its own thread. SetWindowPos is safe cross-thread (unlike
// DestroyWindow), so the window keeps bouncing at a steady 60 fps even while
// the STA thread is blocked inside a synchronous BrowseToIDList.

// Pace the bounce to the display's real refresh rate.
//
// Hardcoding 60 looks fine on a 60 Hz panel but judders badly on 100 Hz: 100/60
// is 1.67, so updates land unevenly between refreshes (sometimes one refresh
// between moves, sometimes two) and the motion reads as a stutter even when
// every frame is dead on time.
static int g_refreshHz = 0;

static int DetectRefreshHz(void) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 480) {
        return (int)dm.dmDisplayFrequency;
    }

    HDC hdc = GetDC(NULL);
    int vr = hdc ? GetDeviceCaps(hdc, VREFRESH) : 0;
    if (hdc) ReleaseDC(NULL, hdc);
    if (vr >= 30 && vr <= 480) return vr;

    return 60;   // sane default
}

// Detected once and cached: the bouncing explorer and the ghost-trail overlay
// must share one cadence, or they judder against each other.
static int RefreshHz(void) {
    if (g_refreshHz == 0) g_refreshHz = DetectRefreshHz();
    return g_refreshHz;
}

static DWORD WINAPI BounceExplorerThread(LPVOID p) {
    (void)p;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;

    // Explicit size rather than 0,0 (which defaults to 1024x576). Measured browse
    // cost was ~29 ms fixed per navigation regardless of folder, so part of
    // that is laying out a 1024x576 view - shrink it and see if it drops.
    g_bounceHwnd = CreateFakeExplorer(L"C:\\Users", 120, 120, 640, 400);
    if (g_bounceHwnd && !g_bounceStop) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        RECT rc;
        GetWindowRect(g_bounceHwnd, &rc);
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
        double vx = 4.0;
        double vy = 3.0;
        if (rand() % 2) vx = -vx;
        if (rand() % 2) vy = -vy;

        g_bx = x; g_by = y; g_bvx = vx; g_bvy = vy;
        g_bmaxX = maxX; g_bmaxY = maxY;

        // Topmost once; the move thread never touches Z-order again. Re-asserting
        // HWND_TOPMOST every frame changes the Z-order each time, which sends
        // WM_WINDOWPOSCHANGING/CHANGED to every other top-level window - with
        // 50-odd delete boxes on screen that forced them all to repaint every frame.
        SetWindowLongPtrW(g_bounceHwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(g_bounceHwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
        SetWindowPos(g_bounceHwnd, HWND_TOPMOST, (int)x, (int)y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);

// Loop structure copied from payload 6's BounceExplorerThread, which bounces
        // and navigates without stuttering. Differences that matter:
        //   - one message pump per frame, always AFTER the navigation, so the
        //     shell control's paint is dispatched every frame on a steady
        //     cadence instead of being bunched up.
        //   - navigation deadline accumulates (navT += delay) from a single
        //     start point rather than being re-armed after each browse.
        //   - plain Sleep(15) pacing, no QPC deadline loop.
        // payload 3 keeps its own nav paths and delays.
        MSG m;
        const int navCount = (int)(sizeof(BOUNCE_NAV_DELAYS) / sizeof(BOUNCE_NAV_DELAYS[0]));
        const int pathCount = (int)(sizeof(BOUNCE_NAV_PATHS) / sizeof(BOUNCE_NAV_PATHS[0]));
        int navIdx = 0;
        DWORD navT = GetTickCount();

        double bx = g_bx, by = g_by;
        double bvx = g_bvx, bvy = g_bvy;

        while (!g_bounceStop) {
            bx += bvx;
            by += bvy;
            if (bx <= 0.0) { bx = 0.0; bvx = -bvx; }
            else if (bx >= g_bmaxX) { bx = g_bmaxX; bvx = -bvx; }
            if (by <= 0.0) { by = 0.0; bvy = -bvy; }
            else if (by >= g_bmaxY) { by = g_bmaxY; bvy = -bvy; }

            SetWindowPos(g_bounceHwnd, HWND_TOPMOST, (int)bx, (int)by, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
            InterlockedIncrement(&g_moveCount);

            if (navIdx < navCount && (LONG)(GetTickCount() - navT) >= 0) {
                ULONGLONG n0 = NowUs();
                NavigateFakeExplorer(g_bounceHwnd, BOUNCE_NAV_PATHS[navIdx % pathCount]);
                LONG64 us = (LONG64)(NowUs() - n0);
                InterlockedIncrement(&g_navCount);
                g_navTotalUs += us;
                if (us > g_navMaxUs) g_navMaxUs = us;
                navT += BOUNCE_NAV_DELAYS[navIdx];
                navIdx++;
                if ((g_navCount % 10) == 0) DumpTiming();
            }

            ULONGLONG p0 = NowUs();
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            {
                // Dispatching is where the shell control RENDERS the folder
                // view - BrowseToIDList only fills the model.
                LONG64 us = (LONG64)(NowUs() - p0);
                g_pumpTotalUs += us;
                InterlockedIncrement(&g_pumpCount);
                if (us > g_pumpMaxUs) g_pumpMaxUs = us;
            }

            // Frame gap: the wall time between consecutive frames. This is the
            // number that corresponds to what the eye sees as a stutter.
            {
                ULONGLONG now = NowUs();
                if (g_lastFrameUs) {
                    LONG64 gap = (LONG64)(now - g_lastFrameUs);
                    g_frameGapTotal += gap;
                    if (gap > g_frameGapMax) g_frameGapMax = gap;
                    if (gap > 50000) g_frameGapOver50++;
                }
                g_lastFrameUs = now;
            }

            Sleep(15);
        }

        DumpTiming();

        DestroyFakeExplorer(g_bounceHwnd);
        g_bounceHwnd = NULL;
    }
    CoUninitialize();
    return 0;
}

static void CaptureGhost(WarBox& b) {
    RECT rc;
    GetWindowRect(b.dlg, &rc);
    b.w = rc.right - rc.left;
    b.h = rc.bottom - rc.top;
    if (b.w <= 0 || b.h <= 0) { b.w = 300; b.h = 150; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = b.w;
    bi.bmiHeader.biHeight = -b.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC ref = GetDC(NULL);
    b.ghostBmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &b.pix, NULL, 0);
    b.ghostDC = CreateCompatibleDC(ref);
    SelectObject(b.ghostDC, b.ghostBmp);
    ReleaseDC(NULL, ref);
    b.stride = b.w * 4;

    BOOL ok = PrintWindow(b.dlg, b.ghostDC, 0);
    if (!ok) {
        HDC wdc = GetWindowDC(b.dlg);
        BitBlt(b.ghostDC, 0, 0, b.w, b.h, wdc, 0, 0, SRCCOPY);
        ReleaseDC(b.dlg, wdc);
    }
}

static void UpdateBoxMotion(WarBox& b, DWORD now) {
    double u = 0.0;
    if (now > b.moveStart) {
        u = (double)(now - b.moveStart) / (double)MOVE_MS;
        if (u > 1.0) u = 1.0;
    }
    double e = u * u * (3.0 - 2.0 * u);
    b.x = b.sx + (b.fx - b.sx) * e;
    b.y = b.sy + (b.fy - b.sy) * e;
    for (int j = 0; j < TRAIL; j++) {
        double g = u - (j + 1) * 0.05;
        if (g < 0.0) g = 0.0;
        double eg = g * g * (3.0 - 2.0 * g);
        b.tx[j] = b.sx + (b.fx - b.sx) * eg;
        b.ty[j] = b.sy + (b.fy - b.sy) * eg;
    }
}

static void CompositeGhost(void* dst, int dstW, int dstH, int dstStride,
                           const void* src, int srcW, int srcH, int srcStride,
                           int dx, int dy, int alpha, int step) {
    if (alpha <= 0) return;
    int sx0 = 0, sy0 = 0, ddx = dx, ddy = dy;
    if (ddx < 0) { sx0 = -ddx; ddx = 0; }
    if (ddy < 0) { sy0 = -ddy; ddy = 0; }
    int w = srcW - sx0;
    if (ddx + w > dstW) w = dstW - ddx;
    int h = srcH - sy0;
    if (ddy + h > dstH) h = dstH - ddy;
    if (w <= 0 || h <= 0) return;

    const unsigned char* s = (const unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;
    for (int y = 0; y < h; y += step) {
        const unsigned char* sp = s + (sy0 + y) * srcStride + sx0 * 4;
        unsigned char* dp = d + (ddy + y) * dstStride + ddx * 4;
        for (int x = 0; x < w; x += step) {
            dp[0] = (unsigned char)((unsigned int)sp[0] * alpha / 255);
            dp[1] = (unsigned char)((unsigned int)sp[1] * alpha / 255);
            dp[2] = (unsigned char)((unsigned int)sp[2] * alpha / 255);
            dp[3] = (unsigned char)alpha;
            sp += 4 * step;
            dp += 4 * step;
        }
    }
}

static HWND CreateOverlay(int sw, int sh, HBITMAP& bmp, HDC& memDC, void*& pix, int& stride) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"errerWarOverlay";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                L"errerWarOverlay", L"", WS_POPUP, 0, 0, sw, sh,
                                NULL, NULL, GetModuleHandle(NULL), NULL);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC ref = GetDC(NULL);
    bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &pix, NULL, 0);
    memDC = CreateCompatibleDC(ref);
    SelectObject(memDC, bmp);
    stride = sw * 4;
    ReleaseDC(NULL, ref);
    return hwnd;
}

static void RenderFrame(HWND hOverlay, HDC memDC, void* pix, int sw, int sh, int stride,
                        WarBox* boxes, int count) {
    if (count <= 0) return;
    memset(pix, 0, (size_t)sw * sh * 4);

    for (int i = 0; i < count; i++) {
        WarBox& b = boxes[i];
        if (!b.alive || !b.pix) continue;
        for (int j = 0; j < TRAIL; j++) {
            int alpha = 216 - j * 16;
            CompositeGhost(pix, sw, sh, stride, b.pix, b.w, b.h, b.stride,
                           (int)b.tx[j], (int)b.ty[j], alpha, 2);
        }
    }

    POINT ptDst = { 0, 0 };
    SIZE sz = { sw, sh };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hOverlay, NULL, &ptDst, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

struct TitleEntry {
    HWND hwnd;
    wchar_t original[256];
};

static TitleEntry g_titles[512];
static int g_titleCount = 0;

static BOOL CALLBACK TitleEnumProc(HWND hwnd, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (g_titleCount >= 512) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    TitleEntry& te = g_titles[g_titleCount];
    te.hwnd = hwnd;
    GetWindowTextW(hwnd, te.original, 256);
    SetWindowTextW(hwnd, L"bad apple");
    g_titleCount++;
    return TRUE;
}

static void SaveAndRenameTitles() {
    if (g_titleCount > 0) return;
    EnumWindows(TitleEnumProc, 0);
}

void RestoreTitles() {
    for (int i = 0; i < g_titleCount; i++) {
        if (IsWindow(g_titles[i].hwnd)) SetWindowTextW(g_titles[i].hwnd, g_titles[i].original);
    }
    g_titleCount = 0;
}

static void InitBox(WarBox& b, int idx, int sw, int sh, BoxTask* task, WarBox* boxes, int liveCount) {
    double rnd = (double)rand() / RAND_MAX;
    b.sx = 150 + rnd * (sw - 300);
    rnd = (double)rand() / RAND_MAX;
    b.sy = 150 + rnd * (sh - 300);
    for (int attempt = 0; attempt < 40; attempt++) {
        bool ok = true;
        for (int j = 0; j < liveCount; j++) {
            double dx = boxes[j].sx - b.sx;
            double dy = boxes[j].sy - b.sy;
            if (dx * dx + dy * dy < 280.0 * 280.0) { ok = false; break; }
        }
        if (ok) break;
        rnd = (double)rand() / RAND_MAX;
        b.sx = 150 + rnd * (sw - 300);
        rnd = (double)rand() / RAND_MAX;
        b.sy = 150 + rnd * (sh - 300);
    }

    double maxR = (sw < sh ? sw : sh) / 2.0 - 170.0;
    if (maxR < 20.0) maxR = 20.0;
    double cx = sw / 2.0;
    double cy = sh / 2.0;
    double ang = idx * (2.0 * 3.141592653589793 / WARP_COUNT);
    double rr = maxR * (0.15 + 0.85 * ((double)rand() / RAND_MAX));
    b.fx = cx + rr * cos(ang);
    b.fy = cy + rr * sin(ang);

    b.x = b.sx;
    b.y = b.sy;
    for (int j = 0; j < TRAIL; j++) { b.tx[j] = b.sx; b.ty[j] = b.sy; }
    b.moveStart = 0;
    b.w = 300;
    b.h = 150;
    b.alive = true;
    b.dlg = NULL;
    b.ghostBmp = NULL;
    b.ghostDC = NULL;
    b.pix = NULL;

    static LONG anchorRegistered = 0;
    if (InterlockedCompareExchange(&anchorRegistered, 1, 0) == 0) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"errerWarAnchor";
        RegisterClassW(&wc);
    }
    b.anchor = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"errerWarAnchor", L"", WS_POPUP,
                               (int)b.x, (int)b.y, 300, 150,
                               NULL, NULL, GetModuleHandle(NULL), NULL);

    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = b.anchor;
    cfg.dwFlags = TDF_USE_HICON_MAIN;
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = WAR_TITLES[idx % 16];
    cfg.pszContent = WAR_TEXTS[idx % 16];
    if (idx % 2 == 0) cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_WARNING));
    else cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_INFORMATION));
    cfg.pfCallback = BoxCallback;
    cfg.lpCallbackData = (LONG_PTR)&b;
    task->cfg = cfg;

    b.thread = CreateThread(NULL, 0, BoxThreadFn, task, 0, &b.tid);
}

static void DismissBox(WarBox& b) {
    if (!b.alive) return;
    b.alive = false;
    if (b.dlg && IsWindow(b.dlg)) {
        // Original method: blocking click; native task dialogs only close on a
        // synchronous TDM_CLICK_BUTTON.
        SendMessageW(b.dlg, TDM_CLICK_BUTTON, IDOK, 0);
    }
    if (b.anchor && IsWindow(b.anchor)) DestroyWindow(b.anchor);
    b.anchor = NULL;
}

// Once the two parked explorers come on screen, one of them (picked at random
// each step) flips to the next path on this schedule.
static const int WAR_NAV_DELAYS[] = {
    432,   449,   440,   425,   416,
    432,   432,   224,   216,   441,
    464,   416,   441,   399,   441,
    216,   264,   224,   200,   432,
    417,   447,   417,   440,   495,
    393,   224,   216,   425,   399,
    432,   456,   425,   432,   432,
    440,   473,   449,   432,   255,
    177,   447,   401,   416,   201,
    224,   432,   471,   401,   432,
    440,   408,   345,   168,   207,
    225,   432,   432,   432,   399,
    425,   488,   168,   225,   264,
    159,   473,   440,   417,   432,
    407,   433,   447,   456,   440,
    465,   399,   441,   192,   408,
    279,   473,   384,   440,   449,
    440,   201,   231,   225,   200,
    209,   231,   432,   432,   425,
    473,   416,   464,   377,   240,
    264,   408,   423,   417,   480,
    408,   416,   440,   400,   257,
    320,   233,   521,   447,   441,
    447,   408,   449,   425,   159,
    281,   216,   584,   480,   432,
    425,   447,   425,   209,   264,
    207,   264,   425,   408,   408,
    432,   449,   392,   473,   432,
    431,   449,   408,   441,   560,
    368,   377
};

static DWORD WINAPI ExplorerSetupThread(LPVOID p) {
    (void)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    static const wchar_t* dirs[WAR_EXPLORERS_RESERVE] = {
        L"C:\\",
        L"C:\\Users"
    };

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    int cx = sw >= 1120 ? sw / 2 : 220;
    int cy = sh >= 760 ? sh / 2 : 200;
    int w = 560, h = 340;

    for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
        g_warExplorers[i] = CreateFakeExplorer(dirs[i], WAR_EXPLORERS_PARK_X,
                                               0, w, h);
    }
    InterlockedExchange(&g_warExplorersCount, WAR_EXPLORERS_RESERVE);
    InterlockedExchange(&g_warExplorersStop, 0);
    InterlockedExchange(&g_warExplorersGo, 0);

    while (!g_warExplorersStop && !g_warExplorersGo) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(8);
    }

    if (g_warExplorersStop) {
        for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
            if (g_warExplorers[i]) { DestroyFakeExplorer(g_warExplorers[i]); g_warExplorers[i] = NULL; }
        }
        InterlockedExchange(&g_warExplorersCount, 0);
        CoUninitialize();
        return 0;
    }

    for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
        ExplorerMove(g_warExplorers[i],
                     cx - w / 2,
                     (i == 0) ? (cy - h - 40) : (cy + 40));
    }

    // Orbit state lives on THIS pump thread (matching the decompiled
    // FUN_140001610 pattern: message pumping and window movement on one
    // thread). This avoids the cross-thread SendMessage deadlock that used to
    // freeze everything while BrowseToIDList blocked the pump.
    double maxRx = (double)sw / 2.0 - 40.0;   // stretch to left/right edges
    double maxRy = (double)sh / 2.0 - 40.0;   // stretch to top/bottom edges
    if (maxRx < 100.0) maxRx = 100.0;
    if (maxRy < 100.0) maxRy = 100.0;
    double angle = -3.141592653589793 / 2.0;
    const double SEG_DURATION_MS = 1000.0;   // 1 second per segment
    LARGE_INTEGER segStart;
    QueryPerformanceCounter(&segStart);
    double segStartAngle = angle;

    HDC hdcScreen = GetDC(NULL);
    int dpiX = hdcScreen ? GetDeviceCaps(hdcScreen, LOGPIXELSX) : 96;
    if (hdcScreen) ReleaseDC(NULL, hdcScreen);
    if (dpiX <= 0) dpiX = 96;

    double omega = 2.0 * ((double)dpiX / 96.0);
    if (omega < 0.25) omega = 0.25;
    double segEndAngle = segStartAngle + omega * (SEG_DURATION_MS / 1000.0);

    struct St { double w, h; };
    St st[WAR_EXPLORERS_RESERVE];
    for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
        RECT rc;
        GetWindowRect(g_warExplorers[i], &rc);
        st[i].w = (double)(rc.right - rc.left);
        st[i].h = (double)(rc.bottom - rc.top);
        if (st[i].w < 1) st[i].w = 560;
        if (st[i].h < 1) st[i].h = 340;
    }

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double invFreq = 1.0 / (double)freq.QuadPart;

    // Song-time window for explorer movement: start 57320 ms, end 85131 ms.
    // Outside this window the explorers stay parked at their initial positions.
    static const double P3_EXPLORER_START_MS = 57320.0;
    static const double P3_EXPLORER_END_MS   = 85131.0;

    timeBeginPeriod(1);

    // Navigation runs on this thread's pump. BrowseToIDList is synchronous and
    // blocks the pump while enumerating the folder; NavigateFakeExplorer uses
    // WM_SETREDRAW around it so the shell control doesn't paint partial content
    // mid-browse. Because orbit movement is now on the same thread as the
    // windows, SetWindowPos never cross-thread blocks while BrowseToIDList runs
    // - no deadlock, no trails, no jump when the browse returns.
    const int navCount = (int)(sizeof(WAR_NAV_DELAYS) / sizeof(WAR_NAV_DELAYS[0]));
    const int pathCount = (int)(sizeof(BOUNCE_NAV_PATHS) / sizeof(BOUNCE_NAV_PATHS[0]));
    int navIdx = 0;
    ULONGLONG navNext = GetTickCount64() + (ULONGLONG)WAR_NAV_DELAYS[0];

    MSG m;
    while (!g_warExplorersStop) {
        // Pump messages first so the window stays responsive.
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }

        // Song-time gate: only orbit between 57320 and 85131 ms of the track.
        long long songMs = SongNowMs();
        bool inWindow = (songMs >= 0 &&
                         songMs >= (long long)P3_EXPLORER_START_MS &&
                         songMs <= (long long)P3_EXPLORER_END_MS);

        if (inWindow) {
            // Orbit movement - same thread as the windows, so no cross-thread
            // blocking. Ease-out: full speed at segment start, slows to stop at end.
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double dt = (double)(now.QuadPart - last.QuadPart) * invFreq;
            last = now;
            if (dt > 0.1) dt = 0.1;

            double segElapsed = (double)(now.QuadPart - segStart.QuadPart) * invFreq * 1000.0;
            double t = segElapsed / SEG_DURATION_MS;
            if (t > 1.0) {
                QueryPerformanceCounter(&segStart);
                segStartAngle = segEndAngle;
                segEndAngle += omega * (SEG_DURATION_MS / 1000.0);
                segElapsed = 0.0;
                t = 0.0;
            }
            double ease = t;  // linear: constant speed, no stop-and-go
            double curAngle = segStartAngle + (segEndAngle - segStartAngle) * ease;

            // Circle radius shrinks linearly across the song window:
            // max radius at song 57320, zero at song 85131. Orbit moves at
            // constant linear speed; radius just tracks song progress so the
            // explorers come to rest at center exactly when the window ends.
            double windowSpan = P3_EXPLORER_END_MS - P3_EXPLORER_START_MS;
            double windowProgress = (songMs - (long long)P3_EXPLORER_START_MS) / windowSpan;
            if (windowProgress < 0.0) windowProgress = 0.0;
            if (windowProgress > 1.0) windowProgress = 1.0;
            double radiusX = maxRx * (1.0 - windowProgress);
            double radiusY = maxRy * (1.0 - windowProgress);

            for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
                HWND h = g_warExplorers[i];
                if (!IsWindow(h)) continue;
                double theta = curAngle + i * 3.141592653589793;
                double x = cx + radiusX * cos(theta) - st[i].w / 2.0;
                double y = cy + radiusY * sin(theta) - st[i].h / 2.0;
                ExplorerMove(h, (int)x, (int)y);
            }

            if (DebugFlag()) {
                char pos[256] = {0};
                int posLen = 0;
                for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
                    HWND h = g_warExplorers[i];
                    if (!IsWindow(h)) continue;
                    RECT rc;
                    GetWindowRect(h, &rc);
                    posLen += _snprintf_s(pos + posLen, sizeof(pos) - posLen, _TRUNCATE,
                                          " e%d=(%d,%d)", i, rc.left, rc.top);
                }
                CrashLogf("orbit songMs=%lld ease=%.3f rx=%.1f ry=%.1f angle=%.2f%s",
                          songMs, ease, radiusX, radiusY, curAngle, pos);
            }
        } else {
            // Not in the song-time window: just advance the clock so dt stays
            // sane when the window re-opens.
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            last = now;
        }

        if (navIdx < navCount && GetTickCount64() >= navNext) {
            int live = (int)g_warExplorersCount;
            if (live > 0) {
                int pick = P3RndBelow(live);
                HWND target = g_warExplorers[pick];
                if (target && IsWindow(target))
                    NavigateFakeExplorer(target, BOUNCE_NAV_PATHS[navIdx % pathCount]);
            }
            navIdx++;
            if (navIdx < navCount)
                navNext = GetTickCount64() + (ULONGLONG)WAR_NAV_DELAYS[navIdx];
        }
        Sleep(8);
    }
    timeEndPeriod(1);

    for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
        if (g_warExplorers[i]) { DestroyFakeExplorer(g_warExplorers[i]); g_warExplorers[i] = NULL; }
    }
    InterlockedExchange(&g_warExplorersCount, 0);
    CoUninitialize();
    return 0;
}

// Middle wallpaper segment.
//
// The show has two points where the real desktop wallpaper is animated: this
// one, mid-show, and the ending in payload 8. This segment runs on its own
// thread so the bouncing explorer and the error boxes keep their own timing.
//
// It deliberately does NOT put the wallpaper back when it finishes: the user's
// original is copied to a temp backup here and stays there, and the ending
// segment in payload 8 is what puts it back. That way the last frame of this
// segment stays on screen instead of snapping back to the desktop.
#define P3_MID_SONG_MS      53793
#define P3_MID_FRAME_BASE   2000
#define P3_MID_FRAME_COUNT  64
#define P3_MID_FPS          30

static DWORD WINAPI MidWallpaperThread(LPVOID) {
    if (!ChangeWallpaperFlag()) return 0;
    WallpaperAnimSpec spec = {};
    spec.resBase      = P3_MID_FRAME_BASE;
    spec.frameCount   = P3_MID_FRAME_COUNT;
    spec.fps          = P3_MID_FPS;
    spec.songMs       = P3_MID_SONG_MS;
    spec.tag          = WALLPAPER_TAG_MID;
    spec.restoreAtEnd = false;
    // First segment of the show: always capture the wallpaper that is on
    // screen right now, replacing any backup left behind by an aborted run.
    spec.forceBackup  = true;

    WallpaperAnimResult result = WallpaperAnimRun(spec);
    CrashLogf("wallpaper middle: shown=%d skipped=%d failed=%d backedUp=%d restored=%d",
              result.framesShown, result.framesSkipped, result.framesFailed,
              result.backedUp ? 1 : 0, result.restored ? 1 : 0);
    return 0;
}

void runPayload3() {
    if (SongSyncWaitHostFlag()) SongSyncWaitHost();

    // Start the middle wallpaper segment first so its frames are pulled out of
    // the exe and into RAM while this payload is still warming up. The thread
    // then waits for its own song slot on its own.
    HANDLE hMidWallpaper = CreateThread(NULL, 0, MidWallpaperThread, NULL, 0, NULL);
    if (hMidWallpaper) CloseHandle(hMidWallpaper);

    // Parse every nav path into the PIDL cache BEFORE the clock wait. This is
    // slow (virtual folders spin up shell extensions), and SongSyncWait is an
    // absolute deadline - so doing the work first is free, whereas doing it
    // after would push the whole payload (and its delete-box timeline) late.
    FakeExplorerPrimePaths(BOUNCE_NAV_PATHS,
                           (int)(sizeof(BOUNCE_NAV_PATHS) / sizeof(BOUNCE_NAV_PATHS[0])));

    bool anchored = false;
    DWORD base = SongSyncBase(29421, &anchored);
    SongSyncWait(base);

    srand((unsigned)GetTickCount());

    EnumWindows(KillProc, 0);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    CascadeStart();

    HANDLE hToastTimeline = CreateThread(NULL, 0, ToastTimelineThread, NULL, 0, NULL);
    if (hToastTimeline) CloseHandle(hToastTimeline);

    HANDLE hBounce = CreateThread(NULL, 0, BounceExplorerThread, NULL, 0, NULL);
    if (hBounce) CloseHandle(hBounce);

    // Delete boxes across payload 3: 52 boxes, first at 6921 ms then on the
    // listed gaps.
    SayonaraDeleteScheduleFrom(P3_DELETE_DELAYS,
                           (int)(sizeof(P3_DELETE_DELAYS) / sizeof(P3_DELETE_DELAYS[0])),
                           base ? base : GetTickCount());

    HANDLE hSetup = CreateThread(NULL, 0, ExplorerSetupThread, NULL, 0, NULL);
    if (hSetup) CloseHandle(hSetup);

    int sw = g_screenW;
    int sh = g_screenH;
    if (sw <= 0) sw = GetSystemMetrics(SM_CXSCREEN);
    if (sh <= 0) sh = GetSystemMetrics(SM_CYSCREEN);

    HBITMAP ovBmp = NULL;
    HDC ovDC = NULL;
    void* ovPix = NULL;
    int ovStride = 0;
    HWND hOverlay = CreateOverlay(sw, sh, ovBmp, ovDC, ovPix, ovStride);

    DWORD spawnAt[WARP_COUNT];
    DWORD acc = 0;
    for (int i = 0; i < WARP_COUNT; i++) { spawnAt[i] = acc; acc += NAV_DELAYS[i]; }

    static WarBox boxes[    WARP_COUNT];
    static BoxTask tasks[    WARP_COUNT];
    DWORD boxStart[    WARP_COUNT];
    double lead = 90.0;
    int spawned = 0;
    DWORD warStart = SongSyncStart(29421, base ? base : GetTickCount());

    bool running = true;
    DWORD lastRender = 0;
    MSG msg;
    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        DWORD now = GetTickCount();
        DWORD elapsed = now - warStart;

        while (spawned < WARP_COUNT && (double)elapsed + lead >= (double)spawnAt[spawned]) {
            int live = 0;
            for (int i = 0; i < spawned; i++) if (boxes[i].alive) live++;
            if (live >= MAX_LIVE) {
                for (int i = 0; i < spawned; i++) {
                    if (boxes[i].alive) { DismissBox(boxes[i]); break; }
                }
            }
            boxStart[spawned] = elapsed;
            InitBox(boxes[spawned], spawned, sw, sh, &tasks[spawned], boxes, spawned);
            boxes[spawned].moveStart = elapsed + 500;
            spawned++;
        }

        for (int i = 0; i < spawned; i++) {
            WarBox& b = boxes[i];
            if (!b.alive) continue;

            if (!b.dlg) {
                EnumThreadWindows(b.tid, EnumThrProc, (LPARAM)&b);
                if (b.dlg) {
                    CaptureGhost(b);
                    double lat = (double)(now - warStart) - boxStart[i];
                    lead = lead * 0.85 + lat * 0.15;
                    if (lead < 20.0) lead = 20.0;
                    if (lead > 250.0) lead = 250.0;
                }
            }

            if (b.dlg && !IsWindow(b.dlg)) { b.alive = false; continue; }

            UpdateBoxMotion(b, now);
            if (b.dlg) {
                SetWindowPos(b.dlg, HWND_TOPMOST, (int)b.x, (int)b.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        // Same cadence as the bouncing explorer. This used to be hardcoded 16 ms
        // (60 fps) while the explorer ran at the display rate, so on a 100 Hz
        // panel the ghost trails updated at 60 Hz against a window moving at
        // 100 Hz - two animations at different rhythms, which reads as judder.
        const DWORD frameMs = (DWORD)(1000 / (DWORD)RefreshHz());
        if (now - lastRender >= frameMs) {
            ULONGLONG r0 = NowUs();
            RenderFrame(hOverlay, ovDC, ovPix, sw, sh, ovStride, boxes, spawned);
            LONG64 rus = (LONG64)(NowUs() - r0);
            g_renderTotalUs += rus;
            InterlockedIncrement(&g_renderCount);
            if (rus > g_renderMaxUs) g_renderMaxUs = rus;
            lastRender = now;
        }

        if (spawned >= RENAME_AT) {
            InterlockedExchange(&g_bounceStop, 1);
            SaveAndRenameTitles();
        }

        // The two orbiting explorers come in on their own beat - the 106th
        // dialog - independent of the rename above.
        if (spawned >= WAR_EXPLORERS_AT) {
            if (!g_warExplorersGo) {
                InterlockedExchange(&g_warExplorersGo, 1);
            }
        }

        if (spawned >= WARP_COUNT &&
            elapsed >= spawnAt[WARP_COUNT - 1])
            running = false;

        Sleep(2);
    }

    InterlockedExchange(&g_bounceStop, 1);
    RestoreTitles();
    SayonaraDeleteCloseAll();
    ToastShutdown();
    CrashLog("end");
    TerminateProcess(GetCurrentProcess(), 0);
}
