#!/usr/bin/env python3
"""Bind compiled digest source and known-answer vectors to reviewed upstream bytes."""
import hashlib
import json
from pathlib import Path
import subprocess

EXPECTED = json.loads(r'''[{"source":"c/blake3.h","path":"third_party/blake3/c/blake3.h","git_blob":"c5b799870ad392890e33478629788919a8e89930"},{"source":"c/blake3.c","path":"third_party/blake3/c/blake3.c","git_blob":"00f91f444922df7c8e7b5ff69343852cbbf4c613"},{"source":"c/blake3_impl.h","path":"third_party/blake3/c/blake3_impl.h","git_blob":"88e71e41e90f3f3cca4d39f19d50e74325fb5ce2"},{"source":"c/blake3_dispatch.c","path":"third_party/blake3/c/blake3_dispatch.c","git_blob":"14dfbbe0c8f385296adf691643481b87e8768522"},{"source":"c/blake3_portable.c","path":"third_party/blake3/c/blake3_portable.c","git_blob":"062dd1b47fb6424f4d3db4c6edd2f90efcb92973"},{"source":"LICENSE_A2","path":"third_party/blake3/LICENSE_A2","git_blob":"d512ca94d0ca6bf6a7272b16c83f51b7045902ca"},{"source":"LICENSE_A2LLVM","path":"third_party/blake3/LICENSE_A2LLVM","git_blob":"8d11bec0e86f88276b155cd3214a3de45cdafeb7"},{"source":"LICENSE_CC0","path":"third_party/blake3/LICENSE_CC0","git_blob":"0e259d42c996742e9e3cba14c677129b2c1b6311"},{"source":"test_vectors/test_vectors.json","path":"third_party/blake3/test_vectors/test_vectors.json","git_blob":"f6da91792c6cdf5c6a0f6dad01803045bb204a68"}]''')
root=Path(__file__).resolve().parents[2]
manifest=json.loads((root/"third_party/blake3/provenance.json").read_text(encoding="utf-8"))
if manifest["commit"]!="f3149ec5bb5449af877ba20377a11008ff499fa2" or manifest["tag"]!="1.8.7" or manifest["files"]!=EXPECTED:
    raise RuntimeError("Unreviewed digest source identity or inventory")
for entry in EXPECTED:
    path=entry["path"]
    data=subprocess.run(["git","-C",str(root),"show","HEAD:"+path],
                        check=True,capture_output=True,timeout=5).stdout
    if (root/path).read_bytes().replace(b"\r\n",b"\n")!=data:
        raise RuntimeError("Compiled source differs from pinned Git bytes: "+path)
    digest=hashlib.sha1(b"blob "+str(len(data)).encode()+b"\0"+data).hexdigest()
    if digest!=entry["git_blob"]:
        raise RuntimeError("Vendored source differs from upstream: "+path)
vectors=json.loads((root/"third_party/blake3/test_vectors/test_vectors.json").read_text(encoding="utf-8"))
expected="#pragma once\n#include <array>\n#include <cstddef>\nnamespace digest_vectors {\nstruct Case {std::size_t bytes;const char* hex;};\ninline constexpr std::array<Case,35> cases{{\n"
expected+="\n".join('  {'+str(case["input_len"])+',"'+case["hash"][:64]+'"},' for case in vectors["cases"])
expected+="\n}};\n}\n"
if (root/"tests/blake3_vectors.hpp").read_text(encoding="utf-8")!=expected:
    raise RuntimeError("Known-answer fixture differs from upstream vectors")
print(json.dumps({"commit":manifest["commit"],"unmodified_files":len(EXPECTED),"known_vectors":len(vectors["cases"])}))
