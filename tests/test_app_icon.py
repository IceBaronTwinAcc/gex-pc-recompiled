from pathlib import Path
import struct
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "assets/icons"


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Expected a PNG icon")
    return struct.unpack_from(">II", data, 16)


class ApplicationIconTests(unittest.TestCase):
    def test_linux_icons_are_square_rgba(self):
        for size in (16, 24, 32, 48, 64, 128, 256, 512):
            image = (ICONS / f"gex-{size}.png").read_bytes()
            self.assertEqual(png_size(image), (size, size))
            self.assertEqual(image[25], 6)

    def test_windows_icon_contains_all_resolutions(self):
        data = (ICONS / "gex.ico").read_bytes()
        reserved, kind, count = struct.unpack_from("<HHH", data)
        self.assertEqual((reserved, kind, count), (0, 1, 7))
        sizes = set()
        for index in range(count):
            width, height, _, _, _, _, length, offset = struct.unpack_from(
                "<4B2H2I", data, 6 + index * 16)
            size = width or 256
            self.assertEqual(height or 256, size)
            self.assertEqual(png_size(data[offset:offset + length]), (size, size))
            sizes.add(size)
        self.assertEqual(sizes, {16, 24, 32, 48, 64, 128, 256})

    def test_macos_icon_includes_retina_sizes(self):
        data = (ICONS / "gex.icns").read_bytes()
        self.assertEqual(data[:4], b"icns")
        self.assertEqual(struct.unpack_from(">I", data, 4)[0], len(data))
        offset, sizes = 8, set()
        while offset < len(data):
            length = struct.unpack_from(">I", data, offset + 4)[0]
            self.assertGreaterEqual(length, 8)
            self.assertLessEqual(offset + length, len(data))
            image = data[offset + 8:offset + length]
            if image.startswith(b"\x89PNG"):
                sizes.add(png_size(image))
            offset += length
        self.assertTrue({(128, 128), (256, 256), (512, 512), (1024, 1024)} <= sizes)

    def test_android_launcher_and_adaptive_foreground(self):
        namespace = "{http://schemas.android.com/apk/res/android}"
        application = ET.parse(ROOT / "android/AndroidManifest.xml").find("application")
        self.assertEqual(application.get(namespace + "icon"), "@mipmap/ic_launcher")
        self.assertEqual(application.get(namespace + "roundIcon"), "@mipmap/ic_launcher")
        resources = ROOT / "android/res"
        for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                              ("xxhdpi", 144), ("xxxhdpi", 192)):
            self.assertEqual(png_size((resources / f"mipmap-{density}/ic_launcher.png").read_bytes()),
                             (size, size))
        adaptive = ET.parse(resources / "mipmap-anydpi-v26/ic_launcher.xml").getroot()
        self.assertEqual(adaptive.tag, "adaptive-icon")
        self.assertEqual(adaptive.find("foreground").get(namespace + "drawable"),
                         "@drawable/ic_launcher_foreground")
        self.assertEqual(png_size((resources / "drawable-nodpi/ic_launcher_foreground.png").read_bytes()),
                         (432, 432))
