#!/usr/bin/env python3
"""Turn an XCI containing a supported base+update into decrypted ExeFS/RomFS.

Requires locally supplied decryption material. Never prints hactool's raw output
(which may contain keys), and never uploads keys or game content.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from s2a import packages
from tools.game_package import parse_cnmt
from tools.game_profile import make_profile

DEFAULT_TITLE = "0100a3501946e000"
DEFAULT_VERSION = 131072


def hactool(binary, keys, *arguments):
    # Even errors from upstream can contain sensitive key material.
    result = subprocess.run(
        [str(binary), "-k", str(keys), "--disablekeywarns", "--suppresskeys",
         *map(str, arguments)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise ValueError("hactool failed during XCI/NCA extraction (exit %d); "
                         "raw output suppressed to protect key material" % result.returncode)
    return result.stdout.decode("utf-8", "replace")


def sha256(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def inventory(directory, meta_tmp, binary, keys):
    records = []
    for index, nca in enumerate(sorted(directory.glob("*.cnmt.nca"))):
        root = meta_tmp / str(index)
        hactool(binary, keys, "-t", "nca", "--section0dir=" + str(root), nca)
        entries = list(root.glob("*.cnmt"))
        if len(entries) != 1:
            raise ValueError("Unable to read exactly one CNMT from " + nca.name)
        data = entries[0].read_bytes()
        record = parse_cnmt(data)
        record["required_version"] = (int.from_bytes(data[40:44], "little")
                                      if record["type"] == 0x82 else 0)
        records.append(record)
    if not records:
        raise ValueError("XCI secure partition contains no readable CNMT")
    return records


def content_path(record, kind, directory, verified, allow_modified_program=False):
    matches = [c for c in record["contents"] if c["type"] == kind]
    if len(matches) != 1 or matches[0]["id_offset"] != 0:
        raise ValueError("Expected exactly one matching NCA content in CNMT")
    item = matches[0]
    nca = directory / (item["id"] + ".nca")
    if nca.is_symlink() or not nca.is_file() or nca.stat().st_size != item["size"]:
        raise ValueError("Missing or unexpected NCA: " + nca.name)
    if nca.name not in verified:
        if sha256(nca) != item["sha256"]:
            if not (kind == 1 and allow_modified_program):
                raise ValueError("NCA hash mismatch against CNMT: " + nca.name +
                                 " (use --allow-modified-program-nca only if you trust this modification)")
            print("WARNING: Program NCA differs from its CNMT SHA-256: " + nca.name +
                  "; explicitly accepting modified executable/resource container", flush=True)
        verified.add(nca.name)
    return nca


def extract(ncas, staging, binary, keys, title, version, include_dlc=True,
            allow_modified_program=False):
    metadata_dir = staging / "metadata"
    metadata_dir.mkdir()
    records = inventory(ncas, metadata_dir, binary, keys)
    base, updated, dlcs = packages.select(records, title, version, include_dlc)
    if updated["version"] != version or updated is base:
        raise ValueError("Required game update is missing")
    if base["title_id"] != title:
        raise ValueError("Unexpected game title")
    verified = set()
    base_program = content_path(base, 1, ncas, verified,
                                allow_modified_program=allow_modified_program)
    update_program = content_path(updated, 1, ncas, verified,
                                  allow_modified_program=allow_modified_program)
    target = staging / "decrypted"
    target.mkdir()
    print("Combining base Program and v%d update..." % version, flush=True)
    hactool(binary, keys, "-t", "nca",
            "--basenca=" + str(base_program),
            "--exefsdir=" + str(target / "ExeFS"),
            "--romfsdir=" + str(target / "RomFS"), update_program)
    if not (target / "ExeFS/main").is_file() or not (target / "RomFS").is_dir():
        raise ValueError("Unable to extract complete ExeFS/main and RomFS")
    control_record = updated if any(c["type"] == 3 for c in updated["contents"]) else base
    control_nca = content_path(control_record, 3, ncas, verified)
    hactool(binary, keys, "-t", "nca",
            "--romfsdir=" + str(target / "Control"), control_nca)
    nacp = target / "Control/control.nacp"
    if not nacp.is_file() or nacp.stat().st_size < 0x3070:
        raise ValueError("Control NACP unavailable or truncated")
    raw = nacp.read_bytes()
    display = raw[0x3060:0x3070].split(b"\0")[0].decode("utf-8", "replace")
    name = next((raw[i * 0x300:i * 0x300 + 0x200].split(b"\0")[0]
                 .decode("utf-8", "replace") for i in range(16)
                 if raw[i * 0x300]), title)
    for index, record in dlcs.items():
        dlc_nca = content_path(record, 2, ncas, verified)
        dlc_dir = target / "AddOnContent" / str(index)
        hactool(binary, keys, "-t", "nca",
                "--romfsdir=" + str(dlc_dir), dlc_nca)
        if not dlc_dir.is_dir():
            raise ValueError("DLC extraction failed for index %d" % index)
    profile_path = Path(__file__).resolve().parents[1] / "profiles" / (title + ".json")
    profile = json.loads(profile_path.read_text())
    game = make_profile(target, title, version, display)
    main = next(m for m in game["modules"] if m["name"] == "main")
    if main["build_id"] not in profile["main_build_ids"]:
        raise ValueError("Unsupported main build ID: " + main["build_id"])
    if game["resource_adapter"] != profile["resource_adapter"]:
        raise ValueError("Game data does not match the supported engine profile")
    identifier = packages.selection_id((base, updated, dlcs))
    manifest = dict(schema=1, id=identifier, title_id=title,
                    name=name, version=version, display_version=display,
                    dlc=[dict(index=i, **packages.public_record(r))
                         for i, r in dlcs.items()])
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Verified title: %s; update: %d; main build ID: %s" %
          (title, version, main["build_id"]), flush=True)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xci", type=Path)
    parser.add_argument("--keys", type=Path, required=True,
                        help="local private hactool-compatible key file")
    parser.add_argument("--output", type=Path, required=True,
                        help="directory to create with decrypted ExeFS/RomFS")
    parser.add_argument("--hactool", default="hactool")
    parser.add_argument("--title-id", default=DEFAULT_TITLE)
    parser.add_argument("--version", type=int, default=DEFAULT_VERSION)
    parser.add_argument("--no-dlc", action="store_true")
    parser.add_argument(
        "--allow-modified-program-nca", action="store_true",
        help="opt-in: accept Program NCA SHA-256 mismatches only (for trusted voice patches); "
             "still require correct size, decryption, Title ID, v1.1.1 and main Build ID")
    args = parser.parse_args()
    xci, keys, output = args.xci.resolve(), args.keys.resolve(), args.output.resolve()
    if not xci.is_file() or xci.suffix.lower() != ".xci":
        parser.error("Input must be an existing .xci file")
    if not keys.is_file():
        parser.error("Local keys file not found")
    if output.exists():
        parser.error("Output directory already exists; refusing overwrite")
    if output == xci.parent or output == keys.parent:
        parser.error("Output must not contain XCI or keys")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ot2-xci-", dir=output.parent) as temporary:
        stage = Path(temporary)
        ncas = stage / "ncas"
        ncas.mkdir()
        print("Extracting XCI secure NCA containers...", flush=True)
        hactool(args.hactool, keys, "-t", "xci",
                "--securedir=" + str(ncas), xci)
        source = extract(ncas, stage, args.hactool, keys,
                         args.title_id.lower(), args.version,
                         include_dlc=not args.no_dlc,
                         allow_modified_program=args.allow_modified_program_nca)
        source.rename(output)
    print("Decrypted game ready at " + str(output), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError) as err:
        print("Error: " + str(err), file=sys.stderr)
        sys.exit(1)
