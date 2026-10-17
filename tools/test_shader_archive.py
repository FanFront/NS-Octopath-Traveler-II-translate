import struct
import unittest
import lz4.block
from shader_archive import archive_entries


def fixture(compressed):
    payload = b'LSLG' + b'synthetic shader' * 64
    code = lz4.block.compress(payload, store_size=False) if compressed else payload
    def array(width, records):
        assert all(len(r) == width for r in records)
        return struct.pack('<I', len(records)) + b''.join(records)
    data = struct.pack('<I', 2)
    data += array(20, [b'm' * 20]) + array(20, [b's' * 20])
    data += array(16, [struct.pack('<IIII', 0, 1, 0, 1)])
    data += array(17, [struct.pack('<QIIB', 0, len(code), len(payload), 5)])
    data += array(16, [struct.pack('<qq', 0, len(code))])
    data += array(4, [struct.pack('<I', 0)])
    return data + code, payload


class ShaderArchiveTest(unittest.TestCase):
    def test_raw_and_lz4(self):
        for compressed in (False, True):
            data, expected = fixture(compressed)
            rows = list(archive_entries(data))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0][1:], (5, expected))

    def test_all_truncations(self):
        data, _ = fixture(True)
        for size in range(len(data)):
            with self.assertRaises((ValueError, lz4.block.LZ4BlockError)):
                list(archive_entries(data[:size]))

    def test_invalid_tables(self):
        data, _ = fixture(True)
        for offset, value in [(0, 1), (4, 0xffffffff), (28, 2), (56, 1),
                              (80, 0xffffffff), (88, 0xffffffff), (116, 1)]:
            corrupt = bytearray(data)
            struct.pack_into('<I', corrupt, offset, value)
            with self.assertRaises((ValueError, lz4.block.LZ4BlockError)):
                list(archive_entries(corrupt))

    def test_reject_other_payload(self):
        data, _ = fixture(False)
        corrupt = data.replace(b'LSLG', b'NOPE')
        with self.assertRaises(ValueError):
            list(archive_entries(corrupt))


if __name__ == '__main__':
    unittest.main()
