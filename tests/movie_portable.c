#include "dsound.c"
#include "host.h"
#include "movie_decoder.h"

static uint32_t clock_ms;
static uint32_t test_clock(void) { return clock_ms; }
#define hle_ticks_ms test_clock
#include "../src/movie.c"
#undef hle_ticks_ms
#if GEX_ENABLE_GLDC_RENDERER || GEX_ENABLE_PVR_RENDERER
int gex_graphics_gpu_owned(void) { return 0; }
void gex_graphics_movie(const uint8_t *pixels, int width, int height, int pitch) {
    (void)pixels; (void)width; (void)height; (void)pitch;
    hle_fatal("software movie fixture reached native movie output");
}
#endif

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;

struct gex_movie_decoder {
    unsigned index;
    int outstanding, renders, closed;
    int16_t samples[GEX_MOVIE_AUDIO_CHUNK_FRAMES];
};
static struct gex_movie_decoder fake;
static int failures;
void test_audio_render(uint8_t *output, int bytes);
static int expected_width = 2, expected_height = 2, expected_pitch = 4;

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *result,
                                         char *error, size_t error_size) {
    (void)path; (void)error; (void)error_size;
    memset(&fake, 0, sizeof fake);
    for (unsigned i = 0; i < GEX_MOVIE_AUDIO_CHUNK_FRAMES; ++i) fake.samples[i] = 10000;
    *result = (gex_movie_info){2, 2, 44100, 1, 2000};
    return &fake;
}
void gex_movie_decoder_close(gex_movie_decoder *d) { if (d) d->closed = 1; }
const char *gex_movie_decoder_error(const gex_movie_decoder *d) { (void)d; return "test decoder error"; }
void gex_movie_decoder_release(gex_movie_decoder *d) {
    CHECK(d->outstanding);
    d->outstanding = 0;
    ++d->index;
}
void gex_movie_decoder_discard(gex_movie_decoder *d) { d->outstanding = 0; }
int gex_movie_decoder_rewind(gex_movie_decoder *d) { d->index = 0; d->outstanding = 0; return 0; }
int gex_movie_decoder_next(gex_movie_decoder *d, gex_movie_event *result) {
    CHECK(!d->closed && !d->outstanding);
    if (d->index == 42) return 0;
    d->outstanding = 1;
    if (!d->index || d->index == 41)
        *result = (gex_movie_event){.type = GEX_MOVIE_VIDEO, .timestamp_ms = d->index ? 1000 : 0};
    else
        *result = (gex_movie_event){.type = GEX_MOVIE_AUDIO, .samples = d->samples,
                                  .audio_frames = GEX_MOVIE_AUDIO_CHUNK_FRAMES};
    return 1;
}
int gex_movie_decoder_render(gex_movie_decoder *d, uint8_t *destination,
                             int w, int h, int pitch, gex_movie_pixel_format format) {
    CHECK(d->outstanding && w == expected_width && h == expected_height &&
          pitch == expected_pitch && format == GEX_MOVIE_RGB565_LE);
    ++d->renders;
    for (int y = 0; y < h; ++y)
        memset(destination + (size_t)y * pitch, d->renders, (size_t)w * 2);
    return 0;
}

static uint32_t command(uint32_t code) {
    uint32_t args[] = {1, code, code == 0x807 ? 0x100u : 0, 0};
    return hle_call_guest(hle_resolve("mciSendCommandA"), 4, args);
}

