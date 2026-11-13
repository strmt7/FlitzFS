"""CPU-only offline source/path admission; no SDK bootstrap, HIP load or GPU."""
from pathlib import Path
import hashlib
import tempfile
import unittest
from unittest.mock import patch
from qualify_warp_sync import canonical_text, regular, regular_info, probe_source, PROBE_SHA256, dependency_header

def sha(data):
    return hashlib.sha256(data).hexdigest()

class AdmissionTests(unittest.TestCase):
    def test_probe_asset_keeps_exact_canonical_identity(self):
        repo = Path(__file__).resolve().parents[2]
        data, receipt = probe_source(repo)
        self.assertEqual(len(data), 1670)
        self.assertEqual(sha(data), PROBE_SHA256)
        self.assertIn(receipt["representation"], ("exact", "crlf-checkout"))

    def test_only_closed_exact_or_whole_crlf_form(self):
        data = b"one\ntwo\n"
        expected = sha(data)
        self.assertEqual(canonical_text(data, expected, 64)[0], data)
        self.assertEqual(canonical_text(b"one\r\ntwo\r\n", expected, 64)[0], data)
        for raw in (b"one\r\ntwo\n", b"\xef\xbb\xbfone\ntwo\n",
                    b"one\nchanged\n", b"one\r\r\ntwo\r\r\n", b"one\ntwo", b""):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                canonical_text(raw, expected, 64)

    def test_bounds_precede_source_normalization(self):
        for raw, limit in ((b"a" * 65, 64), (b"a", 0), ("not bytes", 64)):
            with self.assertRaises(ValueError):
                canonical_text(raw, sha(b"a"), limit)

    def test_regular_input_read_and_admitted_size(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-wave-source-") as name:
            root = Path(name).resolve()
            path = root / "input"
            path.write_bytes(b"abc")
            self.assertEqual(regular(path, root, 3), b"abc")
            self.assertEqual(regular_info(path, root, 3).st_size, 3)
            with self.assertRaises(ValueError):
                regular(path, root, 2)

    def test_empty_directory_missing_and_foreign_root_reject(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-wave-source-") as name:
            root = Path(name).resolve()
            empty = root / "empty"
            empty.write_bytes(b"")
            nested = root / "nested"
            nested.mkdir()
            with self.assertRaises(ValueError):
                regular(empty, root, 8)
            with self.assertRaises(ValueError):
                regular(nested, root, 8)
            with self.assertRaises(OSError):
                regular(root / "missing", root, 8)
            with self.assertRaises(ValueError):
                regular(empty, nested, 8)
            with self.assertRaises(ValueError):
                regular(Path("relative"), root, 8)

    def test_explicit_parent_components_and_large_ancestor_chain_reject(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-wave-source-") as name:
            root = Path(name).resolve()
            nested = root / "sub"
            nested.mkdir()
            (root / "input").write_bytes(b"a")
            with self.assertRaises(ValueError):
                regular(nested / ".." / "input", root, 8)
            path = root
            for _ in range(33):
                path = path / "x"
                path.mkdir()
            leaf = path / "input"
            leaf.write_bytes(b"a")
            with self.assertRaises(ValueError):
                regular(leaf, root, 8)

    def test_reparse_attribute_is_rejected_before_read(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-wave-source-") as name:
            root = Path(name).resolve()
            path = root / "input"
            path.write_bytes(b"a")
            original = Path.lstat
            def inspect(value):
                info = original(value)
                if value == path:
                    class ReparseInfo:
                        st_file_attributes = 0x400
                    return ReparseInfo()
                return info
            with patch.object(Path, "lstat", inspect), self.assertRaises(ValueError):
                regular(path, root, 8)

    def test_dependency_binding_uses_full_selected_header_not_a_basename(self):
        header = Path("/owned/sdk/include/hip/amd_detail/amd_warp_sync_functions.h")
        exact = b"object: \\\n /owned/sdk/include/hip/amd_detail/amd_warp_sync_functions.h\n"
        self.assertTrue(dependency_header(exact, header))
        for wrong in (b"object: /other/sdk/include/hip/amd_detail/amd_warp_sync_functions.h\n",
                      b"object: /owned/sdk/include/hip/amd_detail/amd_warp_sync_functions.h.bak\n"):
            self.assertFalse(dependency_header(wrong, header))
        with self.assertRaises(ValueError):
            dependency_header(b"\0", header)

    def test_dependency_spaces_and_platform_separators(self):
        header = Path.cwd() / "sdk $with # space" / "include/hip/amd_detail/amd_warp_sync_functions.h"
        spelling = header.as_posix().replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")
        self.assertTrue(dependency_header(("object: " + spelling + "\n").encode(), header))
        native = str(header).replace("\\", "\\\\").replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")
        self.assertTrue(dependency_header(("object: " + native + "\n").encode(), header))

if __name__ == "__main__":
    unittest.main()
