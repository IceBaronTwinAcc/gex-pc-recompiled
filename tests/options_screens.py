"""Real headless Options scrolling, clipping and slider movement."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def pixels(path):
    data = path.read_bytes()
    assert data[:2] == b"BM"
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    assert width > 0 and height != 0 and bits in (24, 32)
    stride = (width * bits + 31) // 32 * 4
    result = []
    for y in range(abs(height)):
        row = offset + (y if height < 0 else height - 1 - y) * stride
        result.append([tuple(data[row + x * (bits // 8):row + x * (bits // 8) + 3])
                       for x in range(width)])
    return result


def green(image, x0, x1, y0, y1):
    return {(x, y) for y in range(y0, y1) for x in range(x0, x1)
            if image[y][x][1] > 100 and image[y][x][1] > image[y][x][0] * 2
            and image[y][x][1] > image[y][x][2] * 2}


def restart_notice(image):
    width, height = len(image[0]), len(image)
    if width * 224 > height * 320:
        canvas_width, canvas_height = height * 320 // 224, height
    else:
        canvas_width, canvas_height = width, width * 224 // 320
    left, top = (width - canvas_width) // 2, (height - canvas_height) // 2
    mask = green(image, left, left + canvas_width,
                 top + 180 * canvas_height // 224, top + canvas_height)
    return {((x - left) * 320 // canvas_width, (y - top) * 224 // canvas_height)
            for x, y in mask}


program, executable = map(lambda value: Path(value).resolve(), sys.argv[1:3])
for backend in sys.argv[3:]:
    video_driver = "offscreen" if backend in ("opengl", "opengl12") else "dummy"
    with tempfile.TemporaryDirectory(prefix="gex-options-") as work:
        directory = Path(work)
        result = subprocess.run(
            [str(program), "--data", str(executable.parent), "--headless", "--skip-movies",
             "--renderer", backend, "--render-size", "320x240", "--smoke-seconds", "12"],
            env={**os.environ, "XDG_DATA_HOME": work, "GEX_OPTIONS_CAPTURE_DIR": work,
                 "SDL_VIDEODRIVER": video_driver, "SDL_AUDIODRIVER": "dummy", "ALSOFT_DRIVERS": "null"},
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=14)
        assert result.returncode == 0, result.stdout
        assert "[options-screens] category navigation" in result.stdout, result.stdout
        full = pixels(directory / "audio-full.bmp")
        half = pixels(directory / "audio-half.bmp")
        # The slider knob must move; the other two volume settings stay full.
        assert len(green(full, 275, 278, 90, 103)) == 39
        assert len(green(half, 165, 168, 90, 103)) == 39
        assert not green(half, 275, 278, 90, 103)
        for name in ("categories", "controls", "video", "returned-title"):
            image = pixels(directory / (name + ".bmp"))
            assert len(green(image, 20, 310, 10, 230)) > 100, name
        for page, captures in (
                ("controls", ("controls-middle", "controls-menus", "controls-bottom",
                              "controls-wrap-bottom", "controls-wrap-top")),
                ("audio-full", ("audio-scrolled", "audio-scrolled-half", "audio-music-test", "audio-bottom")),
                ("video", ("video-resolution", "video-vsync", "video-bottom"))):
            initial = pixels(directory / (page + ".bmp"))
            heading = green(initial, 20, 310, 8, 48)
            view_bottom = 190 if page == "video" else 205
            footer_top = 200 if page == "video" else 215
            for name in captures:
                image = pixels(directory / (name + ".bmp"))
                assert green(image, 20, 310, 8, 48) == heading, (backend, name, "fixed heading")
                assert not green(image, 20, 310, 48, 58), (backend, name, "top clip")
                assert not green(image, 20, 310, view_bottom, footer_top), (backend, name, "bottom clip")
                assert len(green(image, 20, 310, 58, view_bottom)) > 100, (backend, name, "visible rows")
        notice = restart_notice(pixels(directory / "video-resolution.bmp"))
        assert notice, (backend, "restart notice")
        left, right = min(x for x, y in notice), max(x for x, y in notice)
        top, bottom = min(y for x, y in notice), max(y for x, y in notice)
        # The complete glyphs include descenders previously clipped to 17 pixels.
        assert len(notice) == 1443 and bottom - top == 22, (backend, "complete full-size restart font")
        assert left >= 20 and right < 310 and top >= 190 and bottom <= 216, (
            backend, "notice and shadow fit authored picture")
        for name in ("video-vsync", "video-bottom"):
            assert restart_notice(pixels(directory / (name + ".bmp"))) == notice, (
                backend, name, "fixed restart notice")
    with tempfile.TemporaryDirectory(prefix="gex-options-resize-") as work:
        result = subprocess.run(
            [str(program), "--data", str(executable.parent), "--headless", "--skip-movies",
             "--renderer", backend, "--render-size", "640x480", "--smoke-seconds", "12"],
            env={**os.environ, "XDG_DATA_HOME": work, "GEX_OPTIONS_TEST_RESIZE": "1",
                 "GEX_OPTIONS_CAPTURE_DIR": work,
                 "SDL_VIDEODRIVER": video_driver, "SDL_AUDIODRIVER": "dummy", "ALSOFT_DRIVERS": "null"},
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=14)
        assert result.returncode == 0, result.stdout
        assert "[options-screens] category navigation" in result.stdout, result.stdout
        for name in ("video-resolution", "video-vsync", "video-bottom"):
            assert restart_notice(pixels(Path(work) / (name + ".bmp"))) == notice, (
                backend, name, "complete restart notice at 640x480")
        assert len(green(pixels(Path(work) / "returned-title.bmp"), 20, 310, 10, 230)) > 100
        result = subprocess.run(
            [str(program), "--data", str(executable.parent), "--headless", "--skip-movies",
             "--smoke-seconds", "2"],
            env={**os.environ, "XDG_DATA_HOME": work, "GEX_OPTIONS_RELOAD_ONLY": "1",
                 "SDL_VIDEODRIVER": video_driver, "SDL_AUDIODRIVER": "dummy", "ALSOFT_DRIVERS": "null"},
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=5)
        assert result.returncode == 0, result.stdout
        assert "[options-screens] saved video settings restored" in result.stdout, result.stdout
        assert f"[renderer] selected {backend} packet backend" in result.stdout, result.stdout
