#include <SDL3/SDL_main.h>

#include "app/Application.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    ww::Application app;
    if (!app.init())
        return 1;
    const int rc = app.run();
    app.shutdown();
    return rc;
}
