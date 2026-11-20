#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "FakeExplorer.h"
#include "Flags.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ---------------------------------------------------------------------------
// Cross-thread messages posted to an explorer's owning thread.
// The main pump thread never calls BrowseToIDList directly - it posts
// WM_EXP_NAVIGATE and the explorer's own thread calls BrowseToIDList, so
// a synchronous folder enumeration blocks only that one explorer, never
// the shared pump that moves all the other windows.
// ---------------------------------------------------------------------------
#define WM_EXP_MOVE   (WM_APP + 0x101)
#define WM_EXP_NAVIGATE (WM_APP + 0x102)
#define WM_EXP_NAV_DONE (WM_APP + 0x103)

// ---------------------------------------------------------------------------
// PIDL cache (unchanged: parsed once at startup, read-only during navigation)
// ---------------------------------------------------------------------------
#define PIDL_CACHE_MAX 64

struct PidlEntry {
    wchar_t path[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    wchar_t friendly[128];
    bool    haveFriendly;
};

static PidlEntry g_pidlCache[PIDL_CACHE_MAX];
static int        g_pidlCount = 0;
static CRITICAL_SECTION g_pidlCs;
static volatile LONG g_pidlCsReady = 0;

static void PidlCsInit(void) {
    if (InterlockedCompareExchange(&g_pidlCsReady, 1, 0) == 0)
        InitializeCriticalSection(&g_pidlCs);
}

static int CacheIndex(const wchar_t* path) {
    PidlCsInit();
    EnterCriticalSection(&g_pidlCs);
    for (int i = 0; i < g_pidlCount; i++) {
        if (_wcsicmp(g_pidlCache[i].path, path) == 0) {
            LeaveCriticalSection(&g_pidlCs);
            return i;
        }
    }
    LeaveCriticalSection(&g_pidlCs);
    return -1;
}

static PIDLIST_ABSOLUTE CacheLookup(const wchar_t* path) {
    int i = CacheIndex(path);
    return (i >= 0) ? g_pidlCache[i].pidl : NULL;
}

static void CacheStore(const wchar_t* path, PIDLIST_ABSOLUTE pidl) {
    if (!pidl) return;
    PidlCsInit();
    EnterCriticalSection(&g_pidlCs);
    if (g_pidlCount >= PIDL_CACHE_MAX) { CoTaskMemFree(pidl); LeaveCriticalSection(&g_pidlCs); return; }
    wcsncpy_s(g_pidlCache[g_pidlCount].path, MAX_PATH, path, _TRUNCATE);
    g_pidlCache[g_pidlCount].pidl = pidl;
    g_pidlCache[g_pidlCount].haveFriendly = false;
    g_pidlCache[g_pidlCount].friendly[0] = 0;
    g_pidlCount++;
    LeaveCriticalSection(&g_pidlCs);
}

void FakeExplorerPrimePaths(const wchar_t** paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; i++) {
        if (!paths[i]) continue;
        if (CacheLookup(paths[i])) continue;
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHParseDisplayName(paths[i], NULL, &pidl, 0, NULL)) && pidl)
            CacheStore(paths[i], pidl);
    }
}

// ---------------------------------------------------------------------------
// Paint counters
// ---------------------------------------------------------------------------
static volatile LONG g_paintCount = 0;
static volatile LONG g_eraseCount = 0;
static volatile LONG g_posCount = 0;

unsigned long FakeExplorerPaintCount() { return (unsigned long)g_paintCount; }
unsigned long FakeExplorerEraseCount() { return (unsigned long)g_eraseCount; }
unsigned long FakeExplorerPosCount()  { return (unsigned long)g_posCount; }
void FakeExplorerResetCounts() { g_paintCount = g_eraseCount = g_posCount = 0; }

// ---------------------------------------------------------------------------
// Window procedure (runs on the explorer's own thread)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ExplWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        InterlockedIncrement(&g_paintCount);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        (void)dc;
        return 0;
    }
    case WM_ERASEBKGND:
        InterlockedIncrement(&g_eraseCount);
        return 1;
    case WM_WINDOWPOSCHANGED:
        InterlockedIncrement(&g_posCount);
        break;
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

// ---------------------------------------------------------------------------
// Icon
// ---------------------------------------------------------------------------
static HICON GetFolderIcon(bool big) {
    SHFILEINFOW sfi = {};
    SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                   SHGFI_ICON | (big ? SHGFI_LARGEICON : SHGFI_SMALLICON));
    return sfi.hIcon;
}

// ---------------------------------------------------------------------------
// Nav-last cache (avoids re-browsing to the same path)
// ---------------------------------------------------------------------------
#define NAV_LAST_MAX 8
static struct { HWND hwnd; wchar_t path[MAX_PATH]; } g_navLast[NAV_LAST_MAX];
static CRITICAL_SECTION g_navCs;
static volatile LONG g_navCsReady = 0;

static void NavCsInit(void) {
    if (InterlockedCompareExchange(&g_navCsReady, 1, 0) == 0)
        InitializeCriticalSection(&g_navCs);
}

