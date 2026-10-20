"""Tests that do not require games, keys, or Android SDK."""
import tempfile
import unittest
from pathlib import Path

from s2a import extracted

TITLE = "0100a3501946e000"
BUILD_ID = "0d9649011312f62e93f6a5673a09d7e900000000000000000000000000000000"
PROFILE = {
    "title_id": TITLE, "name": "OCTOPATH TRAVELER II", "versions": [131072],
    "main_build_ids": [BUILD_ID], "resource_adapter": "unreal",
}


class ExtractedTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.src = self.root / "game"
        (self.src / "ExeFS").mkdir(parents=True)
        (self.src / "RomFS").mkdir(parents=True)
        header = bytearray(256)
        header[:4] = b"NSO0"
        header[64:96] = bytes.fromhex(BUILD_ID)
        self.main = self.src / "ExeFS/main"
        self.main.write_bytes(header + b"test fixture")
        (self.src / "RomFS/game.pak").write_bytes(b"test pak placeholder")

    def plan(self):
        return extracted.validate(self.src, TITLE, 131072, PROFILE)

    def test_matching_build_and_import(self):
        plan = self.plan()
        self.assertEqual(plan["main_build_id"], BUILD_ID)
        output = self.root / "work/game"
        extracted.import_game(self.src, output, plan)
        self.assertEqual((output / "ExeFS/main").read_bytes(), self.main.read_bytes())
        self.assertEqual(extracted.import_game(self.src, output, plan)["id"], plan["id"])

    def test_wrong_build_id_rejected(self):
        data = bytearray(self.main.read_bytes())
        data[64] ^= 1
        self.main.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "build ID"):
            self.plan()

    def test_missing_pak_rejected(self):
        (self.src / "RomFS/game.pak").unlink()
        with self.assertRaises(ValueError):
            self.plan()

    def test_manifest_mismatch_rejected(self):
        (self.src / "manifest.json").write_text('{"version":999}')
        with self.assertRaisesRegex(ValueError, "version mismatch"):
            self.plan()

    def test_symlink_rejected(self):
        (self.src / "RomFS/fake.pak").symlink_to(self.main)
        with self.assertRaisesRegex(ValueError, "symlinked file"):
            self.plan()

    def test_source_change_rejected(self):
        plan = self.plan()
        (self.src / "RomFS/game.pak").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed"):
            extracted.import_game(self.src, self.root / "work/game", plan)

    def test_dlc_index(self):
        (self.src / "AddOnContent/1").mkdir(parents=True)
        (self.src / "AddOnContent/1/dlc").write_bytes(b"fake DLC")
        self.assertEqual(self.plan()["manifest"]["dlc"], [{"index": 1}])


if __name__ == "__main__":
    unittest.main()
