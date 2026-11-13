"""Copy regular APK contents into a separate tree per split and hash each file."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import tempfile
import zipfile


WINDOWS_DEVICE_NAME = re.compile(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]|CONIN\$|CONOUT\$",
                                 re.IGNORECASE)


def copy_member(archive, member, root):
    parts = member.filename.rstrip("/").split("/")
    if not all(re.fullmatch(r"[\w.$+@%=-]+", part, re.ASCII) and
               not part.endswith(".") and not WINDOWS_DEVICE_NAME.fullmatch(part.split(".")[0])
               for part in parts):
        raise ValueError(f"Unsafe archive member: {member.filename!r}")
    if stat.S_ISLNK(member.external_attr >> 16):
        raise ValueError(f"Symlink in APK: {member.filename!r}")
    destination = root.joinpath(*parts)
    if member.is_dir():
        destination.mkdir(parents=True, exist_ok=True)
        return None
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with archive.open(member) as source, destination.open("xb") as output:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
            output.write(chunk)
    return {"path": member.filename, "bytes": member.file_size,
            "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apks", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    apks = sorted(args.apks.glob("*.apk"))
    if not any(apk.name == "base.apk" for apk in apks):
        parser.error("base.apk is missing")
    if args.output.exists():
        parser.error("output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        stage = Path(temporary) / "extracted"
        stage.mkdir()
        inventory = []
        for apk in apks:
            with apk.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            with zipfile.ZipFile(apk) as archive:
                members = [copy_member(archive, member, stage / apk.stem)
                           for member in archive.infolist()]
            inventory.append({"apk": apk.name, "bytes": apk.stat().st_size,
                              "sha256": digest,
                              "members": [member for member in members if member]})
            print(f"{apk.name}: {len(members)} entries, SHA-256 {digest}", flush=True)
        (stage / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n",
                                                encoding="utf-8")
        stage.rename(args.output)


if __name__ == "__main__":
    main()
