#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <string>
#include <wchar.h>
#include <vector>
#include <map>
#include <math.h>
#include <commctrl.h>
#include "../Cascade.h"
#include "../FakeExplorer.h"
#include "../SayonaraDelete.h"
#include "../SongSync.h"
#include "../CrashLog.h"
#include "../Toast.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int g_icon = MB_ICONERROR;

enum class ErrorPos {
    None, Middle, TopLeft, TopRight, BottomLeft, BottomRight, Random, Cascade, Absolute
};

static ErrorPos g_position = ErrorPos::None;
static int g_absX = 0;
static int g_absY = 0;

struct ThreadInfo {
    std::wstring title;
    std::wstring text;
    int icon;
    ErrorPos pos;
    int absX;
    int absY;
};

static LONG g_posCounts[7] = {0, 0, 0, 0, 0, 0, 0};

static int PosIndex(ErrorPos pos) {
    switch (pos) {
        case ErrorPos::Middle:      return 0;
        case ErrorPos::TopLeft:     return 1;
        case ErrorPos::TopRight:    return 2;
        case ErrorPos::BottomLeft:  return 3;
        case ErrorPos::BottomRight: return 4;
        case ErrorPos::Random:      return 5;
        case ErrorPos::Cascade:     return 6;
        case ErrorPos::Absolute:    return -1;
        default: return -1;
    }
}

static int AllocOffset(ErrorPos pos) {
    int idx = PosIndex(pos);
    if (idx < 0) return 0;
    return (int)InterlockedIncrement(&g_posCounts[idx]) - 1;
}

static void FreeOffset(ErrorPos pos) {
    int idx = PosIndex(pos);
    if (idx >= 0) InterlockedDecrement(&g_posCounts[idx]);
}

static void MeasureBox(const std::wstring& text, int* w, int* h) {
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    if (dpi <= 0) dpi = 96;
    auto PX = [dpi](int v) { return MulDiv(v, dpi, 96); };

    HFONT hFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, hFont);

    // Windows behaves this way: the dialog grows sideways as the text needs,
    // up to a cap, then the text wraps and the box grows downward instead.
    // Model that so short messages stay compact and long ones get tall rather
    // than being pinned to one size.
    const int kMaxContentW = PX(340);
    const int kMinContentW = PX(120);

    // Widest unwrapped line decides the width when it fits under the cap.
    int maxLineW = 0;
    const wchar_t* s = text.c_str();
    while (*s) {
        const wchar_t* nl = wcschr(s, L'\n');
        size_t len = nl ? (size_t)(nl - s) : wcslen(s);
        RECT r = { 0, 0, 0, 0 };
        DrawTextW(hdc, s, (int)len, &r, DT_CALCRECT | DT_SINGLELINE);
        if (r.right > maxLineW) maxLineW = r.right;
        if (!nl) break;
        s = nl + 1;
    }

    int contentW = maxLineW;
    if (contentW > kMaxContentW) contentW = kMaxContentW;
    if (contentW < kMinContentW) contentW = kMinContentW;

    // Re-wrap at the chosen width so the height reflects the real layout.
    RECT rw = { 0, 0, contentW, 0 };
    DrawTextW(hdc, text.c_str(), -1, &rw, DT_CALCRECT | DT_WORDBREAK);
    int textH = rw.bottom - rw.top;

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    ReleaseDC(NULL, hdc);

    // Icon column + left/right margins, then the title/button rows.
    *w = contentW + PX(96);
    *h = textH + PX(84);
}

static HWND CreateAnchorWindow(int x, int y, int w, int h) {
    static const wchar_t* cls = L"errerAnchor";
    static LONG registered = 0;
    if (InterlockedCompareExchange(&registered, 1, 0) == 0) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                cls, L"", WS_POPUP | WS_VISIBLE, x, y, w, h,
                                NULL, NULL, GetModuleHandle(NULL), NULL);
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    return hwnd;
}

static void ShowErrorBox(const wchar_t* title, const wchar_t* text, int mbIcon, ErrorPos pos, int offset, int absX, int absY) {
    int estW = 320, estH = 160;
    MeasureBox(text, &estW, &estH);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int step = 24;
    int x = (sw - estW) / 2, y = (sh - estH) / 2;
    switch (pos) {
        case ErrorPos::TopLeft:     x = step * offset;                   y = step * offset;                  break;
        case ErrorPos::TopRight:    x = sw - estW - step * offset;       y = step * offset;                  break;
        case ErrorPos::BottomLeft:  x = step * offset;                   y = sh - estH - step * offset;      break;
        case ErrorPos::BottomRight: x = sw - estW - step * offset;       y = sh - estH - step * offset;      break;
        case ErrorPos::Middle:      x = (sw - estW) / 2 + step * offset; y = (sh - estH) / 2 + step * offset; break;
        case ErrorPos::Random: {
            int maxX = sw - estW - 16;
            int maxY = sh - estH - 64;
            if (maxX < 4) maxX = 4;
            if (maxY < 4) maxY = 4;
            x = 8 + (rand() % maxX);
            y = 8 + (rand() % maxY);
            break;
        }
        case ErrorPos::Cascade: {
            static POINT cur = { -1, -1 };
            static int dx = 1, dy = 1, life = 0;
            static LONG spamStep = 0;
            (void)InterlockedIncrement(&spamStep);

            int maxX = sw - estW - 8;
            int maxY = sh - estH - 8;
            if (maxX < 8) maxX = 8;
            if (maxY < 8) maxY = 8;

            if (cur.x < 0) {
                switch (rand() % 4) {
                    case 0: cur.x = 8;                  cur.y = 8;                  break;
                    case 1: cur.x = maxX;               cur.y = 8;                  break;
                    case 2: cur.x = 8;                  cur.y = maxY;               break;
                    case 3: cur.x = maxX;               cur.y = maxY;               break;
                }
                dx = (cur.x > (sw - estW) / 2) ? -1 : 1;
                dy = (cur.y > (sh - estH) / 2) ? -1 : 1;
                life = 8 + (rand() % 24);
            } else {
                if (cur.x <= 8)      dx = 1;
                if (cur.x >= maxX)  dx = -1;
                if (cur.y <= 8)      dy = 1;
                if (cur.y >= maxY)  dy = -1;

                if (--life <= 0 || (rand() % 100) < 10) {
                    if (rand() % 2) dx = (rand() % 2) ? 1 : -1;
                    else            dy = (rand() % 2) ? 1 : -1;
                    life = 8 + (rand() % 24);
                }

                const int step = 40;
                cur.x += dx * step;
                cur.y += dy * step;
                if (cur.x < 8)      cur.x = 8;
                if (cur.x > maxX)  cur.x = maxX;
                if (cur.y < 8)      cur.y = 8;
                if (cur.y > maxY)  cur.y = maxY;
            }
            x = cur.x;
            y = cur.y;
            break;
        }
        case ErrorPos::Absolute:
            x = absX;
            y = absY;
            break;
    }

    HWND hAnchor = CreateAnchorWindow(x, y, estW, estH);

    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = hAnchor;
    cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_HICON_MAIN;
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = title;
    cfg.pszContent = text;
    switch (mbIcon) {
        case MB_ICONINFORMATION: cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_INFORMATION)); break;
        case MB_ICONWARNING:     cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_WARNING));     break;
        case MB_ICONERROR:       cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_ERROR));       break;
        case MB_ICONQUESTION:    cfg.hMainIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_QUESTION));    break;
        default:                 cfg.hMainIcon = NULL;                                                break;
    }

    TaskDialogIndirect(&cfg, NULL, NULL, NULL);

    DestroyWindow(hAnchor);
}