static bool AlreadyThere(HWND hwnd, const wchar_t* path) {
    NavCsInit();
    EnterCriticalSection(&g_navCs);
    for (int i = 0; i < NAV_LAST_MAX; i++)
        if (g_navLast[i].hwnd == hwnd) {
            bool same = _wcsicmp(g_navLast[i].path, path) == 0;
            LeaveCriticalSection(&g_navCs);
            return same;
        }
    LeaveCriticalSection(&g_navCs);
    return false;
}

static void MarkNav(HWND hwnd, const wchar_t* path) {
    NavCsInit();
    EnterCriticalSection(&g_navCs);
    for (int i = 0; i < NAV_LAST_MAX; i++) {
        if (g_navLast[i].hwnd == hwnd) {
            wcsncpy_s(g_navLast[i].path, MAX_PATH, path, _TRUNCATE);
            LeaveCriticalSection(&g_navCs);
            return;
        }
    }
    for (int i = 0; i < NAV_LAST_MAX; i++) {
        if (!g_navLast[i].hwnd) {
            g_navLast[i].hwnd = hwnd;
            wcsncpy_s(g_navLast[i].path, MAX_PATH, path, _TRUNCATE);
            LeaveCriticalSection(&g_navCs);
            return;
        }
    }
    g_navLast[0].hwnd = hwnd;
    wcsncpy_s(g_navLast[0].path, MAX_PATH, path, _TRUNCATE);
    LeaveCriticalSection(&g_navCs);
}

static void ForgetNav(HWND hwnd) {
    NavCsInit();
    EnterCriticalSection(&g_navCs);
    for (int i = 0; i < NAV_LAST_MAX; i++)
        if (g_navLast[i].hwnd == hwnd) g_navLast[i].hwnd = NULL;
    LeaveCriticalSection(&g_navCs);
}

// ---------------------------------------------------------------------------
// Navigation helper: runs on a background thread so BrowseToIDList never
// blocks the explorer's STA message pump. The pump keeps processing WM_EXP_MOVE
// messages continuously, so orbit explorers keep moving smoothly during nav.
// ---------------------------------------------------------------------------
struct NavigateCtx {
    HWND        hwnd;
    IStream*    browserStream;  // marshaled IExplorerBrowser, ownership transferred
    wchar_t     path[MAX_PATH];
    wchar_t     title[MAX_PATH];
};

// Background thread: parses path, calls BrowseToIDList, then posts results back
// to the STA thread via WM_EXP_NAV_DONE. Runs entirely off the STA pump so move
// messages are never delayed by synchronous folder enumeration.
static DWORD WINAPI BrowseToIDListThreadProc(LPVOID p) {
    NavigateCtx* ctx = (NavigateCtx*)p;
    if (!ctx || !ctx->hwnd || !ctx->browserStream) {
        if (ctx) { if (ctx->browserStream) ctx->browserStream->Release(); delete ctx; }
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IExplorerBrowser* pBrowser = NULL;
    HRESULT hr = CoGetInterfaceAndReleaseStream(ctx->browserStream, IID_PPV_ARGS(&pBrowser));
    // browserStream is always released by CoGetInterfaceAndReleaseStream.

    if (SUCCEEDED(hr) && pBrowser) {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHParseDisplayName(ctx->path, NULL, &pidl, 0, NULL)) && pidl) {
            pBrowser->BrowseToIDList(pidl, 0);

            if (wcsncmp(ctx->path, L"::", 2) == 0) {
                PWSTR friendly = NULL;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &friendly)) && friendly) {
                    wcsncpy_s(ctx->title, MAX_PATH, friendly, _TRUNCATE);
                    CoTaskMemFree(friendly);
                } else {
                    wcsncpy_s(ctx->title, MAX_PATH, ctx->path, _TRUNCATE);
                }
            } else {
                wcsncpy_s(ctx->title, MAX_PATH, ctx->path, _TRUNCATE);
            }

            CoTaskMemFree(pidl);
        } else {
            wcsncpy_s(ctx->title, MAX_PATH, ctx->path, _TRUNCATE);
        }

        pBrowser->Release();
    }

    CoUninitialize();

    // Hand ownership to the STA thread via WM_EXP_NAV_DONE handler.
    PostMessageW(ctx->hwnd, WM_EXP_NAV_DONE, 0, (LPARAM)ctx);
    return 0;
}

// ---------------------------------------------------------------------------
// Explorer thread context
// ---------------------------------------------------------------------------
struct ExplorerThreadCtx {
    wchar_t path[MAX_PATH];
    int     x, y, w, h;
    bool    visible;
    HWND    hwnd;
    HANDLE  ready;
    bool    stop;
};

