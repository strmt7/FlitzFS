# Compact replicated checkpoint metadata (experimental)

This opt-in storage feature reduces manifest padding without changing payload
codecs, native namespace bytes or the durability barriers. The motivation is the
executed Windows fixture in WINDOWS_SPACE_REPORTING.md: 6,416 live payload bytes
occupied a 786,432-byte image, including checkpoints, padding and history.
Payloads were already contiguous; this change targets manifest isolation waste.

## Explicit policy and compatibility

StoreCheckpointOptions{true} is accepted by new overloads of create/create_file
and new-identity compact, including native compaction. Existing signatures,
result layouts and default format-1/2 bytes remain unchanged. No CLI/mount/app
route changes. A true option is a preference for compact replicated publication:
even ordinary commit uses it. The initial empty generation-zero shell remains
the historical format; the first positive commit publishes the new capability.
Closing an untouched empty shell does not persist that in-memory preference.

Root format field 3 uses the existing FLTXR001 magic, root size and two separate
64 KiB root zones. Object and manifest formats/codecs are unchanged. Namespace
FLTXNS01/02/03 is a different format domain and retains its existing meanings.
Older readers reject the new intact root capability explicitly.

After publication the layout is sticky. Resume inherits it from validated
roots; no caller option can shrink its isolation in place. New-identity
compaction may explicitly choose a different admitted unit for its NEW output.
Default compaction preserves a source's compact layout and rejects an
incompatible target contract rather than silently weakening it.

## Backend contract, not guessed device geometry

Choose U = max(4096, declared write_failure_unit_bytes). The externally qualified
backend declaration must still be a power of two from 512 through 65,536, with
successful flush persistence. U is persisted and must be a power of two from
4096 through 65,536 dividing the existing 64 KiB root-zone size.

No sector/cluster query, Windows version, host package or device name is treated
as proof of the power-loss failure unit. A 4 KiB modeled/owned-fixture contract
is not physical-device attestation. With a 64 KiB contract, manifest isolation
cannot shrink; this option does not promise space savings for that backend.

Writable resume rejects a backend whose declared collateral unit exceeds the
persisted U before any flush/write. Commit and checkpoint Flush recheck the
contract and fence on incompatible/unknown change. Recovery-fence checks also
run before/after the barrier. Read-only CPU admission does not require a write
contract, so recovery inspection remains possible without trusting writable
geometry. Unobservable dishonest hardware remains outside the stated contract.

## Root format 3

Fields 0..111 retain format 2 meanings, including peer locator and checkpoint
generation. Add u32 U at byte 112; bytes 116..4063 stay zero, followed by the
same final 32-byte root digest. The digest binds U. Roots remain at 0 and 65536;
immutable append data still starts at/after 131072.

For manifest size M = 64 + 64*object_count:

- S = align_up(M, U), at most 65,536 bytes under existing count limits.
- Lower manifest offset L = align_up(data_end, U).
- Higher manifest offset H = L + S.
- Committed end E = H + S.

Each complete catalog owns a different, nonoverlapping set of U-sized units.
A full 256-entry catalog has 16,448 bytes; with U=4096 its span is 20,480,
not one truncated 4 KiB page. Root parsing checks exact spans/alignment,
resource bounds, parity, reserved bytes and generation/peer identity before
catalog allocation. Same-pair roots must agree on capability and U.
Digest-valid downgrade, conflicting units, impossible spans and unsupported
semantics fail; they are not treated as harmless tears.

Data/header encodings and complete private CPU round-trip verification remain.
Next append begins beyond the sealed E, or beyond the U-aligned observed
orphan tail after qualified recovery. No acknowledged padding/unit is reused.
A physical image's admitted length may be below the old 192 KiB minimum after
new-volume compaction; legacy roots still enforce their own historical bounds.

## Publication and repair invariants

Dependencies include new records, alignment padding and both complete catalogs.
Flush them first, publish the first root and flush, publish its independent
peer root and flush. Success still follows all THREE barriers.

Before the first root is durable, the previous acknowledged checkpoint survives
damage confined to the declared write units. Afterwards the first root protects
the complete candidate during peer update. Smaller catalog spans remain at
least U and never overlap each other. Old snapshots retain immutable records.
Failure remains terminal/indeterminate; no success count or retry escapes.

Loss of either current root or either current catalog can recover the same
latest object set. Repair appends a NEW complete catalog pair beyond uncertain
tails, publishes both roots, and independently verifies before returning a
writer. Both-copy or payload loss still fails; there is no payload replication,
older-state substitution, in-place reclaim or automatic handoff.

## Bounded qualification

New core groups cover admitted units 512..65536, exact root geometry,
multi-unit maximum catalogs, digest-valid malformed/downgraded roots, read-only
inspection versus incompatible writable admission, contract-change fencing,
single-copy losses, continued CPU writes and source-preserving compaction.

Separate bounded groups fault every observed commit write/barrier at units
4096/16384/65536 and every repair boundary at 4096/65536 under all three existing
persistence modes, raw/automatic. Each retains old snapshots/acknowledged bytes,
requires complete old/new state and continues from independently reopened bytes.
Existing native append/constrained/truncate invariants/fault oracles additionally
run with compact 4 KiB metadata and label their layout explicitly; fixed-layout
groups remain intact. No cases are dropped or per-process timeout increased.

The RAM collateral model now accepts an explicit failure-unit parameter.
Its default remains 65,536 and its old fault behavior stays unchanged.
Smaller-model corruption is limited to the declared unit; it is never used to
pretend a 64 KiB fault fits inside 4 KiB.

The hosted new-file test covers real ordinary-file create/reopen, a compacted
copy below the old minimum length, no-clobber, exact independent CPU contents,
incompatible-contract rejection without source changes and continued writes.
No new production existing-file writer factory is added.

A native RAM profile compares the same logical fixture using fixed versus
compact metadata, then two distinct NEW-identity compaction outputs. It counts
complete encoded image bytes, verifies unchanged regular-file payload totals/content and
retains the source. The rebound namespace has each target's distinct identity;
its candidate encoding may legitimately differ, so both copy payload totals
are reported separately. Copy sizes are captured before a separate continuation.
This is format accounting, not mounted/disk throughput or representative codec
benchmarking. Configured checks need exact-commit executed receipts.

Root-3-specific real writer termination, physical power cuts and writable/
Windows 11 client/live AMD qualification remain separate required work.
The existing 712/976 campaigns retain their default layouts; do not relabel
their passing results as root-3 process coverage. Normal mounted fixtures still
use the fixed layout. No owner-PC operation is authorized.

## Resources and future work

All inode/object/decoded/input/dirty/physical-history limits, CPU recovery,
codec allocation ceilings and existing file-admission reserves remain.
The largest manifest is still 16,448 bytes; no whole-volume copy buffer is
introduced. Root zones keep their original maximum-failure-unit separation.
Catalog grouping/history reclamation and scalable indices remain unfinished.
The initial shell, root zones and obsolete append history are still counted.

Further reuse of obsolete catalog zones would require a separate protocol and
ownership proof; this implementation does not overwrite them. Autonomous
learning/recompression/reclamation and redundant payload repair remain open.
Windows compression reporting still needs its separate qualified driver path;
shrinking storage metadata does not change cache-manager allocation semantics.
