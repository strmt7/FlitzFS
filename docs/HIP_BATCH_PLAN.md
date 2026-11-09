# Bounded HIP kernel candidate and qualification boundary

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
