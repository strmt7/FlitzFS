# Incremental native compression profile measurements

Experimental advisory component, 11 November 2026. This is a measurement primitive
for future data-driven maintenance, not a complete learning model or scheduler.
It measures existing native store codecs; it has no archive-format dependency.

## Contract

StoreCompressionProbe owns an immutable sample of 1..65,536 bytes. Admission
requires a nonzero image identity, generation and object ID, an object no larger
than the existing 1 MiB limit and a valid sample range within that object. Invalid
metadata and encoder caps are rejected before copying. Without the compiled
bounded Zstandard codec, creation returns no probe.

Each advance measures one of levels 3, 6 and 9 through the existing native
encoder. Only one encoder is live per step. Its allocator remains capped at
16 MiB, and the existing CPU verifier remains capped at 2 MiB. The owned sample, copied
input and retained payloads are each at most 64 KiB. Private compression scratch
uses the existing ZSTD_compressBound(input size), and optional sequence vectors
retain the existing 4096-proposal ceiling. Those finite buffers are outside the
codec allocator counters. Allocator telemetry is not a whole-process peak or a
system-headroom guarantee.
A zero payload or one-byte exact motif ends the probe early because the admitted
codec set cannot improve its payload size.

The optional source-bound match proposals use the existing native encoder's
byte validation and CPU fallback. A hinted step can perform two encodes: the CPU
baseline and a private sequence candidate. This component never starts a worker,
loads HIP, discovers hardware or asserts that a supplied hint executed on a GPU.

Measurements record requested level, codec, logical/stored payload bytes,
elapsed nanoseconds and codec allocator peak. Timing is diagnostic and does not
establish a speed improvement. Completion advice selects the smallest measured
payload; ties prefer the lower requested level. It requires the identical image,
generation, object ID, range and exact sample bytes again. Changed or partial
sources and failed probes return no advice. Failure is terminal for that probe,
with the immutable input retained; repeated advance calls do no new work.

## Relationship to maintenance

A future controller must supply qualified quiet/resource opportunities and
enforce worker/process budgets. This single-owner class starts no thread, timer, service or
background loop. Moves/destruction must not race sample views or method calls. It does not assess host load, decide which objects to sample,
learn across samples, persist a model or authorize a rewrite.

Sample advice does not predict whole-image compression. Existing incremental
recompression must still reject source changes, independently verify candidates,
measure complete physical savings and perform its separately guarded handoff.
Its suppression of repeat work on unchanged images remains intact. No ordinary
read/write callback invokes profiling automatically.

The default codec policy, stored codec identifiers, recovery reader and wire
formats are unchanged. Existing data remains readable without this component.

## Validation

The matching commit must pass Zstandard and raw-only builds, Windows-native
checks and sanitizers. The probe tests cover admission/range caps, immutable
ownership, partial/failed/moved-from states, changed source tuples and bytes,
allocator refusal, invalid optional hints and minimal-payload early completion.
A RAM integration fixture reads a committed object, obtains source-bound advice,
passes that policy to the existing recompressor and verifies target readback and
unchanged original bytes/write/flush counters. Aggregate image copies are capped
at 8 MiB. The raw-only lane checks explicit unavailability; it does not execute
the Zstandard profile matrix.

No owner-host execution, live GPU, full learning/scheduling, physical power-loss
or end-to-end performance qualification is implied by these checks.
