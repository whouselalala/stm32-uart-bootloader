import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("upgrade", Path(__file__).resolve().parents[1] / "tools" / "upgrade.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


class TestProtocol(unittest.TestCase):
    def test_crc_and_frame(self):
        self.assertEqual(upgrade.crc16(b"123456789"), 0x4B37)
        packet = upgrade.frame(2, 7, b"abc")
        self.assertEqual(packet[:7], b"BL" + struct.pack("<BHH", 2, 7, 3))
        self.assertEqual(upgrade.crc16(packet), 0)

    def test_image_vectors(self):
        image = struct.pack("<II", 0x20020000, upgrade.APP_ADDR + 9) + bytes(32)
        upgrade.check_image(image)
        with self.assertRaises(ValueError):
            upgrade.check_image(struct.pack("<II", 0x10000000, upgrade.APP_ADDR + 9))


if __name__ == "__main__":
    unittest.main()
