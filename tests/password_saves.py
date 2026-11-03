"""Separate-process save/restart/retail-decode and three-deck menu checks."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib

program, executable = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
runner = sys.argv[3:]


def run(mode, directory, *extra):
    subprocess.run([*runner, program, executable, mode, str(directory), *extra],
                   env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
                        "XDG_DATA_HOME": str(directory)}, check=True, timeout=25)


with tempfile.TemporaryDirectory(prefix="gex-passwords-") as work:
    directory = Path(work)
    run("write", directory)
    run("read", directory)
    run("threaded", directory)
    run("threaded-back", directory)
    run("transition", directory)
    run("menu", directory)
    path = directory / "passwords.sav"
    original = path.read_bytes()
    valid_crc_bad_records = []
    for index, value in ((16, ord("X")), (8, 0), (18, ord("X")), (26, ord("X"))):
        broken = bytearray(original)
        broken[index] = value
        broken[35:] = zlib.crc32(broken[:35]).to_bytes(4, "little")
        valid_crc_bad_records.append(broken)
    for damaged in (b"", original[:18], original + b"x",
                    bytes([original[0] ^ 1]) + original[1:],
                    original[:20] + bytes([original[20] ^ 1]) + original[21:],
                    *valid_crc_bad_records):
        path.write_bytes(damaged)
        run("corrupt", directory)
        assert path.read_bytes() == damaged, "corrupt save was overwritten"
    path.unlink()
    run("operations", directory)
    run("options-write", directory)
    run("options-read", directory)
    run("options-error", directory)
    run("options-close", directory)
    run("options-close-consumed", directory)
    assert sorted(p.name for p in directory.iterdir()) == ["passwords.sav", "settings.reg"], "temporary save leaked"
