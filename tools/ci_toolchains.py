#!/usr/bin/env python3
"""Fetch checksum-locked build inputs; install legacy Vita library archives."""
import hashlib
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath

LOCK = Path(__file__).with_name("toolchains.json")


def read_lock():
    lock = json.loads(LOCK.read_text())
    if lock["version"] != 1:
        raise ValueError("Unknown toolchain lock format")
    for name in ("pspsdk_ref", "vdpm_ref"):
        if not re.fullmatch(r"[0-9a-f]{40}", lock[name]):
            raise ValueError(f"Invalid {name}")
    for name, entry in lock["downloads"].items():
        if not re.fullmatch(r"[a-z0-9_]+", name) or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]):
            raise ValueError(f"Invalid lock entry: {name}")
        if not entry["url"].startswith("https://") or any(c.isspace() for c in entry["url"]):
            raise ValueError(f"Invalid URL: {name}")
    return lock


def download(name, destination):
    entry = read_lock()["downloads"][name]
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    try:
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--show-error",
                        "--header", "Accept: application/octet-stream",
                        "--output", str(temporary), entry["url"]], check=True)
        with temporary.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != entry["sha256"]:
            raise ValueError(f"Checksum mismatch: {name}")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def install_vita_library(archive, sdk):
    """Legacy packages use either include/lib roots or an arm-vita-eabi root."""
    target = Path(sdk) / "arm-vita-eabi"
    target.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError("Unsafe library archive path")
            parts = list(path.parts)
            if "arm-vita-eabi" in parts:
                parts = parts[parts.index("arm-vita-eabi") + 1:]
            if not parts or parts[0] in (".PKGINFO", ".BUILDINFO", ".MTREE"):
                continue
            if parts[0] not in ("include", "lib", "share", "bin"):
                if member.isdir():
                    continue
                raise ValueError(f"Unsupported library archive layout: {member.name}")
            member.name = "/".join(parts)
            members.append(member)
        if not any(m.isfile() and m.name.startswith("lib/") for m in members):
            raise ValueError("Library archive contains no library")
        tar.extractall(target, members=members, filter="data")


def main():
    command, *args = sys.argv[1:]
    if command == "check":
        read_lock()
        print("Toolchain lock is valid")
    elif command == "env":
        lock = read_lock()
        print(f"PSPSDK_REF={lock['pspsdk_ref']}")
        print(f"VDPM_REF={lock['vdpm_ref']}")
        print(f"VITASDK_BOOTSTRAP_URL={lock['downloads']['vitasdk']['url']}")
        print(f"VITASDK_BOOTSTRAP_SHA256={lock['downloads']['vitasdk']['sha256']}")
    elif command == "download":
        download(*args)
    elif command == "vita-library":
        install_vita_library(*args)
    else:
        raise ValueError("Unknown command")


if __name__ == "__main__":
    main()
