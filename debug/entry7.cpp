#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload7();

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--nobsod") == 0) SetEnvironmentVariableW(L"ERRER_NOBSOD", L"1");
    }
    if (argv) LocalFree(argv);

    FreeConsole();
    runPayload7();
    return 0;
}