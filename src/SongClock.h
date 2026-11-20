#pragma once
#include <windows.h>

#define SONGCLOCK_NAME L"Local\\errer_song_clock_v3"

struct SongClockData {
    volatile LONG seq;
    volatile LONG tick;
    volatile LONG posMs;
};

static bool SongClockRead(DWORD* outTick, DWORD* outPosMs) {
    // InterlockedCompareExchange below performs a read-modify-write, which
    // faults on a read-only view. The section is created writable by the
    // parent, so open the view writable (the values only ever read here).
    HANDLE m = OpenFileMappingW(FILE_MAP_WRITE, FALSE, SONGCLOCK_NAME);
    if (!m) return false;
    SongClockData* d = (SongClockData*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(SongClockData));
    bool ok = false;
    if (d) {
        LONG s1 = d->seq;
        DWORD t = (DWORD)InterlockedCompareExchange(&d->tick, 0, 0);
        DWORD p = (DWORD)InterlockedCompareExchange(&d->posMs, 0, 0);
        LONG s2 = d->seq;
        if (s1 != 0 && s1 == s2) {
            *outTick = t;
            *outPosMs = p;
            ok = true;
        }
        UnmapViewOfFile(d);
    }
    CloseHandle(m);
    return ok;
}

static HANDLE SongClockOpen() {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SongClockData), SONGCLOCK_NAME);
}