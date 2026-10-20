#!/usr/bin/env python3
"""Read-only, small-range XCI structural probe. Does NOT decrypt NCAs."""
import argparse
import os
from pathlib import Path
import struct
import sys


class LocalReader:
    def __init__(self, path):
        self.path = Path(path)
        self.size = self.path.stat().st_size

    def read(self, pos, count):
        if pos < 0 or count < 0 or pos + count > self.size:
            raise ValueError("Read exceeds XCI size")
        with self.path.open("rb") as f:
            f.seek(pos)
            result = f.read(count)
        if len(result) != count:
            raise ValueError("Truncated XCI")
        return result


class R2Reader:
    def __init__(self, key):
        import boto3  # only needed for --r2-key
        account = os.environ.get("R2_ACCOUNT_ID", "")
        bucket = os.environ.get("R2_BUCKET", "")
        if not account or not bucket or not os.environ.get("AWS_ACCESS_KEY_ID") or not os.environ.get("AWS_SECRET_ACCESS_KEY"):
            raise ValueError("Missing R2_ACCOUNT_ID, R2_BUCKET, or AWS credentials")
        if not key:
            raise ValueError("Empty R2 key")
        self.client = boto3.client("s3", endpoint_url="https://" + account + ".r2.cloudflarestorage.com",
                                   region_name="auto")
        self.bucket = bucket
        self.key = key
        self.size = self.client.head_object(Bucket=bucket, Key=key)["ContentLength"]

    def read(self, pos, count):
        if pos < 0 or count < 0 or pos + count > self.size:
            raise ValueError("Read exceeds XCI size")
        response = self.client.get_object(Bucket=self.bucket, Key=self.key,
                                          Range=f"bytes={pos}-{pos + count - 1}")
        with response["Body"] as stream:
            data = stream.read(count + 1)
        if len(data) != count:
            raise ValueError("R2 did not return the requested byte range")
        return data


def parse_hfs0(reader, start, limit=None):
    head = reader.read(start, 16)
    magic, count, strings, _ = struct.unpack("<4sIII", head)
    if magic != b"HFS0" or count > 10000 or strings > 1024 * 1024:
        raise ValueError("Invalid HFS0 partition header")
    header_size = 16 + count * 64 + strings
    if header_size > 2 * 1024 * 1024:
        raise ValueError("Oversized HFS0 table")
    data = reader.read(start, header_size)
    table_start = 16 + 64 * count
    entries = []
    for index in range(count):
        offset, size, name_offset = struct.unpack_from("<QQI", data, 16 + 64 * index)
        if name_offset >= strings:
            raise ValueError("Invalid HFS0 filename")
        string_start = table_start + name_offset
        end = data.find(b"\0", string_start, table_start + strings)
        if end < 0:
            raise ValueError("Unterminated HFS0 filename")
        name = data[string_start:end].decode("utf-8", "replace")
        physical = start + header_size + offset
        if physical + size > reader.size or (limit is not None and physical + size > limit):
            raise ValueError("HFS0 entry exceeds available data")
        entries.append({"name": name, "start": physical, "size": size})
    return entries


def probe(reader):
    if reader.size < 0x200:
        raise ValueError("XCI is too small")
    h = reader.read(0, 0x200)
    if h[0x100:0x104] != b"HEAD":
        raise ValueError("No HEAD magic at 0x100; unsupported/nonstandard XCI or damaged file")
    root_offset, root_size = struct.unpack_from("<QQ", h, 0x130)
    if root_offset < 0x200 or root_offset >= reader.size or root_size > 2 * 1024 * 1024:
        raise ValueError("Invalid XCI root partition offset")
    root = parse_hfs0(reader, root_offset)
    secure = next((x for x in root if x["name"] == "secure"), None)
    result = {
        "kind": "Nintendo Switch XCI (structural)",
        "size_gib": round(reader.size / (1024 ** 3), 2),
        "root_partitions": [x["name"] for x in root],
        "secure_partition_present": bool(secure),
    }
    if secure is not None:
        files = parse_hfs0(reader, secure["start"], secure["start"] + secure["size"])
        ncas = [e for e in files if e["name"].lower().endswith(".nca")]
        result["nca_count"] = len(ncas)
        result["ticket_count"] = sum(e["name"].lower().endswith(".tik") for e in files)
        result["nca_headers_checked"] = min(3, len(ncas))
        magic = [reader.read(e["start"] + 0x200, 4) if e["size"] >= 0x204 else b"" for e in ncas[:3]]
        result["plaintext_nca3_headers_found"] = sum(x in (b"NCA3", b"NCA2", b"NCA0") for x in magic)
        if magic and all(x not in (b"NCA3", b"NCA2", b"NCA0") for x in magic):
            result["assessment"] = "NCA header samples are not plaintext. Likely encrypted; XCI alone is insufficient."
        elif magic:
            result["assessment"] = "At least one NCA header looks plaintext. This does NOT establish that NCA content is decrypted."
        else:
            result["assessment"] = "No .nca files identified in the secure partition."
    else:
        result["assessment"] = "No secure partition found; check XCI format."
    result["can_build_apk"] = False
    result["next_step"] = ("A full extraction to ExeFS/RomFS and a matching v1.1.1 main Build ID must "
                           "be verified before Android build; this read-only probe does not perform decryption.")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--local", type=Path)
    src.add_argument("--r2-key")
    args = parser.parse_args()
    reader = LocalReader(args.local) if args.local else R2Reader(args.r2_key)
    data = probe(reader)
    for k, v in data.items():
        print(k + ": " + str(v))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        # Do not print raw HTTP or SDK errors that may contain private bucket information.
        print("XCI probe failed: " + type(exc).__name__ + ": " +
              (str(exc) if isinstance(exc, (ValueError, OSError)) else "could not read R2 object"), file=sys.stderr)
        sys.exit(1)