// Thread procedure: owns one explorer window + its IExplorerBrowser.
// BrowseToIDList runs here, so it blocks only this thread - never the
// shared pump that moves the other windows.
static DWORD WINAPI ExplorerThreadProc(LPVOID p) {
    ExplorerThreadCtx* ctx = (ExplorerThreadCtx*)p;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    if (ctx->visible) style |= WS_VISIBLE;

    ctx->hwnd = CreateWindowExW(0, L"errerFakeExplorer",
                                ctx->path[0] ? ctx->path : L"Explorer",
                                style, ctx->x, ctx->y, ctx->w, ctx->h,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);

    SetEvent(ctx->ready);  // HWND is ready (or NULL on failure)
    if (!ctx->hwnd) { CoUninitialize(); return 0; }

    SetWindowLongPtrW(ctx->hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(ctx->hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);

    IExplorerBrowser* pBrowser = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pBrowser));
    if (SUCCEEDED(hr) && pBrowser) {
        SetWindowLongPtrW(ctx->hwnd, GWLP_USERDATA, (LONG_PTR)pBrowser);
        pBrowser->SetOptions(EBO_SHOWFRAMES);

        RECT rc;
        GetClientRect(ctx->hwnd, &rc);
        FOLDERSETTINGS fs = {};
        fs.ViewMode = 4;
        fs.fFlags = 0;
        pBrowser->Initialize(ctx->hwnd, &rc, &fs);

        if (ctx->path[0]) {
            // Marshal browser to background thread for initial browse too,
            // so the STA pump isn't blocked at startup.
            IStream* stream = NULL;
            if (SUCCEEDED(CoMarshalInterThreadInterfaceInStream(
                    IID_IExplorerBrowser, pBrowser, &stream))) {
                NavigateCtx* nctx = new NavigateCtx();
                nctx->hwnd = ctx->hwnd;
                nctx->browserStream = stream;
                wcsncpy_s(nctx->path, MAX_PATH, ctx->path, _TRUNCATE);
                wcsncpy_s(nctx->title, MAX_PATH, ctx->path, _TRUNCATE);
                HANDLE hNavThread = CreateThread(NULL, 0,
                    BrowseToIDListThreadProc, nctx, 0, NULL);
                if (hNavThread) CloseHandle(hNavThread);
            }
        }
    }

    MSG m;
    while (!ctx->stop && GetMessageW(&m, NULL, 0, 0) > 0) {
        // Intercept cross-thread commands before they reach ExplWndProc.
        if (m.hwnd == ctx->hwnd) {
            if (m.message == WM_EXP_MOVE) {
                // Collapse multiple pending move messages into the latest one,
                // so a burst of moves posted while BrowseToIDList is running
                // on the background thread doesn't make the window jump after
                // navigation returns. The STA pump keeps processing these
                // continuously even during synchronous folder enumeration.
                MSG m2;
                while (PeekMessageW(&m2, ctx->hwnd, WM_EXP_MOVE, WM_EXP_MOVE, PM_REMOVE)) {
                    m = m2;
                }
                SetWindowPos(ctx->hwnd, HWND_TOPMOST,
                             (int)(short)LOWORD(m.lParam), (int)(short)HIWORD(m.lParam),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                continue;
            }
            if (m.message == WM_EXP_NAVIGATE) {
                // Marshal the IExplorerBrowser pointer to a background thread
                // so BrowseToIDList never blocks this STA pump. The pump keeps
                // processing WM_EXP_MOVE messages continuously.
                wchar_t* pathCopy = (wchar_t*)m.lParam;
                IStream* stream = NULL;
                if (SUCCEEDED(CoMarshalInterThreadInterfaceInStream(
                        IID_IExplorerBrowser, pBrowser, &stream))) {
                    NavigateCtx* nctx = new NavigateCtx();
                    nctx->hwnd = ctx->hwnd;
                    nctx->browserStream = stream;
                    wcsncpy_s(nctx->path, MAX_PATH, pathCopy, _TRUNCATE);
                    free(pathCopy);
                    HANDLE hNavThread = CreateThread(NULL, 0,
                        BrowseToIDListThreadProc, nctx, 0, NULL);
                    if (hNavThread) CloseHandle(hNavThread);
                } else {
                    free(pathCopy);
                }
                continue;
            }
            if (m.message == WM_EXP_NAV_DONE) {
                // Background thread finished BrowseToIDList. Apply the title
                // change on the STA thread where it belongs.
                NavigateCtx* nctx = (NavigateCtx*)m.lParam;
                SetWindowTextW(ctx->hwnd, nctx->title);
                MarkNav(ctx->hwnd, nctx->path);
                delete nctx;
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (pBrowser) { pBrowser->Destroy(); pBrowser->Release(); }
    DestroyWindow(ctx->hwnd);
    delete ctx;
    CoUninitialize();
    return 0;
}

// ===========================================================================
// --newexplorer backend
//
// IExplorerBrowser::BrowseToIDList is synchronous: whoever calls it is stuck
// until the folder has been enumerated and the view built. The classic
// backend gives each explorer its own thread, but that thread owns the window
// too, so a cross-thread SetWindowPos still has to wait for the browse.
//
// This backend splits those jobs onto different threads:
//   * host thread - owns the top-level window. It only pumps messages and
//                   answers moves, so it never runs shell enumeration.
//   * view thread - owns the IShellView(s). Parsing, binding and
//                   CreateViewWindow happen here. The old view stays on
//                   screen until the new one is ready (double buffering).
//
// Since the window's owning thread is never the one doing the blocking shell
// work, moving an explorer is instant even while a folder is loading.
// ===========================================================================

#define WM_EXP_NEW_NAV      (WM_APP + 0x201)
#define WM_EXP_NEW_RESIZE   (WM_APP + 0x202)
#define WM_EXP_NEW_QUIT     (WM_APP + 0x203)
#define WM_EXP_NEW_TITLE    (WM_APP + 0x204)
#define WM_EXP_NEW_VIEWGONE (WM_APP + 0x205)

class NewShellBrowser;

struct NewExplorer {
    volatile LONG refs;
    volatile LONG closing;
    HWND    host;
    DWORD   viewTid;
    HANDLE  viewReady;
    wchar_t initialPath[MAX_PATH];
    int     x, y, w, h;
    bool    visible;

    // Navigation pane, owned by the host thread.
    INameSpaceTreeControl* tree;
    HWND    treeHwnd;
    int     sidebarWidth;

    // Current IShellView window. Written by the view thread, read by the host
    // thread so it can move the view immediately even while the view thread is
    // busy loading a folder. Access with Interlocked*Pointer.
    PVOID   viewHwnd;

    // Touched only by the view thread.
    IShellView*       view;
    NewShellBrowser*  browser;
    wchar_t           currentPath[MAX_PATH];
    bool              haveCurrent;
};

static void ReleaseNewExplorer(NewExplorer* ne) {
    if (ne && InterlockedDecrement(&ne->refs) == 0) delete ne;
}

// ---------------------------------------------------------------------------
// Navigation pane (host thread)
//
// This is the Shell's own namespace tree control, the same one Explorer uses
// for Quick Access / This PC. It is independent of IExplorerBrowser, so none
// of the IShellView navigation work ever touches it. The pane is created once
// and lives for the whole window; only the file view on the view thread is
// swapped during navigation.
// ---------------------------------------------------------------------------
#define NEW_SIDEBAR_WIDTH 184

static IShellItem* NewExplorerShellItem(const wchar_t* name) {
    IShellItem* item = NULL;
    if (FAILED(SHCreateItemFromParsingName(name, NULL, IID_PPV_ARGS(&item))))
        return NULL;
    return item;
}

static void NewExplorerAppendRoot(INameSpaceTreeControl* tree,
                                  const wchar_t* name,
                                  SHCONTF enumFlags,
                                  NSTCROOTSTYLE style) {
    if (!tree || !name) return;
    IShellItem* item = NewExplorerShellItem(name);
    if (!item) return;
    tree->AppendRoot(item, enumFlags, style, NULL);
    item->Release();
}

// INameSpaceTreeControl root styles not available in Win7 headers.
#ifndef NSTCRS_FOLDERSONLY
#define NSTCRS_FOLDERSONLY 0x00000002
#endif
#ifndef NSTCS_NOOUTLINE
#define NSTCS_NOOUTLINE 0x00000040
#endif

static void NewExplorerCreateSidebar(NewExplorer* ne, HWND host) {
    if (!ne || !host) return;

    ne->tree = NULL;
    ne->treeHwnd = NULL;
    ne->sidebarWidth = 0;

    INameSpaceTreeControl* tree = NULL;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, NULL,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tree))) ||
        !tree)
        return;

    RECT rc;
    GetClientRect(host, &rc);
    int width = NEW_SIDEBAR_WIDTH;
    if (width > rc.right / 2) width = rc.right / 2;
    if (width < 120) {
        tree->Release();
        return;
    }

    RECT treeRect = { 0, 0, width, rc.bottom };
    // Win11 navigation pane style: full row select, expandos, no infotip,
    // single-click expand, always show selection, tabs.
    DWORD style = NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                  NSTCS_SHOWSELECTIONALWAYS | NSTCS_NOINFOTIP |
                  NSTCS_AUTOHSCROLL | NSTCS_TABSTOP |
                  NSTCS_NOOUTLINE;
    if (FAILED(tree->Initialize(host, &treeRect, style))) {
        tree->Release();
        return;
    }

    // Quick Access group (expanded, visible)
    NewExplorerAppendRoot(tree,
        L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_EXPANDED | NSTCRS_FOLDERSONLY);

    // Gallery
    NewExplorerAppendRoot(tree,
        L"shell:::{0699E2E0-5A9B-4C14-8E3C-DAF7A6C0E5E7}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_EXPANDED | NSTCRS_FOLDERSONLY);

    // Desktop
    NewExplorerAppendRoot(tree,
        L"shell:::{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_EXPANDED | NSTCRS_FOLDERSONLY);

    // User folders (Downloads, Documents, Pictures, Music, Videos)
    NewExplorerAppendRoot(tree,
        L"shell:::{4BD8D571-6D19-48D3-BE97-422220080E43}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_EXPANDED | NSTCRS_FOLDERSONLY);

    // Separator (visual gap between groups)
    NewExplorerAppendRoot(tree,
        L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_EXPANDED | NSTCRS_FOLDERSONLY);

    // This PC (drives) - collapsed by default, like Win11
    NewExplorerAppendRoot(tree,
        L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_FOLDERSONLY);

    // Network
    NewExplorerAppendRoot(tree,
        L"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}",
        SHCONTF_FOLDERS,
        NSTCRS_VISIBLE | NSTCRS_FOLDERSONLY);

    HWND treeHwnd = FindWindowExW(host, NULL, L"NamespaceTreeControl", NULL);
    if (!treeHwnd) {
        tree->Release();
        return;
    }

    // Give the tree padding so items aren't cramped against the edge.
    SetWindowPos(treeHwnd, NULL, 4, 4, width - 8, rc.bottom - 8,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    ne->tree = tree;
    ne->treeHwnd = treeHwnd;
    ne->sidebarWidth = width;
}

static void NewExplorerReleaseSidebar(NewExplorer* ne) {
    if (!ne) return;
    if (ne->tree) {
        ne->tree->Release();
        ne->tree = NULL;
    }
    ne->treeHwnd = NULL;
}

static void NewExplorerLayoutSidebar(NewExplorer* ne) {
    if (!ne || !ne->host || !ne->treeHwnd || !IsWindow(ne->treeHwnd))
        return;
    RECT rc;
    GetClientRect(ne->host, &rc);
    // 4px padding matches Win11 navigation pane spacing.
    SetWindowPos(ne->treeHwnd, NULL, 4, 4, ne->sidebarWidth - 8, rc.bottom - 8,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Minimal IShellBrowser site. The shell view only really needs GetWindow (so
// it can create itself as a child of our host) and QueryActiveShellView.
class NewShellBrowser : public IShellBrowser {
public:
    explicit NewShellBrowser(HWND host) : m_ref(1), m_host(host), m_active(NULL) {}

    void SetActiveView(IShellView* view) {
        if (m_active) m_active->Release();
        m_active = view;
        if (m_active) m_active->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IOleWindow ||
            riid == IID_IShellBrowser) {
            *ppv = static_cast<IShellBrowser*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE GetWindow(HWND* phwnd) override {
        if (!phwnd) return E_POINTER;
        *phwnd = m_host;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG*, WORD) override {
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE, UINT) override {
        // Double-clicking an item inside a fake explorer does nothing; the
        // show only needs the folder view to look right.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream** ppStrm) override {
        if (ppStrm) *ppStrm = NULL;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND* phwnd) override {
        if (phwnd) *phwnd = NULL;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM,
                                             LRESULT* pret) override {
        if (pret) *pret = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView** ppshv) override {
        if (!ppshv) return E_POINTER;
        *ppshv = m_active;
        if (m_active) m_active->AddRef();
        return m_active ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override {
        return S_OK;
    }

private:
    volatile LONG m_ref;
    HWND          m_host;
    IShellView*   m_active;
};

static void NewExplorerSetTitle(HWND host, const wchar_t* text) {
    if (!host || !text) return;
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    wchar_t* copy = (wchar_t*)malloc(bytes);
    if (!copy) return;
    memcpy(copy, text, bytes);
    if (!PostMessageW(host, WM_EXP_NEW_TITLE, 0, (LPARAM)copy)) free(copy);
}

static void NewExplorerResizeView(NewExplorer* ne) {
    if (!ne || !ne->view) return;
    HWND viewHwnd = NULL;
    IOleWindow* ow = NULL;
    if (SUCCEEDED(ne->view->QueryInterface(IID_PPV_ARGS(&ow))) && ow) {
        ow->GetWindow(&viewHwnd);
        ow->Release();
    }
    if (!viewHwnd) return;
    RECT rc;
    GetClientRect(ne->host, &rc);
    int x = ne->sidebarWidth;
    int w = rc.right - x;
    if (w < 1) w = 1;
    SetWindowPos(viewHwnd, NULL, x, 0, w, rc.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Runs on the host thread. Moves the file view window asynchronously so a
// resize is reflected right away even if the view thread is still loading a
// folder. The view thread corrects the shell view's own layout when it gets to
// the posted WM_EXP_NEW_RESIZE.
static void NewExplorerResizeViewWindow(NewExplorer* ne) {
    if (!ne || !ne->host) return;
    HWND viewHwnd = (HWND)InterlockedCompareExchangePointer(
        (PVOID volatile*)&ne->viewHwnd, NULL, NULL);
    if (!viewHwnd || !IsWindow(viewHwnd)) return;
    RECT rc;
    GetClientRect(ne->host, &rc);
    int x = ne->sidebarWidth;
    int w = rc.right - x;
    if (w < 1) w = 1;
    SetWindowPos(viewHwnd, NULL, x, 0, w, rc.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// Runs on the view thread. Blocks only this thread, never the host window.
static bool NewExplorerBuildView(NewExplorer* ne, const wchar_t* path) {
    if (!ne || !path || !*path || ne->closing) return false;
    if (ne->haveCurrent && _wcsicmp(ne->currentPath, path) == 0) return true;

    PIDLIST_ABSOLUTE pidl = NULL;
    if (FAILED(SHParseDisplayName(path, NULL, &pidl, 0, NULL)) || !pidl)
        return false;

    IShellFolder* folder = NULL;
    HRESULT hr = SHBindToObject(NULL, pidl, NULL, IID_PPV_ARGS(&folder));
    if (FAILED(hr) || !folder) {
        IShellFolder* desktop = NULL;
        if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
            desktop->BindToObject(pidl, NULL, IID_PPV_ARGS(&folder));
            desktop->Release();
        }
    }
    if (!folder) {
        CoTaskMemFree(pidl);
        return false;
    }

    SFV_CREATE create = {};
    create.cbSize = sizeof(create);
    create.pshf = folder;
    create.psvOuter = NULL;
    create.psfvcb = NULL;

    IShellView* view = NULL;
    hr = SHCreateShellFolderView(&create, &view);
    folder->Release();
    if (FAILED(hr) || !view) {
        CoTaskMemFree(pidl);
        return false;
    }

    NewShellBrowser* browser = new NewShellBrowser(ne->host);
    if (!browser) {
        view->Release();
        CoTaskMemFree(pidl);
        return false;
    }
    browser->SetActiveView(view);

    FOLDERSETTINGS fs = {};
    fs.ViewMode = 4;   // details, same as the classic backend
    fs.fFlags = 0;

    RECT rc;
    GetClientRect(ne->host, &rc);
    int viewX = ne->sidebarWidth;
    int viewW = rc.right - viewX;
    if (viewW < 1) viewW = 1;
    RECT viewRect = { 0, 0, viewW, rc.bottom };
    HWND viewHwnd = NULL;
    hr = view->CreateViewWindow(NULL, &fs, browser, &viewRect, &viewHwnd);
    if (FAILED(hr) || !viewHwnd) {
        browser->SetActiveView(NULL);
        browser->Release();
        view->Release();
        CoTaskMemFree(pidl);
        return false;
    }

    // Keep the new view hidden until the old one is gone, so the swap is a
    // single frame instead of two folder views on top of each other.
    ShowWindow(viewHwnd, SW_HIDE);

    // Work out the title while the PIDL is still alive.
    wchar_t title[MAX_PATH];
    wcsncpy_s(title, MAX_PATH, path, _TRUNCATE);
    if (wcsncmp(path, L"::", 2) == 0) {
        PWSTR friendly = NULL;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &friendly)) &&
            friendly) {
            wcsncpy_s(title, MAX_PATH, friendly, _TRUNCATE);
            CoTaskMemFree(friendly);
        }
    }
    CoTaskMemFree(pidl);

    if (ne->closing) {
        view->DestroyViewWindow();
        browser->SetActiveView(NULL);
        browser->Release();
        view->Release();
        return false;
    }

    // Swap. Destroying the old view and showing the new one both happen on
    // this thread, so neither can block the window's host thread.
    InterlockedExchangePointer((PVOID volatile*)&ne->viewHwnd, NULL);
    if (ne->browser) ne->browser->SetActiveView(NULL);
    if (ne->view) {
        ne->view->DestroyViewWindow();
        ne->view->Release();
    }
    if (ne->browser) ne->browser->Release();

    ne->view = view;
    ne->browser = browser;
    wcsncpy_s(ne->currentPath, MAX_PATH, path, _TRUNCATE);
    ne->haveCurrent = true;

    GetClientRect(ne->host, &rc);
    SetWindowPos(viewHwnd, HWND_TOP, viewX, 0, viewW, rc.bottom,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InterlockedExchangePointer((PVOID volatile*)&ne->viewHwnd, viewHwnd);
    view->UIActivate(SVUIA_ACTIVATE_NOFOCUS);

    NewExplorerSetTitle(ne->host, title);
    return true;
}

static DWORD WINAPI NewExplorerViewThread(LPVOID p) {
    NewExplorer* ne = (NewExplorer*)p;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        if (ne->host && IsWindow(ne->host))
            PostMessageW(ne->host, WM_EXP_NEW_VIEWGONE, 0, 0);
        ReleaseNewExplorer(ne);
        return 0;
    }

    ne->viewTid = GetCurrentThreadId();
    // Create the thread message queue before the host thread reports the
    // explorer as ready, so PostThreadMessage cannot race queue creation.
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(ne->viewReady);

    NewExplorerBuildView(ne, ne->initialPath);

    for (;;) {
        BOOL got = GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) break;

        if (m.hwnd) {
            // Messages for the IShellView window(s) this thread owns.
            TranslateMessage(&m);
            DispatchMessageW(&m);
            continue;
        }

        if (m.message == WM_EXP_NEW_NAV) {
            wchar_t* path = (wchar_t*)m.lParam;
            MSG m2;
            // If several navigations piled up while the previous folder was
            // loading, only the newest one matters.
            while (PeekMessageW(&m2, NULL, WM_EXP_NEW_NAV, WM_EXP_NEW_NAV,
                                PM_REMOVE)) {
                free(path);
                path = (wchar_t*)m2.lParam;
            }
            if (path) {
                NewExplorerBuildView(ne, path);
                free(path);
            }
            continue;
        }

        if (m.message == WM_EXP_NEW_RESIZE) {
            MSG m2;
            while (PeekMessageW(&m2, NULL, WM_EXP_NEW_RESIZE, WM_EXP_NEW_RESIZE,
                                PM_REMOVE)) {
            }
            NewExplorerResizeView(ne);
            continue;
        }

        if (m.message == WM_EXP_NEW_QUIT) break;
    }

    if (ne->browser) ne->browser->SetActiveView(NULL);
    InterlockedExchangePointer((PVOID volatile*)&ne->viewHwnd, NULL);
    if (ne->view) {
        ne->view->DestroyViewWindow();
        ne->view->Release();
        ne->view = NULL;
    }
    if (ne->browser) {
        ne->browser->Release();
        ne->browser = NULL;
    }
    CoUninitialize();

    // Tell the host thread it can destroy its window now. Destroying a window
    // with a live cross-thread child is exactly what we are avoiding here.
    if (ne->host && IsWindow(ne->host))
        PostMessageW(ne->host, WM_EXP_NEW_VIEWGONE, 0, 0);
    ReleaseNewExplorer(ne);
    return 0;
}

static LRESULT CALLBACK NewExplorerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NewExplorer* ne = (NewExplorer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_PAINT: {
        InterlockedIncrement(&g_paintCount);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        (void)dc;
        return 0;
    }
    case WM_ERASEBKGND:
        InterlockedIncrement(&g_eraseCount);
        return 1;
    case WM_WINDOWPOSCHANGED:
        InterlockedIncrement(&g_posCount);
        break;
    case WM_SIZE:
        NewExplorerLayoutSidebar(ne);
        NewExplorerResizeViewWindow(ne);
        if (ne && ne->viewTid && !ne->closing)
            PostThreadMessageW(ne->viewTid, WM_EXP_NEW_RESIZE, 0, 0);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 0x400;
        mmi->ptMinTrackSize.y = 0x240;
        return 0;
    }
    case WM_EXP_NEW_TITLE:
        if (lp) {
            SetWindowTextW(hwnd, (const wchar_t*)lp);
            free((void*)lp);
        }
        return 0;
    case WM_EXP_MOVE:
        SetWindowPos(hwnd, HWND_TOPMOST,
                     (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_CLOSE:
        if (ne && !ne->closing) {
            InterlockedExchange(&ne->closing, 1);
            NewExplorerReleaseSidebar(ne);
            ShowWindow(hwnd, SW_HIDE);
            if (ne->viewTid)
                PostThreadMessageW(ne->viewTid, WM_EXP_NEW_QUIT, 0, 0);
            else
                DestroyWindow(hwnd);
        }
        return 0;
    case WM_EXP_NEW_VIEWGONE:
        NewExplorerReleaseSidebar(ne);
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (ne) {
            NewExplorerReleaseSidebar(ne);
            ne->host = NULL;
            ReleaseNewExplorer(ne);
        }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct NewExplorerCreateCtx {
    NewExplorer* ne;
    HANDLE       ready;
    HWND         hwnd;
};

static DWORD WINAPI NewExplorerHostThread(LPVOID p) {
    NewExplorerCreateCtx* ctx = (NewExplorerCreateCtx*)p;
    NewExplorer* ne = ctx->ne;

    HRESULT coHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool coInit = SUCCEEDED(coHr);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    if (ne->visible) style |= WS_VISIBLE;

    HWND hwnd = CreateWindowExW(0, L"errerFakeExplorer",
                                ne->initialPath[0] ? ne->initialPath : L"Explorer",
                                style, ne->x, ne->y, ne->w, ne->h,
                                NULL, NULL, GetModuleHandleW(NULL), ne);
    ctx->hwnd = hwnd;
    if (!hwnd) {
        SetEvent(ctx->ready);
        ReleaseNewExplorer(ne);
        if (coInit) CoUninitialize();
        return 0;
    }

    ne->host = hwnd;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);

    // The namespace tree is STA-bound, so it lives on this thread. Build it
    // before the view thread starts, so the first IShellView already knows
    // how much room the sidebar takes.
    if (coInit) {
        NewExplorerCreateSidebar(ne, hwnd);
    } else {
        ne->tree = NULL;
        ne->treeHwnd = NULL;
        ne->sidebarWidth = 0;
    }

    ne->viewReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE viewThread = NULL;
    if (ne->viewReady) {
        // The view thread owns its own reference to NewExplorer. Take it
        // before CreateThread, because the new thread can finish and release
        // immediately.
        InterlockedIncrement(&ne->refs);
        viewThread = CreateThread(NULL, 0, NewExplorerViewThread, ne, 0, NULL);
        if (!viewThread)
            ReleaseNewExplorer(ne);
    }
    if (viewThread) {
        // Wait only until the view thread is alive and its queue exists. The
        // initial browse runs after that and cannot delay CreateFakeExplorer.
        WaitForSingleObject(ne->viewReady, 10000);
        CloseHandle(viewThread);
    }
    if (ne->viewReady) {
        CloseHandle(ne->viewReady);
        ne->viewReady = NULL;
    }

    SetEvent(ctx->ready);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (coInit) CoUninitialize();
    return 0;
}

static HWND CreateNewFakeExplorer(const wchar_t* path, int x, int y,
                                  int w, int h, bool visible) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hIcon = GetFolderIcon(true);
        wc.hIconSm = GetFolderIcon(false);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)MAKEINTRESOURCEW(32512));
        wc.lpfnWndProc = NewExplorerWndProc;
        wc.lpszClassName = L"errerFakeExplorer";
        RegisterClassExW(&wc);
        DestroyIcon(wc.hIcon);
        DestroyIcon(wc.hIconSm);
        registered = true;
    }

    if (w == 0) w = 0x400;
    if (h == 0) h = 0x240;

    NewExplorer* ne = new NewExplorer();
    if (!ne) return NULL;
    ne->refs = 1;
    ne->closing = 0;
    ne->host = NULL;
    ne->viewTid = 0;
    ne->viewReady = NULL;
    wcsncpy_s(ne->initialPath, MAX_PATH, path ? path : L"Explorer", _TRUNCATE);
    ne->x = x; ne->y = y; ne->w = w; ne->h = h;
    ne->visible = visible;
    ne->tree = NULL;
    ne->treeHwnd = NULL;
    ne->sidebarWidth = 0;
    ne->viewHwnd = NULL;
    ne->view = NULL;
    ne->browser = NULL;
    ne->currentPath[0] = 0;
    ne->haveCurrent = false;

    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready) {
        ReleaseNewExplorer(ne);
        return NULL;
    }

    NewExplorerCreateCtx* ctx = new NewExplorerCreateCtx();
    if (!ctx) {
        CloseHandle(ready);
        ReleaseNewExplorer(ne);
        return NULL;
    }
    ctx->ne = ne;
    ctx->ready = ready;
    ctx->hwnd = NULL;

    HANDLE thread = CreateThread(NULL, 0, NewExplorerHostThread, ctx, 0, NULL);
    if (!thread) {
        CloseHandle(ready);
        delete ctx;
        ReleaseNewExplorer(ne);
        return NULL;
    }

    WaitForSingleObject(ready, 10000);
    HWND hwnd = ctx->hwnd;
    CloseHandle(ready);
    delete ctx;
    return hwnd;
}

static void NavigateNewFakeExplorer(HWND hwnd, const wchar_t* path) {
    NewExplorer* ne = (NewExplorer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!ne || ne->closing || !ne->viewTid) return;

    size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    wchar_t* copy = (wchar_t*)malloc(bytes);
    if (!copy) return;
    memcpy(copy, path, bytes);
    if (!PostThreadMessageW(ne->viewTid, WM_EXP_NEW_NAV, 0, (LPARAM)copy))
        free(copy);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

HWND CreateFakeExplorer(const wchar_t* path, int x, int y, int w, int h, bool visible) {
    if (NewExplorerFlag())
        return CreateNewFakeExplorer(path, x, y, w, h, visible);

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

    ExplorerThreadCtx* ctx = new ExplorerThreadCtx();
    wcsncpy_s(ctx->path, MAX_PATH, path ? path : L"Explorer", _TRUNCATE);
    ctx->x = x; ctx->y = y; ctx->w = w; ctx->h = h;
    ctx->visible = visible;
    ctx->hwnd = NULL;
    ctx->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx->stop = false;

    HANDLE hThread = CreateThread(NULL, 0, ExplorerThreadProc, ctx, 0, NULL);
    if (!hThread) {
        SetEvent(ctx->ready);
        delete ctx;
        return NULL;
    }

    // Wait for the window to be created so we can return the HWND.
    WaitForSingleObject(ctx->ready, 10000);
    HWND hwnd = ctx->hwnd;
    CloseHandle(ctx->ready);
    // Keep the thread alive - it owns the window and pumps messages.
    // The ctx is freed when the thread exits (see ExplorerThreadProc).
    // We leak the ctx pointer intentionally; the thread cleans up.
    (void)hThread;

    return hwnd;
}

void NavigateFakeExplorer(HWND hwnd, const wchar_t* path) {
    if (!hwnd || !IsWindow(hwnd) || !path) return;

    if (NewExplorerFlag()) {
        NavigateNewFakeExplorer(hwnd, path);
        return;
    }

    if (AlreadyThere(hwnd, path)) return;

    // Allocate a copy of the path - the explorer thread frees it.
    size_t len = (wcslen(path) + 1) * sizeof(wchar_t);
    wchar_t* pathCopy = (wchar_t*)malloc(len);
    if (!pathCopy) return;
    memcpy(pathCopy, path, len);

    // Post to the explorer's window - its thread picks it up and calls
    // BrowseToIDList there, never blocking the shared pump.
    PostMessageW(hwnd, WM_EXP_NAVIGATE, 0, (LPARAM)pathCopy);
}

void DestroyFakeExplorer(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    ForgetNav(hwnd);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void PumpMessagesFor(unsigned ms) {
    DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(8);
    } while (GetTickCount() < end);
}

// ---------------------------------------------------------------------------
// ExplorerMove: posts a move to the window's own thread. The owner thread
// calls SetWindowPos on itself - zero cross-thread blocking, ever. With
// --newexplorer the host thread is never stuck inside shell navigation, so
// moves are processed on the next pump cycle and the orbit keeps moving
// smoothly during BrowseToIDList.
// ---------------------------------------------------------------------------
void ExplorerMove(HWND hwnd, int x, int y) {
    if (!hwnd || !IsWindow(hwnd)) return;
    PostMessageW(hwnd, WM_EXP_MOVE, 0, MAKELPARAM(x, y));
}
