#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include <math.h>
#include <stdlib.h>
#include <shellapi.h>
#include "../FakeExplorer.h"
#include "../Cascade.h"
#include "../SongSync.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")

#define TRAIL 12
#define MAX_LIVE 43
#define WARP_COUNT 214
#define RENAME_AT 105
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

static DWORD WINAPI WarDriftThread(LPVOID p) {
    (void)p;
    int n = (int)g_warExplorersCount;
    if (n < 2) return 0;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    struct St { double w, h; };
    St st[WAR_EXPLORERS_RESERVE];
    for (int i = 0; i < n; i++) {
        RECT rc;
        GetWindowRect(g_warExplorers[i], &rc);
        st[i].w = (double)(rc.right - rc.left);
        st[i].h = (double)(rc.bottom - rc.top);
        if (st[i].w < 1) st[i].w = 560;
        if (st[i].h < 1) st[i].h = 340;
    }

    double cx = sw / 2.0;
    double cy = sh / 2.0;
    double maxR = (sw < sh ? sw : sh) / 2.0 - 200.0;
    if (maxR < 100.0) maxR = 100.0;
    double radius = maxR;
    double angle = -3.141592653589793 / 2.0;
    double omega = 0.8;

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (!g_warExplorersStop) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt > 0.1) dt = 0.1;

        angle += omega * dt;
        if (angle > 6.283185307179586) angle -= 6.283185307179586;

        for (int i = 0; i < n; i++) {
            HWND h = g_warExplorers[i];
            if (!IsWindow(h)) continue;
            double theta = angle + i * 3.141592653589793;
            double x = cx + radius * cos(theta) - st[i].w / 2.0;
            double y = cy + radius * sin(theta) - st[i].h / 2.0;
            SetWindowPos(h, HWND_TOPMOST, (int)x, (int)y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Sleep(16);
    }
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
    if (b.dlg && IsWindow(b.dlg)) PostMessageW(b.dlg, WM_CLOSE, 0, 0);
    if (b.anchor && IsWindow(b.anchor)) DestroyWindow(b.anchor);
    b.anchor = NULL;
}

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
        SetWindowPos(g_warExplorers[i], HWND_TOPMOST,
                     cx - w / 2,
                     (i == 0) ? (cy - h - 40) : (cy + 40),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HANDLE hDrift = CreateThread(NULL, 0, WarDriftThread, NULL, 0, NULL);
    if (hDrift) CloseHandle(hDrift);

    MSG m;
    while (!g_warExplorersStop) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(8);
    }

    for (int i = 0; i < WAR_EXPLORERS_RESERVE; i++) {
        if (g_warExplorers[i]) { DestroyFakeExplorer(g_warExplorers[i]); g_warExplorers[i] = NULL; }
    }
    InterlockedExchange(&g_warExplorersCount, 0);
    CoUninitialize();
    return 0;
}

void runPayload3() {
    bool anchored = false;
    DWORD base = SongSyncBase(29421, &anchored);
    SongSyncWait(base);

    srand((unsigned)GetTickCount());

    EnumWindows(KillProc, 0);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    CascadeStart();

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
    DWORD warStart = base ? base : GetTickCount();

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

        if (now - lastRender >= 16) {
            RenderFrame(hOverlay, ovDC, ovPix, sw, sh, ovStride, boxes, spawned);
            lastRender = now;
        }

        if (spawned == RENAME_AT) {
            SaveAndRenameTitles();
            if (!g_warExplorersGo) {
                InterlockedExchange(&g_warExplorersGo, 1);
            }
        }

        if (spawned >= WARP_COUNT &&
            elapsed >= spawnAt[WARP_COUNT - 1])
            running = false;

        Sleep(2);
    }

    RestoreTitles();
    TerminateProcess(GetCurrentProcess(), 0);
}
