#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wchar.h>
#include <commctrl.h>
#include "../SongSync.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern void SetErrorIcon(const wchar_t* type);
extern void PositionErrorTo(const wchar_t* pos);
extern void createError(const wchar_t* title, const wchar_t* text);
extern void RestoreTitles();

#define BEAT_COUNT 109

static const DWORD BEAT_DELAYS[BEAT_COUNT] = {
    208, 209, 231, 232, 456,
    200, 217, 208, 232, 192,
    231, 464, 278, 180, 174,
    225, 191, 224, 473, 200,
    208, 232, 208, 208, 232,
    432, 216, 200, 225, 209,
    207, 224, 440, 209, 215,
    209, 232, 208, 225, 481,
    175, 223, 200, 224, 208,
    255, 449, 191, 216, 239,
    200, 240, 217, 407, 208,
    232, 216, 224, 209, 239,
    440, 192, 215, 208, 225,
    207, 224, 432, 281, 136,
    232, 207, 224, 240, 400,
    225, 215, 240, 208, 232,
    217, 401, 223, 299, 150,
    216, 216, 208, 472, 232,
    192, 208, 216, 200, 233,
    431, 216, 232, 201, 231,
    208, 240, 408, 231, 273,
    175, 209, 215, 224
};

static const wchar_t* P4_TITLES[16] = {
    L"Windows", L"System Error", L"Application Error", L"errer.exe - Application Error",
    L"Setup", L"Runtime Error!", L"Windows Explorer", L"System",
    L"Windows Security", L"DLL Host", L"svchost.exe - Application Error", L"Registry Editor",
    L"Task Scheduler", L"Error", L"Microsoft Windows", L"File Explorer"
};

static const wchar_t* P4_TEXTS[16] = {
    L"The instruction at 0x0000000000000000 referenced memory at 0x0000000000000000. The memory could not be read.",
    L"The exception unknown software exception (0xc0000005) occurred in the application at location 0x0000000140001A25.",
    L"The application failed to initialize properly (0xc0000005). Click on OK to terminate the application.",
    L"The system cannot find the file specified.",
    L"A required DLL file could not be found.",
    L"Not enough storage is available to process this command.",
    L"The process cannot access the file because it is being used by another process.",
    L"The instruction at 0x00007FFB4A23A7B0 referenced memory at 0x0000000000000000. The memory could not be written.",
    L"Windows cannot verify the identity of this file. Do you want to run this software anyway?",
    L"An application error has occurred. The exception unknown software exception (0xc0000409) was thrown.",
    L"The instruction at 0x00000000FFC40123 referenced memory at 0x0000000000000000. The memory could not be read.",
    L"Cannot import file: The specified file is not a registry script. You can only import registry files.",
    L"Task Scheduler has encountered an error. The task was not run.",
    L"Access to the specified device, path, or file is denied.",
    L"Windows could not start because the following file is missing or corrupt: \\WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM.",
    L"The item cannot be deleted because it is in use."
};

void runPayload4() {
    bool anchored = false;
    DWORD base = SongSyncBase(85059, &anchored);
    SongSyncWait(base);

    srand((unsigned)GetTickCount() ^ 0x1904);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    DWORD spawnAt[BEAT_COUNT];
    DWORD acc = 0;
    for (int i = 0; i < BEAT_COUNT; i++) { spawnAt[i] = acc; acc += BEAT_DELAYS[i]; }

    double lead = 90.0;
    int spawned = 0;
    DWORD start = base ? base : GetTickCount();

    static const wchar_t* types[3] = { L"error", L"warning", L"information" };

    while (spawned < BEAT_COUNT) {
        DWORD now = GetTickCount();
        DWORD elapsed = now - start;

        while (spawned < BEAT_COUNT && (double)elapsed + lead >= (double)spawnAt[spawned]) {
            SetErrorIcon(types[rand() % 3]);
            PositionErrorTo(L"random");
            int ti = rand() % 16;
            createError(P4_TITLES[ti], P4_TEXTS[ti]);
            spawned++;
        }

        Sleep(5);
    }

    Sleep(500);
    RestoreTitles();
    TerminateProcess(GetCurrentProcess(), 0);
}