#define ERR_AUTO_DISMISS_MS 2500

struct ErrKill { DWORD tid; DWORD after; HANDLE done; };
static DWORD WINAPI ErrKillThread(LPVOID p);

static DWORD WINAPI ErrorThread(LPVOID param) {
    ThreadInfo* info = (ThreadInfo*)param;

    srand((unsigned)GetTickCount() ^ (unsigned)(uintptr_t)GetCurrentThreadId());

    int offset = 0;
    if (info->pos != ErrorPos::None) offset = AllocOffset(info->pos);

    // Every error box dies on its own after ERR_AUTO_DISMISS_MS, independent
    // of TDN_TIMER: a helper watches this thread's dialog and clicks OK once
    // the box has lived long enough (or exits early if the box is already
    // gone). 'done' is signaled so the helper never leaks on manual clicks.
    // The ErrKill is heap-allocated and OWNED solely by the helper: freeing a
    // stack object from another thread would hand ntdll a non-heap pointer
    // (RtlFreeHeap fail-fast -> 0xc0000374) whenever the helper fired, and the
    // event handle is closed exactly once, by whichever side owns it.
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ErrKill* k = new ErrKill{ GetCurrentThreadId(), ERR_AUTO_DISMISS_MS, done };
    HANDLE hKill = CreateThread(NULL, 0, ErrKillThread, k, 0, NULL);
    if (!hKill) {
        delete k;
        CloseHandle(done);
        done = NULL;
    }

    ShowErrorBox(info->title.c_str(), info->text.c_str(), info->icon, info->pos, offset, info->absX, info->absY);

    if (hKill) {
        SetEvent(done);
        WaitForSingleObject(hKill, 2000);
        CloseHandle(hKill);
    }

    if (info->pos != ErrorPos::None) FreeOffset(info->pos);

    delete info;
    return 0;
}

static int ParseIcon(const wchar_t* type) {
    if (_wcsicmp(type, L"information") == 0) return MB_ICONINFORMATION;
    if (_wcsicmp(type, L"warning") == 0)       return MB_ICONWARNING;
    if (_wcsicmp(type, L"error") == 0)          return MB_ICONERROR;
    if (_wcsicmp(type, L"question") == 0)       return MB_ICONQUESTION;
    return 0;
}

static ErrorPos ParsePos(const wchar_t* pos) {
    if (_wcsicmp(pos, L"middle") == 0)        return ErrorPos::Middle;
    if (_wcsicmp(pos, L"topleft") == 0)       return ErrorPos::TopLeft;
    if (_wcsicmp(pos, L"topright") == 0)      return ErrorPos::TopRight;
    if (_wcsicmp(pos, L"bottomleft") == 0)    return ErrorPos::BottomLeft;
    if (_wcsicmp(pos, L"bottomright") == 0)   return ErrorPos::BottomRight;
    if (_wcsicmp(pos, L"random") == 0)        return ErrorPos::Random;
    if (_wcsicmp(pos, L"cascade") == 0)       return ErrorPos::Cascade;
    return ErrorPos::None;
}

void SetErrorIcon(const wchar_t* type) {
    g_icon = ParseIcon(type);
}

void SetErrorIcon(const char* type) {
    wchar_t buf[32];
    MultiByteToWideChar(CP_UTF8, 0, type, -1, buf, 32);
    g_icon = ParseIcon(buf);
}

void PositionErrorTo(const wchar_t* pos) {
    g_position = ParsePos(pos);
}

void PositionErrorTo(const char* pos) {
    wchar_t buf[32];
    MultiByteToWideChar(CP_UTF8, 0, pos, -1, buf, 32);
    g_position = ParsePos(buf);
}

void PositionErrorToAbsolute(int x, int y) {
    g_position = ErrorPos::Absolute;
    g_absX = x;
    g_absY = y;
}

#define ERR_MAX_LIVE 43

struct ErrLive {
    HANDLE thread;
    DWORD  tid;
};

static ErrLive g_errLive[ERR_MAX_LIVE];
static int g_errLiveCount = 0;

static BOOL CALLBACK ErrFindDlgProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[16];
    if (GetClassNameW(hwnd, cls, 16) && _wcsicmp(cls, L"#32770") == 0) {
        *(HWND*)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND ErrFindDlg(DWORD tid) {
    HWND dlg = NULL;
    EnumThreadWindows(tid, ErrFindDlgProc, (LPARAM)&dlg);
    return dlg;
}

static DWORD WINAPI ErrKillThread(LPVOID p) {
    ErrKill* k = (ErrKill*)p;
    // Wait until the box has lived 'after' ms OR the owner thread already
    // closed it (event signaled) -- then click OK if a dialog is still there.
    WaitForSingleObject(k->done, k->after);
    HWND dlg = ErrFindDlg(k->tid);
    if (dlg && IsWindow(dlg)) {
        SendMessageW(dlg, TDM_CLICK_BUTTON, IDOK, 0);
    }
    CloseHandle(k->done);
    delete k;
    return 0;
}

static void ErrDismiss(int idx) {
    ErrLive& e = g_errLive[idx];
    HWND dlg = ErrFindDlg(e.tid);
    if (dlg && IsWindow(dlg)) {
        // Original method: blocking click; native task dialogs only close on a
        // synchronous TDM_CLICK_BUTTON.
        SendMessageW(dlg, TDM_CLICK_BUTTON, IDOK, 0);
    }
}

static void ErrReap() {
    int i = 0;
    while (i < g_errLiveCount) {
        ErrLive& e = g_errLive[i];
        if (WaitForSingleObject(e.thread, 0) == WAIT_OBJECT_0) {
            CloseHandle(e.thread);
            for (int j = i; j < g_errLiveCount - 1; j++) g_errLive[j] = g_errLive[j + 1];
            g_errLiveCount--;
        } else {
            i++;
        }
    }
}

static void ErrEnforceCap() {
    ErrReap();
    while (g_errLiveCount >= ERR_MAX_LIVE) {
        ErrDismiss(0);
        CloseHandle(g_errLive[0].thread);
        for (int j = 0; j < g_errLiveCount - 1; j++) g_errLive[j] = g_errLive[j + 1];
        g_errLiveCount--;
        ErrReap();
    }
}

void createError(const wchar_t* title, const wchar_t* text) {
    ErrEnforceCap();

    ThreadInfo* info = new ThreadInfo{ title, text, g_icon, g_position, g_absX, g_absY };
    HANDLE hThread = CreateThread(NULL, 0, ErrorThread, info, 0, NULL);
    if (!hThread) { delete info; return; }

    if (g_errLiveCount < ERR_MAX_LIVE) {
        g_errLive[g_errLiveCount].thread = hThread;
        g_errLive[g_errLiveCount].tid = GetThreadId(hThread);
        g_errLiveCount++;
    } else {
        CloseHandle(hThread);
    }
}

void createError(const char* title, const char* text) {
    wchar_t wTitle[256], wText[1024];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wTitle, 256);
    MultiByteToWideChar(CP_UTF8, 0, text,  -1, wText,  1024);
    createError(wTitle, wText);
}

