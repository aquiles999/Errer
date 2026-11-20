#include <windows.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload3();

int main() {
    FreeConsole();
    runPayload3();
    return 0;
}