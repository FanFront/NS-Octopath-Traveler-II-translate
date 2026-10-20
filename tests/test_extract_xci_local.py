"""Exercise XCI extraction glue with synthetic NCAs; no keys or games required."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import extract_xci_local as xci

TITLE = "0100a3501946e000"
BUILD_ID = "0d9649011312f62e93f6a5673a09d7e900000000000000000000000000000000"


def record(kind, title, version, contents):
    return dict(type=kind, title_id=title, application_id=TITLE,
                version=version, required_version=0, contents=contents)


class XCIExtractionTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        self.ncas = root / "ncas"
        self.ncas.mkdir()
        self.stage = root / "stage"
        self.stage.mkdir()

    def content(self, name, data, kind):
        path = self.ncas / (name + ".nca")
        path.write_bytes(data)
        return dict(id=name, size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                    type=kind, id_offset=0)

    def test_verify_nca_hash(self):
        entry = self.content("a" * 32, b"valid NCA fixture", 1)
        rec = record(0x80, TITLE, 0, [entry])
        self.assertEqual(xci.content_path(rec, 1, self.ncas, set()).name, "a" * 32 + ".nca")
        (self.ncas / ("a" * 32 + ".nca")).write_bytes(b"tampered fixture!")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 1, self.ncas, set())

    def test_base_and_update_program_extraction(self):
        base_program = self.content("a" * 32, b"base-program", 1)
        update_program = self.content("b" * 32, b"update-program", 1)
        control = self.content("c" * 32, b"update-control", 3)
        base = record(0x80, TITLE, 0, [base_program])
        patch_record = record(0x81, "0100a3501946e800", 131072,
                              [update_program, control])
        calls = []

        def fake_hactool(binary, keys, *args):
            calls.append(args)
            exefs_flags = [a.split("=", 1)[1] for a in args if a.startswith("--exefsdir=")]
            romfs_flags = [a.split("=", 1)[1] for a in args if a.startswith("--romfsdir=")]
            if exefs_flags:
                exefs = Path(exefs_flags[0])
                exefs.mkdir(parents=True)
                hdr = bytearray(256)
                hdr[:4] = b"NSO0"
                hdr[64:96] = bytes.fromhex(BUILD_ID)
                (exefs / "main").write_bytes(hdr)
                romfs = Path(romfs_flags[0])
                romfs.mkdir(parents=True)
                (romfs / "test.pak").write_bytes(b"fake")
            elif romfs_flags:
                control_dir = Path(romfs_flags[0])
                control_dir.mkdir(parents=True)
                nacp = bytearray(0x3070)
                nacp[:13] = b"OCTOPATH TEST"
                nacp[0x3060:0x3065] = b"1.1.1"
                (control_dir / "control.nacp").write_bytes(nacp)
            return ""

        with patch.object(xci, "inventory", return_value=[base, patch_record]), \
             patch.object(xci, "hactool", side_effect=fake_hactool):
            output = xci.extract(self.ncas, self.stage, "hactool", Path("local.keys"),
                                 TITLE, 131072, include_dlc=False)
        self.assertTrue((output / "ExeFS/main").exists())
        self.assertTrue((output / "RomFS/test.pak").exists())
        metadata = json.loads((output / "manifest.json").read_text())
        self.assertEqual(metadata["version"], 131072)
        self.assertEqual(metadata["title_id"], TITLE)
        self.assertTrue(any(any(str(a).startswith("--basenca=") for a in invocation)
                            for invocation in calls))

    def test_reject_missing_update(self):
        base_program = self.content("a" * 32, b"base-program", 1)
        base = record(0x80, TITLE, 0, [base_program])
        with patch.object(xci, "inventory", return_value=[base]):
            with self.assertRaises(ValueError):
                xci.extract(self.ncas, self.stage, "hactool", Path("local.keys"),
                            TITLE, 131072, include_dlc=False)


if __name__ == "__main__":
    unittest.main()