struct ErrSpec {
    const char* icon;
    const char* pos;
    const wchar_t* title;
    const wchar_t* text;
    int delay;
};

static const ErrSpec ERRORS[] = {
    { "error",      "topright",    L"Graphics Device Removed", L"The graphics device has been physically removed, was suspended, or a driver upgrade is in progress. Status 0x00000000.", 232 },
    { "information","bottomleft",  L"Display Driver Recovered", L"The display driver nvlddmkm stopped responding and has successfully recovered. TDR event 0x00000000.", 264 },
    { "error",      "topleft",     L"PCIe Bus Error", L"A PCI Express device reported an uncorrectable error. Bus #2, Device #2, Function #3.", 208 },
    { "error",      "bottomright", L"Controller Error", L"The driver detected a controller error on Device\\Harddisk1\\DR1.", 176 },
    { "error",      "middle",      L"PCI Express Error", L"Link training failed on PCIe slot 1. The device has been removed from the bus.", 288 },
    { "error",      "topright",    L"Disk Error", L"The device Harddisk0\\DR0 has a bad block at logical block address 0x0001A20. The data could not be read.", 312 },
    { "error",      "bottomleft",  L"Disk Error", L"The file system structure on volume C: cannot be corrected. Please run the chkdsk utility.", 256 },
    { "error",      "middle",      L"Disk Error", L"An error was detected on device Harddisk0\\DR0 during a paging operation. Status 0xC000009C.", 224 },
    { "warning",    "bottomright", L"Disk Timeout", L"The device iaStor0 did not respond within the timeout period. A reset to the device was issued.", 200 },
    { "warning",    "topleft",     L"S.M.A.R.T. Failure Predicted", L"SMART failure predicted on hard disk 0. Warranty replacement recommended. Failing attribute 0x0005.", 296 },
    { "error",      "topright",    L"Live Kernel Event", L"The system has rebooted without cleanly shutting down first. Bug check code 0x0000009F.", 240 },
    { "error",      "middle",      L"Kernel-Power Event", L"The system has rebooted without cleanly shutting down first. Bug check code 0x0000009F.", 208 },
    { "error",      "bottomleft",  L"Boot Device Inaccessible", L"The system could not access the boot device. Check cables and drive health. Status 0xC0000001.", 272 },
    { "warning",    "bottomright", L"Voltage Out of Range", L"The +12V rail collapsed to 10.6V under load. The PSU should be replaced as soon as possible.", 264 },
    { "warning",    "topleft",     L"Power Supply Warning", L"The +12V rail collapsed to 10.6V under load. The PSU should be replaced as soon as possible.", 232 },
    { "error",      "topright",    L"CPU Fan Failure", L"The CPU fan has stopped spinning. The processor temperature exceeded the critical threshold at 82 degrees C.", 288 },
    { "warning",    "middle",      L"Chipset Overheat", L"The Platform Controller Hub temperature exceeded the critical threshold at 82 degrees C.", 256 },
    { "information","bottomleft",  L"CPU Configuration Changed", L"Overclocking failed! Please enter Setup to re-configure your system. Post code 0x0000004E.", 296 },
    { "warning",    "bottomright", L"Machine Check Exception", L"The processor has reported an uncorrectable error. Error code 0x00000133.", 224 },
    { "error",      "topleft",     L"WHEA Uncorrectable Error", L"The processor has reported an uncorrectable error. Error code 0x00000133.", 320 },
    { "information","topright",    L"WHEA Corrected Error", L"A corrected hardware error has been logged by Windows Hardware Error Architecture.", 192 },
    { "error",      "middle",      L"Memory Error", L"The Windows Memory Diagnostic detected a hardware problem. Contact your computer manufacturer. Error code 0xC000009F.", 336 },
    { "error",      "bottomleft",  L"ECC Error", L"A Multi Bit ECC error has been found in RAM. The memory module should be replaced.", 248 },
    { "warning",    "topleft",     L"USB Device Not Recognized", L"A USB device has exceeded the power limits of its hub port. Error 0x00000098.", 208 },
    { "warning",    "bottomright", L"Battery Failure", L"The system battery could not maintain the stored charge. The battery may need replacement.", 216 },
    { "error",      "middle",      L"CMOS Checksum Error", L"The checksum of the CMOS configuration is incorrect. The default configuration has been loaded.", 280 },
    { "error",      "topright",    L"The location is not available", L"C:\\ is not available. If the location is on this computer, make sure the device or drive is connected or the disc is inserted, and then try again.", 240 },
    { "warning",    "bottomleft",  L"File Explorer", L"Windows cannot access \\\\SERVER\\share. You might not have permission to use this network resource.", 97 },
    { "error",      "topleft",     L"explorer.exe - Application Error", L"The instruction at 0x0000000000000000 referenced memory at 0x0000000000000000. The memory could not be read.", 222 },
    { "information","bottomleft",  L"Windows", L"A required resource was not found. The operation could not be completed.", 136 },
    { "error",      "topright",    L"Setup", L"Setup was unable to complete this installation. Please run Setup again and try again.", 104 },
    { "warning",    "bottomleft",  L"File Explorer", L"You will need to provide administrator permission to copy to this folder.", 121 },
    { "information","topleft",     L"Windows", L"The system has recovered from an unexpected shutdown.", 223 },
    { "none",       "bottomleft",  L"", L"", 113 },
    { "error",      "topleft",     L"System", L"A fatal exception 0E has occurred at 0028:C0011E36 in VXD VMM(01) + 00010E36. The current application will be terminated.", 231 },
    { "warning",    "bottomright", L"File Explorer", L"The device is not ready.", 113 },
    { "error",      "bottomright", L"Application Error", L"The exception unknown software exception (0xc00000fd) occurred in the application at location 0x0000000140001A25.", 144 },
    { "information","topleft",     L"Windows", L"The operation completed successfully.", 256 },
    { "warning",    "topright",    L"System", L"Insufficient system resources exist to complete the requested service.", 136 },
    { "error",      "topright",    L"Application Error", L"The application failed to initialize properly (0xc0000142). Click on OK to terminate the application.", 288 },
    { "information","topright",    L"", L"", 104 },
    { "error",      "topleft",     L"Windows Explorer", L"Windows Explorer has stopped working. A problem caused the program to stop working correctly. Windows will close the program and notify you if a solution is available.", 135 },
    { "warning",    "topright",    L"Windows Security", L"Windows cannot verify the identity of this file. Do you want to run this software anyway?", 280 },
    { "information","bottomleft",  L"System", L"The process cannot access the file because it is being used by another process.", 97 },
    { "error",      "topright",    L"Application Error", L"The system cannot find the file specified.", 288 },
    { "none",       "topleft",     L"", L".", 96 },
    { "warning",    "bottomright", L"File Explorer", L"The parameter is incorrect.", 144 },
    { "error",      "bottomleft",  L"Error", L"The operation could not be completed. Access is denied.", 320 },
    { "information","topright",    L"Windows", L"The requested operation was unsuccessful.", 265 },
    { "warning",    "middle",      L"System", L"The page fault was in a locked region of memory.", 135 },
    { "error",      "middle",      L"Runtime Error!", L"Program: C:\\Windows\\explorer.exe  R6016 - not enough space for thread data.", 361 },
    { "information","bottomleft",  L"", L"The file or directory is corrupted and unreadable.", 263 },
    { "none",       "topleft",     L"", L"", 104 },
    { "warning",    "middle",      L"Windows", L"Windows cannot find the specified path. Make sure you typed it correctly and then try again.", 112 },
    { "error",      "middle",      L"System Error", L"A service has been terminated unexpectedly.", 208 },
    { "information","bottomleft",  L"System", L"The operation is being processed. Please wait.", 257 },
    { "error",      "middle",      L"Error", L"The requested operation could not be completed due to an error.", 296 },
    { "warning",    "bottomright", L"File Explorer", L"You need to provide administrator permission to rename this file.", 104 },
    { "information","bottomleft",  L"", L"The system cannot find the path specified.", 137 },
    { "error",      "topright",    L"Windows", L"An internal error has occurred.", 224 },
    { "none",       "bottomright", L"", L"", 111 },
    { "warning",    "topleft",     L"System", L"Windows has detected a problem with one or more device drivers.", 111 },
    { "error",      "middle",      L"DLL Host", L"A required DLL file could not be found.", 217 },
    { "information","topright",    L"Windows", L"Windows has recovered from an unexpected error.", 120 },
    { "error",      "topright",    L"Error", L"The system cannot read from the specified device.", 128 },
    { "warning",    "topright",    L"System", L"Windows cannot access the specified device, path, or file. You may not have the appropriate permissions to access the item.", 295 },
    { "error",      "topright",    L"Application Error", L"The instruction at 0x00007FFB4A23A7B0 referenced memory at 0x0000000000000000. The memory could not be written.", 304 },
    { "none",       "middle",      L"", L".", 104 },
    { "information","topleft",     L"Windows", L"Windows has recovered from a serious error.", 416 },
    { "error",      "topleft",     L"Task Scheduler", L"Task Scheduler has encountered an error. The task was not run.", 303 },
    { "warning",    "bottomleft",  L"System", L"Not enough storage is available to process this command.", 218 },
    { "error",      "bottomleft",  L"Error", L"The operation was canceled by the user.", 352 },
    { "information","middle",      L"", L"", 328 },
    { "error",      "middle",      L"System", L"Could not divide by zero. The attempted division exceeds the range of an integer value.", 264 },
    { "error",      "topleft",     L"Application Error", L"A floating point exception occurred in the application. The process has been terminated with 0x80000000.", 152 },
    { "warning",    "bottomright", L"System", L"The stack overflowed in the process csrss.exe.", 231 },
    { "error",      "middle",      L"Security Center", L"The Windows Firewall service could not be started. Error 1068: The dependency service does not exist.", 207 },
    { "information","bottomleft",  L"Windows Update", L"Some updates could not be installed. Error code 0x80070002 was encountered.", 263 },
    { "error",      "topright",    L"services.exe - Application Error", L"The instruction at 0x00007FFD2A123010 referenced memory at 0xFFFFFFFFFFFFFFFF. The memory could not be executed.", 296 },
    { "warning",    "middle",      L"System", L"The Windows library kerne132.dll could not be loaded.", 224 },
    { "error",      "bottomleft",  L"WER", L"An unexpected error occurred. This is a fatal error. The system will now restart.", 352 },
    { "information","topright",    L"Windows Defender", L"The real-time protection has been disabled by an administrator.", 128 },
    { "error",      "topleft",     L"System Error", L"Could not map the segment into memory. The virtual address space could not be allocated.", 264 },
    { "warning",    "bottomleft",  L"Device Manager", L"This device cannot find enough free resources that it can use (Code 12).", 144 },
    { "error",      "middle",      L"Critical Error", L"A fatal error occurred while reading from disk 0 with code 0x0000004A.", 312 },
    { "information","bottomright", L"Windows", L"The system will now restart to finish updating.", 181 },
    { "warning",    "middle",      L"System", L"The clock could not be synchronized. The server is not operational.", 238 },
    { "error",      "bottomright", L"Narrator", L"The screen reader could not start. Try restarting the computer.", 198 },
    { "information","topleft",     L"Windows", L"No hardware devices were found. Check that the installation media is correct.", 272 },
    { "error",      "topright",    L"Hardware Error", L"The hard disk has detected an impending failure. Back up your data now.", 328 },
    { "warning",    "bottomright", L"Windows Security", L"The Windows Task Manager has been disabled by your administrator.", 168 },
    { "error",      "middle",      L"System", L"An error occurred while attempting to decrypt files. Data has been lost.", 296 },
    { "error",      "topleft",     L"", L"Class not registered", 224 },
    { "error",      "bottomright", L"explorer.exe", L"Not implemented", 168 },
    { "error",      "middle",      L"ntoskrnl.exe", L"<window title>\r\n\r\n The application has failed to start because its side-by-side configuration is incorrect. Please see the application event log or use the command-line sxstrace.exe tool for more detail.", 312 },
    { "warning",    "bottomright", L"File Explorer", L"There is already a file with the same name in this location.", 120 },
    { "error",      "middle",      L"Setup", L"An error occurred during installation. The installation could not be completed.", 111 },
    { "information","bottomright", L"Windows", L"Windows was not properly shut down.", 232 },
    { "none",       "topleft",     L"", L"", 119 },
    { "warning",    "bottomright", L"System", L"The specified module could not be found.", 120 },
    { "error",      "topleft",     L"Error", L"The handle is invalid.", 224 },
    { "information","middle",      L"Windows", L"Windows is checking for solutions to the problem...", 105 },
    { "error",      "middle",      L"Application Error", L"The exception unknown software exception (0xc0000005) occurred in the application.", 144 },
    { "warning",    "bottomleft",  L"File Explorer", L"The item cannot be deleted because it is in use.", 239 },
    { "error",      "bottomleft",  L"System", L"The system detected an overrun of a stack-based buffer in this application.", 345 },
    { "none",       "topright",    L"", L".", 208 },
    { "information","middle",      L"Windows", L"The operation completed successfully.", 320 },
    { "error",      "topleft",     L"Error", L"The specified file was not found.", 336 },
    { "warning",    "middle",      L"System", L"The disk in the drive is not formatted. Do you want to format it now?", 128 },
    { "information","bottomright", L"", L"All connected devices are working properly.", 97 },
    { "error",      "bottomright", L"Application Error", L"The application failed to initialize properly (0xc0000005). Click on OK to terminate the application.", 271 },
    { "warning",    "middle",      L"Windows", L"Windows has detected a registry integrity problem.", 345 },
    { "error",      "bottomright", L"Error", L"Data error (cyclic redundancy check).", 264 },
    { "information","bottomleft",  L"Windows", L"All system devices are working properly.", 135 },
    { "error",      "bottomright", L"System", L"The system cannot log you on because the specified domain is not available.", 136 },
    { "none",       "topleft",     L"", L"", 137 },
    { "warning",    "bottomright", L"File Explorer", L"You'll need to provide administrator permission to delete this folder.", 160 },
    { "error",      "bottomright", L"errer", L"The program has stopped working.", 143 },
    { "warning",    "bottomright", L"DLL Host", L"A required DLL file could not be found. Reinstalling the program might fix this problem.", 121 },
    { "error",      "bottomleft",  L"svchost.exe - Application Error", L"The instruction at 0x00000000FFC40123 referenced memory at 0x00000000FFC40123. The memory could not be written.", 161 },
    { "information","topright",    L"Windows Update", L"Some updates were not installed. Check for updates again.", 232 },
    { "error",      "middle",      L"Microsoft Windows", L"Windows could not start because the following file is missing or corrupt: \\WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM.", 403 },
    { "warning",    "bottomleft",  L"System", L"The system is running low on virtual memory. Close some programs and try again.", 199 },
    { "error",      "bottomright", L"Error", L"Windows cannot access the specified device, path, or file. You may not have the appropriate permissions to access the item.", 287 },
    { "none",       "bottomright", L"", L".", 137 },
    { "information","topleft",     L"Registry Editor", L"Cannot import %1: The specified file is not a registry script. You can only import registry files.", 385 },
    { "error",      "middle",      L"System Error", L"This application has requested the Runtime to terminate it in an unusual way. Please contact the application's support team for more information.", 291 },
    { "warning",    "middle",      L"Windows Security", L"An administrator has blocked you from running this app. For more information, contact your support person.", 186 },
    { "error",      "topright",    L"explorer.exe - Application Error", L"The exception unknown software exception (0xc0000409) occurred in the application at location 0x0000000140001A25.", 297 },
    { "information","bottomleft",  L"Windows", L"The update operation completed successfully.", 96 },
    { "error",      "topleft",     L"Setup", L"Setup cannot continue because a required component is missing. Please restart the installation.", 232 },
    { "warning",    "bottomright", L"File Explorer", L"Windows cannot delete this folder because it is being used by another program or person.", 128 },
    { "none",       "middle",      L"", L"", 111 },
    { "error",      "bottomright", L"Application Error", L"The instruction at 0x00007FFC0000000 referenced memory at 0x00007FFC0000000. The memory could not be read.", 319 },
    { "information","bottomleft",  L"System", L"Windows has restarted after a critical error. The system has recovered from an unexpected shutdown.", 152 },
    { "error",      "topleft",     L"Windows", L"Stop: 0x0000007B - INACCESSIBLE_BOOT_DEVICE", 344 },
    { "warning",    "middle",      L"Error", L"Not enough quota is available to process this command.", 168 },
    { "error",      "middle",      L"Runtime Error!", L"Program: C:\\Windows\\system32\\svchost.exe  This application has requested the Runtime to terminate it in an unusual way.", 248 },
    { "warning",    "topright",    L"System", L"Windows has detected a problem with this device. The driver may be malfunctioning.", 170 },
    { "error",      "bottomright", L"Application Error", L"APPCRASH: StackHash_error - The memory could not be read.", 195 },
    { "information","topleft",     L"Windows", L"Windows has finished installing the new driver.", 108 },
    { "error",      "bottomleft",  L"System", L"An unexpected error has occurred. Error code: 0x80004005.", 164 },
    { "none",       "topright",    L"", L"", 129 },
    { "warning",    "middle",      L"System", L"This file cannot be opened because the associated program is corrupted or removed.", 218 },
    { "error",      "topleft",     L"Error", L"The file name you specified is not valid or too long.", 141 },
    { "information","bottomright", L"Windows", L"Windows was unable to complete the update. Changes will not be saved.", 252 },
    { "error",      "bottomright", L"Application Error", L"The memory could not be written. Click OK to terminate the program.", 238 },
    { "warning",    "bottomleft",  L"File Explorer", L"You do not have permission to view the contents of this folder.", 168 },
    { "error",      "middle",      L"Windows", L"An error occurred while saving the file. Check that the disk is not full.", 214 },
    { "information","topright",    L"", L"The requested operation was successful.", 96 },
    { "none",       "bottomleft",  L"", L".", 120 },
    { "error",      "topleft",     L"System Error", L"The system cannot find the drive specified.", 239 },
    { "warning",    "bottomright", L"Security", L"The network path was not found. Check the spelling of the name, or contact your network administrator.", 277 },
    { "error",      "middle",      L"Error", L"An exception occurred in the application. Details: \\Device\\HarddiskVolume2\\Windows\\system32\\ntdll.dll.", 310 },
    { "information","topleft",     L"Windows", L"Your system is low on memory. Close some programs and try again.", 203 },
    { "error",      "bottomleft",  L"explorer.exe - Application Error", L"The application failed to initialize properly (0xc0000135). Click on OK to terminate the application.", 224 },
    { "warning",    "topright",    L"Windows", L"Windows has detected a problem with one or more device drivers. Learn more about this issue.", 174 },
    { "error",      "middle",      L"Disc Utility", L"The disc is not formatted. Do you want to format the disc now?", 207 },
    { "none",       "middle",      L"", L"", 104 },
    { "error",      "bottomright", L"Error", L"Insufficient memory to continue execution of the program.", 254 },
    { "information","bottomleft",  L"Windows", L"Windows Explorer has stopped working. Windows is attempting to find a solution...", 137 },
    { "error",      "topleft",     L"Application Error", L"The exception unknown software exception (0xc0000417) occurred in the application at location 0x0000000000040000.", 301 },
    { "warning",    "middle",      L"System", L"The requested device is not available.", 149 },
    { "error",      "bottomright", L"Error", L"Operation failed. The disk is write-protected.", 187 },
    { "information","topright",    L"Windows", L"The operation is being processed. Please wait.", 256 },
    { "error",      "bottomleft",  L"System Error", L"STOP: c000021a {Fatal System Error} The session manager initialization system process terminated unexpectedly.", 391 },
    { "warning",    "topleft",     L"File Explorer", L"Windows cannot access this folder. The name may contain characters that are not supported.", 191 },
    { "error",      "middle",      L"", L"Class not registered", 226 },
    { "none",       "bottomleft",  L"", L"", 120 },
    { "information","bottomright", L"Windows", L"All Windows services are working properly.", 95 },
    { "error",      "topleft",     L"Runtime Error!", L"Program: C:\\Windows\\explorer.exe  abnormal program termination.", 245 },
    { "warning",    "bottomright", L"System", L"Windows cannot connect to the printer. The specified printer is not available.", 264 },
    { "error",      "middle",      L"Error", L"Access to the specified device, path, or file is denied.", 158 },
    { "error",      "bottomleft",  L"System", L"Generic host process for Win32 Services encountered a problem and had to close.", 349 },
    { "information","topleft",     L"Windows", L"Windows has detected that a new update is ready to install.", 130 },
    { "error",      "bottomright", L"Error", L"The system cannot open the device or file specified.", 241 },
    { "warning",    "middle",      L"System Error", L"An I/O error occurred while accessing the system drive.", 207 },
    { "error",      "topleft",     L"Application Error", L"The memory could not be read. Click on OK to terminate the program.", 198 },
    { "none",       "topright",    L"", L".", 121 },
    { "information","bottomright", L"Windows", L"Windows has successfully installed the new device driver.", 108 },
    { "error",      "middle",      L"Cmd.exe", L"Windows cannot find 'C:\\Windows\\system32\\cmd.exe'. Make sure you typed the name correctly.", 289 },
    { "warning",    "bottomleft",  L"System", L"The Windows logon process has terminated unexpectedly.", 226 },
    { "error",      "bottomright", L"Error", L"The handle specified is invalid.", 159 },
    { "information","topleft",     L"Windows", L"Windows has recovered from a serious error. A problem has been detected and Windows has been shut down.", 391 },
    { "error",      "middle",      L"System", L"Windows cannot load the device driver for this hardware. The driver may be corrupted or missing.", 236 },
    { "warning",    "bottomright", L"File Explorer", L"This folder is no longer available. The item might have been moved to another location.", 146 },
    { "error",      "topleft",     L"Error", L"Invalid data has been passed to a system call.", 175 },
    { "none",       "bottomleft",  L"", L"", 111 },
    { "information","bottomright", L"Windows", L"The operation was canceled by the user.", 128 },
    { "error",      "bottomright", L"Application Error", L"0xc0000005: Access Violation. The program attempted to access memory at an invalid address.", 263 },
    { "warning",    "middle",      L"System", L"The disk space is low. Free up space on the drive to continue.", 169 },
    { "error",      "topleft",     L"Error", L"An error occurred during operation: The parameter is incorrect.", 213 },
    { "information","middle",      L"Windows", L"Windows has found a solution to a problem. The system will be restored.", 318 },
    { "error",      "middle",      L"Multi Bit ECC Error", L"A Multi Bit ECC error has been found in RAM please shutdown immediately", 360 },
    { "question",   "middle",      L"IExpress Wizard", L"Are you sure you want to quit the IExpress Wizard?", 300 },
    { "information","middle",      L"sihost.exe", L"Unknown hard error", 300 },
};

