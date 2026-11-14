import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package_dreamcast", ROOT / "tools/package-dreamcast.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class DreamcastPackageTests(unittest.TestCase):
    def test_stages_converted_movies_without_modifying_originals(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            game = root / "Gex"
            (game / "AVI").mkdir(parents=True)
            for name in ("GEX000.AVI", "GEX001.AVI", "GEX002.AVI"):
                (game / "AVI" / name).write_bytes(b"original")
            elf = root / "game.elf"
            elf.write_bytes(b"elf")
            output = root / "game.cdi"
            config = root / "gex-test.cfg"
            config.write_text("test\n")
            commands = []

            def run(command, check):
                self.assertTrue(check)
                commands.append(command)
                if command[0] == "ffmpeg":
                    self.assertEqual(Path(command[command.index("-i") + 1]).read_bytes(), b"original")
                    Path(command[-1]).write_bytes(b"converted")
                else:
                    staged = Path(command[command.index("-d") + 1])
                    self.assertEqual((staged / "AVI/GEX000.AVI").read_bytes(), b"converted")
                    self.assertEqual((staged / "AVI/GEX001.AVI").read_bytes(), b"converted")
                    self.assertEqual((staged / "AVI/GEX002.AVI").read_bytes(), b"original")
                    self.assertEqual(Path(command[command.index("-f") + 1]), config)
                    Path(command[command.index("-o") + 1]).write_bytes(b"disc")

            argv = ["package-dreamcast.py", "--game-dir", str(game), "--elf", str(elf),
                    "--output", str(output), "--file", str(config)]
            with patch.object(sys, "argv", argv), patch.object(package.subprocess, "run", side_effect=run):
                package.main()
            self.assertEqual(len(commands), 3)
            self.assertEqual(output.read_bytes(), b"disc")
            for name in ("GEX000.AVI", "GEX001.AVI", "GEX002.AVI"):
                self.assertEqual((game / "AVI" / name).read_bytes(), b"original")

    def test_conversion_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            game = root / "Gex"
            (game / "AVI").mkdir(parents=True)
            elf = root / "game.elf"
            elf.touch()
            output = root / "game.cdi"
            output.write_bytes(b"existing")
            argv = ["package-dreamcast.py", "--game-dir", str(game),
                    "--elf", str(elf), "--output", str(output)]
            with patch.object(sys, "argv", argv), patch.object(
                    package.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "ffmpeg")):
                with self.assertRaises(subprocess.CalledProcessError):
                    package.main()
            self.assertEqual(output.read_bytes(), b"existing")

    def test_metadata_profile_keeps_original_movies(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            game = root / "Gex"
            (game / "AVI").mkdir(parents=True)
            (game / "AVI/GEX000.AVI").write_bytes(b"original")
            elf = root / "game.elf"
            elf.touch()
            output = root / "game.cdi"

            def run(command, check):
                self.assertEqual(command[0], "mkdcdisc")
                staged = Path(command[command.index("-d") + 1])
                self.assertEqual((staged / "AVI/GEX000.AVI").read_bytes(), b"original")
                Path(command[command.index("-o") + 1]).write_bytes(b"disc")

            argv = ["package-dreamcast.py", "--game-dir", str(game), "--elf", str(elf),
                    "--output", str(output), "--original-movies"]
            with patch.object(sys, "argv", argv), patch.object(package.subprocess, "run", side_effect=run):
                package.main()
            self.assertEqual(output.read_bytes(), b"disc")
