# Uniform compression messages and compute path (experimental)

The version-2 path carries a resource-admitted uniform window rather than an
eight-element descriptor array. It preserves all version-1 layouts, messages,
private exports and default CPU behavior. This is development work, not a release.

## Admission and ownership

UniformLz4Limits is derived from the canonical 80-byte device layout and contains
the exact input/device/request/reply extents and block geometry. It is a
representation contract, NOT proof of live resource availability. Before calling
proportional APIs, the controller must obtain a current window from the adaptive
planner, account for the complete configuration-qualified child/parent peak,
obtain a resource process allowance, and enforce the resulting sized-channel
capacities. Arbitrary caller-supplied limits are not a launch grant.

PreparedUniformLz4 owns its source and binds input length, chunk width, count and
plaintext using the existing unkeyed BLAKE3 record digest. This detects accidental
corruption; it is not authentication or encryption. Moves clear the old owner.
All borrowed frame/candidate spans remain valid and immutable for the entire
call on one control thread. No filesystem path, native structure, pointer or
independent CPU-reference buffer crosses the wire.

## Portable version-2 frame

All integers are little endian. The fixed header is 80 bytes:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | FLFSLRQ followed by byte 2; replies use FLFSLRP and byte 2 |
| 8 | 8 | Nonzero request identity |
| 16 | 8 | Exact source bytes |
| 24 | 4 | Block count |
| 28 | 4 | Uniform block width, at most the selected codec's 65536 bytes |
| 32 | 4 | Zero for requests; CPU-semantic/HIP declaration for replies |
| 36 | 4 | Reserved zero |
| 40 | 8 | Dense payload bytes |
| 48 | 32 | Source/shape record digest |

Requests append exactly the source. Replies append one eight-byte record per
block (decoded/stored uint32 lengths) and then dense encoded blocks. Parsers
preflight complete shape, reserved fields, bounds, density and identities before
proportional result allocation. Signed publication-length representation bounds
remain; they are independent of available RAM or filesystem operation size.
Unknown versions, trailing bytes, truncated directories and stale replies fail.

Producer and parent independently CPU-decode EVERY block against their OWN
immutable source, with one block of scratch. Missing CPU decoding refuses before
dispatch. Dense candidate payloads use one allocation plus a size directory.
Output metadata and correct backend declarations alone never prove GPU execution.

## Shared slab and typed SDK computation

Upload uses the exact uniform device layout. Every output has a 32-byte guard.
Readback verifies the entire source, alignment gap, every output guard, every
encoded length and independent CPU reconstruction before copying candidate
payloads. Scratch is mutable; no input/output pointer tables are needed.

The SDK-only run_hip_uniform_lz4_in_worker wrapper requires a creation-time
single-process, kill-on-close, below-normal-priority memory/CPU Job Object without
breakaway, fresh physical/commit headroom, authenticated caller-owned module
handles, exact target coverage, and a separately qualified work/device grant.
It preserves exact LUID/node corroboration, lowest supported stream priority,
corroborated function attributes, actual runtime grid limits, one allocation and
one stream. It rechecks the GPU memory envelope before initialization, after
stream creation, after host buffer allocation and after compute. Resource
retirement must succeed before any candidate returns.

The typed dispatch passes a uint64 allocation size and the 80-byte layout by
value. There is no eight-block restriction in this compute path. Kernel quantum
and device bytes remain bounded by the configuration-qualified grant; increasing
available RAM never authorizes an unqualified longer kernel.

## Execution boundary and pending integration

Portable tests and real Windows sized-channel CPU fixtures join preparation,
serialization, independent child parsing, candidate construction, successful
exact-child exit, parent validation and source/handle preservation. SDK-typed
fake runtime tests execute the compute/retirement code without initializing HIP,
loading AMD modules, or executing device code. Both optional-decoder states and
dispatch/allocation/geometry/attribute/guard/pressure/retirement faults are tested.
Their exact-source hosted results are required before qualification.

The new uniform private module is NOT in the supervised runtime bundle yet.
No actual version-2 worker executable, automatic GPU filesystem route, measured
larger-window GPU benefit, or new owner-device trial is claimed. The next work
must integrate authenticated packaging, independently repeated qualified grants,
the existing evidence-aware recovery policy and terminal CPU selection, then
connect controlled Windows writes and low-priority streamed maintenance.
