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
These staging cases passed at 94ec882; the executed evidence is below.

## Current evidence and remaining work

At 38229d1 the preceding slab layer passed all five core configurations and all
13 ordinary integration jobs; enabled builds reported 968 slab checks and raw
builds 939. A live owner-host model/GUI workflow passed with confirmed retirement,
zero remaining owned processes and complete fixture cleanup.
See evidence/gpu-lz4-slab-38229d1.json and evidence/owner-gui-38229d1.json.
Those results are CPU checks, not actual HIP compression or power-loss proof.

This static-decoder step qualified CPU/raw/sanitizer/Windows and complete
manual SDK checks at 94ec882. Actual compression execution additionally needs
qualified target/wave selection, authenticated candidate-module/worker binding,
fresh resource admission, independent candidate verification, exact retirement
and measured whole-operation benefit. Historical D01 and full-product gates
remain open. No owner device experiment follows from successful compilation.

## Retained source-fixture failures

The initial manual run at fea9fc6 passed the preceding 6/28/9 source cases, then
failed one of the ten new staging cases: Git considered all four manually
rewritten CRLF fixture files modified. SDK provisioning, compilation and worker
execution never started. This is a test-fixture failure, not decoder or GPU
execution evidence. [First failure](evidence/cpu-lz4-source-fixture-fea9fc6.json).

At 09b7a56, index renormalization preserved canonical blobs, but checkout-index
left the stat-unchanged LF files in place despite --force. The actual CRLF-byte
assertion refused that incomplete fixture.
[Second failure](evidence/cpu-lz4-checkout-fixture-09b7a56.json).

The current upstream entry.c returns early for stat-unchanged paths before its
force check. Its CRLF tests remove their own fixture files before checkout-index.
The corrected fixture therefore unlinks ONLY its four tracked synthetic files
after proving no staged canonical change. Git then materializes their preserved
index blobs. Actual whole-file CRLF, clean Git state, canonical staging and
unchanged physical bytes during production staging are all still mandatory.
No production guard or pin was relaxed and no case was removed. The later
executed 94ec882 results are recorded below. No owner operation or SDK download occurred in either failure.

At 4797903, actual CRLF bytes and index-LF/worktree-CRLF observations passed,
but Git still reported modified paths. Linux reproduced the same case; no
Windows-specific cause is inferred.
[Third failure](evidence/cpu-lz4-index-fixture-4797903.json).
The fixture now records filtered hashes, actual diffs, version and configuration,
refreshes its index from those physical bytes, and requires byte-identical
canonical index/HEAD plus clean Git state. All production guards and ten cases
remain. This diagnostic/refinement passed the ten staging cases on Windows and Linux
at 94ec882.

## Executed static decoder qualification at 94ec882

All five core configurations and all 13 ordinary integration jobs passed.
Each configuration passed all ten source-staging cases; Linux/ASan logs contain
125 tests, raw Windows 129 and native Windows 144. The manual SDK superset
confirmed the same static decoder's 806 verifier, 621 framing and 968 slab checks.
Enabled and raw-control workers retired with the expected success/unavailable
status, and all original 20 admission exits passed. Source and staged bytes
remained unchanged. Both library notices were independently verified in the
own-binary diagnostic; its private compression DLL and AMD runtime were absent.

[Source-bound evidence](evidence/cpu-lz4-worker-94ec882.json).
The separate source-snapshot job failed its old file-count preflight and is
recorded separately; it does not invalidate these executed CPU/SDK checks or
become a passing snapshot claim. Actual GPU compression and product completion
remain unfinished.
