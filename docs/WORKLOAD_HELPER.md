# Managed workload accounting and helper activation (experimental)

13 November 2026. Publishing-source integration qualification is required.
The underlying capability-6 counter at 14cfb12 passed all 13 core/mounted jobs,
3,945 workload assertions in each examined Windows/Linux/sanitizer log and the
existing application workflow. See
[evidence](evidence/workload-accounting-14cfb12.json).
Those results do not qualify this new managed/helper consumer.

## Explicit managed control

NativeFileSession.enable_workload_accounting requires a healthy managed volume
with its head owner, no pending handoff and no staged recompression. It runs the
existing checked native activation transaction under the caller's serialized
control ownership. It starts at zero on legacy state and never resets an already
enabled counter. It preserves live adopted/cleaned contexts.

An unmanaged session is rejected before backing I/O. A staged-maintenance
deferral also performs no backing I/O, leaves the candidate intact and does not
fence healthy data. A publication failure uses the existing data-writer failure
rules: an ambiguous activation cannot expose a usable diagnostic or authorize
further file mutation without independent recovery. No retry is added.

NativeFilesystemWriter.workload_info and NativeFileSession.workload_info return
only cached scalars, without I/O or allocation. The trivially copyable result is
bounded to 16 bytes. Moved/fenced owners return disabled. The activation method
returns these scalars after successful publication; it never forms diagnostics
by allocating a snapshot or querying a backend afterward.

These getters do not attest to current backing integrity or authorize optimizer
credit. A future spending transaction must check actual native/advisory state
under its control owner before admitting optional work.

## New helper volume lifecycle

The existing Windows helper activates workload accounting only while creating
a NEW private volume, before initial advice and before publishing its owner to
the controller. Activation failure retires the same local source/head/advice/
session owners through the existing exception-safe creation path. It neither
overwrites an existing image nor upgrades a legacy image during normal open.

Saved text now contributes its actual accepted UTF-8 bytes. Read, rejected
NEW-only save, empty file, advice auditing/rebuilding/exhaustion and explicit
image optimization do not mint input. User file access still never consults the
advisory area. Advice loss remains independent of data accounting.

No UI learning claim, new worker, timer, service, mount, GPU launch or persistent
process is added. The helper's domain, residency, headroom, process/CPU/time,
source-preservation and cleanup limits remain unchanged. No new stored encoding
is introduced beyond the already explicit capability 6.

## Configured qualification

Managed tests cover legacy absence, zero-history activation with real live and
cleaned handles, no-I/O cached getters, no-write repeated activation, staged-copy
deferral, selected-image handoff, cleaned-handle writes, move, actual context
retirement and a new-controller resume. Failed advice must not prevent later
input accounting or cause ordinary file operations to query advice.

A separate first-write activation fault checks fencing and unchanged acknowledged
source bytes with an independent CPU reader. This supplements the qualified
underlying counter's every-boundary persistence model; it is not new evidence of
physical power-loss survival.

The actual helper model checks activation before exposure, exact input counts,
rejected and empty saves, UTF-8 byte counts, optimization and advice failure/
exhaustion. Existing allocator cuts, three balanced lifecycle cycles, native GUI
workflow and all raw/codec/Windows/native/sanitizer/mounted regressions remain.

## Remaining automatic optimization work

This integration supplies provenance only. Persisted spent-work/debt, safe
current-input baselines after lost advice, durable provider mapping/reclamation,
helper authorization, resource admission and a low-priority learner/executor
remain unfinished. Neither the counter nor a new helper lease permits automatic
rewrites. Useful GPU and full Windows/repair/scaling qualification remain separate.

The first integration fixture at22ba3b3 failed its staged-target assertion in the
Linux raw lane. It incorrectly expected zero target writes after the established
staging API had already initialized its private header. The correction records
that post-staging state, then requires every target/source/head I/O counter and
the exact target bytes to remain unchanged by rejected activation. Production
code and all budgets are unchanged. The failed job is retained in the source
manifest; the correction requires its own full publishing-source gates.
