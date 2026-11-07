"""Exercise the real CLI on small synthetic FLZIP bytes; never use user files.

Inputs: one built CLI path. Output: assertions/exit status. All files are new,
owned fixtures and are individually removed; no shell or recursive cleanup.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def native(raw: bytes, *, kind: int = 0, version: int = 1) -> bytes:
    """Encode one tiny standard-layout record independently of C++ fixtures."""
    name = b"Data/test.bin"
    index = (b"SUZP" + struct.pack("<IIH", version, 1, len(name)) + name
             + struct.pack("<BQQQII", 0, len(raw), 0, len(raw), zlib.crc32(raw), 1)
             + struct.pack("<BBIQI", kind, 0, len(raw), 0, len(raw)))
    return raw + index + b"SUZF" + struct.pack("<IQQ", version, len(raw), len(index))


def main() -> None:
    """Run bounded CPU commands and verify every owned fixture stays unchanged."""
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_flzip_cli.py <built-flitzfs>")
    executable = Path(sys.argv[1]).resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="flitzfs-cli-flzip-"))
    owned: list[Path] = []
    before: dict[Path, str] = {}
    checks = 0
    try:
        golden = native(b"0123456789abcdef")
        fixtures = {
            "valid.flzip": golden,
            "renamed.bin": golden,
            "corrupt.flzip": bytes([golden[0] ^ 1]) + golden[1:],
            "truncated.flzip": golden[:-1],
            "unsupported.flzip": native(b"0123456789abcdef", kind=14, version=11),
        }
        for name, data in fixtures.items():
            path = root / name
            with path.open("xb") as file:
                owned.append(path)
                file.write(data)
            before[path] = hashlib.sha256(data).hexdigest()

        def run(arguments: list[str], expected: int, phrase: str) -> None:
            """Invoke exactly one trusted executable without a shell or stdin."""
            nonlocal checks
            result = subprocess.run([str(executable), *arguments], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=5, check=False)
            combined = result.stdout + result.stderr
            if result.returncode != expected or phrase not in combined:
                raise AssertionError(f"CLI contract failed ({result.returncode}): {combined[:1000]}")
            checks += 1

        good = str(root / "valid.flzip")
        run(["flzip-list", good], 0, "directory\t0\tData")
        run(["flzip-list", good, "data"], 0, "file\t16\tData/test.bin")
        run(["flzip-verify", good], 0, "1 files, 16 logical bytes; NOT authenticated")
        run(["flzip-verify", str(root / "renamed.bin")], 0, "verified")
        run(["flzip-probe", good, "DATA/TEST.BIN", "7", "9"], 0, "Read 9 bytes")
        run(["flzip-probe", good, "Data/test.bin", "16", "0"], 0, "Read 0 bytes")
        run(["flzip-probe", good, "Data/test.bin", "16", "1"], 1, "bounds")
        run(["flzip-probe", good, "../test.bin", "0", "1"], 1, "invalid")
        run(["flzip-verify", str(root / "corrupt.flzip")], 1, "CRC mismatch")
        run(["flzip-verify", str(root / "truncated.flzip")], 1, "footer")
        run(["flzip-verify", str(root / "unsupported.flzip")], 1, "invalid for its declared version")
        run(["flzip-probe", good, "Data/test.bin", "0", "4194305"], 1, "exceeds 4 MiB")
        for path, digest in before.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise AssertionError("read-only CLI altered a fixture")
        print(f"PASS: {checks} bounded FLZIP CLI checks; all fixture hashes unchanged.")
    finally:
        for path in owned:
            path.unlink()
        root.rmdir()


if __name__ == "__main__":
    main()
