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
PROFILES = {"basic", "mutations", "delete", "crash", "restart", "crash-restart"}
EXTERNAL_PROFILES = {"basic", "mutations", "delete", "crash", "restart", "crash-restart"}
RESTART = {"restart", "crash-restart"}
EXTERNAL = {"source_protocol", "client_process", "host_self_io_rejected", "client_flush_callback_observed"}
RESTART_EXTRA = {"state_digest", "source_ack_digest", "resumer_process"}
HEX = re.compile(r"[0-9a-f]{64}\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")

def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)

def integer(item: dict, key: str, minimum: int, maximum: int) -> None:
    value = item[key]
    require(type(value) is int and minimum <= value <= maximum, f"invalid {key}")

def validate_pair(writer: dict, verifier: dict, profile: str, commit: str, *, require_external: bool = False) -> dict:
    require(profile in PROFILES and SHA.fullmatch(commit) is not None, "invalid selected scope")
    extended = profile != "basic"
    external = type(writer) is dict and bool(set(writer) & EXTERNAL)
    require(not external or profile in EXTERNAL_PROFILES, "external protocol on wrong profile")
    require(not require_external or external, "live selected campaign requires an external client")
    fields = BASE | (EXTRA if extended else set()) | (EXTERNAL if external else set())
    expected_phase = "ack" if profile in {"crash", "crash-restart"} else "state" if extended else "write"
    size, inode, count = {"basic": (7001, 2, 1), "mutations": (89, 2, 2),
                          "delete": (37, 3, 1), "crash": (7001, 2, 1),
                          "restart": (7001, 2, 1), "crash-restart": (7001, 2, 1)}[profile]
    for item, phase in ((writer, expected_phase), (verifier, "verify")):
        required = fields | (RESTART_EXTRA if profile in RESTART and phase == "verify" else set())
        require(type(item) is dict and set(item) == required, "missing/unknown report fields")
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
        if external:
            require(item["source_protocol"] == "external-client", "unknown source actor protocol")
            integer(item, "client_process", 1, 2**32 - 1)
            require(item["client_process"] != item["source_process"]
                    and item["client_process"] != item["observer_process"], "filesystem host/verifier was its own client")
            for key in ("host_self_io_rejected", "client_flush_callback_observed"):
                require(type(item[key]) is bool and item[key] == (phase != "verify"),
                        "unobserved external client/flush claim")
        if extended:
            require(item["profile"] == profile, "mixed profile")
            integer(item, "file_count", 2 if profile in RESTART and phase == "verify" else count,
                    2 if profile in RESTART and phase == "verify" else count)
            for key in ("creation_time", "last_access_time", "last_write_time", "change_time"):
                integer(item, key, 1, 2**63 - 1)
            integer(item, "attributes", 0, 2**32 - 1)
            integer(item, "reservation", size, 16 * 1024 * 1024)
            require(item["reservation"] % 4096 == 0, "unaligned reservation")
            for key in ("data_digest", "security_digest", "keep_digest"):
                require(type(item[key]) is str and HEX.fullmatch(item[key]) is not None, f"invalid {key}")
            require(item["data_digest"] != "0" * 64 and item["security_digest"] != "0" * 64,
                    "missing data/security integrity")
            require((item["keep_digest"] != "0" * 64) == (profile == "mutations" or (profile in RESTART and phase == "verify")), "missing/foreign kept data")
    require(writer["source_process"] == writer["observer_process"]
            == verifier["source_process"], "source process identity changed")
    # The source child has actually exited before the independent verifier is
    # launched by the C++ supervisor. PID inequality alone is not retirement.
    if external:
        require(writer["client_process"] == verifier["client_process"], "source client identity changed")
    stable = {"user_bytes", "inode", "namespace_sequence", "generation", "image_bytes", "image_cap"}
    if extended:
        stable |= EXTRA - {"profile"}
    if profile in RESTART:
        stable = {"user_bytes", "inode", "image_cap", "creation_time", "attributes", "reservation",
                  "data_digest", "security_digest", "last_write_time", "change_time"}
        require(all(verifier[key] > writer[key] for key in ("namespace_sequence", "generation", "image_bytes")),
                "continuation did not append/publish")
        integer(verifier, "resumer_process", 1, 2**32 - 1)
        if external:
            require(verifier["resumer_process"] != writer["client_process"], "resumer aliased source client")
        for key in ("state_digest", "source_ack_digest"):
            require(type(verifier[key]) is str and HEX.fullmatch(verifier[key]) is not None
                    and verifier[key] != "0" * 64, "invalid restart integrity receipt")
    require(all(writer[key] == verifier[key] for key in stable), "acknowledged state changed/rolled back")
    return {"schema": 1, "commit": commit, "profile": profile, "complete": True,
            "image_bytes": verifier["image_bytes"], "generation": verifier["generation"],
            "namespace_sequence": verifier["namespace_sequence"], "user_bytes": size}

