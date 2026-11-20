#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include "CrashLog.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runAll();
extern void runPayload1();
extern void runPayload2();
extern void runPayload3();
extern void runPayload4();
extern void runPayload5();
extern void runPayload6();
extern void runPayload7();
extern void runPayload8();
extern void SetChildMode();

static void PrintHelp() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
    }

    HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;

    const char* help =
        "errer - Bad Apple error spam\n"
        "\n"
        "Usage:\n"
        "  errer.exe [args]                  run the full show\n"
        "  errer.exe --payload<N> [args]     run only payload N (1..8) in this process\n"
        "  errer.exe --payload <N> [args]    spawn errer.exe --payloadN as a separate process\n"
        "\n"
        "Args:\n"
        "  --help, -h        print this help and exit\n"
        "  --nobsod          don't BSOD at the end (payload 7 just exits)\n"
        "  --nodatechange    don't change the system short-date format\n"
        "  --nogdieffect     disable the red/cyan chromatic wobble effect\n"
        "  --notoasts        disable the error toast/balloon spam\n"
        "  --debug            log the payload/toast diagnostics to %%TEMP%%\\errer_crash.log\n"
        "  --endermanch       plays the 2989 soundtrack by Enderman (not guaranteed to sync and i am not fixing sync issues with --endermanch)\n"
        "  --original         plays the original Bad apple!! song\n"
        "  --nosong, --nomusic run the show silently (no audio track)\n"
