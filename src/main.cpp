#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/Application.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    // Some Linux hosts expose USB devices that HIDAPI cannot open without
    // elevated permissions, causing the libusb fallback enumeration to stall
    // for many seconds at startup. The hidraw backend covers standard
    // desktop gamepads, so keep the libusb path off for snappy launches.
    SDL_SetHint("SDL_HIDAPI_LIBUSB", "0");
    ww::Application app;
    if (!app.init())
        return 1;
    const int rc = app.run();
    app.shutdown();
    return rc;
}
