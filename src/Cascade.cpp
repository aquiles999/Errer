#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "comctl32.lib")

#define CASCADE_INTERVAL        90
#define CASCADE_MAX_LIVE        35
#define CASCADE_MAX_DIR_CHANGES 10

struct CascadeBox {
    DWORD tid;
    HANDLE thread;
};

static volatile LONG g_stop = 0;
static HANDLE g_thread = NULL;
static CascadeBox g_boxes[CASCADE_MAX_LIVE];
static volatile LONG g_boxesEnd = 0;

static const wchar_t* C_TITLES[16] = {
    L"Windows", L"System Error", L"Application Error", L"errer.exe - Application Error",
    L"Setup", L"Runtime Error!", L"Windows Explorer", L"System",
    L"Windows Security", L"DLL Host", L"svchost.exe - Application Error", L"Registry Editor",
    L"Task Scheduler", L"Error", L"Microsoft Windows", L"File Explorer"
};

static const wchar_t* C_TEXTS[16] = {
    L"The instruction at 0x0000000000000000 referenced memory at 0x0000000000000000. The memory could not be read.",
    L"The exception unknown software exception (0xc0000005) occurred in the application at location 0x0000000140001A25.",
    L"The application failed to initialize properly (0xc0000005). Click on OK to terminate the application.",
    L"The system cannot find the file specified.",
    L"A required DLL file could not be found.",
    L"Not enough storage is available to process this command.",
    L"The process cannot access the file because it is being used by another process.",
    L"The instruction at 0x00007FFB4A23A7B0 referenced memory at 0x0000000000000000. The memory could not be written.",
    L"Windows cannot verify the identity of this file. Do you want to run this software anyway?",
    L"An application error has occurred. The exception unknown software exception (0xc0000409) was thrown.",
    L"The instruction at 0x00000000FFC40123 referenced memory at 0x00000000FFC40123. The memory could not be written.",
    L"Cannot import file: The specified file is not a registry script. You can only import registry files.",
    L"Task Scheduler has encountered an error. The task was not run.",
    L"Access to the specified device, path, or file is denied.",
    L"Windows could not start because the following file is missing or corrupt: \\WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM.",
    L"The item cannot be deleted because it is in use."
};

static const wchar_t* kAnchorClass = L"errerCascadeAnchor";
static LONG sAnchorReg = 0;

static void MeasureBox(const wchar_t* text, int* w, int* h) {
    HDC hdc = GetDC(NULL);
    HFONT hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, hFont);
    RECT rw = { 0, 0, 0, 0 };
    DrawTextW(hdc, text, -1, &rw, DT_CALCRECT | DT_WORDBREAK);
    int textW = rw.right + 40;
    int textH = rw.bottom + 20;
    if (textW < 160) textW = 160;
    if (textW > 420) textW = 420;
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    ReleaseDC(NULL, hdc);
    *w = textW + 64;
    *h = textH + 86;
}

struct FindDlg {
    HWND hwnd;
};

static BOOL CALLBACK FindDlgProc(HWND hwnd, LPARAM lp) {
    FindDlg* f = (FindDlg*)lp;
    wchar_t cls[16];
    if (GetClassNameW(hwnd, cls, 16) && _wcsicmp(cls, L"#32770") == 0) {
        f->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

struct CascadeTask {
    const wchar_t* title;
    const wchar_t* text;
    int x, y, w, h;
};

static DWORD WINAPI CascadeBoxThread(LPVOID p) {
    CascadeTask* t = (CascadeTask*)p;
    if (InterlockedCompareExchange(&sAnchorReg, 1, 0) == 0) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = kAnchorClass;
        RegisterClassExW(&wc);
    }

    HWND anchor = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                  kAnchorClass, L"", WS_POPUP | WS_VISIBLE,
                                  t->x, t->y, t->w, t->h,
                                  NULL, NULL, GetModuleHandle(NULL), NULL);
    if (anchor) SetLayeredWindowAttributes(anchor, 0, 0, LWA_ALPHA);

    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = anchor;
    cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_HICON_MAIN;
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = t->title;
    cfg.pszContent = t->text;
    cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_ERROR));
    TaskDialogIndirect(&cfg, NULL, NULL, NULL);

    if (anchor) DestroyWindow(anchor);
    delete t;
    return 0;
}

static void ReapFinished() {
    for (int i = 0; i < CASCADE_MAX_LIVE; i++) {
        CascadeBox& s = g_boxes[i];
        if (s.thread && WaitForSingleObject(s.thread, 0) == WAIT_OBJECT_0) {
            CloseHandle(s.thread);
            s.thread = NULL;
            s.tid = 0;
        }
    }
}

