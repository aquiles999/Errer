#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <winreg.h>
#include "SongSync.h"
#include "CrashLog.h"
#include "WallpaperAnim.h"

// Where the desktop background path lives.
#define WP_KEY L"Control Panel\\Desktop"

// The original wallpaper is copied here once, before the first segment runs,
// and restored from here at the very end of the show.
#define WP_BACKUP_BASE  L"errer_wallpaper_backup"
#define WP_BACKUP_META  L"errer_wallpaper_backup.txt"

// Restoring the wallpaper is racy: SPI_SETDESKWALLPAPER repaints
// asynchronously, so a restore issued right after ~272 frame updates is often
// coalesced away behind an in-flight frame repaint and the desktop ends up
// showing a frame from a few updates ago. Drain the queue first, then restore
// repeatedly - repeating is idempotent and the last call that actually
// reaches the shell wins.
#define WP_RESTORE_ATTEMPTS 4
#define WP_RESTORE_DRAIN_MS  400

// --- module state ------------------------------------------------------------

static BYTE*  g_frameData = NULL;
static DWORD* g_frameOff  = NULL;
static DWORD* g_frameSize = NULL;
static int    g_framesLoaded = 0;
static wchar_t g_frameDir[MAX_PATH] = L"";

// --- preloaded frames --------------------------------------------------------
// Every frame is pulled out of the exe once, up front, into one contiguous
// block. Doing this during playback would mean a resource load interleaved
// with every wallpaper apply; doing it here keeps the loop to a pure
// write+apply per frame.

static bool PreloadFrames(int resBase, int frameCount) {
    g_frameOff  = (DWORD*)calloc((size_t)frameCount + 1, sizeof(DWORD));
    g_frameSize = (DWORD*)calloc((size_t)frameCount, sizeof(DWORD));
    if (!g_frameOff || !g_frameSize) return false;

    DWORD total = 0;
    for (int i = 0; i < frameCount; i++) {
        HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(resBase + i), (LPCWSTR)RT_RCDATA);
        if (!hr) { CrashLogf("wallpaper: frame resource %d missing", resBase + i); return false; }
        DWORD sz = SizeofResource(NULL, hr);
        if (sz == 0) return false;
        g_frameOff[i] = total;
        g_frameSize[i] = sz;
        total += sz;
    }

    if (total == 0) return false;
    g_frameData = (BYTE*)malloc(total);
    if (!g_frameData) return false;

    for (int i = 0; i < frameCount; i++) {
        HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(resBase + i), (LPCWSTR)RT_RCDATA);
        HGLOBAL hg = LoadResource(NULL, hr);
        const BYTE* p = hg ? (const BYTE*)LockResource(hg) : NULL;
        if (!p) return false;
        memcpy(g_frameData + g_frameOff[i], p, g_frameSize[i]);
    }

    g_framesLoaded = frameCount;

    char msg[96];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "wallpaper: preloaded %d frames, %lu bytes",
                g_framesLoaded, (unsigned long)total);
    CrashLog(msg);
    return true;
}

static void FreeFrames(void) {
    if (g_frameData) { free(g_frameData); g_frameData = NULL; }
    if (g_frameOff)  { free(g_frameOff);  g_frameOff = NULL; }
    if (g_frameSize) { free(g_frameSize); g_frameSize = NULL; }
    g_framesLoaded = 0;
}

// --- registry ----------------------------------------------------------------

static bool ReadCurrentWallpaper(wchar_t* out, size_t cap) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, WP_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, cb = (DWORD)(cap * sizeof(wchar_t));
    LONG r = RegQueryValueExW(hk, L"Wallpaper", NULL, &type, (LPBYTE)out, &cb);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || type != REG_SZ) return false;
    out[cap - 1] = 0;
    return out[0] != 0;
}

// Overwrite the single "Wallpaper" value. One value, rewritten once per
// frame - not a value per frame.
static bool SetWallpaperPath(const wchar_t* path) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, WP_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD cb = (DWORD)((wcslen(path) + 1) * sizeof(wchar_t));
    LONG r = RegSetValueExW(hk, L"Wallpaper", 0, REG_SZ, (const BYTE*)path, cb);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

