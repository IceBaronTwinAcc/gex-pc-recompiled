from pathlib import Path
import subprocess
import sys
import tempfile

program = Path(sys.argv[1]).resolve()
runner = sys.argv[2:]


def run(mode, path, expected=0, diagnostic=None):
    result = subprocess.run([*runner, str(program), mode, str(path)], capture_output=True,
                            text=True, timeout=10)
    diagnostics = (diagnostic,) if isinstance(diagnostic, str) else diagnostic
    if result.returncode != expected or (diagnostics and not any(
            text in result.stderr for text in diagnostics)):
        raise AssertionError(f"{mode}: exit {result.returncode}\n{result.stderr}")


with tempfile.TemporaryDirectory(prefix="gex-registry-") as work:
    directory = Path(work)
    path = directory / "settings.reg"
    run("load", path)
    run("load", directory, 2, ("cannot read", "error reading"))
    run("write", path)
    run("read", path)
    if list(directory.glob("settings.reg.*")):
        raise AssertionError("atomic save leaked a temporary file")
    saved = path.read_bytes()
    blocked = directory / "blocked"
    blocked.mkdir()
    run("fail-write", blocked / "missing" / "settings.reg",
        diagnostic="cannot create settings file")
    run("fail-replace", directory / "replace-target", diagnostic="cannot save settings")
    if not (directory / "replace-target").is_dir():
        raise AssertionError("failed save replaced the destination")
    if list(directory.glob("replace-target.*")):
        raise AssertionError("failed save leaked a temporary file")
    for text in ("HKEY_CURRENT_USER\\Test\\v=hex:0z\n",
                 "HKEY_CURRENT_USER\\Test\\v=dword:garbage\n",
                 "HKEY_CURRENT_USER\\Test\\v=dword:0x100000000\n",
                 "HKEY_CURRENT_USER\\Test\\v=hex:0\n",
                 "HKEY_CURRENT_USER\\Test\\v=unknown:value\n",
                 "HKEY_CURRENT_USER\\Test\\v=sz:" + "x" * 2048 + "\n",
                 "not a settings entry\n"):
        path.write_text(text)
        run("load", path, 2, "invalid settings")
        if path.read_text() != text:
            raise AssertionError("invalid input was overwritten")
    path.write_bytes(saved)
    run("read", path)
