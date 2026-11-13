# Independent CPU decoder in the SDK worker (experimental)

The normal filesystem remains CPU-only. This separate hosted-VM prerequisite
statically supplies the existing independent CPU LZ4 decoder to a possible future
HIP compression worker. It adds no compression runtime call, candidate-module
load, kernel launch, stored codec or normal filesystem worker route.

## Source and build admission

The optional cpu_lz4 workflow input requires codec_candidate,
native_sync_candidate and link_candidate too. All inputs default to false.
Only the explicit option checks out lz4/lz4 at
aa4647fad575d01a874aaaadcdbe8e6a40e072f5 (v1.11.1).
The existing whole ROCm distribution still governs HIP/compiler/import-library
identity; a CPU codec never substitutes for any SDK component.

Before SDK provisioning, the qualifier verifies the private checkout's actual
Git root, exact revision, tracked cleanliness and canonical Git blob identities.
Four bounded files are admitted: lib/lz4.c, lib/lz4.h, lib/LICENSE and the root
LICENSE. Exact physical bytes or one whole-file CRLF checkout are accepted;
mixed/altered/oversized input rejects. The aggregate canonical source cap is
512 KiB. NEW exclusive outputs retain canonical bytes and physical observations.
The source checkout is never rewritten. Partial outputs remain isolated on the
disposable runner for diagnosis and cannot be reused.

The CMake fixture independently verifies all four staged byte lengths and
SHA-256 pins before enabling the C compiler and building one static LZ4 target.
It invokes no upstream CMake, application, installer or multithreaded codec.
The worker and its CPU tests link the same core, static decoder and static MSVC
runtime. No system-installed LZ4 is searched for in this option. Default SDK
configuration still forces LZ4 off and cannot infer support from the host.

Both complete upstream license notices are retained. The root notice describes
the repository's licensing layout; lib/LICENSE contains the library's actual
BSD copyright, conditions and disclaimer. Source and staged identities are
independently rechecked after compilation. The optional own-binary diagnostic
also includes both notices; the private compression DLL stays unexported and
outside the worker bundle.

## CPU capability and qualification

The worker's explicit --verify-cpu-lz4 diagnostic runs only inside the existing
128-MiB, one-process, 20%-CPU creation-time job. It checks that AMD runtime/kernel
modules are absent, then reports support through successful exit or the existing
unsupported-configuration code. It never opens a channel, discovers a device,
loads a bundle/module, initializes HIP or produces candidate bytes.
No ordinary file operation invokes this diagnostic.

A separate trusted probe supervises one exact worker process and requires its
actual successful retirement, expected capability status and existing memory/
wall limits. The original 20 worker admission/retirement cases remain unchanged.
The optional fixture also runs the existing independent verifier, owned worker
frames and complete-slab tests against the exact same static CPU decoder.

A second default-raw build uses the already verified SDK and original object,
with no second SDK download or modified source. Its supervised child must refuse
CPU LZ4 explicitly. This checks both support states within the manual superset.
All prior SDK, runtime-export, object, typed API, private-link, worker and resource
checks remain mandatory. No timeout, queue, thread, process or memory cap grows.

New source-staging tests cover actual fresh Git identity, exclusive collision,
wrong root/revision/blob/digest/size, tracked changes, canonical CRLF capture and
staged tampering/inventory changes. Their fixture source and pins are synthetic;
the manual SDK fixture must independently admit the actual upstream source.
Publishing-source execution remains pending.

## Current evidence and remaining work

At 38229d1 the preceding slab layer passed all five core configurations and all
13 ordinary integration jobs; enabled builds reported 968 slab checks and raw
builds 939. A live owner-host model/GUI workflow passed with confirmed retirement,
zero remaining owned processes and complete fixture cleanup.
See evidence/gpu-lz4-slab-38229d1.json and evidence/owner-gui-38229d1.json.
Those results are CPU checks, not actual HIP compression or power-loss proof.

The current step still requires its own CPU/raw/sanitizer/Windows and complete
manual SDK qualification. Actual compression execution additionally needs
qualified target/wave selection, authenticated candidate-module/worker binding,
fresh resource admission, independent candidate verification, exact retirement
and measured whole-operation benefit. Historical D01 and full-product gates
remain open. No owner device experiment follows from successful compilation.
