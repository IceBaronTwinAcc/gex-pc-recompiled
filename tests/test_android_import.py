from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import importlib.util

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("javac") and shutil.which("java"), "JDK unavailable")
class AndroidImportTests(unittest.TestCase):
    def test_installed_data_validation(self):
        with tempfile.TemporaryDirectory(prefix="gex-android-import-") as work:
            directory = Path(work)
            source = directory / "GameDataTest.java"
            source.write_text("""
package org.gexrecomp.app;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
public final class GameDataTest {
    public static void main(String[] args) throws Exception {
        File directory = new File(args[0]);
        try {
            if (args.length > 1)
                GameData.validate(path -> new FileInputStream(new File(directory, path)));
            else GameData.validate(directory);
        } catch (IOException error) {
            System.err.println(error.getMessage());
            System.exit(1);
        }
    }
}
""")
            spec = importlib.util.spec_from_file_location("game_asset_manifest", ROOT / "tools/game_asset_manifest.py")
            manifest = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(manifest)
            generated = directory / "GameAssets.java"
            manifest.generate_java(generated)
            subprocess.run(["javac", "-d", work, str(source),
                            str(ROOT / "android/src/org/gexrecomp/app/GameData.java"), str(generated)],
                           capture_output=True, check=True, timeout=30)
            fixture = directory / "Gex"
            fixture.mkdir()
            for name in ("AVI", "IDL", "LEV", "MUS", "SFX", "VFX"):
                (fixture / name).mkdir()
            for name in ("GEX.exe", "AVI/GEX000.AVI", "AVI/GEX001.AVI", "IDL/GEX000.IDL",
                         "LEV/GEX001.LEV", "LEV/GEX125.LEV"):
                (fixture / name).write_bytes(b"fixture")

            def validate(expected, message="", source_mode=False, exact=False):
                result = subprocess.run(["java", "-cp", work,
                    "org.gexrecomp.app.GameDataTest", str(fixture), *(["source"] if source_mode else [])],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode == 0, expected, result.stderr)
                if exact:
                    self.assertEqual(result.stderr.strip(), message)
                else:
                    self.assertIn(message, result.stderr)

            validate(False, "Required game file GEX.exe: Incorrect size: GEX.exe. "
                     "Reinstall the game from GOG.", exact=True)
            executable = ROOT / "Gex/GEX.exe"
            if executable.is_file():
                for folder in manifest.DIRECTORIES:
                    shutil.copytree(executable.parent / folder, fixture / folder, dirs_exist_ok=True)
                shutil.copyfile(executable, fixture / "GEX.exe")
                validate(True)
                validate(True, source_mode=True)
                for folder in ("LEV", "MUS", "SFX", "VFX"):
                    asset = next((fixture / folder).iterdir())
                    relative = asset.relative_to(fixture).as_posix()
                    original = asset.read_bytes()
                    asset.unlink()
                    validate(False, relative, source_mode=True)
                    asset.write_bytes(original[:-1])
                    validate(False, f"Required game file {relative}: Incorrect size: {relative}. "
                             "Reinstall the game from GOG.",
                             source_mode=True, exact=True)
                    changed = bytearray(original)
                    changed[len(changed) // 2] ^= 1
                    asset.write_bytes(changed)
                    for source_mode in (False, True):
                        validate(False,
                                 f"Required game file failed integrity check: {relative}. "
                                 "Reinstall the game from GOG.", source_mode=source_mode, exact=True)
                    asset.write_bytes(original)
