#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "FakeExplorer.h"

static void PumpOnce() {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void PumpMessagesFor(unsigned ms) {
    DWORD end = GetTickCount() + ms;
    do {
        PumpOnce();
        Sleep(8);
    } while (GetTickCount() < end);
}

static LRESULT CALLBACK ExplWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        IExplorerBrowser* pBrowser = (IExplorerBrowser*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (pBrowser) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            pBrowser->SetRect(NULL, rc);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 0x400;
        mmi->ptMinTrackSize.y = 0x240;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HICON GetFolderIcon(bool big) {
    SHFILEINFOW sfi = {};
    SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                   SHGFI_ICON | (big ? SHGFI_LARGEICON : SHGFI_SMALLICON));
    return sfi.hIcon;
}

HWND CreateFakeExplorer(const wchar_t* path, int x, int y, int w, int h) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hIcon = GetFolderIcon(true);
        wc.hIconSm = GetFolderIcon(false);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)MAKEINTRESOURCEW(32512));
        wc.lpfnWndProc = ExplWndProc;
        wc.lpszClassName = L"errerFakeExplorer";
        RegisterClassExW(&wc);
        DestroyIcon(wc.hIcon);
        DestroyIcon(wc.hIconSm);
        reg = true;
    }
    if (w == 0) w = 0x400;
    if (h == 0) h = 0x240;

    HWND hwnd = CreateWindowExW(0, L"errerFakeExplorer", path ? path : L"Explorer",
                                WS_VISIBLE | WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return NULL;

    IExplorerBrowser* pBrowser = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pBrowser));
    if (FAILED(hr) || !pBrowser) return hwnd;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pBrowser);
    pBrowser->SetOptions(EBO_SHOWFRAMES);

    RECT rc;
    GetClientRect(hwnd, &rc);
    FOLDERSETTINGS fs = {};
    fs.ViewMode = 4;
    fs.fFlags = 0;
    pBrowser->Initialize(hwnd, &rc, &fs);

    if (path) NavigateFakeExplorer(hwnd, path);
    PumpOnce();
    return hwnd;
}

void NavigateFakeExplorer(HWND hwnd, const wchar_t* path) {
    if (!hwnd || !IsWindow(hwnd)) return;
    IExplorerBrowser* pBrowser = (IExplorerBrowser*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!pBrowser) return;
    PIDLIST_ABSOLUTE pidl = NULL;
    if (SUCCEEDED(SHParseDisplayName(path, NULL, &pidl, 0, NULL)) && pidl) {
        pBrowser->BrowseToIDList(pidl, 0);
        CoTaskMemFree(pidl);
    }
    SetWindowTextW(hwnd, path);
}

void DestroyFakeExplorer(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    IExplorerBrowser* pBrowser = (IExplorerBrowser*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (pBrowser) {
        pBrowser->Destroy();
        pBrowser->Release();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    PumpMessagesFor(80);
}