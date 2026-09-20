#!/usr/bin/env python3
"""Resolve an Actions dispatch or opt-in release request against platformio.ini."""

import configparser
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG_PATTERN = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


def resolve(event_name, event, root=ROOT):
    if event_name == "workflow_dispatch":
        tag = event.get("inputs", {}).get("tag", "")
        enabled = True
    elif event_name == "push":
        request = json.loads((root / ".github" / "release-request.json").read_text(encoding="utf-8"))
        if set(request) != {"enabled", "tag"}:
            raise ValueError("Release request must contain exactly enabled and tag")
        enabled, tag = request["enabled"], request["tag"]
    else:
        raise ValueError(f"Unsupported release event: {event_name}")

    if type(enabled) is not bool or not isinstance(tag, str):
        raise ValueError("Invalid release request field types")
    if not enabled:
        return {"publish": "false", "tag": "", "version": ""}
    if not TAG_PATTERN.fullmatch(tag):
        raise ValueError("Release tag must look like v1.2.3")
    if os.getenv("GITHUB_REF") != "refs/heads/master":
        raise ValueError("GameBoy releases must be cut from master")

    config = configparser.ConfigParser(interpolation=None)
    if not config.read(root / "platformio.ini"):
        raise ValueError("Missing platformio.ini")
    version = config.get("version", "version")
    if tag != "v" + version:
        raise ValueError(f"Requested tag {tag} does not match platformio.ini version {version}")
    return {"publish": "true", "tag": tag, "version": version}


def main():
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    outputs = resolve(os.environ["GITHUB_EVENT_NAME"], event)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
        for key, value in outputs.items():
            stream.write(f"{key}={value}\n")
    print("GameBoy release request validated" if outputs["publish"] == "true" else "Release request disabled; no publication")


if __name__ == "__main__":
    main()
