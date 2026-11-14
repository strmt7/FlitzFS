# Isolated HIP compression worker — experimental

14 November 2026. Isolated SDK/CPU qualification and one actual AMD GPU
compression diagnostic passed at 979cd512. Useful end-to-end benefit and native
filesystem write integration remain unproved. This is not a release, driver installer or mounted product.

## Route and containment

The optional lz4_runtime_fixture SDK input requires all preceding candidate,
native-sync, typed-link and static CPU-decoder inputs. It creates a separate
flitzfs_hip_lz4_worker executable; the default worker, helper, filesystem and
normal builds retain their existing routes. No normal file operation launches it.

The explicit --driver-lz4-adapter route takes an ordinal, LUID, single-node mask,
exact installed-runtime digest and protected worker channel. It requires the
existing 128-MiB/20%-CPU/single-process/below-normal/kill-on-close job before
reading arguments. The request is privately owned and the independent CPU LZ4
decoder must be available before runtime discovery or loading.

The worker pins its private module directory and read-only file, checks exact
size/SHA-256 and excludes unexpected files. The installed runtime uses the
existing pinned-system-module signature/hash verifier and restricted DLL search.
The kernel getter/table is used only after module authentication and identity
checks. No compiler/runtime package is installed on the owner machine.

Only independently verified complete-slab results may be encoded into a reply.
Explicit stream/buffer retirement and kernel/runtime module retirement must
succeed before publication. The supervisor still requires successful exact
child and supervisor retirement before copying a response; the receiving caller
must independently CPU-verify the LZ4 reply and enforce current result resources.
Unknown exits, stale results and missing/incompatible capabilities do not grant
acceptance or retry authority.

## Target coverage

The separate offline runtime fixture obtains the bounded target list from the
pinned toolchain's architecture-selection mechanism. It does not infer hardware
from the owner's model name. Native object inspection requires one exact host
placeholder, the complete target set, file-bound nonoverlapping device-image
extents, AMDGPU ELF identity, matching native architecture flags and both entry
symbols for every image. Physical source and object bytes remain checked.

The staged module's source-bound receipt and object digest generate its compiled
coverage table. Runtime device properties must match a qualified image and wave,
with the existing geometry, two function-attribute queries and memory gates.
The current RDNA fixture grants wave32 coverage only. The wave64 refusal symbol
is not evidence of wave64 support. Unknown hardware combinations remain CPU
readable; adding another code image requires its own qualification.

The earlier gfx1100-only object did not establish RX 9070 XT coverage. A read-only
post-restart host query found an RX 9070 XT; no kernel was run by that query.
The new build must prove its gfx1201 image rather than assuming compatibility.

## Qualification and artifacts

Twelve CPU staging cases check source/object leases, target inventory, import and
getter closure, source links, immutable copies and exclusive new outputs. Native
metadata cases additionally reject missing, foreign, duplicate and overlapping
images, wrong object URIs and wrong advertised architectures.

The SDK-typed compute tests retain their positive/fault matrix and raw decoder
control, with additional coverage selection checks. A real bounded child probe
checks the authentic private bundle and refuses missing/tampered copies, malformed
frames/selectors and a missing or incorrect runtime. These are ten confirmed
CPU/refusal child exits and no AMD module loading; they are not GPU results.
All original twenty worker cases and ordinary CI gates remain required.

The optional experimental artifact contains only this project's diagnostic
executables, the private kernel module and all required notices. It contains no
AMD runtime, SDK or driver. Every file has a source-bound manifest/hash, and the
expanded/compressed artifact caps remain 8 MiB with at most32 files. A passing
CPU/refusal job authorizes no automatic owner-device execution.

The separate --gpu-once diagnostic is explicit and synthetic: two2048-byte inputs,
one request, no file-data writes or retries, existing parent128/256-MiB/two-process/
20%-CPU/CPU-time limits, child128-MiB/20%-CPU/15-second limits, fresh DXGI memory
admission and completion transition, CPU reply verification and exact retirement.
Resource/ownership failure must stop further GPU attempts. A source audit and
current-source isolated qualification must precede any owner trial.

No process timeout or user-mode check can guarantee immunity to a driver/kernel
fault. The existing historical retirement uncertainty remains open. Do not
describe compiler output, CPU mocks or admission refusals as GPU execution.

## Next product connection

After actual bounded GPU output is verified, connect the accepted representation
to native transactional storage with an independent CPU reader, then the Windows
file-operation path. Preserve immediate CPU selection and quarantine behavior.
Cold collective compression remains a separate size-first requirement:
COLD_COMPRESSION_RESEARCH.md permits larger shared groups and more work where
total size justifies practical access costs. Experiment caps are not permanent
filesystem group limits.


