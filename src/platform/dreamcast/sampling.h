#ifndef GEX_DREAMCAST_SAMPLING_H
#define GEX_DREAMCAST_SAMPLING_H

#include <stdint.h>

typedef struct {
    float first, step;
} gex_pvr_axis_fit;

static int gex_pvr_fixed_floor(int64_t value) {
    return (int)(value >= 0 ? value / 65536 : -((-value + 65535) / 65536));
}

static void gex_pvr_fit_bounds(const int *samples, int count, float step,
                               float *lower, float *upper) {
    float lo = (float)samples[0], hi = lo + 1;
    for (int i = 1; i < count; ++i) {
        float at = samples[i] - i * step;
        if (at > lo) lo = at;
        if (at + 1 < hi) hi = at + 1;
    }
    *lower = lo;
    *upper = hi;
}

/* Find the affine mapping with the widest clearance from texel boundaries. */
static int gex_pvr_fit_axis(int width, int span, int first, int count,
                            gex_pvr_axis_fit *fit) {
    static struct {
        int width, span, first, count, valid;
        gex_pvr_axis_fit fit;
    } cache[64];
    static unsigned next;
    if (width <= 0 || first < 0 || count <= 0 ||
        first > width - count || count > 320)
        return 0;
    for (unsigned i = 0; i < sizeof cache / sizeof *cache; ++i)
        if (cache[i].valid && cache[i].width == width && cache[i].span == span &&
            cache[i].first == first && cache[i].count == count) {
            *fit = cache[i].fit;
            return 1;
        }
    int samples[320];
    int64_t fixed_step = (int64_t)span * 65536 / width;
    for (int i = 0; i < count; ++i)
        samples[i] = gex_pvr_fixed_floor((int64_t)(first + i) * fixed_step);
    if (count == 1) {
        fit->first = samples[0] + 0.5f;
        fit->step = 0;
    } else {
        float delta = (float)(samples[count - 1] - samples[0]);
        float lo = (delta - 1) / (count - 1), hi = (delta + 1) / (count - 1);
        for (int iteration = 0; iteration < 32; ++iteration) {
            float a = (2 * lo + hi) / 3, b = (lo + 2 * hi) / 3;
            float al, ah, bl, bh;
            gex_pvr_fit_bounds(samples, count, a, &al, &ah);
            gex_pvr_fit_bounds(samples, count, b, &bl, &bh);
            if (ah - al < bh - bl) lo = a;
            else hi = b;
        }
        fit->step = (lo + hi) * 0.5f;
        float lower, upper;
        gex_pvr_fit_bounds(samples, count, fit->step, &lower, &upper);
        if (!(upper > lower)) return 0;
        fit->first = (lower + upper) * 0.5f;
    }
    for (int i = 0; i < count; ++i) {
        float value = fit->first + i * fit->step;
        if (!(value > samples[i] && value < samples[i] + 1)) return 0;
    }
    cache[next].width = width;
    cache[next].span = span;
    cache[next].first = first;
    cache[next].count = count;
    cache[next].fit = *fit;
    cache[next].valid = 1;
    next = (next + 1) % (sizeof cache / sizeof *cache);
    return 1;
}

#endif
