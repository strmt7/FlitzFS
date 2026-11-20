# Bounded HIP kernel candidate and qualification boundary

## Direct shared-base search

The fixed-size driver worker accepts one base and up to three member extents.
The SDK module builds a two-position index with device-scope integer atomics.
The index build and member searches execute in order on the least-priority
nonblocking stream.
The worker and parent verify source identity, coordinates and original bytes.
The existing indexed writer independently compares the native representation and
its complete storage cost.
[HIP atomics](https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/how-to/hip_cpp_language_extensions.html#atomic-functions) ·
[HIP asynchronous execution](https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/how-to/hip_runtime_api/asynchronous.html) ·
[Matched trials](evidence/direct-amd-anchor-2026-11-18.json)

The sized driver mode takes an explicit member count, window grant and device
allocation envelope. Both endpoints derive exact mapping capacities from
the same checked extent geometry. The child uploads the base and builds
its index once, then reuses the slab for each funded member window. It samples
current adapter budgets after each window. The child publishes one complete
reply, and the parent accepts it after confirmed exit.
[Sized sweep evidence](evidence/amd-anchor-sweep-2026-11-18.json)

The multi-group worker retains one authenticated runtime, a least-priority
stream and a single device slab. It clears the index and rebuilds it from the
next independent base after the preceding group's work has synchronized. The
sized transport binds each group to its source digest and admits the aggregate
launch count before copying group data. Its Job Object commitment comes from
current physical and commit headroom. A batch returns advice only after every
group passes independent byte checks and the worker retires.

On the public-derived fixture with 144 member extents, four paired trials gave
median times of 0.75 seconds for the shared worker, 1.72 seconds for fresh workers
and 0.10 seconds for CPU matching and verification. At 288 member extents, two
paired trials took 0.69–0.89 seconds for the shared worker and 0.22–0.27 seconds
for CPU matching and verification. A deliberately mismatched runtime digest
rejected the GPU batch and retained the independent CPU result.
[Batch trial evidence](evidence/amd-anchor-batch-2026-11-18.json)

The transfer mode stages several member windows in one pageable upload and
downloads their results together. The base and its index stay resident for
the group. A live device grant determines the staging size. All transfers
before the tail stop at a search-dispatch boundary, and the worker checks headroom
after each dispatch. The authenticated module exposes a separate typed kernel
entry for the resident member window. [HIP synchronous transfers and streams](https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/how-to/hip_runtime_api/asynchronous.html)

At 288 member extents, the full grant reduced 97 upload/download pairs to
three. Four paired trials gave medians of 0.76 seconds for staged transfers,
0.94 seconds for one transfer per dispatch and 0.21 seconds for CPU matching.
The smaller grant used nine transfer pairs with byte-exact results. At 641
member extents, staged transfers reduced 215 pairs to five and took 0.72–1.02
seconds. The corresponding CPU trials took 0.46–0.47 seconds. Run order
affected the GPU timings. At 1,272 member extents, transfers fell from 426
pairs to six. The staged path took 1.24–1.38 seconds, the original AMD path
took 1.79–2.35 seconds and CPU matching took 0.90 seconds in both trials.
[Transfer trial evidence](evidence/amd-anchor-transfers-2026-11-18.json)

The shared-base path now retains one private request allocation at each
endpoint. All groups within an endpoint use verified views into its snapshot.
In four matched 1,272-extent pairs, median peak process-tree commitment
fell from 725 MiB to 302 MiB. Median AMD exchange time fell from 1.22 to
1.07 seconds, with mixed per-pair timing changes. Two 1,824-extent trials
completed with 397 MiB peak commitment. Their AMD exchange times were 1.07
and 1.29 seconds and their paired CPU times were 1.25 and 1.31 seconds.
These larger trials support further calibration across cold-start and
mixed-content workloads. [Snapshot and scale evidence](evidence/amd-anchor-snapshot-2026-11-18.json)

The batch builder now writes each group into the final request and releases
its source buffers before gathering the next group. The fixture charges
the final request plus the largest input group during construction. Windows
Release and AddressSanitizer checks verified the nested request bytes,
source digests, rejection of incomplete batches and snapshot ownership.
[Builder and input evidence](evidence/amd-anchor-streamed-prep-2026-11-18.json)

The opt-in cohort fixture accepts a bank with one 64 KiB sample per base.
The related cohort shifts regions from the selected base. In the mixed
cohort, alternate members use a different sample. Each run prepares one
coalesced AMD exchange, obtains an independent CPU reply, and checks exact
reply equality and retirement. Match coverage counts verified nonzero tile
records before encoding any stored representation. The bank uses five
decoded Silesia samples and a compressed sample.
[Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) ·
[zlib-ng corpus collection](https://github.com/zlib-ng/corpora)

Fifteen live AMD exchanges matched the CPU replies. At 1,398 extents,
five coalesced AMD exchanges took 1.02–1.27 seconds and the CPU reference
took 0.87–0.96 seconds. The two cohort orders include first owned exchanges
after 55 and 71 seconds without work from the fixture. Related cases
matched every tile. The mixed cases matched about half.

The producer now hashes the bytes it just copied and binds its typed
group views directly to the private snapshot. The worker verifies the
received frame independently. In three alternating 1,824-extent pairs,
median producer preparation fell from 57.37 to 35.22 milliseconds.
Complete AMD timings varied. At 2,304 extents the related and mixed AMD
exchanges took 2.05 and 1.79 seconds, against CPU references of 1.59 and
1.41 seconds. Peak owned host commitment was about 480 MiB. The tested
workloads still lack consistent evidence for automatic AMD preference.
[Live calibration and qualification](evidence/amd-anchor-cohorts-2026-11-18.json)

The coalesced worker can attach a diagnostic phase profile to its reply.
The probe checks the profile against the request digest, adapter, transfer
plan and allowed number of dispatches between stream waits. It verifies all
matching records against the independent CPU reply before accepting the
observation.

At 1,824 extents the first profiles measured about 430 ms in launch and
stream waits, 21 ms in uploads and 1 ms in the repeated budget checks.
Device initialization plus stream and slab setup varied from 314 to
620 ms in those two runs. In four alternating pairs, queuing up to four
existing dispatches reduced median launch and wait time from 432.34 to
395.83 ms. The complete AMD exchange median moved from 1.91 to 1.42
seconds in the same trials. Other phases varied and CPU references
finished sooner in each pair.

The queue keeps a fresh memory check before each dispatch and waits for
completion before reusing arguments or reading output. The larger queue
grant is exercised through the profiled comparison mode. The probe also
checks the transport's exact host-memory requirement before preparing its
input. Windows Release, AddressSanitizer and the SDK build passed. The
final live controls covered the original reply, runtime refusal, and both
queue settings at 1,686 extents. The largest GPU test peaked at about
397 MiB of host commitment.
[Phase, queue and admission evidence](evidence/amd-anchor-queue-phases-2026-11-18.json) ·
[AMD stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/reference/hip_runtime_api/modules/stream_management.html) ·
[Windows video memory budgets](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)

The worker appends canonical little-endian match records in one contiguous
region per group. The controller verifies the reply in place against the
original bytes and the digest already bound to the private request.

In six matched pairs on the Windows host at 864 member extents, median
controller verification fell from 45.54 to 32.04 ms for related data and from
26.95 to 17.46 ms for mixed data. Worker reply encoding also improved in the
median. Every AMD reply matched the independent CPU reply byte for byte.
CPU matching and reply encoding still finished sooner than the complete AMD
exchange on both cohorts. These are shared-base exchange measurements.
[Host reply verification evidence](evidence/amd-anchor-reply-verification-2026-11-18.json)

The anchor matcher reads each complete eight-byte window without an alignment
requirement. It uses the first differing byte to end an extension and checks
the final short tail bytewise. The limit follows both source positions, so a
word read fits within both extents. Candidate selection and reply verification
use the existing per-tile order and exact independently checked plaintext.
The approach uses the same bounded word comparison technique as the FlitzZip
GPU dictionary matcher. All 16 paired runs on the RX 9070 XT matched the
independent CPU replies exactly. The runs used 570 or 1,818 member extents.
Stream and slab setup remained the largest measured phase. The measured
variation leaves a complete exchange speedup unqualified.

The current CPU and AMD owners reached the same occupied sizes on the public
code and documentation revisions. AMD advice supplied the chosen encoding
for 16 code extents and four documentation extents. A foreground read test
cancelled a pending exchange and verified the completed group through the
independent CPU reader.
[AMD performance guide](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html) ·
[FlitzZip dictionary matcher](https://github.com/strmt7/FlitzZip/blob/11396b5/src/gpu/hip_dictionary_matcher.hip.cpp) ·
[Matched copy-search evidence](evidence/amd-word-copy-search-2026-11-20.json)

The version 11 copy window admits a larger measured dispatch within the
same finite worker lifetime. Each group retains one index across its
dispatches and shares the stream and slab allocation with the other groups
in the batch. The versioned window leaves existing copy coordinates and
the CPU decoder intact. The worker checks the actual device grid, and the
host supplies an approved ceiling and live memory grant.
[Wide copy protocol](WORKER_CHANNEL.md#wider-amd-copy-dispatches) ·
[Wide copy evidence](evidence/amd-wide-copy-dispatch-2026-11-20.json)

Shared-base transfers use the same least-priority HIP stream as clears and
kernels. The immutable request owns queued upload bytes through completion.
The synchronous stream readback completes the window before the worker
decodes results, releases pageable host buffers or reuses the GPU slab.
[AMD HIP memory management](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/memory_management.html) ·
[AMD HIP stream management](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/stream_management.html)

In six alternating pairs on the Windows host, the mixed and Lucide
fixtures retained the same occupied sizes and verified bytes. Their
median peak job commitments fell by 16.2 and 16.4 MiB. Median mixed
worker time moved from 2.46 to 2.19 seconds. The Lucide median moved
from 2.23 to 2.31 seconds. Timing ranges overlapped for both fixtures.
Kernel and runtime artifacts were identical in each pair. After cancellation
of one admitted AMD request, five CPU groups published on subsequent
modeled idle opportunities and all files reopened byte for byte.
[Same-stream transfer evidence](evidence/amd-same-stream-transfers-2026-11-20.json)

The configured queue depth is an upper bound. The route uses the number
of dispatches that fit in the admitted slab and reserves the matching
profile trailer only when it fits the worker budget. With the limit set
to four, the mixed fixture verified two dispatches per wait on the AMD
device in all three trials. Image size and exact readback matched the
single-dispatch control. Worker timing ranges overlapped.
[Partial queue evidence](evidence/amd-partial-dispatch-groups-2026-11-20.json)

## SDK-only compression compute candidate

The [typed LZ4 compute implementation and CPU probe](HIP_LZ4_COMPUTE.md)
have a separate SDK worker. Its offline object was requalified, the worker
rebuilt and the CPU decoder probe passed. Keep authenticated coverage,
CPU-first refusal, device admission and exact retirement requirements.
Use a fresh SDK build directory after requalification. Bundle staging
creates a new set of outputs and preserves earlier qualification artifacts.
[SDK build and CPU decoder evidence](evidence/amd-anchor-transfers-2026-11-18.json)


## Current resource-envelope candidate

The source now shares a live-resource admission policy across worker, result
and match paths. [GPU_RESOURCE_ENVELOPE.md](GPU_RESOURCE_ENVELOPE.md) records the
contract, current official source captures and pending exact-source validation.
Earlier zero-growth observations below retain their historical meaning. The
new host fields are deliberately absent from the fixed diagnostic wire, so a
decoded historical observation cannot supply complete admission evidence.


Baseline: b48156fa3c6885639c7179150d838e3fe2f3275f.
Research date: 7 November 2026. This is an experimental development gate.

## Plan before implementation

1. Keep the CLI, catalog, mounted filesystem and default CTest CPU-only. No HIP
   runtime initialization, GPU execution, owner-PC operations or writable store.
2. Introduce immutable bounded batches for six simple/entropy extent mechanisms:
   raw, fill, pattern, static prefix, adaptive prefix and Huffman. At most eight
   blocks, 64 KiB per block and 256 KiB total output. Independently CPU-decode
   every input before creating a plan. Reject unsupported kinds explicitly.
3. Implement allocation-free segment decoding reusable in HIP device compilation
   and CPU semantic tests, with disjoint output extents and checked indexes.
   Every work item produces at most 4096 bytes; no unbounded device loop.
4. Compile an actual AMDGPU HIP kernel/code object without loading a GPU runtime.
   An offline compiler result is not hardware execution or runtime compatibility.
5. Add a one-outstanding-request result gate: fresh adapter-matched memory
   budgets, explicit opt-in, generation checks, all task statuses and byte-exact
   comparison with the independent CPU implementation before accepting output.
   Errors/timeouts poison the gate; late output cannot reopen it.
6. Test all caps, malformed frames, segmentation/tails, arithmetic boundaries,
   rejected budgets, stale completions, and output corruption. Re-run existing
   default/codec-disabled/sanitizer/Windows/mount and upstream-reference lanes.
7. Do not wire an unqualified kernel into filesystem reads or claim a GPU speed
   benefit. A real-runtime worker with supervised hardware tests remains a gate.

## Research and decisions

AMD documents that hipMemGetInfo on Windows accounts for the current process
and can overestimate free memory. Use a matched DXGI local-segment budget and
current process usage instead; these are OS budgets, not physical reservations.
Match adapters through the eight-byte LUID and single-node mask, not their
order or human-readable names. Unknown, stale or mismatched telemetry fails.

AMD's hipStreamDestroy can wait for work; hipFree also implies synchronization.
A user-space polling timeout therefore cannot guarantee bounded driver teardown.
The future runtime must live outside the mounted filesystem process and use an
external supervisor. Do not disable TDR, change clocks, reset the GPU, or claim
that process isolation prevents a kernel/driver freeze.

The initial candidate uses compiler-supported HIP device attributes and AMDGPU
builtins, with no device math/runtime library requirement. Compile-only CI can
validate code generation without installing a many-gigabyte SDK or a GPU driver.
Production HIP runtime loading and Windows compiler qualification are separate.
This is not a performance policy: it deliberately retains full CPU comparisons.

Primary sources:
- https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___memory.html
- https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/initialization_and_version.html
- https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___stream.html
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery
- https://llvm.org/docs/AMDGPUUsage.html
- https://releases.llvm.org/20.1.0/docs/AMDGPUUsage.html

Cross-repository reference: FlitzZip's pinned device_memory_budget.hpp and
rocm-toolchain.md. Reuse the distinction between accounting and residency,
explicit target qualification, and source-bound evidence, not its larger
allocation ceilings. No other repository is modified or private code copied.

## Implemented qualification contract

`PreparedGpuBatch` owns its payload, tasks and independent CPU result. Input
count/sizes are admitted before proportional allocation. A task covers at most
4096 output bytes, and tasks have disjoint dense output ranges. The gate owns
its own batch; moving/destroying caller buffers cannot invalidate it. Gate copy
and move are disabled. Request generations are unique within the process and
cannot wrap. No worker/runtime is started by any of these operations.

`GpuResultGate` requires opt-in, a nonzero matching LUID/single-node mask,
AMD-dedicated-memory telemetry, at most 50 ms snapshot age, and 256 MiB of
remaining local process budget after explicit buffer costs. Completion requires
a new snapshot not predating submission, no observed non-local usage growth,
all task statuses successful, exact output length and byte-for-byte equality.
The chosen 1–100 ms deadline is a result-rejection deadline, NOT a mechanism
that stops GPU execution. There is no automatic reinitialization/retry after
failure. An external worker/supervisor is still needed before live use.

The DXGI implementation neither enumerates by preference nor reserves memory.
It binds the requested LUID with EnumAdapterByLuid, checks AMD vendor/dedicated
memory/no software adapter, and queries both local and non-local process usage.
Those counters cannot prove zero paging or uninterrupted physical residency.
Current Windows tests cover rejection before OS queries, not an actual AMD
hardware budget observation. The collector is not called from normal CLI paths.

CPU tests execute the exact allocation-free device function in reverse task
order and check output canaries and independent CPU results. These are host
semantic tests. The separate Linux LLVM compile-only lane checks a linked ELF64
AMDGPU code object, gfx1201 identity, kernel symbol and metadata, and retains
source/code-object hashes and observed compiler/linker versions. No compiled binary is installed or released.
This avoids downloading the 4.87 GB / 14.84 GB unpacked Windows SDK merely to
check a small arithmetic-only kernel. It does not qualify that Windows SDK.

## Local checkpoint before publication

All 16 portable CTest entries passed with GCC/codecs, GCC/external-codecs-off
and Clang ASan/UBSan (48 matrix executions). The new test reports 1,819
assertions. A regression initially accepted a fresh-but-pre-submission budget
on completion; it failed before the fix and passed after enforcing that the
completion snapshot does not predate submission. Existing tests were retained.
This is not Windows, offline-device-compiler or hardware evidence; those need
separate results at the published commit.

A local attempt to obtain an offline compiler via Debian package metadata timed
out because this isolated container cannot resolve the repository hostname. No
compiler was installed locally. Offline device compilation is delegated to the
explicit GitHub-hosted compile-only lane, not represented as locally completed.
The owner PC, storage devices and AMD GPU remain untouched.

## Compiler-stage correction and executed evidence

The initial hosted compiler run reached device generation but failed because
HIP device-only `-c` defaults to linked/bundled output, not a relocatable object.
The qualification script was corrected using LLVM 19.1.1's own driver regression:
`-fhip-emit-relocatable --no-offload-new-driver`, followed by validation of an
ELF relocatable AMDGPU object before explicit linking. Final architecture,
shared-object, symbol, metadata and resource checks remain in place.

Reference: https://github.com/llvm/llvm-project/blob/llvmorg-19.1.1/clang/test/Driver/hip-device-compile.hip

The corrected run compiled/linked a gfx1201 code object without loading or
executing it. Matching CI IDs, compiler identity, hashes and test scope are
recorded in [HIP_BATCH_VALIDATION.md](HIP_BATCH_VALIDATION.md). Tool version
strings are observed provenance, not authenticated compiler-binary identities
or a reproducible-build/security attestation.

## Current compiler migration

Offline qualification now stages the official LLVM 23.1.5 CPU tool archive,
verifies its full SHA-256/size, and extracts only bounded compiler support.
The explicit relocatable/device-only flags remain represented in that release's
clang/test/Driver/hip-device-compile.hip source. Existing ELF machine/target,
kernel metadata, size, resource and no-runtime-execution checks remain in force.
Compiler evidence must match the executed commit; a newer compiler does not
establish hardware acceleration, runtime compatibility or host-device safety.


## Current toolchain direction

The prior LLVM-only no-header device experiment is historical compile evidence.
Current qualification uses the complete ROCm distribution selected by
tools/rocm-sdk-lock.json and the unchanged current FlitzZip bootstrap/compiler
mechanisms. The kernel now uses that SDK's HIP language definitions and thread
indices, rather than independently assembled headers or compiler assumptions.
HIP_RUNTIME_WORKER.md defines source provenance, bounded hosted compilation,
header observations and deferred module loading. Ordinary CPU builds still
require no SDK; no owner-PC operation or live GPU result follows from compilation.


The separate current-SDK module now exposes a typed filesystem kernel entry
table using the current FlitzZip deferred-registration pattern. Its typed address
is bound by the HIP translation unit and its task ABI, sizes, limits and launch
geometry are checked explicitly. The actual module is linked only on disposable
hosted VMs; no caller loads it yet. CPU semantic table tests and SDK compilation
remain separate from live execution and performance qualification.

## Bounded match producer

The separate match entry uses one 64 KiB input, 16-byte anchors and an eight-byte result per anchor. Explicit device buffers total at most 229,376 bytes within the existing 256 KiB ceiling. A CPU-built sorted exact four-byte prefix index uses 16-bit strictly backward links. Each lane searches eight candidates with 1024 byte comparisons and an 8192-byte match-length ceiling. No new runtime or worker path is enabled. See MATCH_PROPOSALS.md for CPU admission and staged compression.