// Set the path and ask Explorer to load/repaint it.
//
// SPI_SETDESKWALLPAPER is the only path verified to actually repaint the
// desktop. The cost is dominated by the shell repaint + WM_SETTINGCHANGE
// broadcast, not by the registry write or by decoding the image. When a frame
// overruns its slot the playback loop skips ahead rather than sliding further
// behind, so the run stays on schedule even though each apply is slow.
static bool WriteWallpaper(const wchar_t* path) {
    DWORD started = GetTickCount();
    CrashLogf("wallpaper apply begin path=%ls", path);
    if (!SetWallpaperPath(path)) {
        CrashLogf("wallpaper registry set FAILED path=%ls", path);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    BOOL ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    DWORD spiError = ok ? ERROR_SUCCESS : GetLastError();
    CrashLogf("wallpaper apply end ok=%d spiErr=%lu elapsed=%lu path=%ls",
              ok ? 1 : 0, (unsigned long)spiError,
              (unsigned long)(GetTickCount() - started), path);
    return ok != FALSE;
}

// --- original wallpaper backup ------------------------------------------------
// The backup lives in a fixed temp location so it survives between the two
// segments, which run in different processes.

static void BackupPaths(wchar_t* meta, size_t metaCap, wchar_t* img, size_t imgCap) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) wcscpy_s(tmp, MAX_PATH, L".");
    _snwprintf_s(meta, metaCap, _TRUNCATE, L"%s%s", tmp, WP_BACKUP_META);
    _snwprintf_s(img, imgCap, _TRUNCATE, L"%s%s", tmp, WP_BACKUP_BASE);
}

// Reads the saved metadata: line 1 is the original wallpaper path, line 2 is
// the path of the copied image.
static bool ReadBackup(wchar_t* orig, size_t origCap, wchar_t* img, size_t imgCap) {
    wchar_t meta[MAX_PATH];
    BackupPaths(meta, MAX_PATH, img, imgCap);

    HANDLE h = CreateFileW(meta, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[2048] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(h);
    if (read == 0) return false;
    buf[read] = 0;

    char* first = strchr(buf, '\r');
    if (!first) first = strchr(buf, '\n');
    if (!first) return false;
    *first = 0;
    char* second = first + 1;
    while (*second == '\r' || *second == '\n') second++;

    MultiByteToWideChar(CP_ACP, 0, buf, -1, orig, (int)origCap);
    MultiByteToWideChar(CP_ACP, 0, second, -1, img, (int)imgCap);
    return orig[0] != 0;
}

// Copies the user's wallpaper image to temp and remembers where it came from.
//
// force = false: never overwrite an existing backup. The backup must stay the
// wallpaper the user had BEFORE the first segment started, so the second
// segment reuses what the first one saved.
//
// force = true: overwrite. The first segment of the show uses this, so it
// always captures the wallpaper that is actually on screen right now. A
// leftover backup from an aborted earlier run would otherwise be reused, and
// the "original" restored at the end would be the wrong image.
static bool SaveOriginalBackup(bool force) {
    wchar_t meta[MAX_PATH], existingImg[MAX_PATH], existingOrig[MAX_PATH];
    BackupPaths(meta, MAX_PATH, existingImg, MAX_PATH);
    if (!force && ReadBackup(existingOrig, MAX_PATH, existingImg, MAX_PATH)) {
        CrashLogf("wallpaper backup already saved (original=%ls)", existingOrig);
        return true;
    }
    if (force) {
        CrashLog("wallpaper backup: replacing any existing backup");
        DeleteFileW(meta);
    }

    wchar_t original[MAX_PATH] = L"";
    if (!ReadCurrentWallpaper(original, MAX_PATH)) {
        CrashLog("wallpaper backup: could not read current wallpaper");
        return false;
    }
    CrashLogf("wallpaper backup: original=%ls", original);

    const wchar_t* slash = wcsrchr(original, L'\\');
    const wchar_t* dot   = wcsrchr(original, L'.');
    const wchar_t* ext   = L".jpg";
    if (dot && (!slash || dot > slash) && wcslen(dot) < 16) ext = dot;

    wchar_t img[MAX_PATH];
    BackupPaths(meta, MAX_PATH, img, MAX_PATH);
    wchar_t imgFull[MAX_PATH];
    _snwprintf_s(imgFull, MAX_PATH, _TRUNCATE, L"%s%s", img, ext);
    DeleteFileW(imgFull);

    if (!CopyFileW(original, imgFull, FALSE)) {
        CrashLogf("wallpaper backup: copy FAILED err=%lu", (unsigned long)GetLastError());
        return false;
    }

    // Metadata: original path on line 1, copied image path on line 2.
    char text[2048] = {};
    int n = _snprintf_s(text, sizeof(text), _TRUNCATE, "%ls\r\n%ls\r\n", original, imgFull);
    HANDLE h = CreateFileW(meta, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, text, (DWORD)n, &written, NULL);
        CloseHandle(h);
    }

    CrashLogf("wallpaper backup saved: %ls", imgFull);
    return true;
}