static const int kErrSpecCount = (int)(sizeof(ERRORS) / sizeof(ERRORS[0]));

void SpawnError() {
    const ErrSpec& e = ERRORS[rand() % kErrSpecCount];
    SetErrorIcon(e.icon);
    PositionErrorTo(e.pos);
    createError(e.title, e.text);
}

#define ERRER_ICON_ID 111

#define COLS 27
#define ROWS 30
#define APPLE_PX 18
#define LATE_MS 100
#define WAIT_MS 100
static const COLORREF APPLE_COL_KEY = RGB(255, 0, 255);

struct ApplePixel {
    int color;
    float startX, startY;
    float endX, endY;
    float delay;
    float duration;
    bool revealed;
    bool is_colored;
};

static float AppleEase(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f;
}

static HICON g_preloadIcon = NULL;

static HICON LoadIconFromDisk() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return NULL;
    wchar_t* sl = wcsrchr(exe, L'\\');
    if (sl) *sl = L'\0';

    wchar_t path[MAX_PATH * 2];
    wsprintfW(path, L"%s\\Resources\\Icon.ico", exe);
    HICON hIcon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTCOLOR);
    if (!hIcon) {
        wsprintfW(path, L"%s\\..\\Resources\\Icon.ico", exe);
        hIcon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTCOLOR);
    }
    if (!hIcon)
        hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(ERRER_ICON_ID), IMAGE_ICON, 96, 96, LR_DEFAULTCOLOR);
    return hIcon;
}

