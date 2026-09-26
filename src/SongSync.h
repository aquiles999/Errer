#pragma once
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

static DWORD SongSyncAnchor() {
    wchar_t buf[64];
    DWORD n = GetEnvironmentVariableW(L"ERRER_SONG_START", buf, 64);
    if (n > 0 && n < 64) return (DWORD)_wtoi(buf);
    return 0;
}

// Absolute tick a payload's effect must start at, or 0 when no
// song anchor is set (standalone/debug runs fall back to local clock).
static DWORD SongSyncBase(DWORD offsetMs, bool* anchored) {
    DWORD anchor = SongSyncAnchor();
    if (anchored) *anchored = (anchor != 0);
    if (anchor == 0) return 0;
    return anchor + offsetMs;
}

static void SongSyncWait(DWORD base) {
    if (base == 0) return;
    while ((LONG)(GetTickCount() - base) < 0) Sleep(8);
}