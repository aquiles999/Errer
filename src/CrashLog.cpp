#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "CrashLog.h"
#include "Flags.h"

static const char* g_who = "";
static CRITICAL_SECTION g_lock;

// All logging is off unless --debug was passed. The environment variable is
// inherited by every payload child process, so one flag covers the show.
static bool DebugOn() {
    static int on = -1;
    if (on < 0) on = DebugFlag() ? 1 : 0;
    return on != 0;
}

static void LogLine(const char* fmt, ...) {
    if (!DebugOn()) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, MAX_PATH, L"errer_crash.log");

    EnterCriticalSection(&g_lock);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    PEXCEPTION_RECORD er = ep ? ep->ExceptionRecord : NULL;
    DWORD code = er ? er->ExceptionCode : 0;
    void* addr = er ? er->ExceptionAddress : NULL;
    // Silent death (WER dialogs just delay an already-gone process), but the
    // log pins down exactly which payload faulted and where.
    CrashLogf("EXCEPTION code=0x%08X addr=%p thread=%lu", code, addr, GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashLog(const char* who) {
    InitializeCriticalSection(&g_lock);
    g_who = who;
    SetUnhandledExceptionFilter(CrashFilter);
}

void CrashLog(const char* what) {
    LogLine("[%s] %s", g_who, what);
}

void CrashLogf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogLine("[%s] %s", g_who, buf);
}