// Puts the user's original wallpaper back from the temp backup.
static bool RestoreOriginalBackup(void) {
    wchar_t original[MAX_PATH] = L"", img[MAX_PATH] = L"";
    if (!ReadBackup(original, MAX_PATH, img, MAX_PATH)) {
        CrashLog("wallpaper restore: no backup found; leaving current wallpaper");
        return false;
    }

    // If the copied image is gone (cleaned up, or the temp folder was wiped),
    // fall back to the original path itself as long as that file still exists.
    const wchar_t* restorePath = img;
    bool useOriginalPath = false;
    if (GetFileAttributesW(img) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(original) != INVALID_FILE_ATTRIBUTES) {
            useOriginalPath = true;
            restorePath = original;
            CrashLogf("wallpaper restore: backup image missing, using original path %ls",
                      original);
        } else {
            CrashLogf("wallpaper restore: neither backup nor original exists "
                      "(backup=%ls original=%ls)", img, original);
            return false;
        }
    }

    // Let the shell finish repainting the tail of the frame queue first.
    Sleep(WP_RESTORE_DRAIN_MS);

    int ok = 0;
    for (int attempt = 1; attempt <= WP_RESTORE_ATTEMPTS; attempt++) {
        if (WriteWallpaper(restorePath)) ok++;

        // Put the user's original registry path back WITHOUT triggering another
        // repaint. The shell has already loaded the pixels, so the visible
        // wallpaper is correct and the registry no longer points at our temp
        // folder.
        if (!useOriginalPath)
            SetWallpaperPath(original);

        if (attempt < WP_RESTORE_ATTEMPTS) Sleep(250);
    }

    // Confirm what the registry ended up pointing at.
    wchar_t now[MAX_PATH] = L"";
    if (ReadCurrentWallpaper(now, MAX_PATH))
        CrashLogf("wallpaper restore: registry now points at %ls", now);

    CrashLogf("wallpaper restored from backup (%d/%d attempts, source=%s, original=%ls)",
              ok, WP_RESTORE_ATTEMPTS, useOriginalPath ? "original-path" : "backup",
              original);
    return ok > 0;
}

// --- frame files --------------------------------------------------------------
// SPI_SETDESKWALLPAPER needs a path on disk. Each frame gets its OWN file
// under a temp folder, and that is not tidiness - it is required. The shell
// caches the desktop background by PATH: reusing one filename means the new
// bytes are written under a name the shell already has cached, so it keeps
// showing the old image. That is why only the final frame was ever visible.
//
// The extension is .jpg because that is what Windows 7 accepts. Windows 7
// only supports .bmp and .jpg for the desktop wallpaper - a .png makes
// SPI_SETDESKWALLPAPER fail outright and return FALSE, which is exactly what
// PNG frames did on the Windows 7 target. Windows 8 and later added PNG
// support, so the same code works there too.

#define WP_FRAME_EXT L".jpg"

static void MakeFrameDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) wcscpy_s(tmp, MAX_PATH, L".");
    _snwprintf_s(g_frameDir, MAX_PATH, _TRUNCATE, L"%serrer_wp_%s", tmp, tag);
    CreateDirectoryW(g_frameDir, NULL);
}

static bool WriteFrameToTemp(int i) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\f%04d" WP_FRAME_EXT, g_frameDir, i);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        CrashLogf("wallpaper frame=%d file open FAILED err=%lu",
                  i, (unsigned long)GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, g_frameData + g_frameOff[i], g_frameSize[i], &written, NULL);
    DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok || written != g_frameSize[i]) {
        CrashLogf("wallpaper frame=%d file write FAILED ok=%d written=%lu/%lu err=%lu",
                  i, ok ? 1 : 0, (unsigned long)written,
                  (unsigned long)g_frameSize[i], (unsigned long)writeError);
        return false;
    }
    return true;
}

static void RemoveFrameDir(const wchar_t* dir, int frameCount) {
    if (!dir[0]) return;
    for (int i = 0; i < frameCount; i++) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\f%04d" WP_FRAME_EXT, dir, i);
        DeleteFileW(path);
    }
    RemoveDirectoryW(dir);
}

// --- playback -----------------------------------------------------------------

