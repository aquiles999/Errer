#ifndef FAKEEXPLORER_H
#define FAKEEXPLORER_H

#include <windows.h>

// visible=false creates the window hidden (no WS_VISIBLE). Used for windows
// that are parked off-screen until a later beat - they would otherwise render a
// full folder view at x=100000 where nobody can see it.
HWND CreateFakeExplorer(const wchar_t* path, int x, int y, int w, int h,
                        bool visible = true);
void NavigateFakeExplorer(HWND hwnd, const wchar_t* path);
void DestroyFakeExplorer(HWND hwnd);
void PumpMessagesFor(unsigned ms);

// Posts a position update to the explorer's owning thread. The explorer's
// thread calls SetWindowPos on itself, so the shared pump never cross-thread
// blocks. Use this instead of SetWindowPos for orbit/window movement.
void ExplorerMove(HWND hwnd, int x, int y);

// Diagnostics: how often the fake explorer is actually painting vs moving.
unsigned long FakeExplorerPaintCount();
unsigned long FakeExplorerEraseCount();
unsigned long FakeExplorerPosCount();
void FakeExplorerResetCounts();

// Parses the given paths into the PIDL cache up front, so later navigations
// skip SHParseDisplayName. Safe to call off the UI thread; call it early
// (before the animation starts) so the cost is not paid mid-run.
void FakeExplorerPrimePaths(const wchar_t** paths, int count);

#endif