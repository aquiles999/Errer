#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../SongSync.h"
#include "../CrashLog.h"
#include "../Flags.h"
#include "../WallpaperAnim.h"

#define SE_DEBUG_PRIVILEGE 20

// Payload 8 slot on the song timeline, as tapped with tap_beats.py.
#define P8_SONG_MS 210315

// Wallpaper frames are embedded in the exe as RCDATA, one per frame, with
// consecutive IDs from P8_FRAME_BASE (index 0 == frame_6298.png).
#define P8_FRAME_BASE  1000
#define P8_FRAME_COUNT 272
#define P8_FPS 30

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pfnRtlAdjustPrivilege)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
typedef NTSTATUS(NTAPI* pfnRtlSetProcessIsCritical)(BOOLEAN, PBOOLEAN, BOOLEAN);

// Set once the wallpaper ending is finished and the original wallpaper has
// been put back. Payload 7 waits on this so the show does not end early.
#define P8_DONE_EVENT L"Local\\errer_p8_done"

void runPayload8() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) { TerminateProcess(GetCurrentProcess(), 1); return; }

    pfnRtlAdjustPrivilege RtlAdjustPrivilege =
        (pfnRtlAdjustPrivilege)GetProcAddress(ntdll, "RtlAdjustPrivilege");
    pfnRtlSetProcessIsCritical RtlSetProcessIsCritical =
        (pfnRtlSetProcessIsCritical)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
    if (!RtlAdjustPrivilege || !RtlSetProcessIsCritical) {
        MessageBoxW(NULL, L"ntdll exports missing", L"p8", MB_OK | MB_ICONERROR);
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }

    BOOLEAN old = FALSE;
    RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE, TRUE, FALSE, &old);

    // The ending wallpaper segment. The user's original wallpaper was already
    // copied to the temp backup by the mid-show segment in payload 3, so this
    // segment only has to put it back - and it does that before the crash.
    if (ChangeWallpaperFlag()) {
        WallpaperAnimSpec spec = {};
        spec.resBase      = P8_FRAME_BASE;
        spec.frameCount   = P8_FRAME_COUNT;
        spec.fps          = P8_FPS;
        spec.songMs       = P8_SONG_MS;
        spec.tag          = WALLPAPER_TAG_END;
        spec.restoreAtEnd = true;
        // Second segment: reuse the backup the middle segment saved, but save one
        // if this payload runs on its own.
        spec.forceBackup  = false;

        WallpaperAnimResult result = WallpaperAnimRun(spec);
        CrashLogf("wallpaper ending: shown=%d skipped=%d failed=%d backedUp=%d restored=%d",
                  result.framesShown, result.framesSkipped, result.framesFailed,
                  result.backedUp ? 1 : 0, result.restored ? 1 : 0);
    }

    // Remove every temp file this show created (also runs when wallpaper
    // change is skipped, to clean up any leftovers from an aborted run).
    WallpaperAnimCleanup();

    // Plain wall-clock delay, NOT SongSyncWaitBeat: the frame sequence is
    // complete, so waiting on a final song position could block forever if a
    // driver reports the end of the audio as a frozen position.
    Sleep(400);

    // The ending is done: the frames have played and the original wallpaper is
    // back. Tell payload 7 and the parent so the show can end. The handle is
    // deliberately left open so the event object survives until this process
    // exits.
    {
        HANDLE done = CreateEventW(NULL, TRUE, FALSE, P8_DONE_EVENT);
        if (done) {
            SetEvent(done);
            CrashLog("wallpaper ending complete; signalled end of show");
        }
    }

    if (NoBsodFlag()) {
        CrashLog("--nobsod: wallpaper ending complete; skipping critical process");
        TerminateProcess(GetCurrentProcess(), 0);
        return;
    }

    NTSTATUS st = RtlSetProcessIsCritical(TRUE, NULL, FALSE);
    if (st != 0) {
        wchar_t buf[64];
        wsprintfW(buf, L"RtlSetProcessIsCritical returned 0x%08lX", (ULONG)st);
        MessageBoxW(NULL, buf, L"p8", MB_OK | MB_ICONERROR);
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }

    TerminateProcess(GetCurrentProcess(), (UINT)STATUS_INTEGER_DIVIDE_BY_ZERO);
}
