"""Stage the pinned AMD LZ4 kernel and FlitzFS entry in a new directory."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Pinned hipCOMP-core checkout")
    parser.add_argument("--output", type=Path, required=True, help="New private source directory")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repo / "tools/ci"))
    from hipcomp_native_sync import stage_header
    from qualify_hipcomp_candidate import stage, verify_checkout

    lock = json.loads((repo / "tests/rocm_compile/hipcomp_lz4_source.json").read_bytes())
    source = verify_checkout(args.source.absolute(), lock["commit"])
    output = args.output.absolute()
    if output.parent.resolve(strict=True) != output.parent:
        raise ValueError("native decode staging requires a stable local directory")

    inputs = {}
    for name in ("native_lz4_decode_layout.hpp", "native_lz4_decode_api.hpp", "native_group_decode.hpp", "native_lz4_decode.hip"):
        path = repo / "src/gpu" / name
        data = path.read_bytes().replace(b"\r\n", b"\n")
        if not 0 < len(data) <= 65536:
            raise ValueError(f"native decode source size: {name}")
        inputs[name] = data

    _, assets = stage(source, output, lock)
    assets, sync = stage_header(output, assets, sum(map(len, inputs.values())))
    for name, data in inputs.items():
        target = output / "src" / name
        with target.open("xb") as stream:
            stream.write(data)
        assets.append({
            "source": "src/gpu/" + name,
            "target": target.relative_to(output).as_posix(),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })

    receipt = {"upstream_commit": lock["commit"], "native_sync": sync, "assets": assets}
    with (output / "flitzfs-native-lz4-source.json").open("x", encoding="utf-8") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print(output / "src/native_lz4_decode.hip")


if __name__ == "__main__":
    main()
