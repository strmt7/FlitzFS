# ROCm distribution governs HIP integration

Important implementation and dependency decisions must use recent reputable
sources and current primary-source evidence. Do not infer current behavior or
versions from model training, a legacy GitHub latest-release pointer, search
snippets or an unverified installation assumption.

## Current reference, verified 9 November 2026

FlitzZip main was inspected at 113ca5f30655f0d373c17d1448e35ce1b69f2819.
Its tools/rocm-sdk-lock.json selects the complete native Windows ROCm 10.1.0
distribution. It records package build 10.1.0rc3, observed HIP
7.16.26385-7da43026e4 and AMD Clang 24.0.0git. ROCm and HIP use different version
numbers: the selected whole distribution controls component identity.

The previous FlitzFS 7.2.4 direction was an incorrect selection from the legacy
HIP repository release pointer. Its standalone-header provisioner/ABI drafts
were not published or executed and are discarded. That direction is superseded.
Historical captures remain historical evidence, not current SDK authority.

Reuse FlitzZip's current complete-SDK bootstrap, strict archive admission,
bounded process runner, compiler-environment scoping, native compile helper and
trusted-runtime mechanism. Pin the source and verify actual consumed inputs.
Do not rebuild an independent subset of headers or mix compiler/import libraries/
device bitcode/runtime metadata from different distributions. No archive format,
archive CLI or codec registration becomes a filesystem product dependency.

The same lock's SHA-256 was locally observed from AMD's HTTPS artifact on
25 October; it is NOT an independently published vendor checksum. Preserve that
provenance distinction and verify every downloaded archive byte before use.
Current official release/install pages are captured through the isolated
Crawl4AI provider; inspect successful native content and dates before claiming
a distribution is the newest compatible choice.

## Safety and remaining qualification

Complete SDK provisioning and compile tests run only on fresh disposable hosted
Windows VMs with capacity checks, no global PATH edits, no driver install and no
GPU execution. Missing eligible SDK files trigger the verified complete-package
bootstrap. Insufficient resources or unverifiable integrity must refuse safely,
preserving owner data and CPU operation.

The runtime must remain in a separately supervised child. Adapt the current
trusted driver loader and stream/device/resource mechanisms only after verifying
their current sources and tests. Keep immutable CPU verification, explicit
qualification, bounded work, terminal failure, configuration quarantine and
exact retirement. A compiler-only check is not GPU execution or a speed result.

The current one-shot/predecoded batch remains a development correctness fixture;
it cannot establish production GPU throughput. Runtime/device compatibility,
worker lifecycle and actual benefit still require off-host hardware evidence.
Normal filesystem paths stay CPU-only and owner-PC execution remains forbidden.


## Executable complete-package qualification

qualify_rocm_sdk.py uses the unchanged current reference bootstrap and native
HIP compile helper from the pinned private checkout. It verifies reference Git
identity/cleanliness and byte-identical whole-distribution locks before import.
It runs the reference bootstrap regressions, admits the entire download/extracted
size plus 4 GiB free reserve, automatically provisions the complete package,
and uses that same SDK for headers, import library, compiler and device inputs.

The reference bounded command runner limits each owned tree to 2 GiB committed
memory, below-normal priority, bounded output and explicit deadlines; its fresh
RAM admission also retains half free RAM and at least 2 GiB. SDK provisioning
has a 3600-second outer deadline; compilation commands have 900 seconds.
No runner software is removed to make room. Insufficient capacity is a failure,
not permission for a partial SDK or an unbounded retry.

The filesystem kernel is copied byte-for-byte into a fresh include-layout fixture
so the unchanged reference compile helper can consume it. The full release-target
preset is compiled. Header observation and a separate kernel DLL link use the
same complete SDK. The DLL is NEVER loaded in this job. The CPU probe's actual
PE imports are audited before execution; it reports actual header and required
runtime identities without initializing HIP. Critical SDK hashes are compared
again afterwards. Only JSON provenance/results are uploaded, no generated
binary, SDK archive, driver or release package.

Current-source inputs include the reference lock/bootstrap, native process runner,
compiler-environment helpers and source GPU kernel. These mechanisms are build
dependencies only; no FlitzZip archive code is linked into FlitzFS. Successful
exact-commit results are required before claiming compile qualification.

## Observed complete SDK, 9 November 2026

At 35dfac90503e46b9f988b8ecfbe40f255ca9b809, hosted run 42684196735
(job 129386491608) passed all 12 unchanged reference bootstrap regressions,
complete package provisioning, filesystem device compilation, module linking
and CPU-only header observation. The actual SDK reported HIP
7.16.26385-7da43026e4, properties structure 1472 bytes and required runtime
amdhip64_7.dll. Its actual compiler reported AMD Clang 24.0.0git commit
d6f6cb691863e9258034ab48c1dfce21689d5606. The native compiler helper selected
MSVC 14.51; CMake observed MSVC 19.51.36260.0 / Visual Studio 18 2026.
These are observed test identities, not a universal supported-toolset promise.

