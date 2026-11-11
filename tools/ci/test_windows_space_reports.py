"""Pure validator regressions with SYNTHETIC values, not measured Windows results."""
import json
import unittest

from verify_windows_space_reports import decode_report, reconcile, FILES, PHASES, UUID

COMMIT = "ab" * 20


def report(phase="normal"):
    result = {
        "schema": 1, "phase": phase, "commit": COMMIT, "filesystem": "FlitzFS", "source_uuid": UUID,
        "native_namespace_sequence": 3, "native_generation": 7, "native_image_bytes": 1024 * 1024,
        "backing_standard_allocation_bytes": 1024 * 1024,
        "native_committed_end": 1024 * 1024, "native_live_decoded_bytes": 400000,
        "native_live_stored_payload_bytes": 1000, "volume_serial": 0x5700004E, "volume_flags": 0x80000,
        "volume_read_only": True, "volume_file_compression": False, "maximum_component_characters": 100,
        "allocation_unit_bytes": 4096, "total_clusters": 256, "free_clusters": 0,
        "total_bytes": 1024 * 1024, "free_bytes": 0, "caller_available_bytes": 0,
        "records": [], "record_count": 3, "complete": True,
    }
    for name, (inode, size) in FILES.items():
        allocation = (size + 4095) // 4096 * 4096
        result["records"].append({
            "file": name, "inode": inode, "logical_bytes": size, "standard_allocation_bytes": allocation,
            "standard_eof_bytes": size, "standard_links": 1, "compression_info_ok": False,
            "compression_info_error": 123, "compression_info_bytes": 0, "compression_format": 0,
            "compression_unit_shift": 0, "compression_chunk_shift": 0, "compression_cluster_shift": 0,
            "get_compressed_low": allocation & 0xFFFFFFFF, "get_compressed_high": allocation >> 32,
            "get_compressed_last_error": 0,
        })
    return result


def decode(value):
    return decode_report(json.dumps(value).encode(), COMMIT)


class Reports(unittest.TestCase):
    def test_complete_closure_and_raw_error_preservation(self):
        values = [report(phase) for phase in PHASES]
        values[1]["records"][0]["get_compressed_last_error"] = 456
        result = reconcile(values, COMMIT)
        self.assertTrue(result["complete"])
        self.assertEqual(result["records_per_phase"], 3)
        self.assertEqual(result["get_compressed_raw"]["sample"][1]["get_compressed_last_error"], 456)
        # Failed secondary queries remain explicit errors, never "0-byte compression".
        self.assertFalse(result["observations"][0]["compression_info_ok"])
        self.assertEqual(result["observations"][0]["compression_info_error"], 123)

    def test_wrong_source_missing_duplicate_and_incomplete(self):
        for key, value in (("commit", "cd" * 20), ("source_uuid", "0" * 32),
                           ("phase", "other"), ("complete", False), ("record_count", 2),
                           ("native_namespace_sequence", 4), ("native_generation", 0),
                           ("volume_read_only", False), ("volume_file_compression", True)):
            with self.subTest(key=key):
                bad = report(); bad[key] = value
                with self.assertRaises(ValueError): decode(bad)
        for records in ([], report()["records"][:2], [report()["records"][0]] * 3):
            bad = report(); bad["records"] = records
            with self.assertRaises(ValueError): decode(bad)
        for values in ([report()], [report(), report(), report("recovery")]):
            with self.assertRaises(ValueError): reconcile(values, COMMIT)
        bad = report("recovery"); bad["native_live_stored_payload_bytes"] += 1
        with self.assertRaises(ValueError): reconcile([report(), report("crash"), bad], COMMIT)

    def test_size_identity_query_and_integer_rejections(self):
        for key, value in (("native_image_bytes", 2 * 1024 * 1024), ("free_bytes", 4096),
                           ("total_clusters", 255), ("allocation_unit_bytes", 512),
                           ("native_live_stored_payload_bytes", 400000), ("volume_flags", True)):
            bad = report(); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): decode(bad)
        for key, value in (("inode", 99), ("standard_eof_bytes", 7), ("standard_allocation_bytes", 7),
                           ("compression_info_bytes", 1), ("compression_info_error", 0),
                           ("compression_info_ok", 1), ("get_compressed_low", 1 << 32),
                           ("get_compressed_last_error", -1), ("compression_format", 1 << 16),
                           ("compression_unit_shift", 256), ("standard_links", 2)):
            bad = report(); bad["records"][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): decode(bad)
        bad = report("recovery"); bad["records"][0]["get_compressed_low"] += 1
        with self.assertRaises(ValueError): reconcile([report(), report("crash"), bad], COMMIT)

    def test_success_and_failure_outputs_not_conflated(self):
        values = [report(phase) for phase in PHASES]
        for value in values:
            value["records"][0].update(compression_info_ok=True, compression_info_error=0,
                                       compression_info_bytes=42, compression_format=2)
        self.assertEqual(reconcile(values, COMMIT)["observations"][0]["compression_info_bytes"], 42)
        values[0]["records"][0]["compression_info_error"] = 123
        with self.assertRaises(ValueError): reconcile(values, COMMIT)
        # High words after a sentinel error are raw observations, not forced stable.
        values = [report(phase) for phase in PHASES]
        for i, value in enumerate(values):
            value["records"][0].update(get_compressed_low=0xFFFFFFFF,
                                       get_compressed_high=i, get_compressed_last_error=123)
        self.assertEqual(reconcile(values, COMMIT)["get_compressed_raw"]["sample"][2]["get_compressed_high"], 2)

    def test_json_schema_duplicate_fields_and_bounds(self):
        raw = json.dumps(report()).encode()
        for invalid in (raw + b" " * 8192, b"\xff", b"{}", raw.replace(b'"schema": 1', b'"schema": 1, "schema": 1')):
            with self.assertRaises(ValueError): decode_report(invalid, COMMIT)
        bad = report(); bad["unexpected"] = 1
        with self.assertRaises(ValueError): decode(bad)
        bad = report(); bad["records"][0]["unexpected"] = 1
        with self.assertRaises(ValueError): decode(bad)
        with self.assertRaises(ValueError): decode_report(raw, "not-a-commit")


if __name__ == "__main__":
    unittest.main()