void PreloadIcon() {
    if (!g_preloadIcon) g_preloadIcon = LoadIconFromDisk();
}

static int LoadIconPixels(std::vector<ApplePixel>& pixels, int sw, int sh) {
    bool owned = false;
    HICON hIcon = g_preloadIcon;
    if (!hIcon) { hIcon = LoadIconFromDisk(); owned = true; }
    if (!hIcon) return 0;

    const int w = COLS, h = ROWS;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC ref = GetDC(NULL);
    unsigned char* bits = NULL;
    HBITMAP dib = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HDC mem = dib ? CreateCompatibleDC(ref) : NULL;
    HGDIOBJ old = mem ? SelectObject(mem, dib) : NULL;
    BOOL drawn = FALSE;
    if (mem && bits) {
        memset(bits, 0, (size_t)w * h * 4);
        drawn = DrawIconEx(mem, 0, 0, hIcon, w, h, 0, NULL, DI_NORMAL);
    }

    srand((unsigned)GetTickCount() ^ (unsigned)(uintptr_t)GetCurrentThreadId());

    float appleW = (float)(COLS * APPLE_PX);
    float appleH = (float)(ROWS * APPLE_PX);
    float ox = (sw - appleW) / 2.0f;
    float oy = (sh - appleH) / 2.0f;

    if (drawn && bits) {
        bool alphaOk = false;
        for (int i = 0; i < w * h; i++)
            if (bits[i * 4 + 3] != 255) { alphaOk = true; break; }

        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                const unsigned char* px = bits + ((size_t)r * w + c) * 4;
                unsigned int b = px[0], g = px[1], rr = px[2], a = px[3];
                bool visible = alphaOk ? (a > 30) : ((int)rr + g + b >= 24);
                if (!visible) continue;
                DWORD col = (DWORD)((b << 16) | (g << 8) | rr);

                ApplePixel p = {};
                p.color = col;
                p.startX = (float)(rand() % sw);
                p.startY = (float)(rand() % sh);
                p.endX = ox + c * APPLE_PX + APPLE_PX / 2.0f;
                p.endY = oy + r * APPLE_PX + APPLE_PX / 2.0f;
                p.delay = WAIT_MS / 1000.0f + (float)(rand() % LATE_MS) / 1000.0f;
                p.duration = 0.8f + (float)(rand() % 1400) / 1000.0f;
                p.revealed = false;
                p.is_colored = ((rr * 299 + g * 587 + b * 114) / 1000) >= 220;
                pixels.push_back(p);
            }
        }
    }

    if (old) SelectObject(mem, old);
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (ref) ReleaseDC(NULL, ref);
    if (owned) DestroyIcon(hIcon);
    return (int)pixels.size();
}