The compiler warned that -fms-runtime-lib=static was unused in six compilation
passes. The unchanged reference helper was retained and the linked module
passed qualification. This warning is retained rather than hidden; no runtime
portability or redistribution claim follows from the link result.

The corresponding native Crawl4AI run 42684196818 (job 129386491901) accepted
all three official current/selected release and installation pages (HTTP 200).
AMD's current and selected release headings both identified ROCm Core SDK
10.1.0. Native bodies/digests were retained. An existing navigation regex omitted
rocm.docs.amd.com links while retaining rocmdocs.amd.com; it is corrected in the
next checkpoint without changing the captured source bodies.

## Typed filesystem kernel entry

gpu_kernel_api.hpp defines a module-local typed entry table with exact size,
API/task versions, task size, hard task/output limits, 64-thread geometry and a
zero reserved field. The address is bound inside the SDK-compiled HIP translation
unit, following the CURRENT FlitzZip bind_codec_kernels / HipKernelApi design.
It avoids handwritten HIP registration ABI or guessed symbol lookup. The
separate module exports flitzfs_rocm_kernel_api_v1 and retains the earlier scalar
task-ABI export. Its table remains immutable and owned by the loaded module.

This is not an IPC struct, binary authorization or hardware attestation. The
future worker must authenticate and retain the selected runtime/module before
calling the entry, validate the table before launching, and retain all existing
CPU verification, adapter-matched child telemetry and bounded supervision.
Normal CPU builds can inspect this interface without HIP headers or runtime.
CPU semantic tests exercise the typed argument contract across existing batches
and reject every incompatible metadata field or missing entry. Hosted compile
qualification must link the real SDK-bound binder; it never loads the module.
A real runtime worker, launch/transfer/cleanup implementation and hardware
qualification still remain to be implemented.

Current selected-distribution HIP stream, memory and error documentation plus
Microsoft TDR guidance are requested through the isolated native crawler before
those runtime mechanisms are implemented. Failed native captures remain failures;
older component-version documentation is not a substitute.

HIP transport timeout/cancellation is now treated as unknown device health,
including cancellation triggered by an expired foreground result gate. Exact
process retirement and fresh telemetry cannot clear this quarantine; a separately
qualified recovery generation must advance. A CPU fixture uses pressure
hysteresis. A valid, independently checked late reply can remain pressure; the
selected CPU bytes never change. See GPU_WORKER_CONTROLLER.md for test scope.


## Typed-entry checkpoint and current-source limitation

At fddd1b7108d8a6e1b4d9359475749c27b22ffe85, the complete ROCm compile/link
run 42686078956 (job 129393145918) passed with the real typed kernel binder and
separate module. Native Windows CPU run 129393147149 passed 27 CTest entries,
including 528 worker assertions with handles 88 to 88; raw Windows run
129393146564 passed 25 entries, 529 worker assertions and handles 87 to 87.
These tests include expected-HIP cancellation/timeout/expiry quarantine and
separate recovery evidence, but all helper execution remained CPU semantics.

Native research run 42686078775 (job 129393145786) accepted the selected 10.1.0
memory/error references and current Microsoft TDR guidance. The Doxygen stream
page returned a Cloudflare challenge (HTTP 429 after 307) and was rejected with
its native failure/digests retained. The research job correctly failed; do not
describe it as complete or bypass its challenge. A separately published stream-
management reference linked from the successful current pages is requested
normally through the same reviewed crawler.

The actual observed HIP GITHASH resolves to the current ROCm/rocm-systems
monorepo, not the legacy HIP or CLR release repositories:
7da43026e4f2ef7600433c38c820732f7a205d22, dated 25 September 2026.
The native repository API verified these exact source paths:

- projects/hip/include/hip/hip_runtime_api.h
- projects/hip/docs/reference/hip_runtime_api/modules/stream_management.rst
- projects/clr/hipamd/src/hip_stream.cpp

This source is bound through the selected package's actual observed identity;
it is not an independently selected replacement HIP distribution. The header
documents lower numerical stream priorities as higher priority, possible waiting
in hipStreamDestroy and implicit device synchronization in hipFree. Its priority-
range comment says "under development", whereas the exact implementation returns
Low/High enum values and clamps requested creation priorities. Preserve this
source/documentation distinction. Neither fact proves OS scheduling isolation or
a latency/driver-cleanup guarantee; low-impact background work still needs actual
hardware and Windows scheduling evidence.

Native source references:
https://github.com/ROCm/rocm-systems/blob/7da43026e4f2ef7600433c38c820732f7a205d22/projects/hip/include/hip/hip_runtime_api.h
https://github.com/ROCm/rocm-systems/blob/7da43026e4f2ef7600433c38c820732f7a205d22/projects/clr/hipamd/src/hip_stream.cpp
