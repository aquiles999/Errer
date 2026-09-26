#ifndef FAKEEXPLORER_H
#define FAKEEXPLORER_H

#include <windows.h>

HWND CreateFakeExplorer(const wchar_t* path, int x, int y, int w, int h);
void NavigateFakeExplorer(HWND hwnd, const wchar_t* path);
void DestroyFakeExplorer(HWND hwnd);
void PumpMessagesFor(unsigned ms);

#endif