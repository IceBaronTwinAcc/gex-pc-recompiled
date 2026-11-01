from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

BACKEND = r'''
#include <stdio.h>
#include "movie_decoder.h"
struct gex_movie_decoder { int unused; };
gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                         char *error, size_t size) {
    (void)path; (void)info;
    if (size) snprintf(error, size, "build-test backend cannot open movies");
    return NULL;
}
void gex_movie_decoder_close(gex_movie_decoder *d) { (void)d; }
int gex_movie_decoder_next(gex_movie_decoder *d, gex_movie_event *e) {
    (void)d; (void)e; return -1;
}
int gex_movie_decoder_render(gex_movie_decoder *d, uint8_t *p, int w, int h,
                             int pitch, gex_movie_pixel_format format) {
    (void)d; (void)p; (void)w; (void)h; (void)pitch; (void)format; return -1;
}
void gex_movie_decoder_release(gex_movie_decoder *d) { (void)d; }
void gex_movie_decoder_discard(gex_movie_decoder *d) { (void)d; }
int gex_movie_decoder_rewind(gex_movie_decoder *d) { (void)d; return -1; }
const char *gex_movie_decoder_error(const gex_movie_decoder *d) {
    (void)d; return "build-test backend cannot decode movies";
}
'''


class MovieBuildTests(unittest.TestCase):
    def test_custom_source_without_ffmpeg(self):
        pkg_config = shutil.which("pkg-config")
        self.assertIsNotNone(pkg_config)
        with tempfile.TemporaryDirectory(prefix="gex-movie-build-") as work:
            work = Path(work)
            backend = work / "test_backend.c"
            backend.write_text(BACKEND)
            wrapper = work / "pkg-config-no-ffmpeg"
            trace = work / "pkg-config.trace"
            wrapper.write_text(
                "#!/usr/bin/env python3\n"
                "import os, sys\n"
                f"with open({str(trace)!r}, 'a') as log:\n"
                "    log.write(' '.join(sys.argv[1:]) + '\\n')\n"
                "if any(name in argument for argument in sys.argv[1:]\n"
                "       for name in ('libav', 'libswscale', 'libswresample')):\n"
                "    sys.exit(1)\n"
                f"os.execv({pkg_config!r}, [{pkg_config!r}] + sys.argv[1:])\n"
            )
            wrapper.chmod(0o755)
            build = work / "build"
            configure = subprocess.run([
                "cmake", "-S", str(ROOT), "-B", str(build),
                "-DBUILD_TESTING=OFF",
                f"-DPCRECOMP_DIR={os.environ.get('GEX_PCRECOMP_DIR', ROOT / 'pcrecomp')}",
                f"-DPKG_CONFIG_EXECUTABLE={wrapper}",
                f"-DGEX_MOVIE_DECODER_SOURCE={backend}",
                f"-DGEX_RENDERER_BACKEND_SOURCE={ROOT / 'src/renderer_headless.c'}",
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            compile_result = subprocess.run([
                "cmake", "--build", str(build), "--target", "gex_movie_decoder",
                "gex_renderer_backend", "--parallel", "2",
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            queries = trace.read_text()
            self.assertIn("sdl2", queries)
            for name in ("libav", "libswscale", "libswresample"):
                self.assertNotIn(name, queries)
