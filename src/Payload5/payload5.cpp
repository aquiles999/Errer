#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <string>
#include <math.h>
#include "../FakeExplorer.h"
#include "../SongSync.h"
#include "../Flags.h"

#define P5_DURATION_MS 13900
#define P5_REFRESH_EVERY 4
#define P5_SLEEP_MS 12
#define P5_WOBBLE_AMPL -6.0f
#define P5_OSC_SCALE 0.08f
#define P5_NUM_NAV 35

static const int P5_NAV_DELAYS[P5_NUM_NAV] = {
    433,   423,   441,   409,   464,
    447,   385,   224,   225,   415,
    449,   416,   424,   473,   415,
    401,   232,   248,   399,   456,
    426,   438,   440,   440,   368,
    200,   320,   520,   464,   336,
    409,   440,   447,   384,   352
};

static const wchar_t* P5_NAV_PATHS[] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\SysWOW64",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\Users\\Public",
    L"C:\\Windows\\Temp",
    L"C:\\Windows\\Fonts",
    L"C:\\Windows\\System32\\Drivers",
    L"C:\\ProgramData"
};

static float P5Osc(unsigned long frame) {
    return sinf((float)frame * P5_OSC_SCALE);
}

static std::wstring P5UserProfile() {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(L"%USERPROFILE%", buf, MAX_PATH);
    return buf;
}

static void P5Teleport(HWND hwnd) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int maxX = sw - w;
    int maxY = sh - h;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    int x = maxX ? rand() % (maxX + 1) : 0;
    int y = maxY ? rand() % (maxY + 1) : 0;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void runPayload5() {
    bool anchored = false;
    DWORD base = SongSyncBase(112887, &anchored);
    SongSyncWait(base);

    srand((unsigned)GetTickCount() ^ 0x5EED);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    static int pathCount = (int)(sizeof(P5_NAV_PATHS) / sizeof(P5_NAV_PATHS[0]));
    std::wstring userProfile = P5UserProfile();
    const wchar_t* dirs[P5_NUM_NAV];
    for (int i = 0; i < P5_NUM_NAV; i++) dirs[i] = P5_NAV_PATHS[i % pathCount];
    for (int i = 0; i < P5_NUM_NAV; i += 2) dirs[i] = userProfile.c_str();

    HWND hwnd = CreateFakeExplorer(L"C:\\", 120, 80, 0, 0);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDC = GetDC(NULL);

    const bool fx = !NoGdiFlag();

    HDC baseDC = NULL, mainDC = NULL, ghostDC = NULL, cyanDC = NULL, redDC = NULL;
    HGDIOBJ baseOld = NULL, mainOld = NULL, ghostOld = NULL, cyanOld = NULL, redOld = NULL;
    HBITMAP baseBmp = NULL, mainBmp = NULL, ghostBmp = NULL, cyanBmp = NULL, redBmp = NULL;

    if (fx) {
        baseDC = CreateCompatibleDC(screenDC);
        baseBmp = CreateCompatibleBitmap(screenDC, vw, vh);
        baseOld = SelectObject(baseDC, baseBmp);

        mainDC = CreateCompatibleDC(screenDC);
        mainBmp = CreateCompatibleBitmap(screenDC, vw, vh);
        mainOld = SelectObject(mainDC, mainBmp);

        ghostDC = CreateCompatibleDC(screenDC);
        ghostBmp = CreateCompatibleBitmap(screenDC, vw, vh);
        ghostOld = SelectObject(ghostDC, ghostBmp);

        cyanDC = CreateCompatibleDC(screenDC);
        cyanBmp = CreateCompatibleBitmap(screenDC, vw, vh);
        cyanOld = SelectObject(cyanDC, cyanBmp);

        redDC = CreateCompatibleDC(screenDC);
        redBmp = CreateCompatibleBitmap(screenDC, vw, vh);
        redOld = SelectObject(redDC, redBmp);

        RECT fill = { 0, 0, vw, vh };

        HBRUSH cyanBrush = CreateSolidBrush(RGB(0, 255, 255));
        FillRect(cyanDC, &fill, cyanBrush);
        DeleteObject(cyanBrush);

        HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(redDC, &fill, redBrush);
        DeleteObject(redBrush);
    }

    DWORD start = base ? base : GetTickCount();

    unsigned long frame = 0;

    DWORD navAcc = 0;
    int navIdx = 0;

    for (;;) {
        DWORD now = GetTickCount();
        if (now - start >= (DWORD)P5_DURATION_MS) break;

        while (navIdx < P5_NUM_NAV && hwnd && IsWindow(hwnd) &&
               (LONG)((now - start) - navAcc) >= 0) {
            NavigateFakeExplorer(hwnd, dirs[navIdx]);
            P5Teleport(hwnd);
            PumpMessagesFor(30);
            navAcc += P5_NAV_DELAYS[navIdx];
            navIdx++;
            now = GetTickCount();
        }

        HDC scr = GetDC(NULL);

        if (fx && frame % P5_REFRESH_EVERY == 0) {
            BitBlt(baseDC, 0, 0, vw, vh, scr, vx, vy, SRCCOPY);

            BitBlt(mainDC, 0, 0, vw, vh, baseDC, 0, 0, SRCCOPY);
            BitBlt(mainDC, 0, 0, vw, vh, cyanDC, 0, 0, SRCAND);

            BitBlt(ghostDC, 0, 0, vw, vh, baseDC, 0, 0, SRCCOPY);
            BitBlt(ghostDC, 0, 0, vw, vh, redDC, 0, 0, SRCAND);
        }

        if (fx) {
            float osc = P5Osc(frame);
            int x = 0xE - (int)(osc * P5_WOBBLE_AMPL);
            if (x < 1) x = 1;

            BitBlt(scr, vx, vy, vw, vh, mainDC, 0, 0, SRCCOPY);
            BitBlt(scr, vx, vy, vw - x, vh, ghostDC, x, 0, SRCPAINT);
        }

        ReleaseDC(NULL, scr);
        Sleep(P5_SLEEP_MS);
        frame++;
    }

    if (hwnd) {
        DestroyFakeExplorer(hwnd);
        hwnd = NULL;
    }

    if (fx) {
        HDC scr = GetDC(NULL);
        BitBlt(scr, vx, vy, vw, vh, baseDC, 0, 0, SRCCOPY);
        ReleaseDC(NULL, scr);

        SelectObject(redDC, redOld);
        DeleteObject(redBmp);
        DeleteDC(redDC);
        SelectObject(cyanDC, cyanOld);
        DeleteObject(cyanBmp);
        DeleteDC(cyanDC);
        SelectObject(ghostDC, ghostOld);
        DeleteObject(ghostBmp);
        DeleteDC(ghostDC);
        SelectObject(mainDC, mainOld);
        DeleteObject(mainBmp);
        DeleteDC(mainDC);
        SelectObject(baseDC, baseOld);
        DeleteObject(baseBmp);
        DeleteDC(baseDC);
    }
    ReleaseDC(NULL, screenDC);

    CoUninitialize();

    TerminateProcess(GetCurrentProcess(), 0);
}