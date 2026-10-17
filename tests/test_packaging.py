import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from s2a import bundle, packages
from tools.game_package import parse_cnmt, pfs_entries, copy_entry

TITLE = '0100a3501946e000'


def record(kind, version=0, index=0):
    title = int(TITLE, 16) + (0x800 if kind == 0x81 else 0x1000 + index if kind == 0x82 else 0)
    return dict(title_id=f'{title:016x}', application_id=TITLE, type=kind, version=version, required_version=0, contents=[])


class Packaging(unittest.TestCase):
    def test_select_metadata_not_names(self):
        base, old, new = record(0x80), record(0x81, 1), record(0x81, 3)
        a, b, c = record(0x82, 0, 1), record(0x82, 2, 1), record(0x82, 0, 9)
        chosen = packages.select([c, old, base, b, new, a])
        self.assertIs(chosen[1], new); self.assertEqual(list(chosen[2]), [1, 9]); self.assertIs(chosen[2][1], b)
        self.assertIs(packages.select([base, old, new], version=1)[1], old)
        self.assertFalse(packages.select([base, a], include_dlc=False)[2])

    def test_refuse_ambiguity_or_missing_update(self):
        for records in [[record(0x80), record(0x80)], [record(0x80), record(0x81, 1), record(0x81, 1)], [record(0x81, 1)]]:
            with self.assertRaises(ValueError): packages.select(records)
        with self.assertRaises(ValueError): packages.select([record(0x80)], version=9)
        dlc = record(0x82, 0, 1); dlc['required_version'] = 7
        with self.assertRaises(ValueError): packages.select([record(0x80), dlc])
        with self.assertRaises(ValueError): packages.select([record(0x80), record(0x84)])

    def test_cnmt_size_is_48_bit(self):
        data = bytearray(32+56)
        struct.pack_into('<QIBBHH', data, 0, int(TITLE, 16), 2, 0x80, 0, 0, 1)
        data[32+48:32+54] = (2**40+23).to_bytes(6, 'little')
        self.assertEqual(parse_cnmt(data)['contents'][0]['size'], 2**40+23)
        with self.assertRaises(ValueError): parse_cnmt(data[:-1])

    def test_pfs_paths_and_content_integrity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); path = root / 'input.nsp'
            def make(name, offset=0, size=4):
                names=name.encode()+b'\0'
                path.write_bytes(struct.pack('<4sIII', b'PFS0',1,len(names),0)+struct.pack('<QQII',offset,size,0,0)+names+b'data')
            make('../secret.nca')
            with self.assertRaises(ValueError): pfs_entries(path)
            make('safe.nca', size=9)
            with self.assertRaises(ValueError): pfs_entries(path)
            make('safe.nca'); entry = pfs_entries(path)[0]
            with self.assertRaises(ValueError): copy_entry(path, entry, root/'out.nca', '0'*64)
            self.assertFalse((root/'out.nca').exists())
            copy_entry(path, entry, root/'out.nca', hashlib.sha256(b'data').hexdigest())
            self.assertEqual((root/'out.nca').read_bytes(), b'data')

    def test_bundle_cli_integrity_and_traversal(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); path=root/'test.s2a'
            def make(name, content=b'data', expected=None, duplicate=False):
                records=[dict(path=name,size=len(content),sha256=expected or hashlib.sha256(content).hexdigest())]
                if duplicate: records*=2
                raw=json.dumps(dict(schema=1,title_id=TITLE,total_bytes=len(content)*len(records),files=records)).encode()
                with zipfile.ZipFile(path,'w') as z:
                    z.writestr('index.json',raw)
                    for r in records: z.writestr(r['path'],content)
                return hashlib.sha256(raw).hexdigest()
            identity=make('game/RomFS/data')
            self.assertEqual(bundle.verify(path,identity),identity)
            result=subprocess.run([sys.executable,'switch2android.py','verify',str(path)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for name in ['../escape','game/../../escape','/absolute','game//data','game\\evil','game/./data']:
                make(name)
                with self.assertRaises(ValueError): bundle.verify(path)
            make('game/RomFS/data',expected='0'*64)
            with self.assertRaises(ValueError): bundle.verify(path)
            make('game/RomFS/data')
            with self.assertRaises(ValueError): bundle.verify(path,'0'*64)

    def test_install_reuses_verified_device_content_without_local_bundle(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/'game.apk').write_bytes(b'local apk fixture')
            metadata=dict(application_id='org.switch2android.g'+TITLE, bundle_id='e'*64,
                          embedded=False, apk_sha256=bundle.sha256(root/'game.apk'))
            (root/'package.json').write_text(json.dumps(metadata))
            adb=root/'adb-fixture'
            adb.write_text('#!/usr/bin/env python3\nimport sys\nfrom pathlib import Path\n'
                           'with Path(__file__).with_suffix(".log").open("a") as f: f.write(" ".join(sys.argv[1:])+"\\n")\n'
                           'if "head" in sys.argv: print("e"*64,end="")\n')
            adb.chmod(0o700)
            result=subprocess.run([sys.executable,'switch2android.py','install',str(root),'--serial','fixture','--adb',str(adb)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('no resource transfer',result.stdout)
            log=adb.with_suffix('.log').read_text()
            self.assertIn('install -r',log); self.assertNotIn('push',log); self.assertIn('am start',log)

    def test_cli_rejects_rom_work_in_source_tree(self):
        result=subprocess.run([sys.executable,'switch2android.py','build','.', '-k','missing.keys','--work','.', '-o','.'],capture_output=True,text=True)
        self.assertEqual(result.returncode,1)
        self.assertIn('outside the source repository',result.stderr)


if __name__ == '__main__': unittest.main()