static void DismissBox(CascadeBox& s) {
    if (!s.thread) return;
    FindDlg f = {};
    EnumThreadWindows(s.tid, FindDlgProc, (LPARAM)&f);
    if (f.hwnd && IsWindow(f.hwnd)) {
        SendMessageW(f.hwnd, TDM_CLICK_BUTTON, IDOK, 0);
        PostMessageW(f.hwnd, WM_CLOSE, 0, 0);
    }
    CloseHandle(s.thread);
    s.thread = NULL;
    s.tid = 0;
}

static DWORD WINAPI CascadeLoop(LPVOID p) {
    (void)p;
    srand((unsigned)(GetTickCount() ^ 0x51F15E));
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    double headX = 120.0 + (rand() % (sw > 240 ? sw - 240 : 1));
    double headY = 120.0 + (rand() % (sh > 240 ? sh - 240 : 1));
    const double PI2 = 6.283185307179586;
    double ang = (double)rand() / RAND_MAX * PI2;
    double dirx = cos(ang), diry = sin(ang);

    int dirChanges = 0;
    DWORD nextDirChange = GetTickCount() + 1000 + rand() % 3000;
    DWORD lastSpawn = 0;

    while (!g_stop) {
        DWORD now = GetTickCount();

        if (dirChanges < CASCADE_MAX_DIR_CHANGES && now >= nextDirChange) {
            ang = (double)rand() / RAND_MAX * PI2;
            dirx = cos(ang);
            diry = sin(ang);
            dirChanges++;
            nextDirChange = now + 1000 + rand() % 3000;
        }

        if (now - lastSpawn >= CASCADE_INTERVAL) {
            double step = 30.0 + (rand() % 50);
            double jx = (double)(rand() % 60) - 30.0;
            double jy = (double)(rand() % 30) - 15.0;
            headX += dirx * step + jx;
            headY += diry * step + jy;

            double margin = 70.0;
            if (headX < margin) { headX = margin; dirx = fabs(dirx); }
            if (headX > sw - margin) { headX = sw - margin; dirx = -fabs(dirx); }
            if (headY < margin) { headY = margin; diry = fabs(diry); }
            if (headY > sh - margin) { headY = sh - margin; diry = -fabs(diry); }

            int ti = rand() % 16;
            int w = 0, h = 0;
            MeasureBox(C_TEXTS[ti], &w, &h);
            int x = (int)headX, y = (int)headY;
            if (x + w > sw) x = sw - w;
            if (y + h > sh) y = sh - h;
            if (x < 0) x = 0;
            if (y < 0) y = 0;

            CascadeTask* t = new CascadeTask{ C_TITLES[ti], C_TEXTS[ti], x, y, w, h };
            CascadeBox& slot = g_boxes[g_boxesEnd % CASCADE_MAX_LIVE];
            g_boxesEnd++;
            DismissBox(slot);
            HANDLE hTh = CreateThread(NULL, 0, CascadeBoxThread, t, 0, &slot.tid);
            slot.thread = hTh;
            lastSpawn = now;
        }

        ReapFinished();
        Sleep(10);
    }
    return 0;
}

void CascadeStart() {
    if (g_thread) return;
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, CascadeLoop, NULL, 0, NULL);
}

void CascadeStop() {
    if (g_thread) {
        InterlockedExchange(&g_stop, 1);
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    HANDLE threads[CASCADE_MAX_LIVE];
    int nThreads = 0;
    for (int i = 0; i < CASCADE_MAX_LIVE; i++) {
        CascadeBox& s = g_boxes[i];
        if (!s.thread) continue;
        FindDlg f = {};
        EnumThreadWindows(s.tid, FindDlgProc, (LPARAM)&f);
        if (f.hwnd && IsWindow(f.hwnd)) PostMessageW(f.hwnd, WM_CLOSE, 0, 0);
        threads[nThreads++] = s.thread;
    }
    if (nThreads > 0)
        WaitForMultipleObjects(nThreads, threads, TRUE, 800);
    for (int i = 0; i < CASCADE_MAX_LIVE; i++) {
        CascadeBox& s = g_boxes[i];
        if (s.thread) { CloseHandle(s.thread); s.thread = NULL; s.tid = 0; }
    }
    g_boxesEnd = 0;
    InterlockedExchange(&g_stop, 0);
}