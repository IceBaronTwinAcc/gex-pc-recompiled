#include <SDL2/SDL.h>
#include "packet_gpu.h"

uint64_t gex_packet_gpu_profile_clock(uint64_t *cpu) {
    uint64_t counter = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    *cpu = 0;
    return counter / frequency * 1000000 + counter % frequency * 1000000 / frequency;
}
