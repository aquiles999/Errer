#include <windows.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload8();

int main() {
    FreeConsole();
    runPayload8();
    return 0;
}