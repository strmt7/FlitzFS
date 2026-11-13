# Checked compression slab upload and readback (experimental)

This CPU-only layer connects the existing owned LZ4 batch and complete device
allocation plan to a possible future runtime. It does not load a module, select
a device, launch a worker/kernel or change filesystem I/O or stored data.

The upload accepts a nonzero eight-byte-aligned AMD64 device address only when
the entire allocation has a nonwrapping address range. It checks the existing
one-to-eight chunk, 64-KiB chunk and complete 256-KiB device limits before any
proportional allocation. Device addresses remain integer data; these functions
never dereference them or serialize them into worker frames.

The upload initializes the complete slab, including padding, outputs, output
sizes and scratch. Pointer and input-size tables use explicit eight-byte little
endian entries. Immutable owned source bytes populate their admitted regions.
Every output capacity is followed by 32 initialized 0xa5 guard bytes.

Readback admission requires the complete exact-size slab after confirmed work
completion. Each reported output size must be nonzero and within its own
capacity, including all eight bytes of the size value. Only output-size entries,
output capacities and scratch may change. The verifier rebuilds the canonical
upload and compares every other byte: source data, pointer/input-size tables,
alignment padding and every guard. Wrong device addresses or source bytes fail.

Guard checks diagnose some corruption after execution; they cannot prevent an
invalid kernel write or prove a GPU/driver cannot freeze the system. Scratch and
unused output capacity are intentionally not interpreted or returned. A complete
readback and an intact slab do not authenticate execution.

After structural checks, the existing independent optional CPU LZ4 decoder must
reproduce every chunk's immutable source exactly. A missing decoder refuses
explicitly. A future caller must preflight decoding before any launch. No
candidate data is copied into returned owned vectors until ALL chunks pass.
No partial result escapes on failure; caller-owned source/readback stays unchanged.

The temporary canonical upload is at most 256 KiB and is retired before decoding.
Decode scratch is reused and at most 64 KiB; returned payload bytes together fit
within the admitted 256-KiB slab. The caller still owns and accounts for its
source, upload, complete readback, frames and other live buffers. These bounded
functions do not replace current resource telemetry, exact request identity,
module authentication, supervision or confirmed child/supervisor retirement.
Borrowed spans must remain valid and immutable on one control thread throughout
each call; moves/replacement/destruction must not race those calls.

The new CPU tests cover literal fixtures, actual pinned CPU LZ4 output when
available, all protected-byte mutations in a two-chunk slab, every truncation of
that slab, guard/input/table corruption, zero/excessive/64-bit-extreme sizes,
wrong source/address, address overflow and alignment, moved ownership, returned
ownership, optional-decoder refusal, all eight chunks and 64-KiB source boundaries.
One actual CPU-compressed result also passes through the existing candidate
frames. These publishing-source tests are pending execution. Synthetic CPU
readbacks are not GPU output, hardware safety, kernel qualification or performance
evidence.

The preceding candidate-frame work qualified at 959606a in all five core
configurations, all 13 integration jobs and the trusted Windows CPU process
fixture, with 79 initial/final handles.
[Source-bound evidence](evidence/lz4-worker-frames-959606a.json).
Actual runtime/module/worker integration, useful GPU benefit and the full
filesystem goals remain unfinished; historical D01 remains open.
