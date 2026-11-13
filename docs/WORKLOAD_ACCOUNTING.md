# Persistent write-workload accounting (experimental)

13 November 2026. Implementation and tests require publishing-source qualification.
This is a trusted accounting foundation for future rewrite credit, not a complete
credit governor, learned policy or autonomous executor.

## Meaning and activation

NativeFilesystemWriter.enable_workload_accounting explicitly enables a sticky
namespace capability through the existing checked replicated transaction.
Initialization and normal legacy writes remain unchanged. Enabling starts at
zero: existing files and past writes never become newly earned work. Repeating
enable is a verified no-op and never resets a previously accumulated value.

NativeFilesystemSnapshot.workload_info returns two validated cached scalars:
enabled and committed_input_bytes. Legacy and moved-from snapshots are disabled.
The getter performs no allocation, I/O or device discovery.

The counter measures accepted logical input bytes in selected native write
transactions after activation. Each nonempty path or stable-inode write adds its
actual accepted byte count inside the same private Stage as the file mutation.
Identical and all-zero input count: both are foreground write requests, although
their stored payload costs may differ. Constrained writes count only accepted
bytes and append uses its actual selected input. A batch counts its ordered
writes even if later operations replace or remove their data.

Empty or zero-length constrained writes, file creation, sparse growth, truncate,
rename, deletion, basic/security/allocation metadata, Cleanup, Flush, root repair,
advice publication/rebuilding, compaction and recompression add nothing. Rejected
batches do not publish their private counters. Checked saturation at UINT64_MAX
stops further accrual without wrapping, resetting or refusing ordinary file writes.
Future credit calculations must not turn saturation into repeated entitlement.

## Atomicity and recovery

The counter and namespace/file references occupy one existing namespace object
mutation, published by the same replicated checkpoint commit. This adds no
per-write transaction, separate log, asynchronous increment or advisory dependency.
No counter-only success is returned after a failed data commit. Existing
no-allocation publication diagnostics and terminal failure rules remain.

A failed/unacknowledged operation may nevertheless have reached a complete
recoverable checkpoint. Independent recovery may select its complete new state
or the prior complete state under the existing model; each must contain the
matching counter and file bytes. This is a count of committed input, not a promise
that only calls which returned success survive. Previously acknowledged bytes
remain subject to the existing replicated-checkpoint failure model.

Snapshots retain their original count. Resume and closed-session cleanup preserve
it. Image copy/recompression and selected-image verification preserve it despite
the new image identity, without treating rewritten extents as foreground input.

This qualification uses the existing bounded RAM persistence model, including
three write/flush failure modes at every observed publication boundary. It cannot
prove physical-device power-loss behavior. Counter integrity inherits the native
metadata/object/root checks; this is not authenticated anti-rollback state or
payload reconstruction after loss of all valid copies.

## Stored capability and compatibility

Explicit activation selects FLTXNS06 and little-endian namespace capability 6.
The common header remains 64 bytes. Its previously reserved bytes 56..63 hold the
unsigned little-endian committed-input count. Capabilities 1..5 retain their
unchanged framing and require those eight bytes to be zero.

Capability 6 uses the existing 104-byte inode framing, including optional basic,
security and allocation fields and their existing validation. Enabling an older
capability can therefore expand inode headers within the unchanged 64-KiB
namespace budget. Node/extent/transaction/logical limits remain unchanged.

Existing native readers that support only capabilities 1..5 reject capability 6
explicitly. There is no silent reinterpretation, automatic upgrade, reset API or
downgrade. The new independent CPU reader supports all six capabilities.
Product numbering and the object-store/codec/advisory encodings are unchanged.

## Qualification and remaining credit protocol

The focused test covers legacy absence; zero-history and repeated activation;
identical/zero/append/constrained/batched inputs; rejection before backing writes;
metadata-only activity; resume, copy and recompression; saturation; and intact
malformed/unsupported namespace records. Activation and data updates are interrupted
at every observed write/flush boundary in both raw and automatic codec modes,
then reopened and continued with an independent CPU reader.

This counter alone must never authorize an optimizer. The next protocol still
needs a persisted spent-work position/debt, bounded publication costs, safe advice
loss/rebuild baselines, mapping/reclamation and helper authorization. A reopen,
new helper lease, image identity, counter saturation or advice rebuild must never
mint old workload again. Resource admission, low-priority execution and measured
useful compression remain independent required gates.

The first fixture at 6587ea0 failed its guessed 32-call guard before iterating
faults. Its two-record transaction can perform four record writes, three padding
ranges of at most sixteen 4-KiB writes, two indexes, two roots and three flushes:
a derived maximum of 59 calls. The correction asserts that source-derived bound
and still faults every observed call. This changes no product or process/time
limit. Failed jobs remain recorded in the source manifest; successful publishing-
source fault execution is still required.
