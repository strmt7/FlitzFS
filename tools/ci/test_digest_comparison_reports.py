#!/usr/bin/env python3
"""Pure report-oracle negatives; no subprocesses, fixtures or owner-host use."""
import copy
import unittest
from compare_digest_acceleration import micro,validate_stdio

def report(policy="auto"):
    return dict(schema=1,complete=True,backend="windows-x64-upstream-asm" if policy=="auto" else "portable",
                degree=8 if policy=="auto" else 1,samples=[
        dict(bytes=size,sample=sample,mode=mode,hashed_bytes=8388608,ms=1.5)
        for size in (4096,65536,1048576) for sample in range(5)
        for mode in ("scalar-oracle","production")])

class Reports(unittest.TestCase):
    def test_binary_transport_probe(self):
        payload=bytes((0,10,13,10,26,128,255));text=payload.replace(b"\n",b"\r\n")
        for default in (payload,text):
            value=validate_stdio(default,text,payload)
            self.assertTrue(value["complete"])
            self.assertEqual(value["binary_bytes"],7)
        for args in ((text,text,text),(payload,payload,payload),(b"wrong",text,payload)):
            with self.assertRaises(RuntimeError):
                validate_stdio(*args)

    def test_complete_auto_and_scalar(self):
        for policy in ("auto","scalar"):
            value=report(policy)
            self.assertIs(micro(value,policy),value)
    def test_missing_relabelled_unsafe_or_duplicate(self):
        def reject(change):
            value=report();change(value)
            with self.assertRaises(RuntimeError):
                micro(value,"auto")
        reject(lambda v:v.update(schema=True))
        reject(lambda v:v.update(complete=1))
        reject(lambda v:v.update(backend="portable"))
        reject(lambda v:v.update(degree=1))
        reject(lambda v:v["samples"].pop())
        reject(lambda v:v["samples"].__setitem__(1,copy.deepcopy(v["samples"][0])))
        reject(lambda v:v["samples"][0].update(sample=False))
        reject(lambda v:v["samples"][0].update(bytes=4096.0))
        reject(lambda v:v["samples"][0].update(hashed_bytes=1))
        reject(lambda v:v["samples"][0].update(ms=float("nan")))
        reject(lambda v:v["samples"][0].update(ms=float("inf")))
        reject(lambda v:v["samples"][0].update(ms=-1))
        reject(lambda v:v["samples"][0].update(mode="other"))
        value=report("scalar");value["degree"]=True
        with self.assertRaises(RuntimeError):
            micro(value,"scalar")

if __name__=="__main__":
    unittest.main()
