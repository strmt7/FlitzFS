"""Stage verified AITDCC 2026 inputs and FlitzFS packing fixtures in a new directory."""

import argparse
import hashlib
import json
import struct
import urllib.request
from pathlib import Path

CATALOG = Path(__file__).with_name("aitdcc2026-corpus.json")
BASE_URL = "https://aitdcc.github.io/data/"
FILE_NAMES = "ABCDEFGHIJKLMNOP"
FILE_LIMIT = 4 * 1024 * 1024


def checked(data, record, name):
    if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise ValueError(f"AITDCC file {name} differs from the pinned corpus")
    return data


def stage(output):
    dataset = json.loads(CATALOG.read_bytes())
    files = dataset["files"]
    if set(files) != set(FILE_NAMES) or any(not 0 < record["bytes"] <= FILE_LIMIT for record in files.values()):
        raise ValueError("Invalid AITDCC corpus manifest")
    output.mkdir()
    for name in FILE_NAMES:
        record = files[name]
        request = urllib.request.Request(BASE_URL + name, headers={"User-Agent": "FlitzFS-Corpus-Fixture"})
        with urllib.request.urlopen(request, timeout=20) as response:
            data = checked(response.read(record["bytes"] + 1), record, name)
        with (output / name).open("xb") as stream:
            stream.write(data)

    for split, definition in dataset["splits"].items():
        names = definition["files"]
        packing = output / f"{split}.flitz-fixture"
        with packing.open("xb") as stream:
            stream.write(struct.pack("<I", len(names)))
            for name in names:
                record = files[name]
                with (output / name).open("rb") as source:
                    data = checked(source.read(record["bytes"] + 1), record, name)
                stream.write(struct.pack("<I", len(data)))
                stream.write(data)
        with packing.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != definition["packed_sha256"]:
                raise ValueError(f"AITDCC packed fixture differs for {split}")

    print(f"Verified AIT Data Compression Challenge 2026 files A–P in {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New directory under a prepared development workspace")
    args = parser.parse_args()
    stage(args.output)


if __name__ == "__main__":
    main()
