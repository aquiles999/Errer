#pragma once
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>
#include "SongClock.h"
#include "Flags.h"
#include "CrashLog.h"
#include <stdio.h>

static DWORD SongSyncAnchor() {
    wchar_t buf[64];
    DWORD n = GetEnvironmentVariableW(L"ERRER_SONG_START", buf, 64);
    if (n > 0 && n < 64) return (DWORD)_wtoi(buf);
    return 0;
}

// Absolute tick a payload's effect must start at, or 0 when no clock or song
// anchor is set. Pinned to the live clock first (real MCI position), so the
// slot lands on the exact song ms even after stalls; the env anchor is only a
// bootstrap fallback for standalone/debug runs.
static DWORD SongSyncBase(DWORD offsetMs, bool* anchored) {
    DWORD t = 0, p = 0;
    if (SongClockRead(&t, &p)) {
        if (anchored) *anchored = true;
        long long target = (long long)t + (long long)offsetMs - (long long)p;
        return target < 0 ? 0 : (DWORD)target;
    }
    DWORD anchor = SongSyncAnchor();
    if (anchor != 0) {
        if (anchored) *anchored = true;
        return anchor + offsetMs;
    }
    if (anchored) *anchored = false;
    return 0;
}

static void SongSyncWait(DWORD base) {
    if (base == 0) return;
    while ((LONG)(GetTickCount() - base) < 0) Sleep(8);
}

static bool SongSyncAvailable() {
    DWORD t = 0, p = 0;
    return SongClockRead(&t, &p);
}

// Current position on the song timeline, in ms, or -1 when the parent's audio
// clock isn't running. This reads the SHARED clock, so it is valid in child
// payload processes - unlike SongAudioPosMs(), which reads the local waveOut
// handle and only means anything in the process actually playing the audio.
static long long SongNowMs(void) {
    DWORD t = 0, p = 0;
    if (!SongClockRead(&t, &p)) return -1;
    return (long long)p;
}

// Absolute GetTickCount value at which the track reaches songMs, pinned to
// the live MCI timeline (re-sampled each call, so MCI/clock drift and slow
// spawns self-correct). 0 when the parent's audio clock is unavailable.
static DWORD SongSyncTarget(DWORD songMs) {
    DWORD tick = 0, pos = 0;
    if (!SongClockRead(&tick, &pos)) return 0;
    if (pos >= songMs) return tick;
    return tick + (songMs - pos);
}

// How far past `songMs` the audio has already run - 0 when on time or early.
// This is the actual drift a payload is carrying, measured against the shared
// clock rather than its own wall time.
static DWORD SongSyncLagMs(DWORD songMs) {
    DWORD t = 0, pos = 0;
    if (!SongClockRead(&t, &pos)) return 0;
    return (pos > songMs) ? (pos - songMs) : 0;
}

// A payload that lands more than this many ms past its beat is considered
// drifted and gets reported instead of silently falling further behind.
#define SONG_SYNC_TOLERANCE_MS 80

// Wait against the live audio clock, then render.
//
// Returns how late the wait landed, in ms, so callers can decide what to do
// about drift. Because every wait targets an absolute song position rather
// than a running local accumulator, the timeline re-converges on its own once
// the payload is given a moment - this just makes the overshoot visible.
static DWORD SongSyncWaitBeat(DWORD songMs) {
    DWORD target = SongSyncTarget(songMs);
    if (target == 0) return 0;
    while ((LONG)(GetTickCount() - target) < 0) Sleep(4);

    DWORD lag = SongSyncLagMs(songMs);
    if (lag > SONG_SYNC_TOLERANCE_MS) {
        static volatile LONG reported = 0;
        if (InterlockedExchange(&reported, 1) == 0) {
            char b[96];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "sync drift %lu ms at song %lu (tolerance %d) - resyncing",
                        lag, songMs, SONG_SYNC_TOLERANCE_MS);
            CrashLog(b);
        }
    }
    return lag;
}

// Anchor a locally-paced sequence right at the audio offset songMs.
// Returns an absolute GetTickCount() "now" captured at that instant, or
// the fallback when no live clock is available.
static DWORD SongSyncStart(DWORD songMs, DWORD fallback) {
    DWORD target = SongSyncTarget(songMs);
    if (target == 0) return fallback;
    while ((LONG)(GetTickCount() - target) < 0) Sleep(8);
    return GetTickCount();
}

#define SONG_SYNC_HOST_WAIT_MS 25000

// Children launched by the parent get "--wait": they block here until the
// parent's live audio clock exists, so pre-warmed payloads never start before
// the song does. Standalone/debug runs (no flag) skip straight to local time.
static bool SongSyncWaitHostFlag() {
    wchar_t b[8] = {};
    return GetEnvironmentVariableW(L"ERRER_WAITCLOCK", b, 8) > 0 && b[0] == L'1';
}

static void SongSyncWaitHost(DWORD timeoutMs = SONG_SYNC_HOST_WAIT_MS) {
    // --nosong means no clock will ever exist, so waiting for one would just
    // stall every payload for the full timeout. The payloads fall back to the
    // ERRER_SONG_START wall-clock anchor instead.
    if (NoSongFlag()) return;

    DWORD start = GetTickCount();
    while (!SongSyncAvailable()) {
        if ((DWORD)(GetTickCount() - start) >= timeoutMs) return;
        Sleep(150);
    }
}