def validate_continuation(writer: dict, continued: dict, verifier: dict, profile: str, commit: str, *, require_external: bool = False) -> dict:
    require(profile in RESTART, "not a restart profile")
    result = validate_pair(writer, verifier, profile, commit, require_external=require_external)
    fields = {"schema", "profile", "phase", "commit", "source_process", "observer_process",
              "source_ack_digest", "continued_state_digest", "generation", "namespace_sequence", "image_bytes"}
    require(type(continued) is dict and set(continued) == fields, "partial/foreign continuation receipt")
    integer(continued, "schema", 1, 1)
    require(continued["profile"] == profile and continued["phase"] == "continued"
            and continued["commit"] == commit, "wrong continuation source/profile")
    for key in ("source_process", "observer_process"):
        integer(continued, key, 1, 2**32 - 1)
    require(continued["source_process"] == writer["source_process"]
            and continued["observer_process"] == verifier["resumer_process"], "continuation process binding changed")
    for key in ("generation", "namespace_sequence", "image_bytes"):
        integer(continued, key, 1, CAP if key == "image_bytes" else 2**63 - 1)
        require(continued[key] == verifier[key], "continuation checkpoint changed after resumed-owner exit")
    for key in ("source_ack_digest", "continued_state_digest"):
        require(type(continued[key]) is str and HEX.fullmatch(continued[key]) is not None
                and continued[key] != "0" * 64, "invalid continuation digest")
    require(continued["source_ack_digest"] == verifier["source_ack_digest"]
            and continued["continued_state_digest"] == verifier["state_digest"],
            "continuation receipt is not the independently observed state")
    return result

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

def read_restart_records(path: Path) -> list[dict]:
    size = regular(path).st_size
    require(0 < size <= REPORT_CAP, "restart report byte budget")
    data = path.read_bytes()
    require(len(data) == size and b"\0" not in data, "restart report changed/NUL")
    lines = data.decode("utf-8").splitlines()
    require(len(lines) == 2 and all(lines), "exactly source and continuation records required")
    return [json.loads(line, object_pairs_hook=unique_object) for line in lines]

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=sorted(PROFILES | {"ram-handoff"}), required=True)
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
    if args.profile == "ram-handoff":
        require({entry.name for entry in fixture.iterdir()} == {"ram-handoff.json"},
                "RAM fixture contains an image, mount or unexpected output")
        report = read_report(fixture / "ram-handoff.json")
        exact = {
            "schema": 1, "phase": "ram-handoff", "commit": os.environ["GITHUB_SHA"],
            "epoch": 2, "user_bytes": 65609, "locked_peak": 16 * 1024**2 + 512 * 1024,
            "locked_after": 0, "contexts": 0, "cpu_verified": True,
            "mounted": True, "unmounted": True, "recovered_capacity_failure": True,
            "gpu": False, "complete": True,
            "stale_candidate_rejected": True, "repeat_suppressed": True,
        }
        measured = {"source_process", "volume_serial", "raw_payload", "compressed_payload", "generation", "idle_copy_steps"}
        require(type(report) is dict and set(report) == set(exact) | measured,
                "unexpected RAM handoff report fields")
        for key, value in exact.items():
            require(type(report[key]) is type(value) and report[key] == value, f"invalid {key}")
        for key in ("source_process", "volume_serial"):
            integer(report, key, 1, 2**32 - 1)
        integer(report, "idle_copy_steps", 2, 256)
        integer(report, "raw_payload", 65536, CAP)
        integer(report, "compressed_payload", 1, report["raw_payload"] - 1)
        integer(report, "generation", 1, 2**63 - 1)
        require(fixture.name == f"flitzfs-write-probe-{report['source_process']}", "fixture process binding")
        print("Windows RAM handoff reconciliation: " + json.dumps(report, sort_keys=True))
        return
    names = ("write.json", "verify.json") if args.profile == "basic" else ("state.json", "state-verify.json")
    if args.profile in RESTART:
        source, continued = read_restart_records(fixture / names[0])
        final = read_report(fixture / names[1])
        reports = [source, final]
        result = validate_continuation(source, continued, final, args.profile, os.environ["GITHUB_SHA"],
                                       require_external=args.profile in EXTERNAL_PROFILES)
    else:
        reports = [read_report(fixture / name) for name in names]
        result = validate_pair(*reports, args.profile, os.environ["GITHUB_SHA"],
                               require_external=args.profile in EXTERNAL_PROFILES)
    require(fixture.name == f"flitzfs-write-probe-{reports[0]['source_process']}", "fixture process binding")
    image_bytes = regular(fixture / "native.fltx").st_size
    require(image_bytes == result["image_bytes"] and image_bytes
            + sum((fixture / name).stat().st_size for name in names) <= CAP + 2 * REPORT_CAP,
            "image/aggregate byte budget")
    print("Windows writable reconciliation: " + json.dumps(result, sort_keys=True))

if __name__ == "__main__":
    main()
