#pragma once
#include <windows.h>

// Wallpaper animation segments.
//
// The show has two separate points where the real desktop wallpaper is
// animated:
//
//   1. mid-show, at song ms 53793  (Payload 3)
//   2. the ending, at song ms 210315 (Payload 8)
//
// Each segment is a run of PNG frames embedded in the exe as RCDATA with
// consecutive numeric IDs, played by writing one file per frame and pointing
// the desktop wallpaper at it.
//
// The user's original wallpaper is copied to a temp backup ONCE, before the
// first segment runs, and is only put back at the very end of the show - so
// the first segment deliberately leaves its last frame on screen.

#define WALLPAPER_TAG_MID L"mid"
#define WALLPAPER_TAG_END L"end"

struct WallpaperAnimSpec {
    int            resBase;     // first RCDATA id of the frame run
    int            frameCount;  // number of frames
    int            fps;         // playback rate
    int            songMs;      // song position the segment starts at
    const wchar_t* tag;         // temp folder suffix, unique per segment
    bool           restoreAtEnd;// true: put the original wallpaper back
    // true: overwrite the temp backup with the current wallpaper even if one
    // already exists. The FIRST segment of the show sets this, so it always
    // captures the wallpaper the user actually had. Without it a leftover
    // backup from an aborted run would be reused and the "original" restored
    // at the end would be the wrong image.
    bool           forceBackup;
};

struct WallpaperAnimResult {
    int  framesShown;
    int  framesSkipped;
    int  framesFailed;
    bool backedUp;   // the original wallpaper was copied to the temp backup
    bool restored;   // the original wallpaper was put back
};

// Runs one segment end to end: waits for its song slot, plays the frames,
// and optionally restores the user's original wallpaper afterwards.
WallpaperAnimResult WallpaperAnimRun(const WallpaperAnimSpec& spec);

// Deletes every temp file this module created (both segment frame folders
// and the original-wallpaper backup). Called once, at the very end of the show.
void WallpaperAnimCleanup(void);
