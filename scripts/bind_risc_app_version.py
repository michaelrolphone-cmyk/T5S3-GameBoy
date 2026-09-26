#!/usr/bin/env python3
"""Bind the RiscRTE app manifest version to the validated firmware release."""
from __future__ import annotations

import argparse
import configparser
import json
import re
from pathlib import Path

VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")


def read_platformio_version(path: Path) -> str:
    config = configparser.ConfigParser(interpolation=None)
    if not config.read(path):
        raise ValueError(f"missing PlatformIO config: {path}")
    try:
        version = config.get("version", "version")
    except (configparser.NoSectionError, configparser.NoOptionError) as exc:
        raise ValueError("platformio.ini must define [version] Version") from exc
    if not VERSION_RE.fullmatch(version):
        raise ValueError("platformio.ini version must be MAJOR.MINOR.PATCH")
    return version


def bind_version(manifest_path: Path, version: str, *, check: bool = False) -> None:
    if not VERSION_RE.fullmatch(version):
        raise ValueError("app release version must be MAJOR.MINOR.PATCH")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("RiscRTE app manifest must be a JSON object")
    current = manifest.get("version")
    if check:
        if current != version:
            raise ValueError(f"RiscRTE app version {current!r} does not match firmware release {version}")
        return
    manifest["version"] = version
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path("riscrte/gameboy.json"))
    parser.add_argument("--platformio", type=Path, default=Path("platformio.ini"))
    parser.add_argument("--version", help="explicit version override; defaults to platformio.ini")
    parser.add_argument("--check", action="store_true", help="fail unless the manifest already has this version")
    args = parser.parse_args()
    try:
        version = args.version or read_platformio_version(args.platformio)
        bind_version(args.manifest, version, check=args.check)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(str(exc))
    print(f"RiscRTE app manifest version matches firmware release {version}")


if __name__ == "__main__":
    main()
