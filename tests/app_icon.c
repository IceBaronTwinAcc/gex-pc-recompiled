#include <stdio.h>
#include "platform/desktop/app_icon.h"
#include "app_icon_data.h"

int main(void) {
    SDL_RWops *input = SDL_RWFromConstMem(gex_app_icon_bmp, sizeof gex_app_icon_bmp);
    SDL_Surface *icon = input ? SDL_LoadBMP_RW(input, 1) : NULL;
    if (!icon || icon->w != 128 || icon->h != 128 || icon->format->BytesPerPixel != 4) {
        fprintf(stderr, "application icon bitmap: %s\n", SDL_GetError());
        SDL_FreeSurface(icon);
        return 1;
    }
    unsigned transparent = 0, orange = 0;
    for (int y = 0; y < icon->h; ++y)
        for (int x = 0; x < icon->w; ++x) {
            Uint32 pixel;
            SDL_memcpy(&pixel, (Uint8 *)icon->pixels + y * icon->pitch + x * 4, sizeof pixel);
            Uint8 r, g, b, a;
            SDL_GetRGBA(pixel, icon->format, &r, &g, &b, &a);
            if (!a) ++transparent;
            if (a > 200 && r > 200 && g > 50 && b < 60) ++orange;
        }
    SDL_FreeSurface(icon);
    if (transparent < 1000 || orange < 1000) {
        fprintf(stderr, "application icon lost transparency or artwork\n");
        return 1;
    }
    if (gex_app_video_init()) {
        fprintf(stderr, "icon test video: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = gex_app_window("Icon test", 0, 0, 128, 128, SDL_WINDOW_HIDDEN);
    int failed = !window ||
        SDL_strcmp(SDL_getenv("SDL_VIDEO_X11_WMCLASS"), "org.gexrecomp.app") ||
        SDL_strcmp(SDL_getenv("SDL_VIDEO_WAYLAND_WMCLASS"), "org.gexrecomp.app");
    if (failed) fprintf(stderr, "application icon window: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failed;
}
