"""CPU-only admission/retention checks for private offline compression assets."""
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from hipcomp_lz4_entry import ASSETS, source_assets, stage_entry, unchanged

REPO=Path(__file__).resolve().parents[2]

class EntryAssets(unittest.TestCase):
    def fixture(self,name):
        root=Path(name).resolve()
        (root/"src/lowlevel").mkdir(parents=True)
        return root

    def test_actual_canonical_assets(self):
        values=source_assets(REPO)
        self.assertEqual(len(values),4)
        self.assertEqual([row["path"] for row in values],[row["path"] for row in ASSETS])
        self.assertEqual(sum(len(row["body"]) for row in values),7562)

    def test_new_staging_and_exact_records(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-entry-") as name:
            root=self.fixture(name)
            entry,records,proof=stage_entry(REPO,root,[],68*1024)
            self.assertEqual(entry,root/ASSETS[-1]["target"])
            self.assertEqual(len(records),4)
            for spec in source_assets(REPO):
                self.assertEqual((root/spec["target"]).read_bytes(),spec["body"])
            unchanged(REPO,proof)

    def test_complete_budget_rejects_before_writes(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-entry-") as name:
            root=self.fixture(name)
            for extra in (-1,True,16*1024*1024):
                with self.assertRaises(ValueError):
                    stage_entry(REPO,root,[],extra)
            self.assertFalse(any((root/row["target"]).exists() for row in ASSETS))

    def test_both_collision_positions_preserve_existing_bytes(self):
        for spec in ASSETS:
            with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-entry-") as name:
                root=self.fixture(name)
                path=root/spec["target"]
                path.write_bytes(b"sentinel")
                with self.assertRaises(ValueError):
                    stage_entry(REPO,root,[],0)
                self.assertEqual(path.read_bytes(),b"sentinel")
                for other in ASSETS:
                    if other is not spec:self.assertFalse((root/other["target"]).exists())

    def test_manifest_alias_rejects_before_writes(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-entry-") as name:
            root=self.fixture(name)
            with self.assertRaises(ValueError):
                stage_entry(REPO,root,[dict(target=ASSETS[0]["target"].upper(),bytes=1)],0)
            self.assertFalse(any((root/row["target"]).exists() for row in ASSETS))

    def test_partial_failure_retains_only_new_private_input(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-entry-") as name:
            root=self.fixture(name)
            real_open=Path.open
            def fail_second(path,*args,**kwargs):
                if path==root/ASSETS[1]["target"] and args and args[0]=="xb":
                    raise OSError("synthetic second exclusive-create failure")
                return real_open(path,*args,**kwargs)
            with patch.object(Path,"open",fail_second):
                with self.assertRaises(OSError):
                    stage_entry(REPO,root,[],0)
            self.assertEqual((root/ASSETS[0]["target"]).read_bytes(),source_assets(REPO)[0]["body"])
            self.assertFalse((root/ASSETS[1]["target"]).exists())

    def test_control_observation_must_remain_identical(self):
        values=source_assets(REPO)
        proof=[{key:value for key,value in row.items() if key!="body"} for row in values]
        changed=copy.deepcopy(proof)
        changed[0]["physical"]["physical_bytes"]+=1
        with self.assertRaises(ValueError):unchanged(REPO,changed)


class DeviceObjectAdmission(unittest.TestCase):
    def fixture(self,name,image=None):
        root=Path(name).resolve()
        obj=root/"fixture.obj"
        prefix=b"outside-before"
        image=image if image is not None else b"\x7fELF\x02\x01"+b"synthetic-image"
        obj.write_bytes(prefix+image+b"outside-after")
        line="hip-amdgcn-amd-amdhsa--gfx1100\tfile://"+str(obj)+"#offset="+str(len(prefix))+"&size="+str(len(image))
        return root,obj,image,line

    def reader(self,mode="ok"):
        def run(bounds,arguments,report,name,seconds):
            header="Format: elf64-amdgpu\nMachine: EM_AMDGPU\nFlags [\n  EF_AMDGPU_MACH_AMDGCN_GFX1100 (0x41)\n]\n"
            symbols="  Name: flitzfs_candidate_lz4_wave32 (1)\n  Name: flitzfs_candidate_lz4_wave64 (2)\n"
            if mode=="wrong-target":header=header.replace("GFX1100","GFX1201")
            if mode=="arch":header="Format: elf64-x86-64\nMachine: EM_X86_64\n"
            if mode=="missing":symbols=symbols.splitlines()[0]+"\n"
            if mode=="duplicate":symbols+=symbols
            return dict(stdout=header if "--file-header" in arguments else symbols,
                        stderr="warning" if mode=="warning" else "",
                        output_truncated=mode=="truncated")
        return run

    def inspect(self,root,obj,line,mode="ok"):
        from hipcomp_lz4_entry import inspect_entry_object
        report=dict(commands=[])
        with patch("qualify_rocm_sdk.run",self.reader(mode)):
            inspect_entry_object(None,root/"unused-reader.exe",obj,root,line,report)
        return report

    def test_exact_selected_bytes_and_native_symbols(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-device-") as name:
            root,obj,image,line=self.fixture(name)
            result=self.inspect(root,obj,line)
            self.assertEqual((root/"bounded-entry-device.elf").read_bytes(),image)
            self.assertTrue(result["entry_object"]["complete"])
            self.assertFalse(result["entry_object"]["loaded"])
            self.assertFalse(result["entry_object"]["executed"])

    def test_absent_duplicate_or_outside_image_rejects(self):
        for mode in ("absent","duplicate","offset","size","zero"):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(prefix="flitzfs-lz4-device-") as name:
                root,obj,image,line=self.fixture(name)
                if mode=="absent":line="unrelated"
                elif mode=="duplicate":line+="\n"+line
                elif mode=="offset":line=line.split("#offset=")[0]+"#offset=999999999&size=1"
                elif mode=="size":line=line.split("&size=")[0]+"&size=16777217"
                elif mode=="zero":line=line.split("&size=")[0]+"&size=0"
                with self.assertRaises(ValueError):self.inspect(root,obj,line)
                self.assertFalse((root/"bounded-entry-device.elf").exists())

    def test_non_ELF_or_short_image_rejects(self):
        for image in (b"not-device-ELF",b"\x7fELF",b"\x7fELF\x01\x01"):
            with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-device-") as name:
                root,obj,_,line=self.fixture(name,image)
                with self.assertRaises(ValueError):self.inspect(root,obj,line)
                self.assertFalse((root/"bounded-entry-device.elf").exists())

    def test_native_arch_symbol_warning_and_truncation_refusals(self):
        for mode in ("arch","wrong-target","missing","duplicate","warning","truncated"):
            with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-device-") as name:
                root,obj,image,line=self.fixture(name)
                with self.assertRaises(ValueError):self.inspect(root,obj,line,mode)
                self.assertEqual((root/"bounded-entry-device.elf").read_bytes(),image)

    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-device-") as name:
            root,obj,image,line=self.fixture(name)
            output=root/"bounded-entry-device.elf";output.write_bytes(b"sentinel")
            with self.assertRaises(FileExistsError):self.inspect(root,obj,line)
            self.assertEqual(output.read_bytes(),b"sentinel")

if __name__=="__main__":unittest.main()

