#!/usr/bin/env python3
"""Hosted-only hostile-wheel and authoritative-digest admission regressions."""
import hashlib
import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import warnings
import zipfile
import hosted_semantic as subject

class QueryGroups(unittest.TestCase):
    def test_closed_groups_preserve_default_and_bound_maintenance_queries(self):
        self.assertEqual(set(subject.QUERY_GROUPS),{"filesystem","maintenance","hipcomp"})
        self.assertIs(subject.QUERY_GROUPS["filesystem"],subject.QUERIES)
        self.assertEqual(len(subject.QUERIES),8)
        self.assertEqual(len(subject.QUERY_GROUPS["maintenance"]),2)
        self.assertEqual(len(subject.QUERY_GROUPS["hipcomp"]),2)
        for queries in subject.QUERY_GROUPS.values():
            self.assertEqual(len(queries),len(set(queries)))
            self.assertTrue(all(isinstance(query,str) and 0<len(query)<=512 for query in queries))
    def test_unreviewed_group_fails_before_source_access_or_commands(self):
        provider=object.__new__(subject.Provider)
        with self.assertRaisesRegex(RuntimeError,"unreviewed semantic query group"):
            provider.route("unused","arbitrary")

    def test_cross_source_queries_fail_before_source_access(self):
        for repository,group in (("strmt7/FlitzFS","hipcomp"),(subject.HIPCOMP_REPOSITORY,"filesystem"),
                                 ("other/repository","maintenance")):
            provider=object.__new__(subject.Provider);provider.repository=repository
            with self.subTest(repository=repository,group=group),\
                 self.assertRaisesRegex(RuntimeError,"source/query binding"):
                provider.route("unused",group)

