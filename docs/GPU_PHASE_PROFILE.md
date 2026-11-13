# GPU match cost profiling and input-only parent preparation

Experimental independent G03 preparation, 13 November 2026. New-source CPU,
Windows transport and complete-SDK qualification are required before any
hardware use. This does not close D01, enable a resident worker, widen a resource
limit or establish GPU compression/throughput benefit.

## Avoided parent work

PreparedGpuMatchSearch::prepare_request owns and bounds the source without
building CPU predecessor links or sorting prefix keys. Request encoding and
independent reply byte validation need only those source bytes. The trusted
worker still decodes through prepare(), independently rebuilds the complete
index and performs every original semantic/byte comparison.

At 64 KiB this removes the parent's 128 KiB logical predecessor array and up to
512 KiB logical temporary keys, including their sort. Device accounting still
reserves the SAME input/index/result footprint, at most 229,376 bytes; absence
of a CPU index must not lower planned GPU admission. predecessors() rejects an
input-only plan explicitly. Existing prepare() still supplies the old CPU index.

Both explicit hardware/memory-composition callers use input-only preparation.
The child, kernel geometry, effort caps, CPU fallback, proposal admission and
stored encoding are unchanged. Tests require byte-identical old/new requests,
independent child index equality, unchanged proposals and invalid/stale reply
rejection. Eight alternating CPU-only preparation pairs report construction
wall-time diagnostics without a pass/fail speed threshold. They are not an
end-to-end GPU or filesystem benchmark.

## Separate diagnostic mode

The worker accepts --driver-match-profile with the SAME qualified ordinal, LUID,
node mask, runtime digest and protected channel as --driver-match-adapter.
The explicit hardware probe exposes --profile-amd-driver-match-once. It uses a
4 KiB synthetic source, the existing one-child/128 MiB/20%-CPU/five-second bounds,
and confirmed actual process exit before reading metadata. It installs nothing.

Normal requests make no phase-clock calls. The diagnostic uses host
steady_clock durations, not HIP events or GPU-only timings. Fifteen serial phases
cover request/channel decode and CPU indexing; bundle/system-runtime verification;
adapter admission; module load/path checks; kernel entry binding; device init;
stream/allocation; host-to-device copies; launch plus stream wait; device-to-host
copy; CPU byte/full-semantic comparison; post-budget admission; resource release;
normal reply encoding/revalidation; and module release.

The complete worker wall time encloses those phases. Unattributed gaps remain
visible as total minus their sum. Parent exchange wall time additionally includes
process/CRT startup before the recorder, transport, parent metadata decoding and
post-recorder retirement. Neither residual is called pure process-start time.
Native encoding/publication remain separately measured by the existing native
composition probe; this diagnostic itself accepts no compression candidate.

No phase can overlap, repeat, skip or run out of order. Backward, failed or
over-budget clock readings poison the recorder. A complete profile requires
explicit finish after all phases, no active work and actual successful cleanup.
Failed/unavailable worker execution returns no profile: the probe emits null,
never a fabricated zero measurement. A genuinely measured zero remains valid.
Timings include observation overhead and blocking within the named host calls.

## Metadata boundary

The separate 256-byte little-endian FLHPH001 frame is diagnostic only. Existing
match/decode messages and stored formats are unchanged. Its fields are:

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 8 | Magic |
| 8 | 8 | Nonzero request identity |
| 16 | 4 | Qualified ordinal |
| 20 | 4 | One-bit node mask |
| 24 | 8 | LUID |
| 32 | 4 | Input bytes, 4..65536 |
| 36 | 4 | Exact bounded lane count |
| 40 | 32 | Existing canonical request source digest |
| 72 | 8 | Complete worker wall nanoseconds |
| 80 | 4 | All 15 completion bits plus bit31 finish seal |
| 84 | 4 | Expected execution declaration |
| 88 | 120 | Fifteen phase wall durations in enum order |
| 208 | 48 | Reserved zero |

Every duration and their sum must fit the enclosing total, at most five seconds.
The receiver supplies all expected binding fields and execution type; it never
derives authority from the received profile. Wrong source/identity/backend,
missing seal, incomplete mask, reserved bytes, overflow, trailing or truncated
frames reject. A profile cannot pass the normal match-data reply reader.

This is trusted-worker metadata, not hardware attestation, a hostile same-user
sandbox or a declaration that driver recovery succeeded. The existing supervisor,
telemetry, full CPU oracle and no-publication-before-cleanup rules remain intact.
It cannot change policy, return file bytes, clear quarantine or authorize retry.

## Current source and qualification

The current owner reference was inspected at FlitzZip
2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f. Its native dictionary kernel blob
4e202fe37476445f315d27301d674fc3fcc96c48 uses GPU radix indexing; adopting that
workspace unchanged would exceed this producer's present device allowance.
Its observation_mode.hpp and neutron_stage_clock.hpp distinguish unrequested,
unavailable and measured timing and require completion before reuse. This change
adapts those ownership/observation principles without archive formats or a new
GPU primitive. Exact native API/source routing identities are retained in
evidence/gpu-profile-source-66c86a1.json.

Deterministic CPU tests use injected clocks. Windows CPU fixtures exercise valid,
corrupt and publish-then-fail metadata with actual child retirement, explicitly
declaring CPU semantics. SDK admission adds malformed-profile, bad-runtime-digest
and unavailable-runtime refusals, plus a hardware-probe refusal with null timings.
These are not GPU execution. The new source must pass its actual publishing-commit
results before hardware invocation; no owner experiment is automatically enabled.
