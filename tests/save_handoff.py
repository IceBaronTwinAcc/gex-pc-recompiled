"""Selecting empty and occupied decks must not redisplay the outgoing title."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


program, fixture, executable = (Path(value).resolve() for value in sys.argv[1:4])
for backend in sys.argv[4:]:
    for mode in ("empty", "occupied", "manual"):
        with tempfile.TemporaryDirectory(prefix="gex-save-handoff-") as work:
            env = {**os.environ, "XDG_DATA_HOME": work, "SDL_AUDIODRIVER": "dummy",
                   "SDL_VIDEODRIVER": "offscreen" if backend in ("opengl", "opengl12") else "dummy",
                   "ALSOFT_DRIVERS": "null"}
            if mode == "occupied":
                directory = Path(work) / "gex-recomp/Gex"
                directory.mkdir(parents=True)
                subprocess.run([str(fixture), str(executable), "write", str(directory)],
                               env=env, check=True, timeout=5)
                env["GEX_SAVE_HANDOFF_OCCUPIED"] = "1"
            if mode == "manual":
                env["GEX_SAVE_HANDOFF_MANUAL"] = "1"
            result = subprocess.run(
                [str(program), "--data", str(executable.parent), "--headless", "--skip-movies",
                 "--renderer", backend, "--render-size", "320x240", "--smoke-seconds", "8"],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
            assert result.returncode == 0, (backend, mode, result.stdout)
            expected = "manual password entry remains visible" if mode == "manual" else "no title flash"
            assert "[save-handoff] " + expected in result.stdout, result.stdout
