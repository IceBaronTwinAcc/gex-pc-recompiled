"""Exercise the real importer with isolated folders and a non-graphical picker."""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

program, executable = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
runner = sys.argv[3:]


def run(arguments, cwd, expected, message="", selected=None):
    env = {**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
    env.pop("GEX_TEST_PICK", None)
    if selected is not None:
        env["GEX_TEST_PICK"] = str(selected)
    result = subprocess.run([*runner, str(program), *map(str, arguments)], cwd=cwd,
                            env=env, capture_output=True, text=True, timeout=40)
    assert result.returncode == expected, result.stderr
    assert message in result.stderr, result.stderr


with tempfile.TemporaryDirectory(prefix="gex-install-") as temporary:
    root = Path(temporary)
    source = root / "original game"
    source.mkdir()
    shutil.copyfile(executable, source / "GEX.exe")
    for name in ("AVI", "IDL", "LEV", "MUS", "SFX", "VFX"):
        shutil.copytree(executable.parent / name, source / name)
    (source / "read me.txt").write_text("Preserve unrelated original files too.\n")
    (source / "notes-\u00e9.txt").write_text("Preserve Unicode filenames.\n")
    (source / "VFX" / "empty").mkdir()
    def hashes(directory):
        return {p.relative_to(directory): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in directory.rglob("*") if p.is_file()}

    expected = hashes(source)
    destination = root / "Gex"
    run(["prepare", destination, 0], root, 2, "picker-calls=0")
    run(["prepare", destination, 1], root, 1, "picker-calls=1")
    assert not destination.exists()
    run(["prepare", destination, 1], root, 0, "picker-calls=1", source)
    assert hashes(destination) == expected
    assert (destination / "VFX" / "empty").is_dir()
    run(["prepare", destination, 1], root, 0, "picker-calls=0")
    run(["install", source, destination], root, 2, "will not be overwritten")
    assert hashes(source) == expected
    run(["install", source, source / "nested"], root, 2, "must not be inside")
    run(["install", source / "GEX.exe", root / "not-a-folder"], root, 2, "not an ordinary game directory")
    assert not (source / "nested").exists()
    (source / "AVI" / "GEX001.AVI").unlink()
    run(["install", source, root / "missing"], root, 2, "AVI/GEX001.AVI")
    shutil.copyfile(executable.parent / "AVI/GEX001.AVI", source / "AVI/GEX001.AVI")
    (source / "GEX.exe").write_bytes(b"Unsupported executable")
    run(["prepare", root / "invalid", 1], root, 1,
        "Required game file has an incorrect size: GEX.exe. Reinstall the game from GOG.", source)
    assert not (root / "invalid").exists()
    shutil.copyfile(executable, source / "GEX.exe")
    for folder in ("LEV", "MUS", "SFX", "VFX"):
        asset = next(p for p in (source / folder).iterdir() if p.is_file())
        original = asset.read_bytes()
        relative = asset.relative_to(source)
        asset.unlink()
        run(["install", source, root / "missing-asset"], root, 2, str(relative))
        assert not (root / "missing-asset").exists()
        asset.write_bytes(original[:-1])
        run(["install", source, root / "truncated"], root, 2,
            f"Required game file has an incorrect size: {relative}. Reinstall the game from GOG.")
        assert not (root / "truncated").exists()
        altered = bytearray(original)
        altered[len(altered) // 2] ^= 1
        asset.write_bytes(altered)
        run(["install", source, root / "corrupt"], root, 2,
            f"Required game file failed integrity check: {relative}. Reinstall the game from GOG.")
        assert not (root / "corrupt").exists()
        asset.write_bytes(original)
    if not runner and os.name != "nt":
        (source / "link").symlink_to(source, target_is_directory=True)
        run(["install", source, root / "linked"], root, 2, "Links and special files")
        (source / "link").unlink()
        assert not (root / "linked").exists()
        run(["install", source, root / "absent-parent" / "Gex"], root, 2, "Cannot resolve")
    existing = root / "existing"
    existing.mkdir()
    (existing / "keep").write_text("Do not touch")
    run(["install", source, existing], root, 2, "will not be overwritten")
    run(["prepare", existing, 0], root, 2, "picker-calls=0")
    assert (existing / "keep").read_text() == "Do not touch"
    assert not list(root.glob(".gex-import-*")), "Incomplete staging folders remain"
print("Validated copying, original preservation, cancellation, headless behavior, and failure cleanup")
