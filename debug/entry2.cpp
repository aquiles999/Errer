#include <windows.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload2();

int main() {
    FreeConsole();
    runPayload2();
    return 0;
}