#ifndef GEX_PLATFORM_VIDEO_H
#define GEX_PLATFORM_VIDEO_H

#include <stddef.h>

int gex_video_display(int mode, char *error, size_t size);
int gex_video_display_count(void);
int gex_video_desktop(int *width, int *height);

#endif
