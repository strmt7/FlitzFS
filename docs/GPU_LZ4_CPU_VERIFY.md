# Independent CPU check of private LZ4 candidates (experimental)

The new verifier is a CPU-only prerequisite for a possible GPU compressor. No
filesystem operation, worker or stored codec invokes it yet. It does not select
a compression policy, authenticate a GPU result or establish interoperability
with the upstream GPU encoder.

The wrapper reuses the existing optional CPU LZ4 dependency and its
LZ4_decompress_safe API. The current pinned source is lz4/lz4 commit
aa4647fad575d01a874aaaadcdbe8e6a40e072f5 (v1.11.1), whose API declaration and
complete compressed-size/destination-capacity requirements were read through
the native GitHub source API. It introduces no package installation or separate
codec implementation.

Each call admits a nonempty source of at most 64 KiB, a nonempty candidate no
larger than the shared pinned-candidate output bound, and scratch exactly the
source length. It rejects scratch overlap with either immutable source or
compressed input, including partial overlap and address-range overflow.
Framing and alias refusal occur before any decoder writes, including builds
without LZ4. A missing optional decoder returns an explicit unavailable result.

For an admitted call with LZ4, decoding receives the exact complete compressed
length and bounded private destination. A negative or short result is rejected.
A full-length result must equal every source byte. Corruption or mismatch may
leave partial bytes in private scratch; it never authorizes publication. The
wrapper performs no proportional allocation, GPU call, process operation or
driver cleanup. The caller owns valid spans and must hold source/input immutable
on its control thread throughout the call.

The tests combine independently formatted terminal-literal blocks with blocks
from the pinned CPU LZ4 encoder, across small/token/hash boundaries, 64-KiB
limits and varied synthetic content. They also exercise malformed/truncated
blocks, valid but wrong plaintext, wrong decoded length, oversized/empty inputs,
private scratch canaries, full/partial buffer overlap and codec-disabled
behavior. At 86bc830, all five core configurations passed: codec-enabled Linux, Windows
native and ASan/UBSan each reported 806 verifier assertions; Linux/Windows
codec-disabled builds each reported 389 assertions. Source-bound artifacts are
recorded in [CPU evidence](evidence/gpu-lz4-cpu-86bc830.json).
CPU-generated fixtures are not GPU output.

A future runtime still needs owned immutable request data, exact generation and
module/package identity, bounded source/result frames, independent process
retirement, current resource admission and terminal CPU fallback. A verified
raw block cannot be stored until its complete format, reader compatibility,
integrity and transaction rules are separately implemented and qualified.
Useful end-to-end compression/speed and the full filesystem remain unfinished.
