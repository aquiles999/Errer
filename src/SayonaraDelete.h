#pragma once

HWND SayonaraSpawnDeleteBox(int lifetimeMs = 0);

// Summons a series of fake "Delete File" boxes. delays[0] is how long to wait
// before the first one; every later value is the gap until the next one. Each
// box stays up until closed, and picks a random System32 file / position.
void SayonaraDeleteSchedule(const DWORD* delays, int count);

// Same, but anchored to an absolute GetTickCount value (the payload's song
// beat) instead of "whenever this got called". Any setup work that runs
// between the beat and the schedule call would otherwise shift every box.
void SayonaraDeleteScheduleFrom(const DWORD* delays, int count, DWORD startTick);

void SayonaraDeleteCloseAll(void);

// Brings up the single shared delete-box pump thread (creating it on first use)
// and returns its message window so callers can post work to it directly.
HWND SayonaraEnsurePump(void);

// Where successive boxes land. Random scatters them anywhere on screen
// (keeping the title bar visible); Cascade stair-steps each new box down and
// to the right of the last one, wrapping when it runs off the work area.
enum SayonaraPlacement {
    SAYONARA_PLACEMENT_RANDOM = 0,
    SAYONARA_PLACEMENT_CASCADE = 1
};
void SayonaraDeleteSetPlacement(int mode);
