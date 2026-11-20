#pragma once
#include <windows.h>
#include <math.h>

#define WOBBLE_REFRESH_EVERY 4
#define WOBBLE_SLEEP_MS      12
#define WOBBLE_AMPL          -6.0f
#define WOBBLE_OSC_SCALE     0.08f

static HANDLE       g_wobMut      = NULL;
static volatile LONG g_wobActive   = 0;
static HDC          g_wobMainDC    = NULL;
static HDC          g_wobGhostDC   = NULL;
static int          g_wobVx = 0, g_wobVy = 0, g_wobVw = 0, g_wobVh = 0;
static int          g_wobX  = 0;

static float WobbleOsc(unsigned long frame) {
    return sinf((float)frame * WOBBLE_OSC_SCALE);
}

// Re-apply the chroma layers over whatever the payload drew since the
// ambient loop's last pass, so the effect stays on top of the error spam.
static void WobbleRetintNow() {
    if (!g_wobActive || !g_wobMut || !g_wobMainDC || !g_wobGhostDC) return;
    if (WaitForSingleObject(g_wobMut, INFINITE) != WAIT_OBJECT_0) return;
    HDC scr = GetDC(NULL);
    if (scr) {
        BitBlt(scr, g_wobVx, g_wobVy, g_wobVw, g_wobVh, g_wobMainDC, 0, 0, SRCCOPY);
        int x = g_wobX;
        if (x < 1) x = 1;
        BitBlt(scr, g_wobVx, g_wobVy, g_wobVw - x, g_wobVh, g_wobGhostDC, x, 0, SRCPAINT);
        ReleaseDC(NULL, scr);
    }
    ReleaseMutex(g_wobMut);
}

struct WobbleRun {
    DWORD ms;        // 0 => run until the event is signaled
    HANDLE ev;       // stop event, may be NULL
};

// Same ambient effect as RunWobbleAmbient, but a stop event can cut it short
// (used by the main process to run one effect for the whole show).
static void RunWobbleAmbientEx(DWORD durationMs, HANDLE stopEvent) {
    g_wobVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_wobVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_wobVw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_wobVh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDC = GetDC(NULL);

    HDC baseDC = CreateCompatibleDC(screenDC);
    HBITMAP baseBmp = CreateCompatibleBitmap(screenDC, g_wobVw, g_wobVh);
    HGDIOBJ baseOld = SelectObject(baseDC, baseBmp);

    HDC mainDC = CreateCompatibleDC(screenDC);
    HBITMAP mainBmp = CreateCompatibleBitmap(screenDC, g_wobVw, g_wobVh);
    HGDIOBJ mainOld = SelectObject(mainDC, mainBmp);

    HDC ghostDC = CreateCompatibleDC(screenDC);
    HBITMAP ghostBmp = CreateCompatibleBitmap(screenDC, g_wobVw, g_wobVh);
    HGDIOBJ ghostOld = SelectObject(ghostDC, ghostBmp);

    HDC cyanDC = CreateCompatibleDC(screenDC);
    HBITMAP cyanBmp = CreateCompatibleBitmap(screenDC, g_wobVw, g_wobVh);
    HGDIOBJ cyanOld = SelectObject(cyanDC, cyanBmp);

    HDC redDC = CreateCompatibleDC(screenDC);
    HBITMAP redBmp = CreateCompatibleBitmap(screenDC, g_wobVw, g_wobVh);
    HGDIOBJ redOld = SelectObject(redDC, redBmp);

    RECT fill = { 0, 0, g_wobVw, g_wobVh };

    HBRUSH cyanBrush = CreateSolidBrush(RGB(0, 255, 255));
    FillRect(cyanDC, &fill, cyanBrush);
    DeleteObject(cyanBrush);

    HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(redDC, &fill, redBrush);
    DeleteObject(redBrush);

    g_wobMainDC = mainDC;
    g_wobGhostDC = ghostDC;
    g_wobMut = CreateMutexW(NULL, FALSE, NULL);
    g_wobActive = 1;

    DWORD start = GetTickCount();
    unsigned long frame = 0;

    for (;;) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        if (durationMs && GetTickCount() - start >= durationMs) break;

        HDC scr = GetDC(NULL);

        float osc = WobbleOsc(frame);
        int x = 0x0E - (int)(osc * WOBBLE_AMPL);
        if (x < 1) x = 1;

        if (frame % WOBBLE_REFRESH_EVERY == 0) {
            WaitForSingleObject(g_wobMut, INFINITE);
            g_wobX = x;
            BitBlt(baseDC, 0, 0, g_wobVw, g_wobVh, scr, g_wobVx, g_wobVy, SRCCOPY);

            BitBlt(mainDC, 0, 0, g_wobVw, g_wobVh, baseDC, 0, 0, SRCCOPY);
            BitBlt(mainDC, 0, 0, g_wobVw, g_wobVh, cyanDC, 0, 0, SRCAND);

            BitBlt(ghostDC, 0, 0, g_wobVw, g_wobVh, baseDC, 0, 0, SRCCOPY);
            BitBlt(ghostDC, 0, 0, g_wobVw, g_wobVh, redDC, 0, 0, SRCAND);
            ReleaseMutex(g_wobMut);
        } else {
            g_wobX = x;
        }

        BitBlt(scr, g_wobVx, g_wobVy, g_wobVw, g_wobVh, mainDC, 0, 0, SRCCOPY);
        BitBlt(scr, g_wobVx, g_wobVy, g_wobVw - x, g_wobVh, ghostDC, x, 0, SRCPAINT);

        ReleaseDC(NULL, scr);
        Sleep(WOBBLE_SLEEP_MS);
        frame++;
    }

    g_wobActive = 0;
    if (g_wobMut) {
        WaitForSingleObject(g_wobMut, INFINITE);
        ReleaseMutex(g_wobMut);
        CloseHandle(g_wobMut);
        g_wobMut = NULL;
    }

    HDC scr = GetDC(NULL);
    BitBlt(scr, g_wobVx, g_wobVy, g_wobVw, g_wobVh, baseDC, 0, 0, SRCCOPY);
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
    ReleaseDC(NULL, screenDC);

    g_wobMainDC = NULL;
    g_wobGhostDC = NULL;
}

static void RunWobbleAmbient(DWORD durationMs) {
    RunWobbleAmbientEx(durationMs, NULL);
}

static DWORD WINAPI WobbleAmbientProc(LPVOID p) {
    DWORD ms = *(DWORD*)p;
    delete (DWORD*)p;
    RunWobbleAmbient(ms);
    return 0;
}

static DWORD WINAPI WobbleAmbientProcEx(LPVOID p) {
    WobbleRun* r = (WobbleRun*)p;
    RunWobbleAmbientEx(r->ms, r->ev);
    delete r;
    return 0;
}