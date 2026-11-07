# HIP batch candidate: executed qualification evidence

Date: 7 November 2026. Product version 0.3.2 remains experimental and CPU-only.
Implementation commit: `58c353e601b9d4b7a8aca0628f13fab9bb665cd3`.
Compiler-stage correction: `b64f86e72e929ae3cbb4b2b1bf19b542d71bc415`.
Validated source tree: `b6433771983848c5470b96a5ed9a73fff936dc04`.
Local publication indexes matched the corresponding GitHub trees exactly.

## What was implemented

An allocation-free HIP candidate decodes raw, fill, pattern, static prefix,
adaptive prefix and Huffman segments. Prepared batches own all source/task/CPU
reference bytes, admit at most eight blocks, 64 KiB per block and 256 KiB output,
and split work into at most 72 independent 4 KiB tasks. No file format changed.
The result gate requires opt-in, matching adapter identity, fresh budget data,
a bounded deadline, a unique generation, every status and exact byte equality.
Any failed completion terminally disables that gate; late output is not exposed.

The DXGI collector compiles against the real Windows API. It is not invoked by
normal CLI or mounted operations. Its current Windows test rejects invalid
identities before OS queries; it does not qualify successful AMD budget queries.
The GPU admission limits have been exercised with injected telemetry, not by
allocating VRAM on hardware. No runtime worker or kernel launch is wired in.

## Local test evidence

The unchanged baseline passed all 15 portable CTest entries. After adding the
candidate, all 16 entries passed in each of GCC with codecs, GCC with external
codecs disabled, and Clang AddressSanitizer/UndefinedBehaviorSanitizer: 48 matrix
executions. The batch/kernel-semantics/gate suite performed 1,819 assertions.
A further final codec-enabled CTest run passed 16/16 after the compiler script
repair. These runs executed the shared device function on the CPU; they do not
establish GPU correctness, scheduling behavior or performance.

Tests include exact boundaries, mixed blocks, reversed task order, output
canaries, malformed task/framing metadata, input ownership, rejected budgets,
clock overflow/backwards movement, stale generations and metadata, incomplete
statuses, size mismatches, altered output, deadline expiry and terminal failure.
A completion snapshot that was fresh but predated submission was initially
accepted; a failing regression exposed this and the gate was corrected before
publication. No test was removed or weakened to make the suite pass.

## Completed hosted checks at b64f86e

[Main CI run 42415244785](https://github.com/strmt7/FlitzFS/actions/runs/42415244785)
completed all six jobs successfully:

| Job | Result and scope |
|---|---|
| Ubuntu 24.04, codecs enabled | Passed portable CPU tests |
| Ubuntu 24.04, external codecs disabled | Passed dependency-absence paths |
| Windows Server 2022 / MSVC, external codecs disabled | Passed CPU and Windows process/budget rejection tests |
| Linux ASan/UBSan | Passed portable memory/undefined-behavior checks |
| Windows native | Passed actual SDK callback and injected lifecycle tests |
| Windows mount | Passed existing driver-backed synthetic read-only mount/recovery probes |

[Upstream comparison run 42415244840](https://github.com/strmt7/FlitzFS/actions/runs/42415244840)
completed successfully as well, retaining the pinned FlitzZip serializer/CPU
reference checks alongside the new host semantic tests. Existing mounted tests
still use CPU decoding; their success is not a GPU-mounted-filesystem result.

## Actual offline device compilation

[Compile run 42415244771](https://github.com/strmt7/FlitzFS/actions/runs/42415244771)
compiled the `.hip` source and linked an AMDGPU `gfx1201` code object using:

- Ubuntu Clang 19.1.1 (`1ubuntu1~24.04.2`).
- Ubuntu LLD 19.1.1.
- Kernel entrypoint `flitzfs_decode_v1`, protocol version 1.
- Generated code object: **9,664 bytes**.
- Code-object SHA-256:
  `ba1d5e128fb74ea5f4646a035e92a98ff13c5c6ea722c71480788e10505e20e1`.

The exact retained [JSON receipt](evidence/hip-compile-b64f86e.json) records all
three kernel-source hashes. The downloaded artifact SHA-256 was
`d7e1e74245404111eb2d7feb3d27835eca4c65fcece519ba9794ac66d7a737b5`;
its source hashes were independently checked against the local published tree.
No generated code object, SDK, driver or runtime binary is committed/distributed.

The compiler was constrained to 2 GiB address space, a 4 MiB output-file ceiling,
40 seconds CPU and 45 seconds wall time per invocation. It emitted a relocatable
AMDGPU ELF object; the linker rejected undefined references. Final checks
required little-endian ELF64 shared-object type, AMDGPU machine, `gfx1201` flag,
kernel symbol and AMDHSA metadata. This is offline code-generation evidence,
not runtime loading, hardware execution, Windows HIP compiler qualification,
a security audit or reproducible-build attestation. Compiler versions are
observed strings, not authenticated compiler-binary identities.

The first compile attempt (42414867911) failed: HIP device-only `-c` defaults to
linked/bundled code objects, and the bundler was absent. The repair selected
LLVM 19's explicitly tested `-fhip-emit-relocatable` path and added relocatable
ELF validation before linking. It did not skip compilation, use a mock compiler,
change the kernel or remove any resource/target checks.

## Still unqualified and deliberately disabled

No HIP runtime was loaded, no kernel ran on a GPU, and no owner-PC command or
GPU setting change occurred. There is no live supervised HIP worker, production
CPU/GPU routing, GPU-enabled filesystem, throughput/latency claim or VRAM
residency proof. DXGI process budgets do not reserve physical memory, and the
result deadline does not stop a hung driver. Runtime cleanup may synchronize;
a supervised hardware experiment and its recovery behavior require separate
qualification. Authentication, encryption and transactional writes are unchanged
future stages. Keep normal file operations CPU-only and retain independent data.
