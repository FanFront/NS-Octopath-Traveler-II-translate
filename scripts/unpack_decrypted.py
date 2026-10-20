#!/usr/bin/env python3
"""Unpack an R2 decrypted-tree TAR/ZIP into a safe local game directory.

Accepts ExeFS/, RomFS/, optional Control/ and AddOnContent/, possibly under
one wrapper directory. Raw XCI/NCA images are deliberately unsupported.
"""
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tarfile
import zipfile

PARTS = {"ExeFS", "RomFS", "Control", "AddOnContent"}
MAX_BYTES = 30 * 1024**3
MAX_ENTRIES = 150000


def output_path(root, name):
    if "\\" in name or "\0" in name or name.startswith("/"):
        raise ValueError("Unsafe archive path")
    parts = PurePosixPath(name).parts
    if any(p in (".", "..") for p in parts):
        raise ValueError("Unsafe archive path")
    if parts and parts[0] not in PARTS and parts[0] != "manifest.json":
        # Allow exactly one enclosing directory.
        parts = parts[1:]
    if not parts or (parts[0] not in PARTS and parts != ("manifest.json",)):
        raise ValueError("Unexpected entry in decrypted archive")
    if parts[0] == "manifest.json" and len(parts) != 1:
        raise ValueError("Invalid manifest name")
    return root.joinpath(*parts)


def unpack(archive, dest):
    if dest.exists() and any(dest.iterdir()):
        raise ValueError("Output must be empty")
    dest.mkdir(parents=True, exist_ok=True)
    processed = 0
    total = 0

    def add(name, is_dir, open_source, size):
        nonlocal processed, total
        processed += 1
        if processed > MAX_ENTRIES:
            raise ValueError("Archive has too many entries")
        if is_dir and len(PurePosixPath(name).parts) == 1 and PurePosixPath(name).parts[0] not in PARTS:
            return
        path = output_path(dest, name)
        if is_dir:
            path.mkdir(parents=True, exist_ok=True)
            return
        total += size
        if total > MAX_BYTES:
            raise ValueError("Decrypted archive too large")
        path.parent.mkdir(parents=True, exist_ok=True)
        with open_source() as src, path.open("xb") as dst:
            shutil.copyfileobj(src, dst, 4 * 1024 * 1024)

    name = archive.name.lower()
    if name.endswith((".tar.gz", ".tgz", ".tar")):
        with tarfile.open(archive, "r:*") as src:
            for member in src:
                if not (member.isfile() or member.isdir()):
                    raise ValueError("Archive links and special entries not supported")
                if member.isdir():
                    add(member.name, True, None, 0)
                else:
                    add(member.name, False, lambda item=member: src.extractfile(item), member.size)
    elif name.endswith(".zip"):
        with zipfile.ZipFile(archive) as src:
            for member in src.infolist():
                if stat.S_IFMT(member.external_attr >> 16) == stat.S_IFLNK:
                    raise ValueError("ZIP links are not supported")
                add(member.filename, member.is_dir(),
                    lambda item=member: src.open(item), member.file_size)
    else:
        raise ValueError("Raw .xci cannot be built without decrypted ExeFS/RomFS. Upload a .tar.gz or .zip of your already-decrypted game directory to R2.")

    for part in ("ExeFS", "RomFS"):
        if not (dest / part).is_dir():
            raise ValueError("Decrypted archive must include " + part)
    if not (dest / "ExeFS" / "main").is_file():
        raise ValueError("Decrypted archive is missing ExeFS/main")
    print("Decrypted input prepared successfully")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("Usage: unpack_decrypted.py INPUT.tar.gz OUTPUT_DIRECTORY")
    try:
        unpack(Path(sys.argv[1]), Path(sys.argv[2]))
    except (ValueError, OSError, tarfile.TarError, zipfile.BadZipFile) as exc:
        sys.exit("Error: " + str(exc))
