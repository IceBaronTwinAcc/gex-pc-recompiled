"""Check launcher errors and option ordering without starting the guest."""

from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

program, executable = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
software = sys.argv[3] == "1"
opengl = sys.argv[4] == "1"
opengl12 = sys.argv[5] == "1"
bgfx = sys.argv[6] == "1"
runner = sys.argv[7:]


def run(args, cwd, expected, diagnostic, environment=None):
    result = subprocess.run([*runner, str(program), *args], cwd=cwd,
                            capture_output=True, text=True, timeout=15,
                            env={**os.environ, "SDL_VIDEODRIVER": "dummy",
                                 "SDL_AUDIODRIVER": "dummy",
                                 "XDG_DATA_HOME": str(cwd), **(environment or {})})
    if result.returncode != expected or diagnostic not in result.stderr:
        raise AssertionError(f"{args}: exit {result.returncode}\n{result.stderr}")


with tempfile.TemporaryDirectory(prefix="gex-launcher-") as work:
    directory = Path(work)
    run(["--help"], directory, 0, "Usage:")
    default = "bgfx" if bgfx else "software" if software else "opengl12" if opengl12 else "opengl"
    run(["--help"], directory, 0, f"Default packet renderer: {default}")
    run(["--headless", "--check"], directory, 1, "game data not found")
    run(["--check"], directory, 1, "game data not found")
    assert not (directory / "Gex").exists()
    run(["--renderer"], directory, 64, "--renderer requires")
    run(["--renderer", "invalid"], directory, 64, "--renderer requires")
    run(["--renderer", "pvr"], directory, 64, "pvr renderer was excluded")
    for name, enabled in (("software", software), ("opengl", opengl), ("opengl12", opengl12),
                          ("bgfx", bgfx)):
        run(["--renderer", name, "--data", "missing"], directory,
            1 if enabled else 64,
            "cannot enter missing" if enabled else f"{name} renderer was excluded")
    run(["--hardware-renderer", "--data", "missing"], directory,
        1 if opengl or opengl12 or bgfx else 64,
        "cannot enter missing" if opengl or opengl12 or bgfx else "opengl renderer was excluded")
    run(["--renderer", "opengl", "--renderer", "software", "--data", "missing"],
        directory, 1 if software else 64,
        "cannot enter missing" if software else "software renderer was excluded")
    for size in ["0x240", "320x0", "-1x240", "8193x480", "8192x8192", "320x240garbage"]:
        run(["--render-size", size], directory, 64, "--render-size requires")
    run(["--render-size"], directory, 64, "--render-size requires")
    run(["--smoke-seconds", "0"], directory, 64, "between 1 and 3600")
    run(["--screenshot", "frame.bmp"], directory, 64, "require --smoke-seconds")
    run(["--autoplay", "--smoke-seconds", "1"], directory, 64, "requires --skip-movies")
    run(["--verify-gameplay"], directory, 64, "requires --headless")
    run(["--verify-gameplay", "--autoplay", "--skip-movies", "--smoke-seconds", "24"],
        directory, 64, "requires --headless")
    run(["--verify-gameplay", "--autoplay", "--headless", "--skip-movies",
         "--smoke-seconds", "180", "--data", "missing"],
        directory, 1, "cannot enter missing")
    run(["--verify-gameplay", "--autoplay", "--headless", "--skip-movies",
         "--smoke-seconds", "24", "--data", "missing"],
        directory, 1, "cannot enter missing")
    run(["--screenshot", "frame.bmp", "--smoke-seconds", "1",
         "--data", "missing"], directory, 1, "cannot enter missing")
    run(["--data", work, "--check"], directory, 1, "cannot read executable")
    (directory / "GEX.exe").write_bytes(b"not the supported executable")
    run(["--data", work, "--check"], directory, 1, "unsupported executable SHA-256")
    if executable.is_file():
        run(["--data", str(executable.parent), "--check"], directory, 0,
            "statically recompiled functions")
        run(["--render-size", "1237x701", "--data", str(executable.parent), "--check"],
            directory, 0, "statically recompiled functions")
        capture = directory / "native.bmp"
        if not software:
            # CLI/check coverage does not need a GL device; GPU scene checks
            # are separately run under the platform's hidden GL driver.
            sys.exit(0)
        run(["--renderer", "software", "--data", str(executable.parent), "--headless", "--skip-movies",
             "--render-size", "1237x701", "--smoke-seconds", "1",
             "--screenshot", str(capture)],
            directory, 0, "selected software packet backend")
        if not runner and sys.platform in {"linux", "darwin"}:
            if not (directory / "gex-recomp" / "Gex").is_dir():
                raise AssertionError("XDG_DATA_HOME did not isolate the settings directory")
            blocked = directory / "blocked-settings"
            blocked.write_text("not a directory")
            run(["--data", str(executable.parent), "--headless", "--skip-movies",
                 "--smoke-seconds", "1"], directory, 1, "settings directory",
                {"XDG_DATA_HOME": str(blocked)})
        header = capture.read_bytes()[:54]
        if header[:2] != b"BM" or struct.unpack_from("<ii", header, 18) != (1237, 701):
            raise AssertionError("custom raster dimensions were not preserved through presentation")
        run(["--renderer", "software", "--data", str(executable.parent), "--headless", "--autoplay",
             "--skip-movies", "--verify-gameplay", "--smoke-seconds", "2"],
            directory, 3, "first-graveyard=FAIL")
