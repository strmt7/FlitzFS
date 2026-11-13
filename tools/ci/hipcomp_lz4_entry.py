"""Private bounded compression-entry assets; never a runtime or worker route."""
from __future__ import annotations
import hashlib
from pathlib import Path

ASSETS = (
    dict(path="tests/rocm_compile/hipcomp_lz4_contract.hpp",
         target="src/flitzfs_lz4_contract.hpp", bytes=3165,
         sha256="a99bccffc054b86184b260d7be5e0ad3d7f052afaefb3f069da7a73fa5184f50",
         blob="08907da5c497e2f5cd5e5e4d41e85409f460f884"),
    dict(path="tests/rocm_compile/hipcomp_lz4_entry.hip",
         target="src/lowlevel/FlitzFsLz4Candidate.hip", bytes=1860,
         sha256="b6c5155c9d19ae8826e40bfaba8275202f6e4e4e7d4f714f854b758a22a1cc88",
         blob="af9384f9a1f099d3791943d1f4c72b343efbf545"),
)

def source_assets(repo: Path):
    from qualify_warp_sync import regular, canonical_text
    from qualify_rocm_sdk import native_git
    repo = repo.resolve(strict=True)
    result = []
    for spec in ASSETS:
        if native_git(repo, "rev-parse", "HEAD:" + spec["path"]) != spec["blob"]:
            raise ValueError("bounded entry canonical Git asset differs")
        raw = regular(repo / spec["path"], repo, 64 * 1024)
        data, observation = canonical_text(raw, spec["sha256"], 64 * 1024)
        if len(data) != spec["bytes"]:
            raise ValueError("bounded entry canonical asset size")
        result.append(dict(spec, body=data, physical=observation))
    return result

def stage_entry(repo: Path, root: Path, records: list, extra_bytes: int):
    from qualify_warp_sync import regular_info
    root = root.resolve(strict=True)
    assets = source_assets(repo)
    if (type(extra_bytes) is not int or extra_bytes < 0 or
            sum(row["bytes"] for row in records) + extra_bytes +
            sum(len(row["body"]) for row in assets) > 16 * 1024 * 1024):
        raise ValueError("complete bounded-entry source budget")
    targets = [root / row["target"] for row in assets]
    if (len({row["target"].casefold() for row in records} |
            {row["target"].casefold() for row in assets}) != len(records) + len(assets)):
        raise ValueError("bounded-entry source target collision")
    for path in targets:
        if (not path.is_relative_to(root) or path.parent.resolve(strict=True) != path.parent or
                path.exists() or path.is_symlink()):
            raise ValueError("bounded-entry requires NEW private regular inputs")
    # Exclusive opens also reject a collision after preflight. A partial failure
    # retains only this newly owned fixture for disposable-VM diagnosis.
    result = [dict(row) for row in records]
    for path, row in zip(targets, assets):
        with path.open("xb") as stream:
            stream.write(row["body"])
        regular_info(path, root, 64 * 1024)
        if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
            raise ValueError("bounded-entry private copy changed")
        result.append(dict(source="control:" + row["path"], target=row["target"],
                           bytes=row["bytes"], sha256=row["sha256"]))
    proof = [{key: value for key, value in row.items() if key != "body"} for row in assets]
    return targets[-1], result, proof

def unchanged(repo: Path, proof):
    current = [{key: value for key, value in row.items() if key != "body"}
               for row in source_assets(repo)]
    if current != proof:
        raise ValueError("bounded-entry physical control assets changed")

def inspect_entry_object(bounds, reader: Path, obj: Path, output: Path, metadata: str, report):
    """Extract exactly the already-admitted embedded image; native readers only."""
    import re
    from qualify_rocm_sdk import run, digest
    from qualify_warp_sync import regular_info
    selected = []
    expected_target = "hip-amdgcn-amd-amdhsa--gfx1100"
    location = re.compile(re.escape("file://" + str(obj)) + r"#offset=([0-9]{1,9})&size=([0-9]{1,9})")
    for line in metadata.splitlines():
        fields = line.split("\t")
        if len(fields) == 2 and fields[0] == expected_target:
            match = location.fullmatch(fields[1])
            if match is None:
                raise ValueError("bounded entry selected image location")
            selected.append(tuple(map(int, match.groups())))
    if len(selected) != 1:
        raise ValueError("one previously admitted bounded entry image required")
    offset, size = selected[0]
    info = regular_info(obj, output, 16 * 1024 * 1024)
    if not 0 < size <= 16 * 1024 * 1024 or offset > info.st_size or size > info.st_size-offset:
        raise ValueError("bounded entry device image extent")
    with obj.open("rb") as stream:
        stream.seek(offset)
        image = stream.read(size + 1 if offset + size == info.st_size else size)
    if len(image) != size or image[:6] != b"\x7fELF\x02\x01":
        raise ValueError("bounded entry image is not complete little-endian ELF64")
    path = output / "bounded-entry-device.elf"
    with path.open("xb") as stream:
        stream.write(image)
    regular_info(path, output, 16 * 1024 * 1024)
    observed = dict(bytes=size, sha256=hashlib.sha256(image).hexdigest(), offset=offset,
                    complete=False, loaded=False, executed=False)
    report["entry_object"] = observed
    headers = run(bounds,[str(reader),"--file-header",str(path)],
                  report,"bounded-entry-device-headers",30)
    symbols = run(bounds,[str(reader),"--symbols",str(path)],
                  report,"bounded-entry-device-symbols",30)
    for result in (headers,symbols):
        if result["output_truncated"] or result["stderr"].strip():
            raise ValueError("bounded entry native reader warned or truncated")
    if "EM_AMDGPU" not in headers["stdout"] or "elf64-amdgpu" not in headers["stdout"].lower():
        raise ValueError("bounded entry actual AMDGPU ELF identity")
    names = ["flitzfs_candidate_lz4_wave32","flitzfs_candidate_lz4_wave64"]
    for name in names:
        pattern = r"(?m)^\s+Name: " + re.escape(name) + r"(?: \([0-9]+\))?\r?$"
        if len(re.findall(pattern,symbols["stdout"])) != 1:
            raise ValueError("bounded entry exact device symbol missing or ambiguous")
    if digest(path) != observed["sha256"]:
        raise ValueError("native reader changed bounded entry device image")
    observed.update(symbols=names,complete=True)