"  --changewallpaper  play both wallpaper animation segments (mid-show\n"
"                     and ending), changing the desktop wallpaper\n"
"  --newexplorer      use the async IShellView fake explorer backend, which\n"
"                      keeps moving smoothly while a folder is loading\n";

    DWORD written = 0;
    WriteFile(hOut, help, (DWORD)strlen(help), &written, NULL);

    CONSOLE_SCREEN_BUFFER_INFO cbi;
    if (GetConsoleScreenBufferInfo(hOut, &cbi)) {
        COORD pos = cbi.dwCursorPosition;
        pos.X = 0;
        if (pos.Y < 0) pos.Y = 0;
        if (pos.Y < cbi.srWindow.Bottom) pos.Y = cbi.srWindow.Bottom;
        pos.Y++;
        SetConsoleCursorPosition(hOut, pos);
    }

    CONSOLE_CURSOR_INFO ci = { 25, TRUE };
    SetConsoleCursorInfo(hOut, &ci);
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    bool sawHelp = false;
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
                sawHelp = true;
            } else if (_wcsicmp(argv[i], L"--nobsod") == 0) {
                SetEnvironmentVariableW(L"ERRER_NOBSOD", L"1");
            } else if (_wcsicmp(argv[i], L"--nodatechange") == 0) {
                SetEnvironmentVariableW(L"ERRER_NODATECHANGE", L"1");
            } else if (_wcsicmp(argv[i], L"--wait") == 0) {
                SetEnvironmentVariableW(L"ERRER_WAITCLOCK", L"1");
            } else if (_wcsicmp(argv[i], L"--nogdieffect") == 0) {
                SetEnvironmentVariableW(L"ERRER_NOGDI", L"1");
            } else if (_wcsicmp(argv[i], L"--notoasts") == 0) {
                SetEnvironmentVariableW(L"ERRER_NOTOASTS", L"1");
            } else if (_wcsicmp(argv[i], L"--debug") == 0) {
                SetEnvironmentVariableW(L"ERRER_DEBUG", L"1");
            } else if (_wcsicmp(argv[i], L"--endermanch") == 0) {
                SetEnvironmentVariableW(L"ERRER_ENDERMANCH", L"1");
            } else if (_wcsicmp(argv[i], L"--original") == 0) {
                SetEnvironmentVariableW(L"ERRER_ORIGINAL", L"1");
            } else if (_wcsicmp(argv[i], L"--nosong") == 0 ||
                       _wcsicmp(argv[i], L"--nomusic") == 0) {
                SetEnvironmentVariableW(L"ERRER_NOSONG", L"1");
            } else if (_wcsicmp(argv[i], L"--changewallpaper") == 0) {
                SetEnvironmentVariableW(L"ERRER_CHANGEWALLPAPER", L"1");
            } else if (_wcsicmp(argv[i], L"--newexplorer") == 0) {
                SetEnvironmentVariableW(L"ERRER_NEWEXPLORER", L"1");
            }
        }
    }
    if (sawHelp) {
        PrintHelp();
        if (argv) LocalFree(argv);
        return 0;
    }

    const wchar_t* arg = (argc > 1 && argv) ? argv[1] : NULL;

    char who[24];
    if (arg && _wcsnicmp(arg, L"--payload", 9) == 0) {
        snprintf(who, sizeof who, "p%c", (char)(arg[9] ? arg[9] : L'0'));
    } else {
        strcpy_s(who, sizeof who, "all");
    }
    InstallCrashLog(who);
    CrashLog("start");
    CrashLogf("dispatch arg0=%ls argc=%d", arg ? arg : L"(null)", argc);

    // Scan all args for --payload <N> (with space) -> spawn separate process
    // Also check --payloadN (no space) -> run in-process
    for (int i = 1; i < argc && argv; i++) {
        if (_wcsicmp(argv[i], L"--payload") == 0 && i + 1 < argc && argv[i + 1]) {
            int n = _wtoi(argv[i + 1]);
            CrashLogf("found --payload %d at argv[%d]", n, i);
            if (n >= 1 && n <= 8) {
                wchar_t exePath[MAX_PATH];
                DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
                CrashLogf("GetModuleFileName len=%lu", len);
                if (len > 0 && len < MAX_PATH) {
                    wchar_t cmdLine[1024];
                    int written = wsprintfW(cmdLine, L"\"%s\" --payload%d", exePath, n);
                    CrashLogf("cmdLine built: %ls written=%d", cmdLine, written);
                    // Pass through any additional args after the number
                    for (int j = i + 2; j < argc && written > 0; j++) {
                        size_t clen = wcslen(cmdLine);
                        size_t needed = clen + 1 + wcslen(argv[j]) + 1;
                        if (needed < sizeof(cmdLine) / sizeof(wchar_t)) {
                            cmdLine[clen] = L' ';
                            cmdLine[clen + 1] = L'\0';
                            wcscat_s(cmdLine, argv[j]);
                        }
                    }
                    CrashLogf("spawning --payload%d: %ls", n, cmdLine);
                    STARTUPINFOW si = {};
                    si.cb = sizeof(si);
                    PROCESS_INFORMATION pi = {};
                    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        WaitForSingleObject(pi.hProcess, INFINITE);
                        DWORD exitCode = 0;
                        GetExitCodeProcess(pi.hProcess, &exitCode);
                        CloseHandle(pi.hProcess);
                        CrashLogf("spawned --payload%d exited with %lu", n, exitCode);
                        if (argv) LocalFree(argv);
                        return (int)exitCode;
                    } else {
                        CrashLogf("CreateProcessW failed for --payload%d: %lu", n, GetLastError());
                    }
                }
            }
            // If spawn fails or invalid N, fall through to in-process handling below
        }
    }

    // In-process payload dispatch (--payload1 through --payload8)
    if (arg && _wcsicmp(arg, L"--payload1") == 0) {
        SetChildMode();
        runPayload1();
    } else if (arg && _wcsicmp(arg, L"--payload2") == 0) {
        SetChildMode();
        runPayload2();
    } else if (arg && _wcsicmp(arg, L"--payload3") == 0) {
        SetChildMode();
        runPayload3();
    } else if (arg && _wcsicmp(arg, L"--payload4") == 0) {
        SetChildMode();
        runPayload4();
    } else if (arg && _wcsicmp(arg, L"--payload5") == 0) {
        SetChildMode();
        runPayload5();
    } else if (arg && _wcsicmp(arg, L"--payload6") == 0) {
        SetChildMode();
        runPayload6();
    } else if (arg && _wcsicmp(arg, L"--payload7") == 0) {
        SetChildMode();
        runPayload7();
    } else if (arg && _wcsicmp(arg, L"--payload8") == 0) {
        SetChildMode();
        runPayload8();
    } else {
        runAll();
    }

    if (argv) LocalFree(argv);
    return 0;
}