## First publishing-source build

At dfe94ac, all six provider-selected targets compiled and passed native image,
architecture, symbol, extent and unchanged-input checks. All five ordinary
CPU/native/sanitizer configurations passed (145 native tests), but the SDK host
build failed: two executable targets attempted the same exclusive bundle
generation. The new runtime worker/admission probe did not run and no AMD module
loaded. The correction assigns each bundle to one shared custom target and
preserves the exclusive-create guard. [Retained failure and native target records](evidence/lz4-runtime-build-dfe94ac.json).

## Transport versus worker rejection

At d3073fe the shared-bundle build correction succeeded: both worker executables
and the private module linked, and all six native target images passed inspection.
The optional admission test then failed with "invalid asynchronous worker admission".
Its 12-byte truncated frame was below the transport's 32-byte minimum, so it
never reached the LZ4 worker. This was an incorrect test expectation, not permission
to relax the transport. The corrected probe separately requires that short frame
to throw before consuming an admission slot; a 64-byte LZ4 header with its source
directory missing exercises the worker rejection and confirmed retirement.
[Retained exact-source failure](evidence/lz4-runtime-admission-d3073fe.json).
The corrected full admission suite still needs execution before artifact promotion.

## Admission passed; artifact packaging correction

At 516e4e9 the corrected private-worker admission passed55 checks with10
confirmed child exits. The SDK compute matrix passed828 checks across 58
scenarios (6 positive,52 refusal), and the raw decoder control passed8 checks.
All20 original worker-admission cases passed. These remain CPU/refusal results.

The run then stopped before promotion because a pre-existing topology loop
rebound the argparse namespace name to its argument list; the newly added final
package condition exposed that collision. Rename the loop's local command
arguments, preserving the parsed options and every qualification gate.
[Exact failed-run receipt](evidence/lz4-runtime-packaging-516e4e9.json).
No artifact from this incomplete run is qualified for owner execution.

## Executed runtime and owner-device qualification at 979cd512

[The exact evidence](evidence/hip-lz4-runtime-979cd512.json) binds the source,
SDK job 132378683450/run 43572160545, archive/binary hashes and owner observations.
The complete SDK run passed, including six native architecture images,
55 admission checks/10 confirmed child exits, 828 compute checks across 58 scenarios,
eight raw-control checks and all 20 original worker cases. All five ordinary configurations and all 13 integration jobs also passed at
979cd512 (run 43572160653). The earlier admission-test and packaging failures
remain retained above.

Before compute, bounded metadata confirmed the installed runtime and matching
HIP/DXGI LUID. HIP returned node mask 0; separate native Windows topology proved
one physical node on that exact AMD adapter. The existing worker contract
independently repeats that proof before accepting mask 1. No identity guard,
driver, runtime installation or system setting was changed.

One explicit two-block synthetic request then compressed 4,096 input bytes into
118 payload bytes. Complete-slab guards and independent CPU decompression passed
before publication and again at the receiving parent. Explicit device bytes
were 16,544. The cold worker exchange was 575,130,900 ns; outer supervision observed
647 ms. Worker/controller job peaks were 80,535,552/82,534,400 bytes.
Child exit and transport quiescence were confirmed, all staged bytes remained
unchanged and postflight found no owned worker/probe processes.

These are payload-only correctness observations. They exclude native file/index/
record costs and establish no comparative compression, throughput or power-loss
claim. No LZ4 stored codec or ordinary filesystem GPU route is added. Do not repeat
this unchanged diagnostic: the next implementation connects storage/CPU recovery/
Windows writes and asynchronous fallback, followed by useful net-benefit testing.
## Native compression composition diagnostic

The separate experimental [native LZ4 composition fixture](NATIVE_LZ4_COMPOSITION.md) adds hosted CPU candidate/fallback verification and an explicit GPU-to-native-file diagnostic. It requires matching qualification before owner execution. Normal filesystem I/O does not launch this probe. Its exact-source SDK and 13-job core qualification passed at a047952. One actual GPU payload was stored and independently CPU-recovered; useful end-to-end benefit and physical durability remain unproved.

The new [asynchronous compression controller](ASYNC_LZ4_CONTROLLER.md) reuses bounded worker supervision and evidence-aware recovery while making CPU source selection terminal and independent of worker retirement. Its 13-job core matrix and full SDK qualification passed at f08e6b8; ordinary Windows I/O remains CPU-only.

The experimental [Windows LZ4 advice bridge](NATIVE_WINDOWS_LZ4_ADVICE.md) forwards optional verified candidates through owned session writes and actual SDK dispatch. Existing access, budget, metadata, count and terminal-failure rules remain; its added source-bound tests are pending. This does not start GPU work from normal writes.
