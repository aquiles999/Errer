#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>

extern void PlayBadApple();
extern void StopBadApple();
extern void RestoreTitles();
extern DWORD g_audioStartTick;

#define KILL_ABORT_EXIT 0xA11D
#define MAX_CHILDREN 7

int g_screenW = 0;
int g_screenH = 0;

struct ScreenSizeInit {
    ScreenSizeInit() {
        g_screenW = GetSystemMetrics(SM_CXSCREEN);
        g_screenH = GetSystemMetrics(SM_CYSCREEN);
    }
};
static ScreenSizeInit g_screenSizeInit;

static volatile LONG g_childMode = 0;
static HANDLE g_children[MAX_CHILDREN] = {};
static int g_childCount = 0;

void SetChildMode() {
    timeBeginPeriod(1);
    InterlockedExchange((volatile LONG*)&g_childMode, 1);
}

static void KillChildren() {
    for (int i = 0; i < g_childCount; i++) {
        if (g_children[i]) {
            TerminateProcess(g_children[i], KILL_ABORT_EXIT);
            CloseHandle(g_children[i]);
            g_children[i] = NULL;
        }
    }
    g_childCount = 0;
}

static DWORD WINAPI KillSwitchThread(LPVOID) {
    int held = 0;
    for (;;) {
        bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ralt  = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (ctrl && shift && ralt) {
            if (++held >= 3) {
                RestoreTitles();
                StopBadApple();
                if (g_childMode) {
                    TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT);
                } else {
                    KillChildren();
                    TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT);
                }
            }
        } else {
            held = 0;
        }
        Sleep(10);
    }
    return 0;
}

static void InstallKillSwitch() {
    CreateThread(NULL, 0, KillSwitchThread, NULL, 0, NULL);
}

struct KillSwitchInstaller {
    KillSwitchInstaller() { InstallKillSwitch(); }
};
static KillSwitchInstaller g_killSwitchInstaller;

static DWORD SpawnPayload(const wchar_t* arg) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return 0;

    wchar_t cmdLine[560];
    wsprintfW(cmdLine, L"\"%s\" %s", exePath, arg);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;

    CloseHandle(pi.hThread);
    if (g_childCount < MAX_CHILDREN)
        g_children[g_childCount++] = pi.hProcess;

    DWORD code = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);

    for (int i = 0; i < g_childCount; i++) {
        if (g_children[i] == pi.hProcess) {
            g_children[i] = NULL;
            g_childCount--;
            for (int j = i; j < g_childCount; j++) g_children[j] = g_children[j + 1];
            break;
        }
    }
    CloseHandle(pi.hProcess);
    return code;
}

static DWORD MciPositionMs() {
    wchar_t pos[32];
    if (mciSendStringW(L"status errer_mp3 position", pos, 32, NULL) == 0) return (DWORD)_wtol(pos);
    return 0;
}

// MCI's decode clock drifts vs wall clock, so re-align the song anchor to
// MCI's live position before spawning the next payload. Later payloads then
// ride the audio timeline instead of accumulating drift.
static DWORD CorrectSongAnchor() {
    DWORD pos = MciPositionMs();
    if (pos == 0) return g_audioStartTick;
    DWORD expected = GetTickCount() - g_audioStartTick;
    if (expected == 0) return g_audioStartTick;
    LONG drift = (LONG)((LONGLONG)pos - expected);
    if (drift < -1500 || drift > 1500) return g_audioStartTick;
    DWORD corrected = g_audioStartTick + (DWORD)drift;
    wchar_t buf[32];
    wsprintfW(buf, L"%lu", corrected);
    SetEnvironmentVariableW(L"ERRER_SONG_START", buf);
    return corrected;
}

static void WaitSongPoint(DWORD corrected, DWORD songMs, DWORD leadMs) {
    LONG remaining = (LONG)(corrected + songMs - leadMs - GetTickCount());
    while (remaining > 0) {
        Sleep(5);
        remaining = (LONG)(corrected + songMs - leadMs - GetTickCount());
    }
}

// A payload must be spawned *before* its song offset so its own SongSyncWait
// can align to the exact ms we assigned. Spawn ~250ms early; the child then
// clamps to the precise beat.
static DWORD SpawnPayloadAt(DWORD songMs, const wchar_t* arg) {
    DWORD corrected = CorrectSongAnchor();
    WaitSongPoint(corrected, songMs, 250);
    return SpawnPayload(arg);
}

void runAll() {
    timeBeginPeriod(1);
    FreeConsole();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    PlayBadApple();

    wchar_t songStartBuf[32];
    wsprintfW(songStartBuf, L"%lu", g_audioStartTick);
    SetEnvironmentVariableW(L"ERRER_SONG_START", songStartBuf);

    DWORD c1 = SpawnPayloadAt(0, L"--payload1");
    if (c1 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c2 = SpawnPayloadAt(15572, L"--payload2");
    if (c2 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c3 = SpawnPayloadAt(29421, L"--payload3");
    if (c3 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c4 = SpawnPayloadAt(85059, L"--payload4");
    if (c4 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c5 = SpawnPayloadAt(112887, L"--payload5");
    if (c5 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c6 = SpawnPayloadAt(126834, L"--payload6");
    if (c6 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    DWORD c7 = SpawnPayloadAt(182850, L"--payload7");
    if (c7 == KILL_ABORT_EXIT) { StopBadApple(); KillChildren(); TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT); }

    StopBadApple();
    timeEndPeriod(1);
    CoUninitialize();
}