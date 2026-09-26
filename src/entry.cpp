#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <string.h>

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
        "  errer.exe --payload<N> [args]     run only payload N (1..7)\n"
        "\n"
        "Args:\n"
        "  --help, -h        print this help and exit\n"
        "  --nobsod          don't BSOD at the end (payload 7 just exits)\n"
        "  --nodatechange    don't change the system short-date format\n"
        "  --nogdieffect     disable the red/cyan chromatic wobble effect\n";

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
            } else if (_wcsicmp(argv[i], L"--nogdieffect") == 0) {
                SetEnvironmentVariableW(L"ERRER_NOGDI", L"1");
            }
        }
    }
    if (sawHelp) {
        PrintHelp();
        if (argv) LocalFree(argv);
        return 0;
    }

    const wchar_t* arg = (argc > 1 && argv) ? argv[1] : NULL;

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
    } else {
        runAll();
    }

    if (argv) LocalFree(argv);
    return 0;
}