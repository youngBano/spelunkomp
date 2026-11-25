// Standalone pbkit video presentation smoke test for Original Xbox (NV2A).
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

int main(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    int status = pb_init();
    if (status != 0) {
        debugPrint("pb_init failed: %d\n", status);
        Sleep(2000);
        return 1;
    }

    pb_show_front_screen();

    while (true) {
        pb_wait_for_vbl();
        pb_reset();
        pb_fill(0, 0, 640, 480, 0xFF102040);
        pb_erase_depth_stencil_buffer(0, 0, 640, 480);
        while (pb_busy()) {
        }
        while (pb_finished()) {
        }
    }

    pb_kill();
    return 0;
}