// Play the frames at the requested rate: set one, then sleep whatever is left
// of that frame's slot. If a frame overruns, frames are SKIPPED so the run
// stays on schedule instead of sliding further behind.
static void PlayFrames(int frameCount, int fps, int* outShown, int* outSkipped, int* outFailed) {
    const double FRAME_MS = 1000.0 / (double)fps;
    ULONGLONG t0 = GetTickCount64();
    int shown = 0, skipped = 0, failed = 0;

    for (int i = 0; i < frameCount; i++) {
        ULONGLONG slotEnd = t0 + (ULONGLONG)((double)(i + 1) * FRAME_MS);
        ULONGLONG nowU = GetTickCount64();

        // Must be a SIGNED difference: slotEnd and now are unsigned, so
        // subtracting them underflows the moment we fall behind and the
        // catch-up test below can never be true.
        double behind = (double)((LONGLONG)slotEnd - (LONGLONG)nowU);

        // More than a frame behind: drop straight to the frame we should be on
        // rather than replaying ones whose moment has passed.
        if (behind < -FRAME_MS) {
            int jump = (int)(-behind / FRAME_MS) + 1;
            int next = i + jump;
            if (next >= frameCount) next = frameCount - 1;
            skipped += next - i;
            i = next;
            if (i >= frameCount) break;
            slotEnd = t0 + (ULONGLONG)((double)(i + 1) * FRAME_MS);
        }

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\f%04d" WP_FRAME_EXT, g_frameDir, i);
        CrashLogf("wallpaper frame=%d attempt path=%ls", i, path);
        bool fileOk = WriteFrameToTemp(i);
        bool wallpaperOk = fileOk && WriteWallpaper(path);
        if (fileOk && wallpaperOk) shown++;
        else                    failed++;

        // Sleep out the remainder of this frame's slot.
        ULONGLONG after = GetTickCount64();
        if ((LONGLONG)slotEnd > (LONGLONG)after) {
            DWORD ms = (DWORD)(slotEnd - after);
            if (ms > 1) Sleep(ms - 1);
        }
    }

    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "wallpaper: shown=%d skipped=%d failed=%d elapsed=%lums",
                shown, skipped, failed, (unsigned long)(GetTickCount64() - t0));
    CrashLog(msg);

    if (outShown)   *outShown = shown;
    if (outSkipped) *outSkipped = skipped;
    if (outFailed)  *outFailed = failed;
}

// --- public API ----------------------------------------------------------------

WallpaperAnimResult WallpaperAnimRun(const WallpaperAnimSpec& spec) {
    WallpaperAnimResult result = {};
    result.framesShown = 0;

    // Pull every frame out of the exe into RAM now, while nothing is on screen
    // yet. Doing it here keeps the playback loop free of resource loads.
    if (!PreloadFrames(spec.resBase, spec.frameCount)) {
        CrashLog("wallpaper: preload failed, skipping frames");
        return result;
    }

    // Ride the song clock rather than sleeping a fixed amount, so the segment
    // starts exactly on its slot.
    if (SongSyncWaitHostFlag()) SongSyncWaitHost();
    {
        bool anchored = false;
        DWORD base = SongSyncBase((DWORD)spec.songMs, &anchored);
        CrashLogf("wallpaper: segment tag=%s sync wait begin base=%lu anchored=%d songMs=%d clock=%d",
                  spec.tag, base, anchored ? 1 : 0, spec.songMs,
                  SongSyncAvailable() ? 1 : 0);
        SongSyncWait(base);
        CrashLogf("wallpaper: segment tag=%s sync wait done", spec.tag);
    }

    // Save the user's wallpaper BEFORE the first frame replaces it. The first
    // segment of the show forces a fresh copy; later segments reuse it.
    result.backedUp = SaveOriginalBackup(spec.forceBackup);

    MakeFrameDir(spec.tag);

    int shown = 0, skipped = 0, failed = 0;
    PlayFrames(g_framesLoaded, spec.fps, &shown, &skipped, &failed);

    result.framesShown = shown;
    result.framesSkipped = skipped;
    result.framesFailed = failed;

    if (spec.restoreAtEnd) {
        result.restored = RestoreOriginalBackup();
        // Only now, after the restore has settled, is it safe to delete the
        // temp frame files: the shell may still be resolving one of them.
        Sleep(WP_RESTORE_DRAIN_MS);
        RemoveFrameDir(g_frameDir, g_framesLoaded);
    }
    // Otherwise the segment deliberately leaves its last frame on screen and
    // its files in place; the final segment cleans everything up.

    FreeFrames();
    return result;
}

void WallpaperAnimCleanup(void) {
    // Give the shell a moment to finish with the restored image before the
    // file it was loaded from is deleted.
    Sleep(WP_RESTORE_DRAIN_MS);

    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) wcscpy_s(tmp, MAX_PATH, L".");

    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%serrer_wp_%s", tmp, WALLPAPER_TAG_MID);
    RemoveFrameDir(dir, 4096);
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%serrer_wp_%s", tmp, WALLPAPER_TAG_END);
    RemoveFrameDir(dir, 4096);

    wchar_t meta[MAX_PATH], img[MAX_PATH];
    BackupPaths(meta, MAX_PATH, img, MAX_PATH);
    DeleteFileW(meta);

    wchar_t ext[16];
    for (int e = 0; e < 8; e++) {
        const wchar_t* exts[8] = { L".jpg", L".png", L".bmp", L".jpeg",
                                   L".gif", L".tif", L".tiff", L".webp" };
        _snwprintf_s(ext, 16, _TRUNCATE, L"%s", exts[e]);
        wchar_t imgFull[MAX_PATH];
        _snwprintf_s(imgFull, MAX_PATH, _TRUNCATE, L"%s%s", img, ext);
        DeleteFileW(imgFull);
    }
}
