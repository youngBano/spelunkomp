// First Xbox bootstrap for the spelunkomp-xbox port.
// This intentionally does not link SDL3 or game code yet.
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

int main(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    unsigned int frame = 0;
    while (true) {
        debugPrint("spelunkomp-xbox bootstrap: frame %u\n", frame++);
        Sleep(2000);
    }

    return 0;
}
