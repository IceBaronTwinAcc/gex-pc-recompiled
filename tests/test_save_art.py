from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SaveArtworkTests(unittest.TestCase):
    def test_ten_selections_preserve_bright_indicators(self):
        data = (ROOT / "assets/save-menu.bin").read_bytes()
        self.assertEqual(data[:8], b"GEXART1\0")
        palette = struct.unpack_from("<256H", data, 8)
        states = []
        for state in range(10):
            image = bytearray(data[520:77320])
            at = struct.unpack_from("<I", data, 77320 + state * 4)[0]
            while True:
                offset = struct.unpack_from("<I", data, at)[0]
                at += 4
                if offset == 0xFFFFFFFF:
                    break
                count = struct.unpack_from("<H", data, at)[0]
                at += 2
                self.assertGreater(count, 0)
                self.assertLessEqual(offset + count, 76800)
                image[offset:offset + count] = data[at:at + count]
                at += count
            states.append(bytes(image))
        self.assertEqual(len(set(states)), 10)
        for state, x, y in ((0, 186, 60), (3, 184, 104), (6, 182, 141)):
            color = palette[states[state][y * 320 + x]]
            self.assertGreater((color >> 5) & 31, 20, "selected PLAY green was quantized away")
            self.assertLess(color & 31, 5)
            self.assertLess((color >> 10) & 31, 5)
