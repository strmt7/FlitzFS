"""Source-archive accounting regressions; no file, process or device writes."""
import unittest
import hashlib
import io
import zipfile
from source_inventory import inventory, verify_archive, ENTRY_OVERHEAD, READ_CHUNK

def row(path, size=1, mode="100644", kind="blob", oid="1" * 40):
    return f"{mode} {kind} {oid} {size}\t{path}\0".encode()

class SourceInventory(unittest.TestCase):
    def test_current_scale_and_directory_sharing(self):
        data = b"".join(row(f"src/file-{i}.cpp") for i in range(514))
        result = inventory(data)
        self.assertEqual((result["tracked_files"], result["directory_entries"],
                          result["zip_entry_bound"], result["expanded_bytes"]),
                         (514, 1, 515, 514))

    def test_commit_size_and_directory_growth_change_archive_bound(self):
        data = b"".join(row(f"dir-{i}/file.cpp", 24 * 1024) for i in range(600))
        before = inventory(data)
        self.assertEqual(before["zip_entry_bound"], 1200)
        self.assertEqual(before["expanded_bytes"], 600 * 24 * 1024)
        size = 3 * 1024 * 1024
        after = inventory(data + row("dir-0/new.bin", size))
        self.assertEqual(after["zip_entry_bound"], 1201)
        self.assertEqual(after["archive_byte_bound"] - before["archive_byte_bound"],
                         size + ENTRY_OVERHEAD + 2 * len("dir-0/new.bin"))

    def test_nested_parents_are_unique_and_funded(self):
        result = inventory(row("a/b/c") + row("a/b/d") + row("a/e"))
        self.assertEqual(result["directory_entries"], 2)
        self.assertEqual(result["zip_entry_bound"], 5)
        with self.assertRaises(ValueError):
            inventory(row("a") + row("a/b"))

    def test_reject_malformed_inventory(self):
        for data in (b"", row("x")[:-1], b"\0", row("a") + row("a"),
                     row("x", -1), row("x", "not-a-size"), row("x", oid="z" * 40),
                     row("link", mode="120000"), row("submodule", mode="160000", kind="commit")):
            with self.subTest(data=data[:70]), self.assertRaises(ValueError):
                inventory(data)
        with self.assertRaises(ValueError):
            inventory(b"x" * (256 * 1024 + 1))

    def test_reject_unsafe_paths(self):
        for path in ("/absolute", "../escape", "a/../b", "./a", "a//b", "a/",
                     "C:drive", "a\\b", "a\nb", "a\tb"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                inventory(row(path))

class SourceArchive(unittest.TestCase):
    def fixture(self, actual=None, missing=(), extra=()):
        expected = {"root.txt": b"source", "sub/bytes.bin": b"\x00\xff"}
        data = b""
        for name, body in expected.items():
            blob = hashlib.sha1(b"blob " + str(len(body)).encode() + b"\0" + body).hexdigest()
            data += row(name, len(body), oid=blob)
        memory = io.BytesIO()
        with zipfile.ZipFile(memory, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("sub/", b"")
            for name, body in (actual if actual is not None else expected).items():
                if name not in missing:
                    archive.writestr(name, body)
            for name, body in extra:
                archive.writestr(name, body)
        memory.seek(0)
        return data, memory

    def test_all_canonical_files_and_directories(self):
        data, memory = self.fixture()
        with memory, zipfile.ZipFile(memory) as archive:
            proof = verify_archive(inventory(data), archive)
        self.assertEqual((proof["zip_entries"], proof["expanded_bytes"],
                          proof["canonical_files_verified"]), (3, 8, 2))

    def test_missing_or_additional_member_rejected(self):
        for missing, extra in ((("root.txt",), ()), ((), (("extra", b"x"),))):
            data, memory = self.fixture(missing=missing, extra=extra)
            with memory, zipfile.ZipFile(memory) as archive, self.assertRaises(ValueError):
                verify_archive(inventory(data), archive)

    def test_same_length_corruption_and_substituted_bytes_rejected(self):
        for changed in (b"damage", b"expanded source"):
            data, memory = self.fixture(actual={"root.txt": changed, "sub/bytes.bin": b"\x00\xff"})
            with memory, zipfile.ZipFile(memory) as archive, self.assertRaises(ValueError):
                verify_archive(inventory(data), archive)

    def test_duplicate_member_rejected(self):
        with self.assertWarns(UserWarning):
            data, memory = self.fixture(extra=(("root.txt", b"source"),))
        with memory, zipfile.ZipFile(memory) as archive, self.assertRaises(ValueError):
            verify_archive(inventory(data), archive)

    def test_streams_the_complete_member_and_checks_the_final_byte(self):
        body = b"A" * (3 * READ_CHUNK) + b"Z"
        blob = hashlib.sha1(b"blob " + str(len(body)).encode() + b"\0" + body).hexdigest()
        info = inventory(row("large", len(body), oid=blob))
        for valid in (True, False):
            with io.BytesIO() as memory:
                with zipfile.ZipFile(memory, "w") as archive:
                    archive.writestr("large", body if valid else body[:-1] + b"Y")
                with zipfile.ZipFile(memory) as archive:
                    if valid:
                        self.assertEqual(verify_archive(info, archive)["expanded_bytes"], len(body))
                    else:
                        with self.assertRaisesRegex(ValueError, "canonical Git blob"):
                            verify_archive(info, archive)

if __name__ == "__main__":
    unittest.main()
