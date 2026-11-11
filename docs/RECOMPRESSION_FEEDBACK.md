# Provisional native recompression feedback

Experimental implementation, 11 November 2026. This is G02 preparation, not a
persistent learner, automatic GPU routing or a performance claim.

## Purpose

Producing valid match proposals does not prove that the native encoder selected
their representation or that the complete filesystem became smaller/faster.
The existing encoder compares its optional sequence encoding with its ordinary
CPU result and may retain the CPU result. Equal payload size is also permitted.
A controller must observe the actual choice instead of rewarding work merely
because a GPU returned suggestions.

StoreRecompressionFeedback reports one successfully copied candidate record:

| Field | Meaning |
| --- | --- |
| source_image, source_generation, object_id | Exact source/candidate work identity |
| supplied_matches | Suggestions supplied to this step, including ignored ones |
| forwarded_matches | Suggestions forwarded to the ordinary native encoder; not proof of validity or use |
| logical_bytes, original_stored_bytes | Candidate logical size and old active payload size |
| stored_bytes, codec | Actual candidate record payload size and existing wire codec |
| reencoded | A new encoding was computed; copying an existing record is distinct |
| cpu_baseline_bytes | Ordinary CPU encoding size, meaningful when reencoded |
| match_encoding_selected | The encoder actually chose its independently verified suggestion encoding |

These payload counters exclude roots, manifests, padding, retained history,
dictionaries and repair copies. Total physical image size and full operation
cost still determine benefit. A selected equal-size frame is not a compression
ratio improvement. The feedback is advisory and never authorizes publication,
reclamation, data exposure or a recovery decision.

## Lifetime and safety

ObjectStoreWriter::Rewrite::feedback() and
NativeFileSession::recompression_feedback() return a fixed-size optional value.
The session query performs no backing I/O, allocation, discovery or GPU work.
Reading it twice does not identify two completed steps.

The record appears only after that step's target writes succeed. It remains
provisional until the existing full verification and volume-head publication
succeed. Cancellation, failed copy, source mutation, terminal state and completed
handoff expose no session feedback. An advance on an already-ready candidate
clears its previous feedback rather than fabricating another copied record.
Callers must bind image, generation and object before learning from an outcome.

Replacement namespace metadata is encoded independently and does not receive
caller suggestions. Mismatched object IDs retain ordinary CPU encoding and
report zero forwarded suggestions. Existing source preservation, candidate
bounds, CPU readback and publication barriers are unchanged.

No on-disk format, decoder or worker wire changes. The extra encoder baseline
field is in-memory only. No learning/optimization area is implemented by this
API; L01/A03 remain separate required packages.

## Validation contract

The updated volume-session fixtures compare summed record payload feedback
against the independently reopened published manifest, compare hinted records
with an independent ordinary CPU encoding, and verify source-image/generation
binding and retained byte-exact data.

Additional fixtures check no feedback before work, read-only repeated queries,
foreign-object hints receiving no compression credit, source mutation,
cancellation, target failure and already-ready no-ops. Both metadata layouts
and raw-only/codec lanes must pass. At publication, matching implementation CI
is pending; previous GPU trials do not qualify this new API.

## Current GPU evidence and next integration

[evidence/gpu-admission-04d884b.json](evidence/gpu-admission-04d884b.json)
records a tiny accepted raw/fill decode and a separate 4-KiB match-producer trial
with four CPU-verified proposals. Those trials have no native image pipeline.

The next integration must compose the existing locked-RAM backend, managed
native-session recompression, independently validated worker proposals and
CPU reader. It must report whether hints were selected, whole-image bytes and
complete cold/warm CPU/GPU costs. No owner-mounted or physical-backed image is
authorized. The eventual automatic low-priority resource governor and persistent
learner remain A01–A04, not properties of this telemetry getter.

## Feedback qualification and composition

At d1e66f6, the updated feedback tests passed raw/codec, Windows native, sanitizer
and SDK checks. The complete matrix still had the recurring mounted D01
termination failure (12/13 jobs passed). NATIVE_GPU_RAM_PROBE.md describes the
next explicit integration and its still-pending publishing-commit qualification.
