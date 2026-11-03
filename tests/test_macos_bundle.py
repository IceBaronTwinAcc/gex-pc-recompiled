import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class MacBundleTests(unittest.TestCase):
    def test_package_excludes_imported_assets(self):
        spec = importlib.util.spec_from_file_location("package_macos", ROOT / "tools/package-macos.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix="gex-macos-package-") as work:
            build, prefix = Path(work) / "build", Path(work) / "prefix"
            contents = build / "Gex Recompiled.app/Contents"
            (contents / "MacOS").mkdir(parents=True)
            (contents / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleExecutable": "Gex Recompiled", "CFBundleIdentifier": "org.gexrecomp.app",
            }))
            binary = contents / "MacOS/Gex Recompiled"
            binary.write_bytes(b"test executable")
            binary.chmod(0o755)
            imported = contents / "Resources/Gex"
            imported.mkdir(parents=True)
            (imported / "GEX.exe").write_bytes(b"private installation")
            (contents / "Resources/.gex-import-incomplete").mkdir()
            (prefix / "lib").mkdir(parents=True)
            (prefix / "lib/libtest.dylib").write_bytes(b"test library")
            (build / "licenses").mkdir()
            (build / "licenses/test.txt").write_text("licence")
            bundle, archive = module.package(build, prefix)
            self.assertEqual((imported / "GEX.exe").read_bytes(), b"private installation")
            self.assertEqual((build / "gex_recomp").read_bytes(), binary.read_bytes())
            with zipfile.ZipFile(archive) as output:
                files = output.namelist()
                self.assertIn(f"{bundle.name}/Contents/MacOS/Gex Recompiled", files)
                self.assertIn(f"{bundle.name}/Contents/Frameworks/libtest.dylib", files)
                self.assertIn(f"{bundle.name}/Contents/Resources/licenses/test.txt", files)
                self.assertFalse(any("/Gex/" in name or ".gex-import-" in name for name in files))