static DWORD WINAPI AppleThread(LPVOID param) {
    (void)param;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    std::vector<ApplePixel> pixels;
    if (LoadIconPixels(pixels, sw, sh) <= 0) return 0;

    int appleW = COLS * APPLE_PX;
    int appleH = ROWS * APPLE_PX;
    float ox = (sw - appleW) / 2.0f;
    float oy = (sh - appleH) / 2.0f;

    std::map<DWORD, HBRUSH> brushes;
    auto BrushFor = [&](DWORD color) -> HBRUSH {
        std::map<DWORD, HBRUSH>::iterator it = brushes.find(color);
        if (it != brushes.end()) return it->second;
        HBRUSH b = CreateSolidBrush((COLORREF)color);
        brushes[color] = b;
        return b;
    };

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "AppleAnim";
    RegisterClassExA(&wc);

    // No WS_EX_TOPMOST: a full-screen topmost window can end up covering the
    // toast area. The error dialogs are topmost and float above this anyway.
    HWND hWnd = CreateWindowExA(
        WS_EX_LAYERED,
        "AppleAnim", NULL, WS_POPUP,
        0, 0, sw, sh,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    SetLayeredWindowAttributes(hWnd, APPLE_COL_KEY, 0, LWA_COLORKEY);
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    HDC memDC = NULL;
    HBITMAP memBmp = NULL;
    {
        HDC hdc = GetDC(hWnd);
        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, sw, sh);
        SelectObject(memDC, memBmp);
        ReleaseDC(hWnd, hdc);
    }

    HBRUSH hKeyBrush = CreateSolidBrush(APPLE_COL_KEY);

    auto RenderFrame = [&](bool showAll, float fallTime) {
        RECT rc = { 0, 0, sw, sh };
        FillRect(memDC, &rc, hKeyBrush);

        for (size_t i = 0; i < pixels.size(); i++) {
            ApplePixel& p = pixels[i];
            if (!showAll && !p.revealed && !p.is_colored) continue;

            float cx = p.endX;
            float cy = p.endY;

            if (fallTime >= 0 && p.revealed) {
                cy = p.endY + fallTime * fallTime * 300.0f;
            }

            if (cy > sh + APPLE_PX) continue;

            RECT rc2 = { (int)(cx - APPLE_PX/2), (int)(cy - APPLE_PX/2),
                         (int)(cx - APPLE_PX/2) + APPLE_PX, (int)(cy - APPLE_PX/2) + APPLE_PX };
            FillRect(memDC, &rc2, BrushFor((DWORD)p.color));
        }

        HDC hdc = GetDC(hWnd);
        BitBlt(hdc, 0, 0, sw, sh, memDC, 0, 0, SRCCOPY);
        ReleaseDC(hWnd, hdc);
    };

    MSG msg;
    DWORD start = GetTickCount();

    bool running = true;
    auto PumpMessages = [&]() {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    };

    while (running) {
        PumpMessages();
        if (!running) break;

        float elapsed = (float)(GetTickCount() - start) / 1000.0f;
        bool allDone = true;

        RECT rc = { 0, 0, sw, sh };
        FillRect(memDC, &rc, hKeyBrush);

        for (size_t i = 0; i < pixels.size(); i++) {
            ApplePixel& p = pixels[i];
            float t = (elapsed - p.delay) / p.duration;
            if (t < 0) { t = 0; allDone = false; }
            if (t > 1) t = 1; else allDone = false;
            float et = AppleEase(t);
            et = (int)(et * 3) / 3.0f;

            float cx = p.startX + (p.endX - p.startX) * et;
            float cy = p.startY + (p.endY - p.startY) * et;

            RECT rc2 = { (int)(cx - APPLE_PX/2), (int)(cy - APPLE_PX/2),
                         (int)(cx - APPLE_PX/2) + APPLE_PX, (int)(cy - APPLE_PX/2) + APPLE_PX };
            FillRect(memDC, &rc2, BrushFor((DWORD)p.color));
        }

        HDC hdc = GetDC(hWnd);
        BitBlt(hdc, 0, 0, sw, sh, memDC, 0, 0, SRCCOPY);
        ReleaseDC(hWnd, hdc);

        if (allDone) break;
        Sleep(16);
    }

    Sleep(2000);

    float revealThreshold = APPLE_PX * 3.0f;
    for (int iter = 0; iter < 21; iter++) {
        if (!running) break;
        PumpMessages();
        if (!running) break;

        for (size_t i = 0; i < pixels.size(); i++) {
            ApplePixel& p = pixels[i];
            if (p.revealed || p.is_colored) continue;
            for (size_t j = 0; j < pixels.size(); j++) {
                ApplePixel& q = pixels[j];
                if (!q.is_colored && !q.revealed) continue;
                float dx = p.endX - q.endX;
                float dy = p.endY - q.endY;
                if (dx * dx + dy * dy < revealThreshold * revealThreshold) {
                    p.revealed = true;
                    break;
                }
            }
        }
        RenderFrame(false, -1.0f);
        Sleep(240);
    }

    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i].revealed = true;
    }
    RenderFrame(false, -1.0f);

    Sleep(700);

    float fallStart = (float)GetTickCount() / 1000.0f;
    while (running) {
        PumpMessages();
        if (!running) break;

        float ft = (float)GetTickCount() / 1000.0f - fallStart;
        bool allOff = true;
        for (size_t i = 0; i < pixels.size(); i++) {
            if (pixels[i].endY + ft * ft * 300.0f < sh + APPLE_PX)
                allOff = false;
        }
        if (allOff) break;

        RenderFrame(false, ft);
        Sleep(16);
    }

    for (std::map<DWORD, HBRUSH>::iterator it = brushes.begin(); it != brushes.end(); ++it)
        DeleteObject(it->second);
    DeleteObject(hKeyBrush);
    SelectObject(memDC, (HBITMAP)0);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    DestroyWindow(hWnd);
    return 0;
}

