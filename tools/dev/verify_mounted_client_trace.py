#!/usr/bin/env python3
"""Validate the client-side transcript from a populated Windows mount trial."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(path):
    with path.open("rb") as stream:
        data = stream.read(2 * 1024 * 1024 + 1)
    require(0 < len(data) <= 2 * 1024 * 1024 and data.endswith(b"\n"), "Invalid transcript size or end")
    rows = [json.loads(line) for line in data.splitlines()]
    files = {}
    phase = 0
    population = None
    counts = Counter()
    recovered = []
    statuses = {"enumerate": 18, "conflicting_open": 32, "lookup_deleted": 2}
    for number, row in enumerate(rows, 1):
        operation = row["operation"]
        name = row["path"]
        locus = f"record {number} ({operation}, {name})"
        require(row["id"] == number, f"Missing or repeated ID at {locus}")
        if row["cycle"] != phase:
            require(row["cycle"] == phase + 1 and len(recovered) == phase, f"Unfinished cycle at {locus}")
            phase += 1
        require(len(recovered) < phase, f"Unexpected operation after CPU verification at {locus}")
        require(row["status"] == statuses.get(operation, 0), f"Unexpected Win32 status at {locus}")
        counts[operation] += 1
        offset, requested, transferred = (row[key] for key in ("offset", "requested", "transferred"))
        require(all(type(v) is int and v >= 0 for v in (offset, requested, transferred)) and
                transferred <= requested, f"Invalid range at {locus}")
        if operation in ("write", "read", "held_read", "range_read", "cpu_verified"):
            require(requested == transferred and re.fullmatch(r"[0-9a-f]{64}", row.get("blake3", "")),
                    f"Missing complete byte witness at {locus}")
        match operation:
            case "mkdir":
                require(phase == 1 and name == "Population" and population is None,
                        f"Invalid population creation at {locus}")
                population = name + "/"
            case "create":
                require(name not in files, f"Duplicate create at {locus}")
                files[name] = 0
            case "rename_replace":
                target = row["target"]
                require(name != target and name in files and target in files, f"Rename missed a file at {locus}")
                files[target] = files.pop(name)
            case "unlink":
                require(name in files, f"Unlink missed a file at {locus}")
                del files[name]
            case "lookup_deleted":
                require(name not in files, f"Deleted name is present at {locus}")
            case "enumerate" | "cpu_verified":
                require(population == name + "/" and
                        requested == transferred == sum(file.startswith(population) for file in files),
                        f"Population count differs at {locus}")
                if operation == "cpu_verified":
                    recovered.append(row["blake3"])
            case "overwrite_open" | "write" | "read" | "held_read" | "range_read" | "flush_view" | \
                 "flush" | "flush_mapped" | "flush_cached" | "conflicting_open":
                require(name in files, f"Operation used a missing file at {locus}")
                if operation == "overwrite_open":
                    files[name] = 0
                elif operation == "write":
                    files[name] = max(files[name], offset + transferred)
                else:
                    require(offset + requested <= files[name], f"Range exceeds file at {locus}")
            case _:
                raise ValueError(f"Unknown operation at {locus}")
    require(phase == len(recovered) == 3 and len(set(recovered)) == 1, "Population changed or incomplete")
    require(counts["rename_replace"] == 1 and counts["conflicting_open"] == 1 and
            counts["unlink"] == counts["lookup_deleted"] == 2 and
            counts["enumerate"] == counts["range_read"] == counts["held_read"] == counts["flush_cached"] == 3 and
            counts["flush_view"] == counts["flush_mapped"] == 4, "Required mixed operations are absent")
    return {
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "records": len(rows),
        "files": len(files),
        "logical_bytes": sum(files.values()),
        "cycles": phase,
        "independent_population_blake3": recovered[0],
        "counts": dict(sorted(counts.items())),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transcripts", type=Path, nargs="+", help="Identical workload arms")
    args = parser.parse_args()
    try:
        reports = [validate(path) for path in args.transcripts]
        require(all(report == reports[0] for report in reports), "Client transcripts differ across workload arms")
    except (ValueError, KeyError, TypeError, OSError) as failure:
        parser.exit(1, f"{failure}\n")
    print(json.dumps(reports[0], sort_keys=True))


if __name__ == "__main__":
    main()
