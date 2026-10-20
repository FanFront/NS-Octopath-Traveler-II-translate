"""Verify and stage already-decrypted ExeFS/RomFS for the existing Android build."""
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path

from tools.game_profile import make_profile

PARTS = ("ExeFS", "RomFS", "Control", "AddOnContent")
BLOCK = 4 * 1024 * 1024


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def enumerate_files(source):
    result = []
    for part in PARTS:
        root = source / part
        if not root.exists() and not root.is_symlink():
            continue
        if root.is_symlink() or not root.is_dir():
            raise ValueError("Invalid or symlinked input folder: " + part)
        for directory, dirs, names in os.walk(root, followlinks=False):
            for name in dirs:
                if (Path(directory) / name).is_symlink():
                    raise ValueError("Directory symlinks are not supported")
            for name in names:
                path = Path(directory) / name
                if path.is_symlink() or not path.is_file():
                    raise ValueError("Unsupported or symlinked file: " + name)
                relative = path.relative_to(source)
                if "\\" in relative.as_posix() or any(p in (".", "..") for p in relative.parts):
                    raise ValueError("Unsafe path in decrypted input")
                result.append((relative.as_posix(), path))
    return sorted(result)


def validate(source, title, version, profile, display_version="1.1.1"):
    """Never infer version from a filename or silently bypass supported build IDs."""
    source = Path(source)
    if not re.fullmatch("[0-9a-f]{16}", title):
        raise ValueError("Invalid title ID")
    if title != profile["title_id"] or version not in profile["versions"]:
        raise ValueError("Unsupported title/version")
    if not (source / "ExeFS").is_dir() or not (source / "RomFS").is_dir():
        raise ValueError("Expected decrypted ExeFS/ and RomFS/ directories (not raw XCI/NCA)")
    items = enumerate_files(source)
    if not (source / "ExeFS" / "main").is_file():
        raise ValueError("Missing ExeFS/main")
    if not any(name.startswith("RomFS/") for name, _ in items):
        raise ValueError("Empty RomFS")

    external_manifest = source / "manifest.json"
    if external_manifest.exists() or external_manifest.is_symlink():
        if external_manifest.is_symlink():
            raise ValueError("Symlink manifest not allowed")
        original = json.loads(external_manifest.read_text())
        if not isinstance(original, dict) or original.get("title_id", title).lower() != title:
            raise ValueError("Input manifest title mismatch")
        if original.get("version", version) != version:
            raise ValueError("Input manifest version mismatch")

    detected = make_profile(source, title, version, display_version)
    main = next(m for m in detected["modules"] if m["name"] == "main")
    if main["build_id"] not in profile["main_build_ids"]:
        raise ValueError("Unsupported main NSO build ID; version 1.1.1 executable required")
    if detected["resource_adapter"] != profile["resource_adapter"]:
        raise ValueError("Game resource adapter mismatch; expected Unreal .pak")
    if not any(name.startswith("RomFS/") and name.lower().endswith(".pak") for name, _ in items):
        raise ValueError("Expected at least one Unreal .pak in RomFS")

    dlc = []
    addon = source / "AddOnContent"
    if addon.is_dir():
        for child in sorted(addon.iterdir()):
            if child.is_symlink() or not child.is_dir() or not re.fullmatch("[1-9][0-9]{0,3}", child.name) or int(child.name) > 2000:
                raise ValueError("Unsupported DLC layout")
            if not any(name.startswith("AddOnContent/" + child.name + "/") for name, _ in items):
                raise ValueError("Empty DLC folder")
            dlc.append({"index": int(child.name)})
    nacp = source / "Control" / "control.nacp"
    if nacp.is_file():
        with nacp.open("rb") as f:
            f.seek(0x3060)
            version_bytes = f.read(16).split(b"\0", 1)[0]
        if version_bytes:
            display_version = version_bytes.decode("utf-8", "replace")

    files = [{"path": name, "size": path.stat().st_size, "sha256": digest(path)} for name, path in items]
    identity = hashlib.sha256(json.dumps({
        "title_id": title, "version": version, "display_version": display_version,
        "build_id": main["build_id"], "dlc": dlc, "files": files,
    }, sort_keys=True).encode()).hexdigest()
    manifest = {
        "schema": 1, "id": identity, "title_id": title, "name": profile["name"],
        "version": version, "display_version": display_version,
        "dlc": dlc, "source": "decrypted",
    }
    return {"id": identity, "files": files, "manifest": manifest, "main_build_id": main["build_id"]}


def import_game(source, target, plan):
    """Atomic verified copy to the game's usual work/games cache."""
    source, target = Path(source), Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if json.loads((target / "manifest.json").read_text()).get("id") != plan["id"]:
            raise ValueError("Mismatched cached game identity")
        for record in plan["files"]:
            file = target / record["path"]
            if file.is_symlink() or not file.is_file() or file.stat().st_size != record["size"] or digest(file) != record["sha256"]:
                raise ValueError("Cached game content changed")
        return plan["manifest"]
    with tempfile.TemporaryDirectory(prefix=".decrypted-", dir=target.parent) as temp:
        stage = Path(temp) / "game"
        stage.mkdir()
        for record in plan["files"]:
            src = source / record["path"]
            dst = stage / record["path"]
            if src.is_symlink() or not src.is_file() or src.stat().st_size != record["size"]:
                raise ValueError("Decrypted source changed")
            dst.parent.mkdir(parents=True, exist_ok=True)
            h = hashlib.sha256()
            with src.open("rb") as inp, dst.open("xb") as out:
                while block := inp.read(BLOCK):
                    out.write(block)
                    h.update(block)
            if h.hexdigest() != record["sha256"]:
                raise ValueError("Decrypted source changed during copy")
        (stage / "manifest.json").write_text(json.dumps(plan["manifest"], indent=2) + "\n")
        stage.rename(target)
    return plan["manifest"]
