# Bounded hipCOMP LZ4 allocation plan (experimental)

This host-only planner prepares a possible byte-oriented candidate after the
source review in [HIPCOMP_OFFLINE.md](HIPCOMP_OFFLINE.md). It does not load a
runtime, launch a kernel, change a stored codec or make a filesystem operation
use the GPU.

The planner admits one to eight nonempty chunks, at most 64 KiB each and
256 KiB of input in total. It then applies the stricter existing 256 KiB ceiling
to **all explicit device buffers together**, including alignment, four AMD64
pointer/size tables, input, worst-case output, output guard regions and scratch.
A valid input batch can therefore be refused because its complete allocation
would be too large. No cap is raised to accommodate a candidate.

Scratch uses the pinned implementation's power-of-two hash-table calculation,
capped at 16,384 uint16 entries per chunk, with the largest chunk determining
each table's size. Output capacity follows its exact size function:
round up size + 1 + ceil(size / 255) to an eight-byte boundary. Inputs are
bounded before either calculation, so their intermediate values cannot overflow.
The reviewed source identities are recorded in
[GPU_CODEC_CANDIDATES.md](GPU_CODEC_CANDIDATES.md).

Each input/output region and the scratch base is eight-byte aligned. Each output
also reserves 32 bytes for a future initialized guard check. A guard is a
diagnostic for corruption after execution; it cannot prevent an out-of-bounds
kernel write. No initialization, upload, guard verification or device execution
is implemented by this metadata-only planner.

The four tables describe a future AMD64 candidate contract with eight-byte
pointer/size entries. They do not serialize host addresses or define an IPC ABI.
A future worker must qualify its actual SDK/device pointer types, populate only
owned admitted buffers, check the exact input/output sizes, and compare CPU
decoding against immutable source bytes before accepting any result. The
upstream launcher has no output-capacity argument, making prelaunch enforcement
necessary but insufficient for runtime qualification.

The normal CPU test enumerates every single-chunk length from 1 through 65,536
and checks nonoverlap, alignment, literal-sequence capacity, hash-table shape,
unused slots and the complete allocation bound. It also covers multi-chunk
boundary shapes and permutations, empty/oversized/zero inputs, arithmetic
extremes and unchanged cursors after failed reservations. At 0879cce, these tests passed all five core configurations, including Windows
native and ASan/UBSan: 1,908,560 assertions per configuration, 65,536 single sizes,
143 admitted batch shapes and 25 capacity refusals. They are neither a codec emulation nor
CPU/GPU interoperability or performance evidence.

Before use, the project still needs a fully audited kernel path, independent
bounded CPU decoding, explicit worker identity/lifecycle/fallback integration,
fresh resource telemetry and measured complete-operation benefit. Candidate
object compilation alone cannot close those gates.

The next [independent CPU verifier](GPU_LZ4_CPU_VERIFY.md) shares this bounded
output-capacity calculation and checks raw candidates without adding a stored
codec or runtime caller. At 86bc830 its tests passed all five core configurations, including Windows
native and sanitizers; actual GPU-output verification remains pending.
