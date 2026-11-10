#!/usr/bin/env python3
"""Strict native writable-trial evidence closure. Hosted execution; no repair."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import stat

CAP = 4 * 1024 * 1024
REPORT_CAP = 2048
BASE = {"schema", "phase", "commit", "source_process", "observer_process",
        "user_bytes", "inode", "namespace_sequence", "generation", "image_bytes",
        "image_cap", "contexts", "explicit_flush_observed", "cpu_verified",
        "dispatcher_stopped", "gpu"}
EXTRA = {"profile", "file_count", "creation_time", "last_access_time", "last_write_time", "change_time", "attributes", "reservation",
         "data_digest", "security_digest", "keep_digest"}
PROFILES = {"basic", "mutations", "delete", "crash"}
HEX = re.compile(r"[0-9a-f]{64}\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")

def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)

def integer(item: dict, key: str, minimum: int, maximum: int) -> None:
    value = item[key]
    require(type(value) is int and minimum <= value <= maximum, f"invalid {key}")

def validate_pair(writer: dict, verifier: dict, profile: str, commit: str) -> dict:
    require(profile in PROFILES and SHA.fullmatch(commit) is not None, "invalid selected scope")
    extended = profile != "basic"
    fields = BASE | (EXTRA if extended else set())
    expected_phase = "ack" if profile == "crash" else "state" if extended else "write"
    size, inode, count = {"basic": (7001, 2, 1), "mutations": (89, 2, 2),
                          "delete": (37, 3, 1), "crash": (7001, 2, 1)}[profile]
    for item, phase in ((writer, expected_phase), (verifier, "verify")):
        require(type(item) is dict and set(item) == fields, "missing/unknown report fields")
        integer(item, "schema", 2 if extended else 1, 2 if extended else 1)
        require(item["phase"] == phase and item["commit"] == commit, "wrong phase/source")
        for key in ("source_process", "observer_process"):
            integer(item, key, 1, 2**32 - 1)
        integer(item, "user_bytes", size, size)
        integer(item, "inode", inode, inode)
        integer(item, "namespace_sequence", 1, 2**63 - 1)
        integer(item, "generation", 1, 2**63 - 1)
        integer(item, "image_bytes", 131072, CAP)
        integer(item, "image_cap", CAP, CAP)
        integer(item, "contexts", 1 if phase == "ack" else 0, 256 if phase == "ack" else 0)
        for key in ("explicit_flush_observed", "cpu_verified", "dispatcher_stopped", "gpu"):
            require(type(item[key]) is bool, f"non-boolean {key}")
        require(item["explicit_flush_observed"] == (phase != "verify") and item["cpu_verified"]
                and item["dispatcher_stopped"] == (phase not in {"ack", "verify"})
                and not item["gpu"], "invalid flush/observer/retirement claim")
        if extended:
            require(item["profile"] == profile, "mixed profile")
            integer(item, "file_count", count, count)
            for key in ("creation_time", "last_access_time", "last_write_time", "change_time"):
                integer(item, key, 1, 2**63 - 1)
            integer(item, "attributes", 0, 2**32 - 1)
            integer(item, "reservation", size, 16 * 1024 * 1024)
            require(item["reservation"] % 4096 == 0, "unaligned reservation")
            for key in ("data_digest", "security_digest", "keep_digest"):
                require(type(item[key]) is str and HEX.fullmatch(item[key]) is not None, f"invalid {key}")
            require(item["data_digest"] != "0" * 64 and item["security_digest"] != "0" * 64,
                    "missing data/security integrity")
            require((item["keep_digest"] != "0" * 64) == (profile == "mutations"), "missing/foreign kept data")
    require(writer["source_process"] == writer["observer_process"]
            == verifier["source_process"], "source process identity changed")
    # The source child has actually exited before the independent verifier is
    # launched by the C++ supervisor. PID inequality alone is not retirement.
    stable = {"user_bytes", "inode", "namespace_sequence", "generation", "image_bytes", "image_cap"}
    if extended:
        stable |= EXTRA - {"profile"}
    require(all(writer[key] == verifier[key] for key in stable), "acknowledged state changed/rolled back")
    return {"schema": 1, "commit": commit, "profile": profile, "complete": True,
            "image_bytes": writer["image_bytes"], "generation": writer["generation"],
            "namespace_sequence": writer["namespace_sequence"], "user_bytes": size}

def regular(path: Path) -> os.stat_result:
    result = path.lstat()
    require(stat.S_ISREG(result.st_mode) and result.st_nlink == 1
            and not (getattr(result, "st_file_attributes", 0) & 0x400), "unsafe report/image type")
    return result

def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result

def read_report(path: Path) -> dict:
    size = regular(path).st_size
    require(0 < size <= REPORT_CAP, "report byte budget")
    data = path.read_bytes()
    require(len(data) == size and b"\0" not in data, "report changed/NUL")
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=sorted(PROFILES), required=True)
    args = parser.parse_args()
    require(os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
            and os.environ.get("RUNNER_OS") == "Windows", "disposable hosted Windows only")
    root = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    fixtures = list(root.glob("flitzfs-write-probe-*"))
    require(len(fixtures) == 1, "exactly one owned writable fixture required")
    fixture = fixtures[0]
    meta = fixture.lstat()
    require(stat.S_ISDIR(meta.st_mode) and not (getattr(meta, "st_file_attributes", 0) & 0x400)
            and fixture.resolve(strict=True).parent == root, "unsafe fixture")
    names = ("write.json", "verify.json") if args.profile == "basic" else ("state.json", "state-verify.json")
    reports = [read_report(fixture / name) for name in names]
    result = validate_pair(*reports, args.profile, os.environ["GITHUB_SHA"])
    require(fixture.name == f"flitzfs-write-probe-{reports[0]['source_process']}", "fixture process binding")
    image_bytes = regular(fixture / "native.fltx").st_size
    require(image_bytes == result["image_bytes"] and image_bytes
            + sum((fixture / name).stat().st_size for name in names) <= CAP + 2 * REPORT_CAP,
            "image/aggregate byte budget")
    print("Windows writable reconciliation: " + json.dumps(result, sort_keys=True))

if __name__ == "__main__":
    main()