int main(void) {
    recomp_host_init();
    gex_movie_register(0);
    if (gex_renderer_init(gex_renderer_backend_get(), 1) || gex_renderer_resize(2, 2)) return 1;
    mach_enter();
    char error[1];
    decoder = gex_movie_decoder_open("test", &info, error, sizeof error);
    CHECK(!command(0x806));
    if (audio_stream < 0) return 1;
    gex_audio_pause(1);
    gex_movie_pump();
    CHECK(fake.renders == 1 && fake.index == 1);
    gex_movie_pump();
    CHECK(pending && event.type == GEX_MOVIE_AUDIO && audio_offset > 0);
    CHECK(fake.outstanding && hle_audio_stream_queued(audio_stream) == STREAM_FRAMES - 1);
    unsigned rendered_audio = 0;
    int16_t output[4096 * 2];
    for (unsigned i = 0; i < 100 && fake.index < 41; ++i) {
        int count = hle_audio_stream_queued(audio_stream);
        if (count > 4096) count = 4096;
        if (count) {
            test_audio_render((uint8_t *)output, count * 4);
            for (int j = 0; j < count * 2; ++j) CHECK(output[j] > 9900);
            rendered_audio += (unsigned)count;
        }
        gex_movie_pump();
    }
    CHECK(fake.index == 41 && pending && fake.renders == 1);
    clock_ms = 500;
    CHECK(!command(0x809));
    int queued = hle_audio_stream_queued(audio_stream);
    test_audio_render((uint8_t *)output, sizeof output);
    CHECK(hle_audio_stream_queued(audio_stream) == queued);
    clock_ms = 2500;
    gex_movie_pump();
    CHECK(fake.renders == 1);
    CHECK(!command(0x855));
    clock_ms = 2999;
    gex_movie_pump();
    CHECK(fake.renders == 1);
    clock_ms = 3000;
    gex_movie_pump();
    CHECK(fake.renders == 2 && fake.index == 42);
    gex_movie_pump();
    CHECK(playing && audio_stream >= 0);
    while ((queued = hle_audio_stream_queued(audio_stream)) > 0) {
        int count = queued > 4096 ? 4096 : queued;
        test_audio_render((uint8_t *)output, count * 4);
        rendered_audio += (unsigned)count;
    }
    CHECK(rendered_audio == 40 * GEX_MOVIE_AUDIO_CHUNK_FRAMES);
    gex_movie_pump();
    CHECK(!playing && audio_stream == -1);
    CHECK(!command(0x807) && fake.index == 0);
    CHECK(!command(0x806));
    gex_audio_pause(1);
    gex_movie_pump();
    CHECK(fake.renders == 3);
    CHECK(!command(0x808));
    CHECK(!playing && !pending && !fake.outstanding && audio_stream == -1);
    CHECK(!command(0x804) && fake.closed && !decoder);
    const int sizes[][8] = {
        {427, 240, 320, 240, 53, 0, 320, 240},
        {1920, 1080, 320, 240, 240, 0, 1440, 1080},
        {2560, 1080, 320, 240, 560, 0, 1440, 1080},
        {3440, 1440, 640, 480, 760, 0, 1920, 1440},
        {240, 427, 320, 240, 0, 123, 240, 180},
        {427, 241, 640, 360, 0, 0, 427, 240},
    };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        const int *size = sizes[i];
        CHECK(!gex_renderer_resize(size[0], size[1]));
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        memset(frame.pixels, 0xAB, (size_t)frame.pitch * frame.height);
        decoder = gex_movie_decoder_open("test", &info, error, sizeof error);
        info.width = size[2]; info.height = size[3]; info.audio_channels = 0;
        expected_width = size[6]; expected_height = size[7]; expected_pitch = frame.pitch;
        CHECK(!command(0x806));
        gex_movie_pump();
        CHECK(fake.renders == 1);
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                unsigned expected = x >= size[4] && x < size[4] + size[6] &&
                                    y >= size[5] && y < size[5] + size[7] ? 1 : 0;
                CHECK(frame.pixels[y * frame.pitch + x * 2] == expected);
                CHECK(frame.pixels[y * frame.pitch + x * 2 + 1] == expected);
            }
        CHECK(!command(0x804));
    }
    mach_leave();
    gex_renderer_shutdown();
    gex_audio_close();
    return failures ? 1 : 0;
}