class SourceBinding(unittest.TestCase):
    def setUp(self):
        self.fixture=tempfile.TemporaryDirectory(prefix="flitzfs-source-binding-",dir=os.environ["RUNNER_TEMP"])
        self.root=Path(self.fixture.name);self.repo=self.root/"workspace";self.repo.mkdir()
        self.env=os.environ.copy();self.env.update(GIT_CONFIG_NOSYSTEM="1",GIT_CONFIG_GLOBAL=os.devnull)
        self.calls=[]
        self.git_run(["git","-C",self.repo,"init","--initial-branch=main"])
        (self.repo/"core.cpp").write_text("int source_binding_fixture=1;\n",encoding="utf-8")
        self.git_run(["git","-C",self.repo,"add","core.cpp"])
        self.git_run(["git","-C",self.repo,"-c","user.name=FlitzFS fixture",
                  "-c","user.email=qualification@example.invalid","commit","-m","Private qualification fixture"])
        self.commit=self.git_run(["git","-C",self.repo,"rev-parse","HEAD"]).decode().strip()
        self.calls.clear()
    def tearDown(self):
        self.fixture.cleanup() # Only the newly created disposable-VM fixture.
    def git_run(self,args,seconds=30):
        self.calls.append([str(x) for x in args])
        result=subprocess.run([str(x) for x in args],cwd=self.root,env=self.env,
                              capture_output=True,timeout=seconds,check=False)
        if result.returncode:raise RuntimeError("fixture Git command failed")
        return result.stdout
    def provider(self,repository="strmt7/FlitzFS"):
        provider=object.__new__(subject.Provider)
        provider.repo=self.repo;provider.root=self.root;provider.run=self.git_run
        provider.repository=repository;provider.commit=subject.HIPCOMP_COMMIT
        return provider
    def test_exact_default_source_and_clean_git_root(self):
        repo,repository,commit=subject.source_selection(self.repo,"filesystem",self.commit)
        self.assertEqual((repo,repository,commit),(self.repo,"strmt7/FlitzFS",self.commit))
        subject.verify_checkout(self.git_run,repo,commit)
        self.assertEqual(len(self.calls),3)
    def test_closed_candidate_identity_ignores_workflow_revision(self):
        path=self.repo/".ci-hipcomp";path.mkdir()
        selected=subject.source_selection(self.repo,"hipcomp",self.commit)
        self.assertEqual(selected,(path,subject.HIPCOMP_REPOSITORY,subject.HIPCOMP_COMMIT))
        self.assertNotEqual(selected[2],self.commit);self.assertEqual(self.calls,[])
    def test_missing_linked_and_foreign_candidate_roots_reject(self):
        with self.assertRaisesRegex(RuntimeError,"candidate checkout boundary"):
            subject.source_selection(self.repo,"hipcomp",self.commit)
        foreign=self.root/"foreign";foreign.mkdir()
        (self.repo/".ci-hipcomp").symlink_to(foreign,target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError,"candidate checkout boundary"):
            subject.source_selection(self.repo,"hipcomp",self.commit)
        self.assertEqual(self.calls,[])
    def test_invalid_source_ids_fail_before_git_or_path_selection(self):
        for value in ("main","A"*40,"0"*39,"../outside"):
            with self.subTest(value=value),self.assertRaisesRegex(RuntimeError,"invalid source identity"):
                subject.verify_checkout(self.git_run,self.repo,value)
            with self.assertRaisesRegex(RuntimeError,"invalid workflow identity"):
                subject.source_selection(self.repo,"filesystem",value)
        self.assertEqual(self.calls,[])
    def test_wrong_revision_and_nested_root_cannot_be_mislabeled(self):
        with self.assertRaisesRegex(RuntimeError,"source commit differs"):
            subject.verify_checkout(self.git_run,self.repo,"0"*40)
        nested=self.repo/"nested";nested.mkdir()
        with self.assertRaisesRegex(RuntimeError,"source Git root differs"):
            subject.verify_checkout(self.git_run,nested,self.commit)
    def test_unstaged_source_change_rejects(self):
        (self.repo/"core.cpp").write_text("int changed_source=2;\n",encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError,"fixture Git command failed"):
            subject.verify_checkout(self.git_run,self.repo,self.commit)
    def test_staged_source_change_rejects(self):
        (self.repo/"core.cpp").write_text("int changed_source=3;\n",encoding="utf-8")
        self.git_run(["git","-C",self.repo,"add","core.cpp"])
        with self.assertRaisesRegex(RuntimeError,"fixture Git command failed"):
            subject.verify_checkout(self.git_run,self.repo,self.commit)
    def test_cuda_inventory_is_candidate_only_and_excludes_oracles(self):
        for name in ("include/api.h","include/codec.cuh","src/codec.cu","src/test/oracle.cu",
                     "tests/oracle.cu",".github/untrusted.py"):
            path=self.repo/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text("int indexed_fixture=4;\n",encoding="utf-8")
        self.git_run(["git","-C",self.repo,"add","."])
        normal=self.provider().inventory()
        self.assertEqual({row["path"] for row in normal},{"core.cpp","include/api.h"})
        candidate=self.provider(subject.HIPCOMP_REPOSITORY).inventory()
        self.assertEqual({row["path"] for row in candidate},
                         {"core.cpp","include/api.h","include/codec.cuh","src/codec.cu"})
        self.assertTrue(all(len(row["sha256"])==64 for row in candidate))
    def test_tracked_source_links_reject(self):
        (self.repo/"alias.hpp").symlink_to(self.repo/"core.cpp")
        self.git_run(["git","-C",self.repo,"add","alias.hpp"])
        with self.assertRaisesRegex(RuntimeError,"source path escaped"):
            self.provider().inventory()
    def test_notice_digest_and_exclusive_creation(self):
        data=b"Private fixture notice\n";(self.repo/"LICENSE").write_bytes(data)
        spec=(("LICENSE","candidate-notice.txt",hashlib.sha256(data).hexdigest()),)
        provider=self.provider(subject.HIPCOMP_REPOSITORY)
        with patch.object(subject,"HIPCOMP_NOTICES",spec):
            provider.candidate_notices()
            self.assertEqual((self.root/"candidate-notice.txt").read_bytes(),data)
            receipt=subject.read_json(self.root/"source-notices.json")
            self.assertEqual(receipt["repository"],subject.HIPCOMP_REPOSITORY)
            self.assertEqual(receipt["commit"],subject.HIPCOMP_COMMIT)
            with self.assertRaises(FileExistsError):provider.candidate_notices()
        self.assertEqual((self.root/"candidate-notice.txt").read_bytes(),data)
    def test_wrong_notice_bytes_reject_before_output(self):
        (self.repo/"LICENSE").write_text("different notice\n",encoding="utf-8")
        with patch.object(subject,"HIPCOMP_NOTICES",(("LICENSE","candidate-notice.txt","0"*64),)),\
             self.assertRaisesRegex(RuntimeError,"notice digest differs"):
            self.provider(subject.HIPCOMP_REPOSITORY).candidate_notices()
        self.assertFalse((self.root/"candidate-notice.txt").exists())
    def test_linked_notice_rejects_before_output(self):
        target=self.root/"notice.txt";target.write_text("owned fixture\n",encoding="utf-8")
        (self.repo/"LICENSE").symlink_to(target)
        with self.assertRaisesRegex(RuntimeError,"notice boundary"):
            self.provider(subject.HIPCOMP_REPOSITORY).candidate_notices()
        self.assertFalse((self.root/"upstream-hipcomp-LICENSE.txt").exists())

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
