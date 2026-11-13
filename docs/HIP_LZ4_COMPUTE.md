# Experimental bounded HIP LZ4 compute implementation

This SDK-only implementation is not connected to the worker command line,
filesystem, application, normal build, stored codec, or distributable bundle.
Selected-SDK compilation and CPU fault qualification passed at df8b08e. CPU fake execution
does not establish GPU execution, interoperability, benefit, target support,
kernel safety, scheduling isolation, or resistance to a driver/kernel hang.

## Admission and execution

The caller must authenticate and retain the runtime and private typed kernel
module and derive exact architecture/wave coverage from qualified object metadata.
Coverage is borrowed metadata, not a launch grant. The existing gfx1100 offline
object does not qualify a wave64 target: that target's wave64 body is a refusal
placeholder. CPU tests may exercise selection with distinct test functions without
making a hardware support claim.

The independent CPU decoder is checked before runtime binding, memory collection,
initialization, or allocation. The wrapper requires the existing exact 128-MiB,
20%-CPU, one-process, below-normal-priority, kill-on-close worker job. A separate
supervisor must still enforce the deadline, exact child/supervisor retirement,
one outstanding request, result generation and resource gates. No runtime retry
or module discovery is implemented here.

The real SDK-typed path performs the existing adapter/node selection and two
priority-capability queries, then rechecks device identity and launch geometry.
Architecture, actual wave, thread/grid dimensions and shared-memory capacity must
cover the request. The separate optional hipFuncGetAttributes binding comes from
the selected SDK's actual declaration/macro. Two opposite-sentinel queries must
agree on thread capacity and static shared-memory use before resource creation.
Absent or inconsistent capability refuses; no launch geometry is guessed.

One nonblocking stream uses the lowest reported priority. Context/stream memory
is re-admitted before allocating one complete slab, whose explicit device bytes
remain at most 256 KiB, including all inputs, outputs, tables, guards and scratch.
Input remains owned and immutable. The existing canonical slab builder initializes
the entire upload. Submission uses the authenticated wave-specific entry with
one block per chunk, one wave per block and no dynamic shared-memory request.

After stream synchronization, the complete slab is copied back. All protected
bytes, address tables, sizes, guards and independent CPU decodes must pass before
any candidate escapes. A fresh completion memory policy must also pass. Explicit
stream/buffer retirement must succeed before return. Any retirement failure
overrides a prior failure and prevents acceptance; it grants no retry authority.
The shared resource owner is extracted unchanged from the decoder/match worker.

HIP calls, including resource destruction, can block in the driver. These typed
checks and CPU tests cannot make teardown or the host kernel infallible. Runtime
integration remains gated on bounded isolated qualification and unresolved
historical retirement evidence; no owner GPU experiment is authorized by a
successful compile or CPU mock.

## Qualification

The optional existing SDK link fixture additionally compiles a static compute
implementation and CPU probe, without linking or loading the candidate DLL.
SDK-typed fake functions check exact allocations, copies, by-value kernel
arguments, wave selection and retirement ordering. Independent literal-block
fixtures exercise real CPU decoding, including one-byte, 64-KiB and eight-chunk
requests. Faults cover initialization, capabilities, geometry, memory at three
phases, allocation, upload, launch, completion, readback, protected corruption,
payload/size corruption and teardown. A cleanup failure after allocation pressure
must upgrade the failure category. A supervised child calls the actual wrapper
with a null runtime to verify early refusal and exact child retirement.

The same SDK/object is reused for a separate build with CPU LZ4 disabled. Its
compute probe must refuse before any fake API or memory query. PE imports must
remain Windows-only, and the selected runtime's native export inventory must
contain the SDK-derived function-attribute symbol. Both probes report that no
HIP runtime or candidate module is loaded and no GPU executes.

The original API oracle, 20 worker admission exits, decoder/framing/slab tests,
bundle integrity, original kernel compilation and all ordinary CI gates remain
required. The diagnostic package excludes the new compute probe and private DLL.

At 4dda4fe, selected-SDK compilation and private linking passed, but the CPU
fault matrix failed. The terminal-literal mutation was not necessarily invalid:
LZ4 explicitly ignores that token's low four bits. The next fixture corrupts an
actual literal and separately requires the alternate valid representation to
pass. All production checks remain intact. [Retained failure](evidence/hip-lz4-compute-first-failure-4dda4fe.json).

At df8b08e, SDK run43509323703 passed823 CPU checks/58 scenarios: six successes
(including the alternate valid terminal token) and52 refusals (including actual
literal corruption). The raw control passed eight checks/one early refusal.
All original20 worker cases,235 API checks/12 priority scenarios and806/621/968
CPU decoder/frame/slab checks passed. No HIP runtime/private DLL loaded or GPU
executed. [Source-bound evidence](evidence/hip-lz4-compute-df8b08e.json).
