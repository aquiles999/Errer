#pragma once
#include <windows.h>
#include <math.h>

#define WOBBLE_REFRESH_EVERY 4
#define WOBBLE_SLEEP_MS      12
#define WOBBLE_AMPL          -6.0f
#define WOBBLE_OSC_SCALE     0.08f

static float WobbleOsc(unsigned long frame) {
    return sinf((float)frame * WOBBLE_OSC_SCALE);
}

static void RunWobbleAmbient(DWORD durationMs) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDC = GetDC(NULL);

    HDC baseDC = CreateCompatibleDC(screenDC);
    HBITMAP baseBmp = CreateCompatibleBitmap(screenDC, vw, vh);
    HGDIOBJ baseOld = SelectObject(baseDC, baseBmp);

    HDC mainDC = CreateCompatibleDC(screenDC);
    HBITMAP mainBmp = CreateCompatibleBitmap(screenDC, vw, vh);
    HGDIOBJ mainOld = SelectObject(mainDC, mainBmp);

    HDC ghostDC = CreateCompatibleDC(screenDC);
    HBITMAP ghostBmp = CreateCompatibleBitmap(screenDC, vw, vh);
    HGDIOBJ ghostOld = SelectObject(ghostDC, ghostBmp);

    HDC cyanDC = CreateCompatibleDC(screenDC);
    HBITMAP cyanBmp = CreateCompatibleBitmap(screenDC, vw, vh);
    HGDIOBJ cyanOld = SelectObject(cyanDC, cyanBmp);

    HDC redDC = CreateCompatibleDC(screenDC);
    HBITMAP redBmp = CreateCompatibleBitmap(screenDC, vw, vh);
    HGDIOBJ redOld = SelectObject(redDC, redBmp);

    RECT fill = { 0, 0, vw, vh };

    HBRUSH cyanBrush = CreateSolidBrush(RGB(0, 255, 255));
    FillRect(cyanDC, &fill, cyanBrush);
    DeleteObject(cyanBrush);

    HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(redDC, &fill, redBrush);
    DeleteObject(redBrush);

    DWORD start = GetTickCount();
    unsigned long frame = 0;

    for (;;) {
        if (GetTickCount() - start >= durationMs) break;

        HDC scr = GetDC(NULL);

        if (frame % WOBBLE_REFRESH_EVERY == 0) {
            BitBlt(baseDC, 0, 0, vw, vh, scr, vx, vy, SRCCOPY);

            BitBlt(mainDC, 0, 0, vw, vh, baseDC, 0, 0, SRCCOPY);
            BitBlt(mainDC, 0, 0, vw, vh, cyanDC, 0, 0, SRCAND);

            BitBlt(ghostDC, 0, 0, vw, vh, baseDC, 0, 0, SRCCOPY);
            BitBlt(ghostDC, 0, 0, vw, vh, redDC, 0, 0, SRCAND);
        }

        float osc = WobbleOsc(frame);
        int x = 0x0E - (int)(osc * WOBBLE_AMPL);
        if (x < 1) x = 1;

        BitBlt(scr, vx, vy, vw, vh, mainDC, 0, 0, SRCCOPY);
        BitBlt(scr, vx, vy, vw - x, vh, ghostDC, x, 0, SRCPAINT);

        ReleaseDC(NULL, scr);
        Sleep(WOBBLE_SLEEP_MS);
        frame++;
    }

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
    ReleaseDC(NULL, screenDC);
}

static DWORD WINAPI WobbleAmbientProc(LPVOID p) {
    DWORD ms = *(DWORD*)p;
    delete (DWORD*)p;
    RunWobbleAmbient(ms);
    return 0;
}