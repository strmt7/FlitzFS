"""CPU-only transform tests against the exact optional upstream source."""
import argparse
import hashlib
from pathlib import Path
import tempfile
import unittest
from hipcomp_native_sync import (SOURCE_BYTES, SOURCE_SHA256, SYNCHRONIZATION,
    NATIVE_SYNCHRONIZATION, EMPTY_CHECK, BOUNDED_CHECK, DISTANCE_CHECK,
    BOUNDED_DISTANCE, patch_header, replace_once, stage_header)

SOURCE = None

class NativeSyncTests(unittest.TestCase):
    def test_exact_source_patch_and_only_closed_replacements(self):
        patched, proof = patch_header(SOURCE)
        expected = SOURCE.decode().replace(SYNCHRONIZATION, NATIVE_SYNCHRONIZATION).replace(
            EMPTY_CHECK, BOUNDED_CHECK).replace(DISTANCE_CHECK, BOUNDED_DISTANCE).encode()
        self.assertEqual(patched, expected)
        self.assertEqual(proof["canonical_sha256"], SOURCE_SHA256)
        self.assertEqual(hashlib.sha256(patched).hexdigest(), proof["sha256"])
        self.assertFalse(proof["upstream_checkout_changed"])
        self.assertFalse(proof["gpu_executed"])

    def test_unknown_bytes_bom_crlf_and_truncation_reject(self):
        for data in (b"", SOURCE + b"\n", SOURCE[:-1], b"\xef\xbb\xbf" + SOURCE,
                     SOURCE.replace(b"\n", b"\r\n"), SOURCE.replace(b"NULL_OFFSET", b"NULL_OFFSEU", 1)):
            with self.assertRaises(ValueError):
                patch_header(data)

    def test_missing_or_duplicate_anchor_is_terminal(self):
        for text in ("none", "x x"):
            with self.assertRaises(ValueError):
                replace_once(text, "x", "safe")
        self.assertEqual(replace_once("x", "x", "safe"), "safe")

    def test_guards_precede_conversion_and_read(self):
        patched = patch_header(SOURCE)[0].decode()
        start = patched.index("isValidHash(")
        guard = patched.index("static_cast<position_type>(hashed_offset) > decomp_idx", start)
        conversion = patched.index("offset = convertIdx(", start)
        distance = patched.index(BOUNDED_DISTANCE, start)
        load = patched.index("readWord<word_type>(data + offset)", start)
        self.assertLess(guard, conversion)
        self.assertLess(conversion, distance)
        self.assertLess(distance, load)

    def test_native_wave_calls_never_insert_block_barrier(self):
        old = SOURCE.decode()
        new = patch_header(SOURCE)[0].decode()
        self.assertEqual(new.count("__syncthreads"), old.count("__syncthreads"))
        self.assertIn("__syncwarp(static_cast<unsigned long long>(m))", new)
        self.assertIn("!defined(__HIP_PLATFORM_AMD__)", new)
        self.assertEqual(new[:new.index("#define SHFL10")], old[:old.index("#define SHFL10")])

    def fixture(self, name):
        root = Path(name).resolve()
        header = root / "src/LZ4Kernels.cuh"
        header.parent.mkdir()
        header.write_bytes(SOURCE)
        records = [dict(source="src/LZ4Kernels.cuh", target="src/LZ4Kernels.cuh",
                        bytes=SOURCE_BYTES, sha256=SOURCE_SHA256)]
        return root, header, records

    def test_private_composition_retains_canonical_and_updates_manifest(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-native-sync-") as name:
            root, header, records = self.fixture(name)
            updated, proof = stage_header(root, records, 0)
            self.assertEqual((root / "native-sync-original-LZ4Kernels.cuh").read_bytes(), SOURCE)
            self.assertEqual(header.read_bytes(), patch_header(SOURCE)[0])
            self.assertEqual(len(updated), 2)
            self.assertEqual(records[0]["sha256"], SOURCE_SHA256)
            self.assertEqual(updated[0]["sha256"], proof["sha256"])

    def test_collision_and_budget_failure_preserve_original(self):
        for collision in (False, True):
            with tempfile.TemporaryDirectory(prefix="flitzfs-native-sync-") as name:
                root, header, records = self.fixture(name)
                if collision:
                    (root / "native-sync-original-LZ4Kernels.cuh").write_bytes(b"sentinel")
                    error = FileExistsError
                else:
                    records.append(dict(target="budget", bytes=16 * 1024 * 1024, sha256="0"*64))
                    error = ValueError
                with self.assertRaises(error):
                    stage_header(root, records, 0)
                self.assertEqual(header.read_bytes(), SOURCE)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    from qualify_hipcomp_candidate import verify_checkout, read_blob, SOURCE_COMMIT
    root = verify_checkout(args.source, SOURCE_COMMIT)
    SOURCE = read_blob(root, "src/LZ4Kernels.cuh", dict(bytes=SOURCE_BYTES, sha256=SOURCE_SHA256))
    unittest.main(argv=[__file__] + remaining)
