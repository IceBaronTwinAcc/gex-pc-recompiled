#include "platform/desktop/app_icon.h"
#include "app_icon_data.h"

int gex_app_video_init(void) {
    if (SDL_setenv("SDL_VIDEO_X11_WMCLASS", "org.gexrecomp.app", 0) ||
        SDL_setenv("SDL_VIDEO_WAYLAND_WMCLASS", "org.gexrecomp.app", 0))
        return SDL_SetError("cannot set application window identity");
    return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

SDL_Window *gex_app_window(const char *title, int x, int y, int width, int height, Uint32 flags) {
    SDL_RWops *input = SDL_RWFromConstMem(gex_app_icon_bmp, sizeof gex_app_icon_bmp);
    if (!input) return NULL;
    SDL_Surface *icon = SDL_LoadBMP_RW(input, 1);
    if (!icon) return NULL;
    SDL_Window *window = SDL_CreateWindow(title, x, y, width, height, flags);
    if (window) SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
    return window;
}
