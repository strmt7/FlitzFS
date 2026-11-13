"""Stage the exact optional CPU decoder for the isolated SDK worker fixture."""
from __future__ import annotations
import hashlib
import stat
from pathlib import Path

SOURCE_COMMIT = "aa4647fad575d01a874aaaadcdbe8e6a40e072f5"
ASSETS = (
    dict(path="lib/lz4.c", target="lz4.c", bytes=126490,
         blob="2ab4a114f14143f901899c4bcd8aa18ee9ee866a",
         sha256="2e58f124f1032f15aecc99772b7de624c4289245729fa5403506f00b06a54937"),
    dict(path="lib/lz4.h", target="lz4.h", bytes=44407,
         blob="e69ae5627dcee63f71c0a4e42c4c8c58f337426c",
         sha256="c204caad0638a12c9ce80831094ec8fffb34aa57e74307067b08fc9452fc2611"),
    dict(path="lib/LICENSE", target="LICENSE", bytes=1311,
         blob="488491695a6b6984aa3a49a8392b192ed2bcac1d",
         sha256="8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c"),
    dict(path="LICENSE", target="LICENSE-repository", bytes=646,
         blob="ba8a2900f8b180fefc0c16f8a35076ef290c9492",
         sha256="4bc9c403f6b679cc076dee210e0e21c27b4e8f6d3fb43d85364c76657bfaaac5"),
)
SOURCE_LIMIT = 512 * 1024

def source_assets(source: Path, control: Path):
    from qualify_rocm_sdk import native_git
    from qualify_warp_sync import regular, canonical_text
    source = source.absolute()
    control = control.resolve(strict=True)
    info = source.lstat()
    if (not source.is_relative_to(control) or source == control or
            not stat.S_ISDIR(info.st_mode) or source.is_symlink() or
            (getattr(info, "st_file_attributes", 0) & 0x400) or
            source.resolve(strict=True) != source):
        raise ValueError("CPU decoder requires a private ordinary checkout")
    reported = Path(native_git(source, "rev-parse", "--show-toplevel"))
    if (not reported.is_absolute() or not reported.samefile(source) or
            native_git(source, "rev-parse", "HEAD") != SOURCE_COMMIT or
            native_git(source, "status", "--porcelain", "--untracked-files=no")):
        raise ValueError("CPU decoder checkout root, revision or cleanliness")
    result = []
    for spec in ASSETS:
        if native_git(source, "rev-parse", "HEAD:" + spec["path"]) != spec["blob"]:
            raise ValueError("CPU decoder Git asset differs")
        raw = regular(source / spec["path"], source, SOURCE_LIMIT)
        body, physical = canonical_text(raw, spec["sha256"], SOURCE_LIMIT)
        if len(body) != spec["bytes"]:
            raise ValueError("CPU decoder canonical size")
        result.append(dict(spec, body=body, physical=physical))
    if sum(len(row["body"]) for row in result) > SOURCE_LIMIT:
        raise ValueError("CPU decoder complete source cap")
    return result

def stage(source: Path, control: Path, output: Path):
    from qualify_warp_sync import regular_info
    assets = source_assets(source, control)
    output = output.absolute()
    parent = output.parent.resolve(strict=True)
    if (parent != output.parent or output.is_symlink() or output.exists() or
            output.name != "cpu-lz4-source"):
        raise ValueError("CPU decoder requires NEW private source output")
    output.mkdir()  # Exclusive directory creation; never reuse partial outputs.
    records = []
    for row in assets:
        target = output / row["target"]
        with target.open("xb") as stream:
            stream.write(row["body"])
        regular_info(target, output, SOURCE_LIMIT)
        if hashlib.sha256(target.read_bytes()).hexdigest() != row["sha256"]:
            raise ValueError("CPU decoder staged bytes changed")
        records.append({key: value for key, value in row.items() if key != "body"})
    return records

def unchanged(source: Path, control: Path, output: Path, records):
    from qualify_warp_sync import regular
    current = [{k: v for k, v in row.items() if k != "body"}
               for row in source_assets(source, control)]
    if current != records or {p.name for p in output.iterdir()} != {r["target"] for r in records}:
        raise ValueError("CPU decoder source/inventory changed")
    for row in records:
        raw = regular(output / row["target"], output, SOURCE_LIMIT)
        if len(raw) != row["bytes"] or hashlib.sha256(raw).hexdigest() != row["sha256"]:
            raise ValueError("CPU decoder staged source changed")
