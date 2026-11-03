import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("game_asset_manifest", ROOT / "tools/game_asset_manifest.py")
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class GameAssetManifestTests(unittest.TestCase):
    def test_complete_metadata_and_generators(self):
        entries = manifest.load_manifest()
        self.assertEqual(len(entries), 661)
        self.assertEqual(entries[0], ("GEX.exe", 746496,
            "e1fd63ecb09fca29d63286e22447752dcb120d7e682f8759d1021776f7698b86"))
        with tempfile.TemporaryDirectory(prefix="gex-manifest-") as work:
            c, java = Path(work) / "assets.h", Path(work) / "GameAssets.java"
            manifest.generate_c(c)
            manifest.generate_java(java)
            for name, size, digest in entries:
                self.assertIn(f'"{name}", UINT64_C({size}), "{digest}"', c.read_text())
                self.assertIn(f'"{name}", {size}L, "{digest}"', java.read_text())

    def test_rejects_invalid_entries(self):
        with tempfile.TemporaryDirectory(prefix="gex-manifest-") as work:
            path = Path(work) / "manifest"
            for name in ("../GEX.exe", "/GEX.exe", "LEV/../bad", "LEV\\bad", "Gex/extra"):
                path.write_text("0" * 64 + "\t1\t" + name + "\n")
                with self.subTest(name=name), self.assertRaises(ValueError):
                    manifest.load_manifest(path)