static const int P2_DELAYS[85] = {
    183,   112,   208,   112,   129,
     96,   208,   129,   240,   104,
    136,    95,   225,   143,   345,
     92,    82,   264,   112,   113,
    176,   111,    97,   184,   105,
    215,   127,   112,   137,   215,
    112,   209,   128,   136,   295,
    128,   233,   127,    97,   215,
     80,    97,   200,   127,    80,
    232,   104,   192,   137,   119,
    129,   207,   112,   224,   136,
    173,   166,   261,   248,   160,
    136,   240,   112,    88,   193,
    119,    88,   192,   105,   151,
    153,   168,   280,   111,   256,
    104,   161,   408,   152,   232,
    144,   151,   152,   144,   153
};

static const DWORD P2_TOAST_DELAYS[] = {
    817,   848,   881,   856,   856,
    840,   901,   860,   895,   856,
    888,   888,   840,   848,   864,
    873
};

// Explorer window: navigates these paths on this schedule.
static const int P2_NAV_DELAYS[] = {
    432,   440,   417,   392,   456,
    425,   464,   416,   425,   449,
    447,   392,   425,   432,   488,
    377,   248,   209,   464,   432,
    401,   456,   440,   441,   408,
    240,   224,   440,   432,   417,
    423,   473,   416,   185,   216,
    200,   136,   152
};

