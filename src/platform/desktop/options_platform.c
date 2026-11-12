#include "options_menu.h"

uint32_t gex_options_platform(void) {
#if defined(__ANDROID__)
    return GEX_OPTIONS_ANDROID;
#elif defined(_WIN32)
    return GEX_OPTIONS_WINDOWS;
#elif defined(__APPLE__)
    return GEX_OPTIONS_MACOS;
#elif defined(__linux__)
    return GEX_OPTIONS_LINUX;
#else
#error Define the Options platform for this target.
#endif
}
