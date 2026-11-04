#ifndef GEX_APP_ICON_H
#define GEX_APP_ICON_H

#include <SDL2/SDL.h>

int gex_app_video_init(void);
SDL_Window *gex_app_window(const char *title, int x, int y, int width, int height, Uint32 flags);

#endif
