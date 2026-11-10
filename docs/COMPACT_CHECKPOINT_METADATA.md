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

A separate compact-store-interruption workflow now configures a root-3-specific
real writer-termination gate, described below. Physical power cuts and writable/
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

## Executed component checkpoint

At 76c3b927d06a169f5d2c790f55ff3915150a8cbd Windows native job
129600916301 passed all 65 entries, raw Windows job 129600915830 passed
62, and sanitizer job 129600916225 passed 58. Core compact invariants
completed 548 checks. Every observed new commit/repair fault totaled 855
cases across the stated units/modes; native compact mutation cases added
264 failures and 264 invariant checks. All fixed-layout cases remained.

The ordinary-file test independently reopened a root-3 compacted image of
143,360 bytes, below the old minimum, preserved exact contents and source
bytes, rejected incompatible writable geometry/no-clobber and continued with
the matching contract. Worker handles stayed 94 before/after. These are hosted
VM file checks, not physical-device power-loss or owner-host qualification.

The codec-enabled native profile recorded:
fixed image 786,432 bytes, compact image 237,568; fixed-layout copied image
327,680 and compact copied image 147,456. The same full logical fixture,
file payloads and source retention were verified. The fixed/compact regular
images had the same 6,413 live stored payload bytes and 328,394 decoded bytes.
The two NEW copies reported 6,413/6,414 payload bytes separately because each
rebinds a distinct namespace identity; regular-file payload totals were equal.

Raw profile: 1,114,112 to 561,152 bytes, with copied images 655,360/471,040
and equal 328,394-byte payload/decoded totals. Codec-unavailable automatic
builds honestly retained that same raw-sized profile. These are complete
encoded-image accounting results for a bounded synthetic fixture, not
general filesystem ratios, GPU speedups or disk throughput.

The unchanged fixed-layout metadata and legacy real-process campaigns passed
712/976 cases in partitions of 39.402/55.476 and 58.661/59.486 seconds.
They are not root-3-specific writer-termination evidence. The next gate must
exercise that new layout explicitly before broader adoption.

All nine triggered workflows succeeded. Default-layout mounted job
129604220436 passed normal I/O, confirmed helper termination and independent
CPU recovery/remount, with source parity and all three Windows space reports.
Normal/recovery exits were zero; deliberate killed exit was 1460, confirmed.
Peaks were 6,164,480 / 6,123,520 / 5,128,192 bytes. The default image retained
786,432 bytes and 6,416 payload bytes; this confirms it was NOT silently
converted to root 3. That probe is default-layout integration evidence only.

## Dedicated root-3 writer-termination gate

The compact-store-interruption workflow uses the existing bounded owned-file
probe with a separately identified schema-4 dense campaign. It explicitly
creates root format 3 with the declared 4096-byte hosted fixture contract.
The six raw/automatic upgrade, tuple replacement and compound data/namespace/
tuple traces retain every observed write-before/after, flush-before/after and
acknowledgement cut. Fixed schema-2/976 and schema-3/712 campaigns remain
separate and unchanged.

Before each baseline/candidate acknowledgement, and after independent CPU
recovery and continued publication, the producer inspects the selected root
bytes and verifies the root digest, selected generation, format 3 and stored
unit 4096. A top-level label alone is insufficient. Every acknowledgement,
full-trace recovery and interrupted recovery binds that geometry together
with the existing decoded-state closure digests. The merger rejects missing,
mixed, relabelled, duplicate or misrouted evidence and omitted cuts/barriers.

Actual TerminateProcess and confirmed process/output-thread retirement precede
independent recovery. Recovery verifies the complete baseline or candidate,
then writes keep-file data and its basic tuple together, closes and independently
reopens/decodes the result. Existing 180-second partition, 15-second child,
768-MiB Windows child/20-percent CPU, 1024-case, 256-event/256-byte line,
8-event queue and 3-MiB combined fixture caps are unchanged.

This is a configured gate until exact-commit hosted execution succeeds.
Neither successful process termination nor a declared 4-KiB fixture unit
proves physical power-loss behavior, Windows 11 client qualification or actual
device failure geometry. Default and mounted image selection stays fixed.

### Initial exclusive-sharing failure

At 4440713a136a18d2fa08a5ac60ee2a26d18174bd the new hosted job
129609277610 compiled successfully and passed all seven report-validation
tests, but rejected its first full trace before any interruption case.
The additional root inspection attempted a ReadOnlyFile open while the
owned image writer retained its exclusive OS sharing lease; Windows denied
that open. This is a failed campaign, not root-3 recovery evidence.

The corrected producer reads acknowledgement roots through the existing
owned backend, after independent library graph admission, without reopening
or weakening the writer lease. Recovery root inspection still uses a separate
read-only file handle after confirmed writer retirement and again after the
continued writer closes. The selected-generation/root-digest/format/unit
checks and all resource/fault oracles remain unchanged.
