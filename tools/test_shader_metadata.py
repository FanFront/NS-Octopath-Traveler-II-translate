import struct
import unittest
from shader_metadata import compute_metadata, validate_compute_binary


class ComputeMetadataTest(unittest.TestCase):
    def test_histogram_layout_and_independent_assembly(self):
        control = bytearray(0x880)
        struct.pack_into('<I', control, 0x744, 0)
        struct.pack_into('<IIII', control, 0x748, 8, 4, 1, 8192)
        self.assertEqual(compute_metadata(control, 'GROUP_SIZE 8 4;\nSHARED_MEMORY 8192;'),
                         dict(local=[8, 4, 1], shared=8192))
        struct.pack_into('<I', control, 0x744, 1024)
        self.assertEqual(compute_metadata(control)['shared'], 8192)

    def test_disagreement_is_rejected(self):
        control = bytearray(0x880)
        struct.pack_into('<IIII', control, 0x748, 8, 4, 1, 4096)
        with self.assertRaises(ValueError):
            compute_metadata(control, 'GROUP_SIZE 8 4;\nSHARED_MEMORY 8192;')

    def test_binary_allocation_must_match_control(self):
        def shader(elements):
            words = [0x07230203, 0x00010300, 0, 10, 0]
            for op, args in [(16, [1, 17, 8, 4, 1]), (21, [2, 32, 0]),
                             (43, [2, 3, elements]), (28, [4, 2, 3]),
                             (32, [5, 4, 4]), (59, [5, 6, 4])]:
                words += [((len(args) + 1) << 16) | op] + args
            return struct.pack(f'<{len(words)}I', *words)
        job = dict(key='histogram', local=[8, 4, 1], shared=8192)
        with self.assertRaises(ValueError):
            validate_compute_binary(job, shader(1024))
        validate_compute_binary(job, shader(2048))


if __name__ == '__main__':
    unittest.main()
