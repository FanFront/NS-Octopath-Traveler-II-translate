"""NVN compute launch metadata shared by offline and captured shader jobs."""
import re
import struct


def compute_metadata(control, assembly=None):
    if len(control) != 0x880:
        raise ValueError('Expected an NVN 0x880-byte shader control block')
    local = list(struct.unpack_from('<III', control, 0x748))
    shared = struct.unpack_from('<I', control, 0x754)[0]
    if not all(local) or shared % 4:
        raise ValueError('Invalid NVN compute dimensions or shared-memory size')
    # The preserved assembly is an independent cross-check of the control
    # layout. 0x744 is not shared memory; reading it silently selected the
    # translator's 4 KiB fallback, even for the game's 8 KiB histogram.
    if assembly is not None:
        match = re.search(r'^SHARED_MEMORY (\d+);$', assembly, re.M)
        expected = int(match[1]) if match else None
        # The compiler can introduce scratch storage for subgroup operations
        # even when the input assembly has no SHARED_MEMORY declaration.
        if expected is not None and shared < expected:
            raise ValueError(f'NVN shared memory disagrees with assembly: {shared} != {expected}')
        group = re.search(r'^GROUP_SIZE ([\d ]+);$', assembly, re.M)
        if group:
            expected_local = [int(n) for n in group[1].split()]
            expected_local += [1] * (3 - len(expected_local))
            if local != expected_local:
                raise ValueError('NVN workgroup dimensions disagree with assembly')
    return dict(local=local, shared=shared)


def spirv_compute_metadata(binary):
    """Read LocalSize and the sizes of Workgroup variables from validated SPIR-V."""
    words = struct.unpack(f'<{len(binary) // 4}I', binary)
    types, constants, variables = {}, {}, []
    local = None
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 65535
        if not count or i + count > len(words):
            raise ValueError('Malformed SPIR-V instruction')
        operands = words[i + 1:i + count]
        if opcode == 16 and operands[1] == 17:  # OpExecutionMode LocalSize
            local = list(operands[2:5])
        elif opcode in (21, 22, 23, 24, 28, 30, 32):
            types[operands[0]] = (opcode, operands[1:])
        elif opcode == 43:  # OpConstant
            constants[operands[1]] = operands[2]
        elif opcode == 59 and operands[2] == 4:  # OpVariable Workgroup
            variables.append(operands[0])
        i += count

    def size(type_id):
        op, args = types[type_id]
        if op in (21, 22):
            return args[0] // 8
        if op in (23, 24):
            return size(args[0]) * args[1]
        if op == 28:
            return size(args[0]) * constants[args[1]]
        if op == 30:
            return sum(size(t) for t in args)
        if op == 32:
            return size(args[1])
        raise ValueError('Unsupported Workgroup storage type')

    return dict(local=local, shared=sum(size(t) for t in variables))


def validate_compute_binary(job, binary):
    actual = spirv_compute_metadata(binary)
    if actual['local'] != job['local']:
        raise ValueError(f"{job['key']}: SPIR-V workgroup differs from NVN metadata")
    # An optimized shader may not use its reserved shared memory at all.
    if actual['shared'] and actual['shared'] != job['shared']:
        raise ValueError(f"{job['key']}: shared memory {actual['shared']} != NVN {job['shared']}")
