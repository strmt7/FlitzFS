#!/usr/bin/env python3
"""CPU-only source-staging failures using a fresh private Git fixture."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import stage_lz4_worker_decoder as decoder

class DecoderStaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="flitzfs-decoder-source-")
        self.addCleanup(self.temporary.cleanup)
        self.control = Path(self.temporary.name).resolve()
        config = mock.patch.dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        config.start()
        self.addCleanup(config.stop)
        self.source = self.control / "source"
        self.source.mkdir()
        self.git("init", "--initial-branch=main")
        self.git("config", "core.autocrlf", "false")
        specs = []
        for index, original in enumerate(decoder.ASSETS):
            body = ("synthetic CPU decoder fixture " + str(index) + "\nsecond line\n").encode()
            path = self.source / original["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(body)
            blob = hashlib.sha1(b"blob " + str(len(body)).encode() + b"\0" + body).hexdigest()
            specs.append(dict(original, bytes=len(body), blob=blob,
                              sha256=hashlib.sha256(body).hexdigest()))
        self.git("add", "--all")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-m", "Synthetic fixture")
        commit = self.git("rev-parse", "HEAD")
        for patcher in (mock.patch.object(decoder, "SOURCE_COMMIT", commit),
                        mock.patch.object(decoder, "ASSETS", tuple(specs))):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.output = self.control / "cpu-lz4-source"

    def git(self, *args):
        env = os.environ.copy()
        env.update(GIT_NO_REPLACE_OBJECTS="1", GIT_CONFIG_NOSYSTEM="1",
                   GIT_CONFIG_GLOBAL=os.devnull)
        result = subprocess.run(["git", "-c", "core.hooksPath=" + str(self.control / "no-hooks"),
                                 "-C", str(self.source), *args], check=True, capture_output=True,
                                text=True, timeout=10, env=env)
        self.assertLessEqual(len(result.stdout) + len(result.stderr), 8192)
        return result.stdout.strip()

    def test_new_exact_inputs_and_both_notices(self):
        records = decoder.stage(self.source, self.control, self.output)
        self.assertEqual(len(records), 4)
        self.assertEqual({p.name for p in self.output.iterdir()},
                         {"lz4.c", "lz4.h", "LICENSE", "LICENSE-repository"})
        self.assertTrue(all(r["physical"]["representation"] == "exact" for r in records))
        decoder.unchanged(self.source, self.control, self.output, records)

    def test_existing_output_preserved(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_bytes(b"existing bytes")
        with self.assertRaises(ValueError):
            decoder.stage(self.source, self.control, self.output)
        self.assertEqual(marker.read_bytes(), b"existing bytes")
        self.assertEqual(len(list(self.output.iterdir())), 1)

    def test_wrong_output_name(self):
        with self.assertRaises(ValueError):
            decoder.stage(self.source, self.control, self.control / "other")
        self.assertFalse((self.control / "other").exists())

    def test_wrong_root_and_revision(self):
        with self.assertRaises(ValueError):
            decoder.source_assets(self.source, self.source)
        outside = self.control / "outside"
        outside.mkdir()
        with self.assertRaises(ValueError):
            decoder.source_assets(self.source, outside)
        with mock.patch.object(decoder, "SOURCE_COMMIT", "0" * 40):
            with self.assertRaises(ValueError):
                decoder.source_assets(self.source, self.control)

    def test_dirty_source_rejected_before_output(self):
        (self.source / "lib/lz4.c").write_bytes(b"changed\n")
        with self.assertRaises(ValueError):
            decoder.stage(self.source, self.control, self.output)
        self.assertFalse(self.output.exists())

    def test_wrong_canonical_blob(self):
        altered = [dict(row) for row in decoder.ASSETS]
        altered[0]["blob"] = "0" * 40
        with mock.patch.object(decoder, "ASSETS", tuple(altered)):
            with self.assertRaises(ValueError):
                decoder.stage(self.source, self.control, self.output)
        self.assertFalse(self.output.exists())

    def test_wrong_digest_size_and_total(self):
        for field, value in (("sha256", "0" * 64), ("bytes", 1)):
            altered = [dict(row) for row in decoder.ASSETS]
            altered[0][field] = value
            with mock.patch.object(decoder, "ASSETS", tuple(altered)):
                with self.assertRaises(ValueError):
                    decoder.stage(self.source, self.control, self.output)
        with mock.patch.object(decoder, "SOURCE_LIMIT", 80):
            with self.assertRaises(ValueError):
                decoder.stage(self.source, self.control, self.output)
        self.assertFalse(self.output.exists())

    def test_crlf_checkout_admitted_without_rewrite(self):
        self.git("config", "core.autocrlf", "true")
        before = {}
        for spec in decoder.ASSETS:
            path = self.source / spec["path"]
            raw = path.read_bytes().replace(b"\n", b"\r\n")
            path.write_bytes(raw)
            before[spec["path"]] = raw
        self.assertEqual(self.git("status", "--porcelain", "--untracked-files=no"), "")
        records = decoder.stage(self.source, self.control, self.output)
        self.assertTrue(all(r["physical"]["representation"] == "crlf-checkout" for r in records))
        for spec in decoder.ASSETS:
            self.assertEqual((self.source / spec["path"]).read_bytes(), before[spec["path"]])
            self.assertNotIn(b"\r", (self.output / spec["target"]).read_bytes())
        decoder.unchanged(self.source, self.control, self.output, records)

    def test_staged_change_and_extra_member_rejected(self):
        records = decoder.stage(self.source, self.control, self.output)
        changed = self.output / "lz4.h"
        original = changed.read_bytes()
        changed.write_bytes(b"x" * len(original))
        with self.assertRaises(ValueError):
            decoder.unchanged(self.source, self.control, self.output, records)
        changed.write_bytes(original)
        (self.output / "extra").write_bytes(b"x")
        with self.assertRaises(ValueError):
            decoder.unchanged(self.source, self.control, self.output, records)

    def test_late_exclusive_collision_never_overwrites(self):
        real_open = Path.open
        def collide(path, mode="r", *args, **kwargs):
            if path == self.output / "lz4.h" and mode == "xb":
                with real_open(path, "xb") as stream:
                    stream.write(b"collision")
            return real_open(path, mode, *args, **kwargs)
        with mock.patch.object(Path, "open", collide):
            with self.assertRaises(FileExistsError):
                decoder.stage(self.source, self.control, self.output)
        self.assertEqual((self.output / "lz4.h").read_bytes(), b"collision")

if __name__ == "__main__":
    unittest.main()