// Fake "Delete File" boxes on the payload 2 explorer timeline.
static const DWORD P2_DELETE_DELAYS[] = {
    100,   13069,  153,   168,   176,
    144,   144
};

static const wchar_t* P2_NAV_PATHS[] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\Temp",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\Users\\Public",
    L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    L"::{645FF040-5081-101B-9F08-00AA002F954E}",
    L"::{374DE290-123F-4565-9164-39C4925E467B}"
};

static void* g_p2Explorer = NULL;
static volatile LONG g_p2ExplorerStop = 0;

static DWORD WINAPI P2ExplorerThread(LPVOID param) {
    (void)param;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;

    const int navCount = (int)(sizeof(P2_NAV_DELAYS) / sizeof(P2_NAV_DELAYS[0]));
    const int pathCount = (int)(sizeof(P2_NAV_PATHS) / sizeof(P2_NAV_PATHS[0]));

    HWND hwnd = CreateFakeExplorer(L"C:\\", 80, 80, 540, 320);
    g_p2Explorer = (void*)hwnd;
    if (!hwnd) { CoUninitialize(); return 0; }

    for (int i = 0; i < navCount && !g_p2ExplorerStop; i++) {
        if (!IsWindow(hwnd)) break;
        NavigateFakeExplorer(hwnd, P2_NAV_PATHS[i % pathCount]);
        PumpMessagesFor((DWORD)P2_NAV_DELAYS[i]);
    }

    // Keep the explorer alive across payload transitions (P2 -> P3). Do not
    // destroy the window here; it carries over into the next payload.
    g_p2Explorer = (void*)hwnd;
    CoUninitialize();
    return 0;
}

void runPayload2() {
    if (SongSyncWaitHostFlag()) SongSyncWaitHost();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PreloadIcon();

    // Parse the explorer's nav paths BEFORE the clock wait. It is slow (the
    // virtual folders spin up shell extensions) and SongSyncWait is an absolute
    // deadline, so doing it here is free; after the wait it would push the
    // whole payload and its delete-box timeline late.
    FakeExplorerPrimePaths(P2_NAV_PATHS,
                           (int)(sizeof(P2_NAV_PATHS) / sizeof(P2_NAV_PATHS[0])));

    bool anchored = false;
    DWORD base = SongSyncBase(15572, &anchored);
    SongSyncWait(base);
    DWORD t0 = base ? base : 0;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HANDLE hFx = CreateThread(NULL, 0, AppleThread, NULL, 0, NULL);
    CascadeStart();

    HANDLE hExp = CreateThread(NULL, 0, P2ExplorerThread, NULL, 0, NULL);

    srand((unsigned)GetTickCount() ^ 0x2A5E);

    const int errorCount = (int)(sizeof(ERRORS) / sizeof(ERRORS[0]));
    const int delayCount = (int)(sizeof(P2_DELAYS) / sizeof(P2_DELAYS[0]));
    bool haveClock = SongSyncAvailable();
    StartToastTimeline(P2_TOAST_DELAYS,
                       (int)(sizeof(P2_TOAST_DELAYS) / sizeof(P2_TOAST_DELAYS[0])));

    // The explorer owns the delete boxes; they open once the first navigation
    // lands (13041 ms) and then every gap after that.
    SayonaraDeleteScheduleFrom(P2_DELETE_DELAYS,
                           (int)(sizeof(P2_DELETE_DELAYS) / sizeof(P2_DELETE_DELAYS[0])),
                           base ? base : GetTickCount());

    DWORD acc = 0;
    for (int i = 0; i < delayCount; i++) {
        if (haveClock) {
            SongSyncWaitBeat(15572 + acc);
        } else if (t0) {
            DWORD target = t0 + acc;
            while ((LONG)(GetTickCount() - target) < 0) Sleep(5);
        } else if (i > 0) {
            Sleep((DWORD)P2_DELAYS[i - 1]);
        }
        const ErrSpec& e = ERRORS[rand() % errorCount];
        SetErrorIcon(e.icon);
        PositionErrorTo(e.pos);
        createError(e.title, e.text);
        if (i + 1 < delayCount) acc += (DWORD)P2_DELAYS[i];
    }

    InterlockedExchange(&g_p2ExplorerStop, 1);
    if (hExp) {
        WaitForSingleObject(hExp, 3000);
        CloseHandle(hExp);
    }
    if (hFx) CloseHandle(hFx);
    ToastShutdown();
    SayonaraDeleteCloseAll();
    CrashLog("end");
    TerminateProcess(GetCurrentProcess(), 0);
}
