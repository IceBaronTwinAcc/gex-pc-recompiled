#ifndef GEX_OPENAL_LIMITER_H
#define GEX_OPENAL_LIMITER_H

#include <alc.h>

int gex_openal_limiter_attributes(ALCdevice *device, ALCint *attributes, int *count);
int gex_openal_limiter_enable(ALCdevice *device, int configured);

#endif
