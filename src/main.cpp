#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include "SongClock.h"
#include "Flags.h"
#include "CrashLog.h"
#include "Wobble.h"

extern void PlayBadApple();
extern void StopBadApple();
extern void RestoreTitles();
extern long long SongAudioPosMs();
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

// Handle for the wallpaper-ending child (payload 8). It is not part of
// g_children on purpose, but the parent still needs it to wait for the
// ending and to take it down on the kill switch.
static HANDLE g_payload8Proc = NULL;

bool IsChildMode() {
    return InterlockedCompareExchange((volatile LONG*)&g_childMode, 0, 0) != 0;
}

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

    // The wallpaper ending is not in g_children, but the kill switch still
    // has to take it down. By the time the normal teardown reaches this, the
    // process has already exited on its own, so this is a no-op there.
    if (g_payload8Proc) {
        TerminateProcess(g_payload8Proc, KILL_ABORT_EXIT);
        CloseHandle(g_payload8Proc);
        g_payload8Proc = NULL;
    }
}

static DWORD WINAPI KillSwitchThread(LPVOID) {
    int held = 0;
    for (;;) {
        bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ralt  = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (ctrl && shift && ralt) {
            // ~500ms deliberate hold, so a stray modifier transients
            // (RDP/VirtualBox rethink, AltGr digraphs, host-key combos)
            // can never trip it.
            if (++held >= 50) {
                CrashLog("KILLSWITCH abort");
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

static void SpawnPayload(const wchar_t* arg) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return;

    wchar_t cmdLine[560];
    wsprintfW(cmdLine, L"\"%s\" %s", exePath, arg);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return;

    CloseHandle(pi.hThread);
    if (g_childCount < MAX_CHILDREN)
        g_children[g_childCount++] = pi.hProcess;
    else
        CloseHandle(pi.hProcess);
}

// Payload 8 is deliberately NOT tracked in g_children. Once payload 7 is done
// the parent tears down the audio, the song clock, and every tracked child;
// payload 8 must outlive that so the wallpaper ending can still start on its
// song slot and play to the finish.
//
// Its handle IS kept, though, so the parent can wait for it before exiting:
// the show must not end until the wallpaper animation has finished AND the
// original wallpaper has been restored. The parent waits for it explicitly
// instead of adding it to g_children, because g_children gets killed.
void LaunchPayload8() {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return;

    wchar_t cmdLine[560];
    wsprintfW(cmdLine, L"\"%s\" --payload8", exePath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return;
    CloseHandle(pi.hThread);
    g_payload8Proc = pi.hProcess;
}

static volatile LONG g_songClockStop = 0;

static void SongClockWrite(SongClockData* d, LONG posMs, LONG tick) {
    InterlockedIncrement(&d->seq);
    InterlockedExchange(&d->tick, tick);
    InterlockedExchange(&d->posMs, posMs);
}

// Broadcasts the song position in plain wall-clock ms (GetTickCount since
// audio start). MCI's "position" unit is unreliable across drivers (can come
// back in samples/frames), so we never feed raw MCI values to the children.
// Children re-sample this per beat: spawn latency, VM clock skew and stalls
// all self-correct, and the parent keeps the whole timeline authoritative.
static DWORD WINAPI SongClockWriterThread(LPVOID) {
    HANDLE map = SongClockOpen();
    if (!map) return 0;
    SongClockData* d = (SongClockData*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(SongClockData));
    if (d) {
        // --endermanch plays a different song, so every sync fix below is
        // tuned to Bad Apple (its exact slot ms, its MCI stepping) and can
        // only fight the other track. Broadcast plain wall-clock time instead.
        bool endermanch = EndermanchFlag();

        // waveOut reports sample-accurate position with no MCI, so no slot
        // snaps are needed -- the clock just tracks the device. We extrapolate
        // smoothly from the last fresh sample so the clock never freezes
        // between the 40ms polls, while re-anchoring to the device every cycle.
        LONG lastRealPos = -1;
        DWORD lastRealWall = 0;

        while (!InterlockedCompareExchange(&g_songClockStop, 0, 0)) {
            DWORD wallNow = GetTickCount();

            LONG posMs;
            if (endermanch) {
                posMs = (LONG)(wallNow - g_audioStartTick);
                if (posMs < 0) posMs = 0;
            } else {
                LONG realPos = (LONG)SongAudioPosMs();
                if (realPos >= 0) {
                    lastRealPos = realPos;
                    lastRealWall = wallNow;
                }

                if (lastRealPos >= 0)
                    posMs = lastRealPos + (LONG)(wallNow - lastRealWall);
                else
                    posMs = (LONG)(wallNow - g_audioStartTick);
                if (posMs < 0) posMs = 0;
            }

            SongClockWrite(d, posMs, (LONG)wallNow);
            Sleep(40);
        }
        UnmapViewOfFile(d);
    }
    CloseHandle(map);
    return 0;
}

static void StartSongClock() {
    if (g_audioStartTick == 0) return;
    InterlockedExchange(&g_songClockStop, 0);
    CreateThread(NULL, 0, SongClockWriterThread, NULL, 0, NULL);
}

// The chroma wobble runs once here, from payload 6's song slot until the
// show ends, instead of every child process starting and tearing down its
// own effect.
#define WOBBLE_SLOT_MS 126834  // payload 6's beat

struct WobbleWatcher {
    DWORD startMs;
    HANDLE stopEv;
};

static void StartWobbleThread(HANDLE stopEv) {
    WobbleRun* wob = new WobbleRun{ 0, stopEv };
    HANDLE hWob = CreateThread(NULL, 0, WobbleAmbientProcEx, wob, 0, NULL);
    if (hWob) CloseHandle(hWob);
}

static DWORD WINAPI WobbleWatcherThread(LPVOID p) {
    WobbleWatcher* w = (WobbleWatcher*)p;
    for (;;) {
        LONG pos = SongAudioPosMs();
        DWORD songMs = pos >= 0 ? (DWORD)pos : (GetTickCount() - g_audioStartTick);
        if (songMs >= w->startMs) break;
        Sleep(25);
    }
    StartWobbleThread(w->stopEv);
    delete w;
    return 0;
}

void runAll() {
    timeBeginPeriod(1);
    FreeConsole();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Pre-warm every payload so boot latency happens BEFORE the music. Each
    // child blocks on the live clock and only runs its payload when the song
    // reaches its slot; audio starts last, so every segment lands on-sync even
    // on slow VMs.
    SpawnPayload(L"--payload1 --wait");
    SpawnPayload(L"--payload2 --wait");
    SpawnPayload(L"--payload3 --wait");
    SpawnPayload(L"--payload4 --wait");
    SpawnPayload(L"--payload5 --wait");
    SpawnPayload(L"--payload6 --wait");
    SpawnPayload(L"--payload7 --wait");

    if (NoSongFlag()) {
        // No audio, but the show still needs a time zero so the payloads keep
        // their song-relative offsets instead of all starting at once. Anchor
        // to the wall clock at the point the music would have begun; the
        // children fall back to ERRER_SONG_START when there is no live clock.
        g_audioStartTick = GetTickCount();
    } else {
        PlayBadApple();
        StartSongClock();
    }

    // Chroma wobble from payload 6 onwards: the watcher below starts it once
    // the song reaches payload 6's slot.
    HANDLE wobStop = NULL;
    if (!NoGdiFlag()) {
        wobStop = CreateEventW(NULL, TRUE, FALSE, NULL);
        WobbleWatcher* w = new WobbleWatcher{ WOBBLE_SLOT_MS, wobStop };
        HANDLE hWobWatch = CreateThread(NULL, 0, WobbleWatcherThread, w, 0, NULL);
        if (hWobWatch) CloseHandle(hWobWatch);
    }

    wchar_t songStartBuf[32];
    wsprintfW(songStartBuf, L"%lu", g_audioStartTick);
    SetEnvironmentVariableW(L"ERRER_SONG_START", songStartBuf);

    if (g_childCount > 0)
        WaitForMultipleObjects(g_childCount, g_children, TRUE, INFINITE);

    // The wallpaper ending owns the last part of the show. Wait for it BEFORE
    // tearing anything down:
    //   - it needs the song clock to still be running when it starts on its
    //     slot, and the clock extrapolates past the end of the track,
    //   - the show must not end until the animation has finished AND the
    //     original wallpaper has been restored,
    //   - the chroma wobble has to stay alive until the ending is over.
    //
    // With --nobsod, payload 8 restores the wallpaper and exits cleanly and
    // the show ends here. Without it, payload 8 brings the box down with a
    // BSOD and this wait never returns.
    if (g_payload8Proc) {
        CrashLog("waiting for wallpaper ending to finish");
        WaitForSingleObject(g_payload8Proc, 60000);
    }

    bool abort = false;
    for (int i = 0; i < g_childCount; i++) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_children[i], &code) && code == KILL_ABORT_EXIT) abort = true;
    }

    StopBadApple();
    InterlockedExchange(&g_songClockStop, 1);
    KillChildren();

    if (wobStop) {
        SetEvent(wobStop);
        CloseHandle(wobStop);
    }

    if (abort) TerminateProcess(GetCurrentProcess(), KILL_ABORT_EXIT);

    CrashLog("end");
    Sleep(60);
    timeEndPeriod(1);
    CoUninitialize();
}