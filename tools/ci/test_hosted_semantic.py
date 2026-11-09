#!/usr/bin/env python3
"""Hosted-only hostile-wheel and authoritative-digest admission regressions."""
import hashlib
import io
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import warnings
import zipfile
import hosted_semantic as subject

class Admission(unittest.TestCase):
    def setUp(self):
        self.fixture=tempfile.TemporaryDirectory(prefix="flitzfs-semantic-admission-",dir=os.environ["RUNNER_TEMP"])
        self.root=Path(self.fixture.name)
        self.provider=object.__new__(subject.Provider)
        self.provider.root=self.root
        self.provider.end=time.monotonic()+30
        self.provider.expanded_bytes=0
        self.provider.wheel_entries=0
        self.provider.downloads=[]
    def tearDown(self):
        self.fixture.cleanup() # Only this fresh disposable-VM fixture.
    def wheel(self,names):
        path=self.root/"candidate.whl"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore",UserWarning) # Deliberately duplicate ZIP entries.
            with zipfile.ZipFile(path,"w") as archive:
                for name in names:archive.writestr(name,b"x")
        return path
    def test_valid_wheel_and_aggregate_accounting(self):
        path=self.wheel(["package/module.py","package.dist-info/METADATA"])
        self.provider.wheel_admit(path)
        self.assertEqual(self.provider.wheel_entries,2)
        self.assertEqual(self.provider.expanded_bytes,2)
        self.provider.expanded_bytes=4*subject.GIB
        with self.assertRaisesRegex(RuntimeError,"expanded budget"):self.provider.wheel_admit(path)
    def test_path_aliases_traversal_and_duplicates(self):
        for name in ("../escape.py","/absolute.py","C:/drive.py","a\\b.py","a//b.py","a/./b.py"):
            with self.subTest(name=name):
                path=self.wheel([name])
                with self.assertRaisesRegex(RuntimeError,"wheel path"):self.provider.wheel_admit(path)
                path.unlink()
        path=self.wheel(["same.py","same.py"])
        with self.assertRaisesRegex(RuntimeError,"duplicate"):self.provider.wheel_admit(path)
    def test_symlink_and_false_expansion(self):
        path=self.root/"link.whl"
        info=zipfile.ZipInfo("link.py");info.create_system=3
        info.external_attr=(stat.S_IFLNK|0o777)<<16
        with zipfile.ZipFile(path,"w") as archive:archive.writestr(info,b"../outside")
        with self.assertRaisesRegex(RuntimeError,"link/special"):self.provider.wheel_admit(path)
        path=self.wheel(["tiny.py"])
        data=bytearray(path.read_bytes());central=data.index(b"PK\x01\x02")
        struct.pack_into("<I",data,central+24,2*subject.GIB)
        path.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError,"member budget"):self.provider.wheel_admit(path)
    def test_report_digest_forms_and_conflicts(self):
        value="1"*64
        self.assertEqual(self.provider.package_digest("example","1",{"archive_info":{"hashes":{"sha256":value}}},0),value)
        self.assertEqual(self.provider.package_digest("example","1",{"archive_info":{"hash":"sha256="+value}},0),value)
        with self.assertRaisesRegex(RuntimeError,"conflicting"):
            self.provider.package_digest("example","1",{"archive_info":{"hashes":{"sha256":value},
                "hash":"sha256:"+"2"*64}},0)
        with self.assertRaisesRegex(RuntimeError,"invalid"):
            self.provider.package_digest("example","1",{"archive_info":{"hashes":{"sha256":"short"}}},0)
    def test_unhashed_report_requires_matching_authority(self):
        url="https://files.pythonhosted.org/example.whl";value="2"*64
        metadata={"urls":[{"url":url,"packagetype":"bdist_wheel","digests":{"sha256":value}}]}
        with patch.object(self.provider,"native_json",return_value=metadata) as capture:
            self.assertEqual(self.provider.package_digest("example","1",{"url":url},3),value)
            self.assertIn("https://pypi.org/pypi/example/1/json",capture.call_args.args[0])
        for metadata in ({"urls":[]},{"urls":[{"url":url+"-other","packagetype":"bdist_wheel",
                                             "digests":{"sha256":value}}]}):
            with patch.object(self.provider,"native_json",return_value=metadata):
                with self.assertRaisesRegex(RuntimeError,"unique official"):
                    self.provider.package_digest("example","1",{"url":url},3)
    def test_cpu_vendor_index_digest_is_exact_and_unambiguous(self):
        value="3"*64
        url="https://download.pytorch.org/whl/cpu/torch-1%2Bcpu-cp314.whl"
        index=self.root/"torch-index.native.log"
        index.write_text('<a href="'+url+"#sha256="+value+'">wheel</a>',encoding="utf-8")
        self.assertEqual(self.provider.package_digest("torch","1+cpu",{"url":url},0),value)
        index.write_text('<a href="'+url+"#sha256="+value+'">one</a>'+
            '<a href="'+url+"#sha256="+"4"*64+'">two</a>',encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError,"unique official"):
            self.provider.package_digest("torch","1+cpu",{"url":url},0)
        with self.assertRaisesRegex(RuntimeError,"unreviewed origin"):
            self.provider.package_digest("torch","1+cpu",
                {"url":url.replace("download.pytorch.org","files.pythonhosted.org")},0)
    def test_actual_mismatched_download_is_retained_and_rejected(self):
        class Response(io.BytesIO):
            status=200
            headers={"Content-Length":"5"}
            def geturl(self):return "https://files.pythonhosted.org/candidate.whl"
        target=self.root/"candidate.whl"
        free=type("Space",(),{"free":100*subject.GIB})()
        with patch.object(subject.shutil,"disk_usage",return_value=free),\
             patch.object(subject.urllib.request,"urlopen",return_value=Response(b"bytes")):
            with self.assertRaisesRegex(RuntimeError,"digest differs"):
                self.provider.download("https://files.pythonhosted.org/candidate.whl",target,1024,"0"*64)
        self.assertEqual(target.read_bytes(),b"bytes")
        self.assertFalse(self.provider.downloads[-1]["success"])
        self.assertEqual(self.provider.downloads[-1]["partial_sha256"],hashlib.sha256(b"bytes").hexdigest())
        self.assertTrue((self.root/"download-1.json").is_file())
if __name__=="__main__":
    if sys.platform!="linux" or os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("admission tests require a disposable hosted Linux VM; never the owner PC")
    unittest.main()
