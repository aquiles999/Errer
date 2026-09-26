#include <windows.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

extern void runPayload1();

int main() {
    FreeConsole();
    runPayload1();
    return 0;
}