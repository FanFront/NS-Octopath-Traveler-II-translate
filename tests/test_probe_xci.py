import struct
import tempfile
import unittest
from pathlib import Path

from scripts.probe_xci import LocalReader, parse_hfs0, probe


def hfs0(name, contents):
    names = name.encode() + b"\0"
    header = struct.pack("<4sIII", b"HFS0", 1, len(names), 0)
    entry = struct.pack("<QQIIQ", 0, len(contents), 0, 0, 0) + b"\0" * 32
    return header + entry + names + contents


class ProbeXciTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / "sample.xci"
        nca = bytearray(0x400)
        nca[0x200:0x204] = b"NCA3"
        secure = hfs0("test.nca", bytes(nca))
        root = hfs0("secure", secure)
        header = bytearray(0x200)
        header[0x100:0x104] = b"HEAD"
        struct.pack_into("<QQ", header, 0x130, 0xF000, 16 + 64 + 7)
        self.path.write_bytes(header + b"\0" * (0xF000 - len(header)) + root)

    def test_detect_plaintext_nca_header(self):
        report = probe(LocalReader(self.path))
        self.assertEqual(report["nca_count"], 1)
        self.assertEqual(report["plaintext_nca3_headers_found"], 1)
        self.assertFalse(report["can_build_apk"])

    def test_nonplaintext_header(self):
        data = bytearray(self.path.read_bytes())
        pos = data.find(b"NCA3")
        data[pos:pos+4] = b"test"
        self.path.write_bytes(data)
        report = probe(LocalReader(self.path))
        self.assertEqual(report["plaintext_nca3_headers_found"], 0)
        self.assertIn("Likely encrypted", report["assessment"])

    def test_invalid_xci_header(self):
        data = bytearray(self.path.read_bytes())
        data[0x100:0x104] = b"NOPE"
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "HEAD"):
            probe(LocalReader(self.path))

    def test_invalid_hfs0(self):
        with self.assertRaises(ValueError):
            parse_hfs0(LocalReader(self.path), 0x100)


if __name__ == "__main__":
    unittest.main()
