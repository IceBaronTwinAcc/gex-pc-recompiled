"""Verify picker subprocess arguments, errors, cancellation, and fallback without a GUI."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

program = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="gex-picker-") as work:
    directory = Path(work)
    trace = directory / "calls"
    environment = {**os.environ, "PATH": work, "GEX_PICKER_TRACE": str(trace)}
    title = "Select the Gex installation folder"
    message = "Select the folder with the installation of the PC version of Gex from GOG."

    def helper(name, code):
        path = directory / name
        path.write_text(f"#!{sys.executable}\n"
                        "import os, sys\n"
                        "with open(os.environ['GEX_PICKER_TRACE'], 'a') as log:\n"
                        "    log.write(repr(sys.argv[1:]) + '\\n')\n" + code)
        path.chmod(0o755)
        return path

    def run(expected, output=""):
        trace.unlink(missing_ok=True)
        result = subprocess.run([program], env=environment, capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == expected, result.stderr
        if expected == 0:
            assert result.stdout == output + "\n", repr(result.stdout)
            calls = trace.read_text().splitlines()
            assert len(calls) == 2, calls
            assert message in calls[0] and title in calls[0], calls
            assert title in calls[1], calls
        if expected == 2:
            assert "Cannot open the folder picker" in result.stderr

    run(2)
    kde = helper("kdialog", "if '--msgbox' not in sys.argv:\n"
                 "    assert '--getexistingdirectory' in sys.argv\n"
                 "    print('/a game folder/with spaces')\n")
    run(0, "/a game folder/with spaces")
    gtk = helper("zenity", "if '--info' not in sys.argv:\n"
                 "    assert '--directory' in sys.argv\n"
                 "    print('/a folder/with $shell; characters')\n")
    run(0, "/a folder/with $shell; characters")
    helper("zenity", "import sys\nsys.exit(1)\n")
    run(1)
    assert len(trace.read_text().splitlines()) == 1, "Cancellation opened the picker"
    helper("zenity", "import sys\nsys.exit(2)\n")
    run(2)
    assert len(trace.read_text().splitlines()) == 1, "Failed instructions opened the picker"
    helper("zenity", "if '--info' not in sys.argv: sys.exit(1)\n")
    run(1)
    assert len(trace.read_text().splitlines()) == 2, "Picker cancellation was not exercised"
    helper("zenity", "if '--info' not in sys.argv: sys.exit(2)\n")
    run(2)
    helper("zenity", "if '--info' not in sys.argv: print('x' * 8192)\n")
    run(2)
print("Folder picker subprocess behavior: PASS")
