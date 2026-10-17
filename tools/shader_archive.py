"""Bounded UE4 version-2 ShaderArchive reader; no game-specific offsets.

Serialization layout reference (format only, independently implemented):
https://github.com/FabianFG/CUE4Parse/blob/master/CUE4Parse/UE4/Shaders/FSerializedShaderArchive.cs
"""
import struct
import lz4.block

MAX_CODE = 16 * 1024 * 1024


def archive_entries(data):
    if len(data) < 4 or struct.unpack_from('<I', data)[0] != 2:
        raise ValueError('Only UE4 ShaderArchive version 2 is supported')
    cursor = 4
    arrays = []
    for width in (20, 20, 16, 17, 16, 4):
        if cursor + 4 > len(data):
            raise ValueError('Truncated shader archive table')
        count = struct.unpack_from('<I', data, cursor)[0]
        cursor += 4
        end = cursor + count * width
        if count > 2_000_000 or end > len(data):
            raise ValueError('Shader archive array exceeds file')
        arrays.append((cursor, count))
        cursor = end
    maps, hashes, map_entries, code_entries, preloads, indices = arrays
    if maps[1] != map_entries[1] or hashes[1] != code_entries[1]:
        raise ValueError('Shader archive hash/entry count mismatch')
    for index in range(map_entries[1]):
        start, count, preload, preload_count = struct.unpack_from('<IIII', data, map_entries[0] + index * 16)
        if start + count > indices[1] or preload + preload_count > preloads[1]:
            raise ValueError('Shader map range exceeds index/preload array')
    for index in range(indices[1]):
        if struct.unpack_from('<I', data, indices[0] + index * 4)[0] >= hashes[1]:
            raise ValueError('Shader index out of range')
    payload_size = len(data) - cursor
    for index in range(preloads[1]):
        offset, size = struct.unpack_from('<qq', data, preloads[0] + index * 16)
        if offset < 0 or size < 0 or offset + size > payload_size:
            raise ValueError('Shader preload range exceeds payload')
    spans = []
    records = []
    for index in range(code_entries[1]):
        offset, size, expanded, frequency = struct.unpack_from('<QIIB', data, code_entries[0] + index * 17)
        if not 0 < size <= expanded <= MAX_CODE or frequency > 5 or offset + size > payload_size:
            raise ValueError('Invalid shader code range/size/frequency')
        records.append((offset, size, expanded, frequency))
        spans.append((offset, offset + size))
    spans.sort()
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError('Overlapping shader code entries')
    for offset, size, expanded, frequency in records:
        payload = data[cursor + offset:cursor + offset + size]
        if size < expanded:
            payload = lz4.block.decompress(payload, uncompressed_size=expanded)
        if len(payload) != expanded or not payload.startswith(b'LSLG'):
            raise ValueError('Unsupported shader payload/codec')
        yield cursor + offset, frequency, payload
