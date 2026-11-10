"""Reconcile bounded, source-bound READ-ONLY Windows space observations.

Secondary API errors are retained evidence, not a supported-compression claim.
This executable is hosted-Windows-only. Unit tests exercise pure validators.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat
import sys

MAX_REPORT_BYTES = 8192
PHASES = ("normal", "crash", "recovery")
UUID = "4e000000000000000000000000000057"
FILES = {"sample": (4, 192 * 1024 + 17), "empty": (6, 0), "sparse": (7, (1 << 32) + 65536)}
HEADER = {
    "schema", "phase", "commit", "filesystem", "source_uuid",
    "native_namespace_sequence", "native_image_bytes", "native_generation", "backing_standard_allocation_bytes",
    "native_committed_end", "native_live_decoded_bytes", "native_live_stored_payload_bytes",
    "volume_serial", "volume_flags", "volume_read_only", "volume_file_compression",
    "maximum_component_characters", "allocation_unit_bytes", "total_clusters", "free_clusters",
    "total_bytes", "free_bytes", "caller_available_bytes", "records", "record_count", "complete",
}
RECORD = {
    "file", "inode", "logical_bytes", "standard_allocation_bytes", "standard_eof_bytes",
    "standard_links", "compression_info_ok", "compression_info_error", "compression_info_bytes",
    "compression_format", "compression_unit_shift", "compression_chunk_shift",
    "compression_cluster_shift", "get_compressed_low", "get_compressed_high", "get_compressed_last_error",
}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def integer(value: object, maximum: int, name: str) -> int:
    require(type(value) is int and 0 <= value <= maximum, f"invalid integer {name}")
    return value


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate field {key}")
        result[key] = value
    return result


def decode_report(raw: bytes, commit: str) -> dict:
    require(0 < len(raw) <= MAX_REPORT_BYTES, "report byte budget")
    require(bool(re.fullmatch(r"[0-9a-f]{40}", commit)), "expected source commit")
    try:
        report = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("invalid report JSON/UTF-8") from error
    require(type(report) is dict and set(report) == HEADER, "report field set")
    require(report["schema"] == 1 and type(report["schema"]) is int, "report schema")
    require(report["phase"] in PHASES and report["commit"] == commit, "report phase/source binding")
    require(report["filesystem"] == "FlitzFS" and report["source_uuid"] == UUID, "wrong source identity")
    require(report["complete"] is True and report["record_count"] == 3
            and type(report["record_count"]) is int, "incomplete report")
    for key in HEADER - {"schema", "phase", "commit", "filesystem", "source_uuid", "records",
                         "complete", "volume_read_only", "volume_file_compression"}:
        integer(report[key], (1 << 64) - 1, key)
    require(type(report["volume_read_only"]) is bool and type(report["volume_file_compression"]) is bool,
            "invalid volume feature observations")
    require(report["native_namespace_sequence"] == 3 and report["native_generation"] > 0, "native state identity")
    require(4096 <= report["native_image_bytes"] == report["native_committed_end"] <= 1536 * 1024,
            "fixture physical/image bound")
    require(0 < report["native_live_stored_payload_bytes"] < report["native_live_decoded_bytes"] <= 16 * 1024 * 1024,
            "fixture must actually contain reduced stored payload")
    require(report["volume_serial"] == 0x5700004E and report["maximum_component_characters"] == 100,
            "wrong volume serial/name contract")
    integer(report["volume_flags"], (1 << 32) - 1, "volume flags")
    require(report["volume_read_only"] == bool(report["volume_flags"] & 0x00080000)
            and report["volume_file_compression"] == bool(report["volume_flags"] & 0x00000010),
            "volume flags/booleans disagree with current Microsoft definitions")
    require(report["volume_read_only"] and not report["volume_file_compression"],
            "selected driver/adapter feature presentation changed")
    require(report["allocation_unit_bytes"] == 4096, "wrong mounted geometry")
    require(report["total_bytes"] == report["native_image_bytes"]
            and report["total_clusters"] * 4096 == report["total_bytes"],
            "read-only volume totals differ from backing-length presentation")
    require(report["free_clusters"] == report["free_bytes"] == report["caller_available_bytes"] == 0,
            "read-only volume advertised writable free space")
    require(type(report["records"]) is list and len(report["records"]) == 3, "missing record")
    seen = set()
    for record in report["records"]:
        require(type(record) is dict and set(record) == RECORD, "file observation field set")
        name = record["file"]
        require(type(name) is str and name in FILES and name not in seen, "wrong/duplicate file observation")
        seen.add(name)
        for key in RECORD - {"file", "compression_info_ok"}:
            integer(record[key], (1 << 64) - 1, key)
        require(type(record["compression_info_ok"]) is bool, "compression-info result type")
        inode, size = FILES[name]
        require(record["inode"] == inode and record["logical_bytes"] == record["standard_eof_bytes"] == size,
                "file inode/EOF mismatch")
        require(record["standard_allocation_bytes"] == (size + 4095) // 4096 * 4096,
                "current rounded-logical allocation contract changed")
        require(record["standard_links"] == 1, "unexpected file link count")
        for key in ("get_compressed_low", "get_compressed_high", "get_compressed_last_error", "compression_info_error"):
            integer(record[key], (1 << 32) - 1, key)
        integer(record["compression_format"], 65535, "compression format")
        for key in ("compression_unit_shift", "compression_chunk_shift", "compression_cluster_shift"):
            integer(record[key], 255, key)
        if record["compression_info_ok"]:
            require(record["compression_info_error"] == 0, "successful compression query carries failure")
            require(record["compression_info_bytes"] <= (1 << 63) - 1, "signed compressed size bound")
        else:
            require(record["compression_info_error"] > 0, "failed compression query lacks error")
            require(all(record[key] == 0 for key in ("compression_info_bytes", "compression_format",
                    "compression_unit_shift", "compression_chunk_shift", "compression_cluster_shift")),
                    "failed compression query fabricated output")
    require(seen == set(FILES), "file observation closure")
    return report


def reconcile(reports: list[dict], commit: str) -> dict:
    require(len(reports) == 3, "three complete phases required")
    by_phase = {}
    for report in reports:
        validated = decode_report(json.dumps(report).encode(), commit)
        require(validated["phase"] not in by_phase, "duplicate report phase")
        by_phase[validated["phase"]] = validated
    require(set(by_phase) == set(PHASES), "phase closure")
    normal = by_phase["normal"]
    stable_header = HEADER - {"phase", "records"}
    for phase in PHASES:
        current = by_phase[phase]
        require(all(current[key] == normal[key] for key in stable_header), "mixed native/volume state between phases")
        wanted = {item["file"]: item for item in normal["records"]}
        for item in current["records"]:
            # LastError is captured raw after GetCompressedFileSizeW. On a
            # successful call it need not be meaningful or phase-stable.
            stable = RECORD - {"get_compressed_low", "get_compressed_high", "get_compressed_last_error"}
            baseline = wanted[item["file"]]
            require(all(item[key] == baseline[key] for key in stable),
                    "file/secondary API observations changed across remount")
            # Do not compare an output high word that may be undefined on
            # failure. Raw low/high/error triplets are retained for every phase.
            if baseline["get_compressed_low"] != (1 << 32) - 1:
                require(item["get_compressed_low"] == baseline["get_compressed_low"]
                        and item["get_compressed_high"] == baseline["get_compressed_high"],
                        "non-sentinel compressed-size return changed across remount")
    return {
        "schema": 1, "commit": commit, "phases": list(PHASES), "records_per_phase": 3,
        "complete": True, "native_image_bytes": normal["native_image_bytes"],
        "backing_standard_allocation_bytes": normal["backing_standard_allocation_bytes"],
        "native_live_decoded_bytes": normal["native_live_decoded_bytes"],
        "native_live_stored_payload_bytes": normal["native_live_stored_payload_bytes"],
        "volume_flags": normal["volume_flags"], "volume_read_only": normal["volume_read_only"],
        "volume_file_compression": normal["volume_file_compression"],
        "observations": normal["records"],
        "get_compressed_raw": {
            name: [{key: next(item[key] for item in by_phase[phase]["records"] if item["file"] == name)
                    for key in ("get_compressed_low", "get_compressed_high", "get_compressed_last_error")}
                   for phase in PHASES] for name in FILES
        },
        "qualification": "hosted Server read-only observations; no writable/client/GPU qualification",
    }


def main() -> None:
    require(sys.argv[1:] == ["--hosted"], "explicit hosted reporting mode required")
    require(os.name == "nt" and os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
            and os.environ.get("RUNNER_OS") == "Windows"
            and os.environ.get("GITHUB_REPOSITORY") == "strmt7/FlitzFS", "hosted Windows repository required")
    commit = os.environ.get("GITHUB_SHA", "")
    root = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    paths = sorted(root.glob("flitzfs-probe-*/space-*.json"))
    require(len(paths) == 3, "exactly three owned phase reports required")
    reports = []
    for path in paths:
        info, parent = path.stat(), path.parent.stat()
        require(path.parent.parent.resolve() == root and not path.is_symlink() and not path.parent.is_symlink()
                and not (getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)
                and not (getattr(parent, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)
                and stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and 0 < info.st_size <= MAX_REPORT_BYTES,
                "unsafe report fixture/file")
        with path.open("rb") as stream:
            raw = stream.read(MAX_REPORT_BYTES + 1)
        report = decode_report(raw, commit)
        require(path.name == f"space-{report['phase']}.json", "report filename/phase mismatch")
        reports.append(report)
    print("Windows space reconciliation: " + json.dumps(reconcile(reports, commit), sort_keys=True))


if __name__ == "__main__":
    main()
