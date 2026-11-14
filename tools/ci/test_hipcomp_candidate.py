#!/usr/bin/env python3
"""CPU-only source/fixture admission for the isolated codec compiler gate."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import qualify_hipcomp_candidate as subject

ASSET=Path(__file__).resolve().parents[2]/"tests/rocm_compile/hipcomp_lz4_source.json"

class ManifestAdmission(unittest.TestCase):
    def setUp(self):
        self.value=json.loads(ASSET.read_text(encoding="utf-8"))
    def test_actual_manifest_and_private_include_view(self):
        mapping,notices=subject.validate_manifest(self.value)
        self.assertEqual(len(self.value["files"]),142)
        self.assertEqual(len(mapping),136)
        self.assertEqual(len({target.casefold() for _,target,_ in mapping}),136)
        self.assertIn(("include/hipcomp/lz4.h","src/hipcomp/lz4.h"),
                      {(original,target) for original,target,_ in mapping})
        self.assertTrue(any(original==subject.ENTRY and record["sha256"]==subject.ENTRY_DIGEST
                            for original,_,record in mapping))
        self.assertEqual([row["path"] for row in notices],["LICENSE","CMakeLists.txt"])
    def test_identity_and_schema_are_closed(self):
        for key,value in (("schema",True),("schema",2),("repository","other/repo"),
                          ("commit","0"*40),("source_sha256","0"*64)):
            candidate=copy.deepcopy(self.value);candidate[key]=value
            with self.subTest(key=key,value=value),self.assertRaisesRegex(RuntimeError,"manifest identity"):
                subject.validate_manifest(candidate)
        candidate=copy.deepcopy(self.value);candidate["unexpected"]=True
        with self.assertRaisesRegex(RuntimeError,"manifest identity"):subject.validate_manifest(candidate)
    def test_empty_and_oversized_source_inventory(self):
        for files in ([],self.value["files"]*4):
            candidate=copy.deepcopy(self.value);candidate["files"]=files
            with self.assertRaisesRegex(RuntimeError,"source count"):subject.validate_manifest(candidate)
    def test_unsafe_windows_paths_and_git_query_injection(self):
        for name in ("../x.h","/x.h","src/../x.h","src//x.h","src/./x.h",
                     "C:/x.h","src\\x.h","src/a:b.h","src/line\nbreak.h","src/x.h.",
                     "src/NUL.hpp","src/aux/data.h","src/COM1","src/LPT9.h","src/space name.h"):
            with self.subTest(name=name),self.assertRaisesRegex(RuntimeError,"source path"):
                subject.admit_path(name)
    def test_case_alias_and_unordered_paths_reject(self):
        candidate=copy.deepcopy(self.value);candidate["files"]=[
            {"path":"include/Foo.h","bytes":1,"sha256":"0"*64},
            {"path":"include/foo.h","bytes":1,"sha256":"0"*64}]
        with self.assertRaisesRegex(RuntimeError,"order/alias"):subject.validate_manifest(candidate)
        candidate=copy.deepcopy(self.value);candidate["files"][0],candidate["files"][1]=candidate["files"][1],candidate["files"][0]
        with self.assertRaisesRegex(RuntimeError,"order/alias"):subject.validate_manifest(candidate)
    def test_malformed_size_digest_and_fields_reject(self):
        for key,value in (("bytes",True),("bytes",0),("bytes",subject.MIB+1),("sha256","z"*64),("sha256","0"*63)):
            candidate=copy.deepcopy(self.value);candidate["files"][0][key]=value
            with self.subTest(key=key,value=value),self.assertRaisesRegex(RuntimeError,"size/digest"):
                subject.validate_manifest(candidate)
        candidate=copy.deepcopy(self.value);candidate["files"][0]["unexpected"]=1
        with self.assertRaisesRegex(RuntimeError,"source fields"):subject.validate_manifest(candidate)
    def test_aggregate_source_budget_rejects_before_digest(self):
        candidate=copy.deepcopy(self.value);candidate["files"]=[
            {"path":f"src/file{i:03}.h","bytes":subject.MIB,"sha256":"0"*64} for i in range(17)]
        with self.assertRaisesRegex(RuntimeError,"source aggregate"):subject.validate_manifest(candidate)
    def test_public_header_overlay_alias_rejects(self):
        candidate=copy.deepcopy(self.value);candidate["files"]=[
            {"path":"include/alias.h","bytes":1,"sha256":"0"*64},
            {"path":"src/alias.h","bytes":1,"sha256":"0"*64}]
        with self.assertRaisesRegex(RuntimeError,"compiler-input alias"):subject.validate_manifest(candidate)
    def test_modified_inventory_cannot_claim_pinned_digest(self):
        candidate=copy.deepcopy(self.value);candidate["files"][0]["sha256"]="0"*64
        with self.assertRaisesRegex(RuntimeError,"inventory digest"):subject.validate_manifest(candidate)
    def test_missing_or_modified_notice_rejects(self):
        candidate=copy.deepcopy(self.value);candidate["notices"]=[]
        with self.assertRaisesRegex(RuntimeError,"notices differ"):subject.validate_manifest(candidate)
        candidate=copy.deepcopy(self.value);candidate["notices"][0]["bytes"]+=1
        with self.assertRaisesRegex(RuntimeError,"notices differ"):subject.validate_manifest(candidate)

class PrivateStaging(unittest.TestCase):
    def setUp(self):
        self.fixture=tempfile.TemporaryDirectory(prefix="flitzfs-codec-stage-",dir=os.environ["RUNNER_TEMP"])
        self.root=Path(self.fixture.name)
    def tearDown(self):
        self.fixture.cleanup() # Only this fresh disposable-VM fixture.
    def mapping(self):
        entry=b"// private compilation fixture\n";header=b"// private public header\n";notice=b"private notice\n"
        data={subject.ENTRY:entry,"include/hipcomp/test.h":header,"LICENSE":notice}
        mapping=[(subject.ENTRY,subject.ENTRY,{"bytes":len(entry)}),
                 ("include/hipcomp/test.h","src/hipcomp/test.h",{"bytes":len(header)})]
        notices=[{"path":"LICENSE","bytes":len(notice)}]
        return data,mapping,notices
    def test_new_fixture_retains_canonical_bytes_and_include_view(self):
        data,mapping,notices=self.mapping();output=self.root/"new-input"
        with patch.object(subject,"validate_manifest",return_value=(mapping,notices)),\
             patch.object(subject,"read_blob",side_effect=lambda _,name,record:data[name]):
            entry,records=subject.stage(self.root,output,{})
        self.assertEqual(entry.read_bytes(),data[subject.ENTRY])
        self.assertEqual((output/subject.ENTRY).read_bytes(),data[subject.ENTRY])
        self.assertEqual((output/"include/hipcomp/test.h").read_bytes(),data["include/hipcomp/test.h"])
        self.assertEqual((output/"src/hipcomp/test.h").read_bytes(),data["include/hipcomp/test.h"])
        self.assertEqual((output/"notices/LICENSE").read_bytes(),data["LICENSE"])
        self.assertEqual(len(records),3)
    def test_existing_fixture_cannot_be_reused(self):
        data,mapping,notices=self.mapping();output=self.root/"owned";output.mkdir()
        sentinel=output/"sentinel";sentinel.write_bytes(b"keep")
        with patch.object(subject,"validate_manifest",return_value=(mapping,notices)),\
             patch.object(subject,"read_blob") as read,\
             self.assertRaisesRegex(RuntimeError,"already exists"):
            subject.stage(self.root,output,{})
        self.assertEqual(sentinel.read_bytes(),b"keep");read.assert_not_called()
    def test_input_budget_refuses_before_fixture_creation(self):
        _,mapping,notices=self.mapping();mapping[0][2]["bytes"]=subject.MAX_BYTES
        output=self.root/"must-not-exist"
        with patch.object(subject,"validate_manifest",return_value=(mapping,notices)),\
             patch.object(subject,"read_blob") as read,\
             self.assertRaisesRegex(RuntimeError,"fixture aggregate"):
            subject.stage(self.root,output,{})
        self.assertFalse(output.exists());read.assert_not_called()
    def test_partial_copy_failure_preserves_owned_bytes(self):
        data,mapping,notices=self.mapping();output=self.root/"partial"
        def copy(_,name,record):
            if name!=subject.ENTRY:raise RuntimeError("injected source read")
            return data[name]
        with patch.object(subject,"validate_manifest",return_value=(mapping,notices)),\
             patch.object(subject,"read_blob",side_effect=copy),\
             self.assertRaisesRegex(RuntimeError,"injected source read"):
            subject.stage(self.root,output,{})
        self.assertEqual((output/subject.ENTRY).read_bytes(),data[subject.ENTRY])
        self.assertFalse((output/"src/hipcomp/test.h").exists())

class NativeGitBinding(unittest.TestCase):
    def setUp(self):
        self.fixture=tempfile.TemporaryDirectory(prefix="flitzfs-codec-git-",dir=os.environ["RUNNER_TEMP"])
        self.root=Path(self.fixture.name);self.workspace=self.root/"workspace";self.workspace.mkdir()
        self.repo=self.workspace/"candidate";self.repo.mkdir()
        config=self.root/"empty.gitconfig";config.write_bytes(b"")
        self.env=os.environ.copy();self.env.update(GIT_CONFIG_NOSYSTEM="1",GIT_CONFIG_GLOBAL=str(config))
        self.git_cmd("init","--initial-branch=main")
        self.data=b"canonical vendor bytes\n";(self.repo/"source.h").write_bytes(self.data)
        self.git_cmd("add","source.h")
        self.git_cmd("-c","user.name=FlitzFS fixture","-c","user.email=qualification@example.invalid",
                     "commit","-m","Private codec qualification fixture")
        self.commit=self.git_cmd("rev-parse","HEAD").decode().strip()
    def tearDown(self):
        self.fixture.cleanup() # Only this fresh disposable-VM fixture.
    def git_cmd(self,*args):
        result=subprocess.run(["git","-C",str(self.repo),"-c","gc.auto=0","-c","maintenance.auto=false",*args],capture_output=True,timeout=15,
                              env=self.env,check=False)
        if result.returncode:raise RuntimeError("fixture Git command failed")
        return result.stdout
    def test_exact_root_revision_and_clean_checkout(self):
        with patch.object(subject,"REPO",self.workspace):
            self.assertEqual(subject.verify_checkout(self.repo,self.commit),self.repo)
    def test_wrong_revision_and_nested_git_root_reject(self):
        nested=self.repo/"nested";nested.mkdir()
        with patch.object(subject,"REPO",self.workspace):
            with self.assertRaisesRegex(RuntimeError,"checkout identity"):
                subject.verify_checkout(self.repo,"0"*40)
            with self.assertRaisesRegex(RuntimeError,"checkout identity"):
                subject.verify_checkout(nested,self.commit)
    def test_dirty_source_rejects(self):
        (self.repo/"source.h").write_bytes(b"changed working tree\n")
        with patch.object(subject,"REPO",self.workspace),self.assertRaisesRegex(RuntimeError,"checkout changed"):
            subject.verify_checkout(self.repo,self.commit)
    def test_canonical_git_bytes_ignore_working_tree_line_endings(self):
        (self.repo/"source.h").write_bytes(self.data.replace(b"\n",b"\r\n"))
        record={"bytes":len(self.data),"sha256":hashlib.sha256(self.data).hexdigest()}
        with patch.object(subject,"SOURCE_COMMIT",self.commit):
            self.assertEqual(subject.read_blob(self.repo,"source.h",record),self.data)
    def test_git_replace_cannot_substitute_source_object(self):
        original=self.git_cmd("rev-parse","HEAD:source.h").decode().strip()
        alternate=self.root/"alternate";alternate.write_bytes(b"replacement bytes\n")
        replacement=self.git_cmd("hash-object","-w",str(alternate)).decode().strip()
        self.git_cmd("replace",original,replacement)
        record={"bytes":len(self.data),"sha256":hashlib.sha256(self.data).hexdigest()}
        with patch.object(subject,"SOURCE_COMMIT",self.commit):
            self.assertEqual(subject.read_blob(self.repo,"source.h",record),self.data)
    def test_blob_substitution_rejects(self):
        with patch.object(subject,"SOURCE_COMMIT",self.commit),self.assertRaisesRegex(RuntimeError,"blob identity"):
            subject.read_blob(self.repo,"source.h",{"bytes":len(self.data),"sha256":"0"*64})

class OffloadMetadata(unittest.TestCase):
    # Captured 86bc830 entry schema; this fixture supplies its own path and length.
    path="D:/new/private.obj"
    object_bytes=217673
    host="host-x86_64-pc-windows-msvc-\tfile://D:/new/private.obj#offset=58443&size=0\n"
    device="hip-amdgcn-amd-amdhsa--gfx1100\tfile://D:/new/private.obj#offset=58443&size=105552\n"
    good=host+device
    def inspect(self,text):
        return subject.inspect_offload_targets(text,self.path,self.object_bytes)
    def test_captured_native_schema_and_target_features(self):
        self.assertEqual(self.inspect("Format: COFF-x86-64\n"+self.good),
            ["hip-amdgcn-amd-amdhsa--gfx1100"])
        native_path=r"D:\a\_temp\flitzfs-rocm-sdk\hipcomp-offline\hipcomp_lz4.obj"
        self.assertEqual(subject.inspect_offload_targets(self.good.replace(self.path,native_path),
            native_path,self.object_bytes),["hip-amdgcn-amd-amdhsa--gfx1100"])
        self.assertEqual(self.inspect(self.good.replace("gfx1100","gfx1100:xnack-:sramecc+")),
            ["hip-amdgcn-amd-amdhsa--gfx1100:xnack-:sramecc+"])
    def test_usage_output_cannot_pass_target_inspection(self):
        with self.assertRaisesRegex(RuntimeError,"target count"):
            self.inspect("USAGE: llvm-objdump.exe --offload-fatbin gfx1100")
    def test_foreign_multiple_and_missing_targets_reject(self):
        for text in (self.good.replace("gfx1100","gfx1101"),self.good+self.device,
                     self.good.replace("hip-","cuda-"),self.host,"Format: COFF-x86-64"):
            with self.subTest(text=text),self.assertRaisesRegex(RuntimeError,"target"):
                self.inspect(text)
    def test_malformed_uri_cannot_supply_target_identity(self):
        for text in (self.good.replace("#offset=58443",""),self.good.replace("&size=105552","&size=x"),
                     self.good.replace("#offset=58443","#offset=1234567890")):
            with self.subTest(text=text),self.assertRaisesRegex(RuntimeError,"target metadata"):
                self.inspect(text)
    def test_metadata_size_limit(self):
        for text in ("","x"*16385):
            with self.assertRaisesRegex(RuntimeError,"metadata size"):self.inspect(text)
    def test_exact_empty_host_placeholder_required(self):
        for text in (self.device,self.host+self.good,self.good.replace("&size=0","&size=1"),
                     self.good.replace("host-x86_64-pc-windows-msvc-","host-x86_64-unknown-linux-gnu-")):
            with self.subTest(text=text),self.assertRaisesRegex(RuntimeError,"target|placeholder"):
                self.inspect(text)
    def test_object_uri_and_extents_are_bound(self):
        for text in (self.good.replace("private.obj","other.obj"),
                     self.good.replace("&size=105552","&size=0"),
                     self.good.replace("#offset=58443","#offset=217674"),
                     self.good.replace("&size=105552","&size=217673")):
            with self.subTest(text=text),self.assertRaisesRegex(RuntimeError,"target"):
                self.inspect(text)
    def test_object_identity_is_bounded(self):
        for path,size in ((self.path,True),(self.path,0),(self.path,16*subject.MIB+1),
                          ("bad\npath",self.object_bytes),("",self.object_bytes)):
            with self.subTest(path=path,size=size),self.assertRaisesRegex(RuntimeError,"object identity"):
                subject.inspect_offload_targets(self.good,path,size)


class RuntimeOffloadMetadata(unittest.TestCase):
    path="D:/new/multi.obj"
    targets=("gfx1100","gfx1201")
    host="host-x86_64-pc-windows-msvc-\tfile://D:/new/multi.obj#offset=32&size=0\n"
    first="hip-amdgcn-amd-amdhsa--gfx1100\tfile://D:/new/multi.obj#offset=32&size=128\n"
    second="hip-amdgcn-amd-amdhsa--gfx1201\tfile://D:/new/multi.obj#offset=160&size=128\n"
    def inspect(self,text):
        return subject.inspect_offload_targets(text,self.path,1024,self.targets)
    def test_exact_multi_target_inventory(self):
        self.assertEqual(self.inspect(self.host+self.first+self.second),
            ["hip-amdgcn-amd-amdhsa--gfx1100","hip-amdgcn-amd-amdhsa--gfx1201"])
    def test_missing_duplicate_foreign_or_overlapping_image_rejects(self):
        for text in (self.host+self.first,self.host+self.first+self.first,
                     self.host+self.first+self.second.replace("gfx1201","gfx1200"),
                     self.host+self.first+self.second.replace("offset=160","offset=100")):
            with self.subTest(text=text),self.assertRaisesRegex(RuntimeError,"target"):
                self.inspect(text)
    def test_multi_target_uri_stays_bound_to_exact_object(self):
        with self.assertRaisesRegex(RuntimeError,"target"):
            self.inspect(self.host+self.first+self.second.replace("multi.obj","other.obj"))

if __name__=="__main__":
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted fixture required; never the owner PC")
    unittest.main()

