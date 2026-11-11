#!/usr/bin/env python3
"""Owned hosted-only archive/provisioning safety regressions, no real tools run."""
import hashlib
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import prepare_hosted_tools as tools

class Response:
    def __init__(self,data):self.data=io.BytesIO(data);self.headers={}
    def __enter__(self):return self
    def __exit__(self,*args):pass
    def read(self,n):return self.data.read(n)

class ProvisioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
            raise RuntimeError("tests require disposable hosted VM; never the owner PC")
        cls.parent=Path(tempfile.mkdtemp(prefix="flitzfs-provision-tests-",dir=os.environ["RUNNER_TEMP"]))
    def fresh(self):
        return Path(tempfile.mkdtemp(prefix="case-",dir=self.parent))
    def test_paths(self):
        for name in ("../x","pkg/../../x","/x","C:x","pkg\\x","pkg/x\n"):
            with self.assertRaises(RuntimeError):tools.archive_name(name)
        self.assertEqual(tools.archive_name("./pkg/bin/tool"),"pkg/bin/tool")
        self.assertEqual(tools.link_target("pkg/bin/clang++","clang"),"pkg/bin/clang")
        with self.assertRaises(RuntimeError):tools.link_target("pkg/bin/x","../../../outside")
    def test_decoder_window_admission(self):
        magic=b"\x28\xb5\x2f\xfd"
        self.assertEqual(tools.zstd_window(magic+b"\x00\xa8"),2*tools.GIB)
        self.assertEqual(tools.zstd_window(magic+b"\x20\x10"),16)
        self.assertEqual(tools.zstd_window(magic+b"\x60\x00\x00"),256)
        for header in (magic+b"\x00\xb0",magic+b"\x08\x00",magic+b"\x60\x00",b"invalid"):
            with self.assertRaises(RuntimeError):tools.zstd_window(header)
    def test_selection_and_bounds(self):
        entries={
            "pkg/bin/clang++":dict(kind="symlink",size=0,target="clang"),
            "pkg/bin/clang":dict(kind="file",size=3),
            "pkg/bin/ld.lld":dict(kind="symlink",size=0,target="lld"),
            "pkg/bin/lld":dict(kind="file",size=4),
            "pkg/lib/libLLVM.so.23":dict(kind="file",size=9),
            "pkg/lib/clang/23/include/stdint.h":dict(kind="file",size=4),
            "pkg/lib/unneeded.a":dict(kind="file",size=tools.GIB),
        }
        selected,total=tools.choose(entries,"pkg","llvm")
        self.assertNotIn("pkg/lib/unneeded.a",selected);self.assertEqual(total,20)
        entries["pkg/bin/clang"]["size"]=tools.MAX_MEMBER+1
        with self.assertRaises(RuntimeError):tools.choose(entries,"pkg","llvm")
        entries["pkg/bin/clang"]["size"]=3
        entries["pkg/bin/clang++"]["target"]="/outside"
        with self.assertRaises(RuntimeError):tools.choose(entries,"pkg","llvm")
    def test_download_identity_and_no_clobber(self):
        root=self.fresh();payload=b"bounded official artifact surrogate"
        digest=hashlib.sha256(payload).hexdigest();target=root/"first"
        with patch.object(tools.urllib.request,"urlopen",return_value=Response(payload)):
            tools.download("https://github.com/vendor/example",target,len(payload),digest)
        self.assertEqual(target.read_bytes(),payload)
        with patch.object(tools.urllib.request,"urlopen",return_value=Response(payload)):
            with self.assertRaises(FileExistsError):
                tools.download("https://github.com/vendor/example",target,len(payload),digest)
        for count,expected in ((len(payload)-1,digest),(len(payload)+1,digest),(len(payload),"0"*64)):
            with patch.object(tools.urllib.request,"urlopen",return_value=Response(payload)):
                with self.assertRaises(RuntimeError):
                    tools.download("https://github.com/vendor/example",root/("bad-"+str(count)+expected[:2]),count,expected)
    def test_tar_content_and_traversal(self):
        root=self.fresh();archive=root/"package.tar.gz"
        with tarfile.open(archive,"w:gz") as package:
            member=tarfile.TarInfo("pkg/bin/cmake");member.size=5;member.mode=0o755
            package.addfile(member,io.BytesIO(b"exact"))
        output,receipt=tools.extract_tar(archive,root/"out","cmake")
        self.assertEqual((output/"bin/cmake").read_bytes(),b"exact")
        self.assertEqual(receipt["extracted_bytes"],5)
        unsafe=root/"bad.tar.gz"
        with tarfile.open(unsafe,"w:gz") as package:
            member=tarfile.TarInfo("../outside");member.size=1;package.addfile(member,io.BytesIO(b"x"))
        with self.assertRaises(RuntimeError):tools.extract_tar(unsafe,root/"bad-out","cmake")
        self.assertFalse((root/"outside").exists())
        with self.assertRaises(FileExistsError):tools.extract_tar(archive,root/"out","cmake")
    @unittest.skipUnless(os.name=="posix","LLVM provisioning is Linux-only")
    def test_llvm_links_and_extraction_subset(self):
        root=self.fresh();archive=root/"llvm.tar.zst"
        with tarfile.open(archive,"w:zst") as package:
            for name,data in (("pkg/bin/clang",b"clang"),("pkg/lib/ignore.a",b"omitted")):
                member=tarfile.TarInfo(name);member.size=len(data);member.mode=0o755
                package.addfile(member,io.BytesIO(data))
            member=tarfile.TarInfo("pkg/bin/clang++");member.type=tarfile.SYMTYPE;member.linkname="clang"
            package.addfile(member)
        output,_=tools.extract_tar(archive,root/"out","llvm")
        self.assertEqual((output/"bin/clang++").read_bytes(),b"clang")
        self.assertFalse((output/"lib/ignore.a").exists())
        cycle=root/"cycle.tar.zst"
        with tarfile.open(cycle,"w:zst") as package:
            for name,target in (("pkg/bin/clang","clang++"),("pkg/bin/clang++","clang")):
                member=tarfile.TarInfo(name);member.type=tarfile.SYMTYPE;member.linkname=target;package.addfile(member)
        with self.assertRaises(RuntimeError):tools.extract_tar(cycle,root/"cycle-out","llvm")
    def test_zip_content_and_escape(self):
        root=self.fresh();archive=root/"package.zip"
        with zipfile.ZipFile(archive,"w") as package:package.writestr("pkg/bin/cmake.exe",b"exact")
        output,receipt=tools.extract_zip(archive,root/"out")
        self.assertEqual((output/"bin/cmake.exe").read_bytes(),b"exact")
        self.assertEqual(receipt["extracted_bytes"],5)
        archive=root/"bad.zip"
        with zipfile.ZipFile(archive,"w") as package:package.writestr("../outside",b"x")
        with self.assertRaises(RuntimeError):tools.extract_zip(archive,root/"bad-out")
        self.assertFalse((root/"outside").exists())

if __name__=="__main__":
    unittest.main()
