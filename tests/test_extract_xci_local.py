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

    def test_modified_program_opt_in_only(self):
        entry = self.content("d" * 32, b"original fixture", 1)
        rec = record(0x80, TITLE, 0, [entry])
        path = self.ncas / ("d" * 32 + ".nca")
        path.write_bytes(b"modified fixture")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 1, self.ncas, set())
        self.assertEqual(
            xci.content_path(rec, 1, self.ncas, set(),
                             allow_modified_program=True), path)

    def test_modified_control_still_rejected(self):
        entry = self.content("e" * 32, b"original fixture", 3)
        rec = record(0x81, "0100a3501946e800", 131072, [entry])
        (self.ncas / ("e" * 32 + ".nca")).write_bytes(b"modified fixture")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 3, self.ncas, set(),
                             allow_modified_program=True)

    def test_modified_program_size_still_rejected(self):
        entry = self.content("f" * 32, b"original fixture", 1)
        rec = record(0x80, TITLE, 0, [entry])
        (self.ncas / ("f" * 32 + ".nca")).write_bytes(b"changed length!")
        with self.assertRaisesRegex(ValueError, "unexpected NCA"):
            xci.content_path(rec, 1, self.ncas, set(),
                             allow_modified_program=True)

    def test_modified_dlc_requires_separate_opt_in(self):
        entry = self.content("1" * 32, b"original DLC data", 2)
        rec = record(0x82, "0100a3501946f001", 0, [entry])
        path = self.ncas / ("1" * 32 + ".nca")
        path.write_bytes(b"modified DLC data")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 2, self.ncas, set(), allow_modified_program=True)
        accepted = set()
        self.assertEqual(xci.content_path(
            rec, 2, self.ncas, set(), allow_modified_dlc_data=True,
            modified_dlc_names=accepted), path)
        self.assertEqual(accepted, {path.name})

    def test_modified_dlc_still_checks_size(self):
        entry = self.content("2" * 32, b"original DLC data", 2)
        rec = record(0x82, "0100a3501946f001", 0, [entry])
        (self.ncas / ("2" * 32 + ".nca")).write_bytes(b"damaged")
        with self.assertRaisesRegex(ValueError, "unexpected NCA"):
            xci.content_path(rec, 2, self.ncas, set(),
                             allow_modified_dlc_data=True)

    def test_modified_dlc_flag_cannot_bypass_control_hash(self):
        entry = self.content("3" * 32, b"original Control", 3)
        rec = record(0x81, "0100a3501946e800", 131072, [entry])
        (self.ncas / ("3" * 32 + ".nca")).write_bytes(b"modified Control")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 3, self.ncas, set(),
                             allow_modified_program=True,
                             allow_modified_dlc_data=True)

    def test_modified_dlc_flag_cannot_bypass_non_dlc_data(self):
        entry = self.content("4" * 32, b"original data", 2)
        rec = record(0x80, TITLE, 0, [entry])
        (self.ncas / ("4" * 32 + ".nca")).write_bytes(b"modified data")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            xci.content_path(rec, 2, self.ncas, set(),
                             allow_modified_dlc_data=True)

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

    def test_empty_dlc_reports_file_count_without_accepting_it(self):
        base_program = self.content("a" * 32, b"base program", 1)
        update_program = self.content("b" * 32, b"update program", 1)
        control = self.content("c" * 32, b"control record", 3)
        dlc_data = self.content("d" * 32, b"dlc public data", 2)
        base = record(0x80, TITLE, 0, [base_program])
        patch_record = record(0x81, "0100a3501946e800", 131072,
                              [update_program, control])
        dlc = record(0x82, "0100a3501946f001", 0, [dlc_data])

        def fake_hactool(binary, keys, *args):
            if "--listromfs" in args:
                return "Visiting romfs\\nrom:file-one\\nrom:file-two\\n"
            if any(str(x).startswith("--exefsdir=") for x in args):
                exe = Path(next(str(x).split("=", 1)[1] for x in args
                                if str(x).startswith("--exefsdir=")))
                exe.mkdir(parents=True)
                (exe / "main").write_bytes(b"NSO0")
                romfs = Path(next(str(x).split("=", 1)[1] for x in args
                                  if str(x).startswith("--romfsdir=")))
                romfs.mkdir(parents=True)
                (romfs / "test.pak").write_bytes(b"pak")
                return ""
            dirs = [str(x).split("=", 1)[1] for x in args
                    if str(x).startswith("--romfsdir=")]
            if dirs and "AddOnContent" in dirs[0]:
                return ("Title ID: 0100a3501946f001\\n"
                        "Partition Type: RomFS\\n")
            if dirs:
                control_root = Path(dirs[0])
                control_root.mkdir(parents=True)
                (control_root / "control.nacp").write_bytes(bytes(0x3070))
            return ""

        with patch.object(xci, "inventory", return_value=[base, patch_record, dlc]), \\
             patch.object(xci, "hactool", side_effect=fake_hactool):
            with self.assertRaisesRegex(ValueError, r"reported 2 file entries"):
                xci.extract(self.ncas, self.stage, "hactool", Path("local.keys"),
                            TITLE, 131072)

    def test_reject_missing_update(self):
        base_program = self.content("a" * 32, b"base-program", 1)
        base = record(0x80, TITLE, 0, [base_program])
        with patch.object(xci, "inventory", return_value=[base]):
            with self.assertRaises(ValueError):
                xci.extract(self.ncas, self.stage, "hactool", Path("local.keys"),
                            TITLE, 131072, include_dlc=False)


if __name__ == "__main__":
    unittest.main()
