# Native allocation reservations (experimental)

This is the native live-object capacity component for the Windows-only writable
engine. It is not a complete Windows AllocationSize implementation. No writable
callback, driver/helper product route, host-file preallocation or mounted write
is enabled.

## Current source contract and reporting boundary

The selected upstream development candidate remains WinFsp v2.2B5, published
13 October 2026, source 14d6e0280ae15e2550c44d6dbc476be51649727f. Its native
release API and these immutable source bytes were reviewed on 10 November 2026:

- [SetFileSize contract](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
  requires aligned allocation, allocation at least EOF, allocation growth for
  EOF growth, and EOF truncation when allocation shrinks below it.
- [Kernel cache sizing](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c)
  clamps returned allocation to at least EOF, aligns it and supplies cache sizes.
- [SetAllocationInformation handling](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c)
  aligns the request and checks mapped truncation before sending it to user mode.
- [Reference semantics](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)
  keep allocation on ordinary EOF shrink, grow it on EOF expansion, and trim at
  the trusted Cleanup allocation event. That implementation is a memory
  filesystem, not a physical-space model for this compressed store.

The native reservation below deliberately counts worst-case decoded live
extent slots. It must NOT be returned as measured compressed physical bytes
or host allocated clusters. The selected driver still rejects compression
information queries; WINDOWS_SPACE_REPORTING.md retains actual observations.
A future writable volume must qualify host backing admission/preallocation,
history/reclamation headroom and its complete size/free-space contract before
mapping this component into successful Windows allocation callbacks.

## Persistent native capacity

Each regular inode may opt into a persisted, aligned reservation. Absence means
the historical sparse/unreserved contract. A present zero is distinct from
absence. Explicit reserve_inode_result/reserve_result enables or replaces it.

Reservations are multiples of 4096 bytes, at least that inode's EOF, and no
larger than the unchanged 255 × 65,536-byte live-extent capacity. Setting a
smaller reservation than EOF truncates EOF and zeros/discards tails in the SAME
private Stage and replicated three-barrier publication. Raising the reservation
preserves EOF and all data. An identical reservation/effect is a verified no-op.

Writes and EOF growth on tracked inodes automatically grow their reservation
to the next 4096-byte boundary. Ordinary EOF shrink and overwrite retain the
existing reservation until an explicit reservation shrink. Generic pathname
mutations use the same accounting. Legacy untracked sparse files retain their
existing logical-size behavior; enabling a small reservation on one explicitly
truncates it. No implicit downgrade, host allocation or reservation is invented.

The shared charge is the sum of:
- ceil(reserved_bytes / 65,536) for every tracked inode;
- actual nonzero extent counts for every untracked inode.

The charge cannot exceed 255. Tracked extents are covered by their reservation,
not charged twice. Empty and highly compressible files still consume reserved
capacity. Pending and detached inodes retain their charge until actual
retirement, so a nonfinal Close cannot give their slots to another file. All
other node, metadata, transaction, dirty-extent, decoded-memory, image/history
and process limits remain unchanged. Reservations do not guarantee that future
RAM, append history or physical storage allocation cannot fail.

## Namespace capability FLTXNS05

Untouched formats FLTXNS01..04 retain exact bytes and support. The first explicit
reservation publishes magic FLTXNS05, format field 5. This is an internal
namespace capability, not a product release/version. It remains sticky after
reservation shrink, deletion, basic/security replacement, recovery and copy.

The namespace header stays 64 bytes. Format-5 inode headers are 104 bytes:
- bytes 0..95 retain format-4 meanings and reserved-zero requirements;
- flag bit 16 indicates reservation presence;
- bytes 96..103 are the little-endian u64 reservation, zero when absent.

Only regular inodes may carry this field; root/directories must have neither
flag nor bytes. Old formats reject the new flag. Admission rejects inconsistent
magic/version, unknown flags, nonzero absent bytes, unaligned/undersized/
over-capacity reservations and aggregate overcommit, even with valid object
digests. Names, opaque descriptors and extent arrays follow the header as before.
All headers/names/descriptors/extents still share the SAME 65,536-byte namespace
ceiling. Upgrading an already-full namespace may fail before writes; it never
enlarges the cap or drops security. Security updates now explicitly retain newer
capabilities rather than assigning format 4.

## Ownership and captured output

NativeInodeAllocationInfo contains the unchanged bounded NativeInodeCommitInfo
and an optional reservation. reserve_inode_result returns this captured state
with the durable transaction. It is copied from the validated private Stage
before publication, assigned only after success and returned without allocation,
backing/path queries or clock sampling. Old public result layouts stay intact.
Snapshot and verified writer/session queries expose reservation and usage
separately from physical storage metrics.

Session reserve_result requires existing already-authorized data-write access.
Provisional ordinary I/O remains denied. Stable inode identity survives rename
and detachment; late paging ownership remains valid until Close. Failed
publication retains contexts and terminal fencing, with no retry. Authorization
is still the future Windows adapter's job; this component is not an ACL evaluator,
kernel-context attestation or a Windows cache-manager implementation.

## Validation gates

The new CPU groups cover absence versus zero, upgrade/sticky capability,
alignment/overflow admission, metadata-cap pressure, mixed legacy/reserved
capacity, compression-independent charging, growth, shrink/tail zeros, old
snapshots, metadata/security preservation, compaction/new identity, detached
lifetime, grants and provisional denial. Digest-valid malformed namespaces
must fail independently of object checksums.

Eight raw/automatic × fixed/compact × reserve/shrink cases deny every backing
operation after the third barrier. The captured return must complete without
I/O; a later query proves denial is active and must fence/retain context.

Four separate fault groups enumerate reserve upgrade, allocation shrink,
write-driven growth and EOF-driven growth. Each covers raw/automatic and
fixed/compact layouts, every observed write/flush, and all three existing
failure modes. Recovery must select complete old or new data/EOF/reservation/
basic/security state, preserve unrelated acknowledged bytes, and continue
through an independently reopened CPU writer. Existing tests and 45-second
per-group limits are retained. Configured tests need exact-commit executed
results; these are not physical power-loss or new process-kill campaigns.

No owner-PC operations, writable mount, Windows 11 client policy or live AMD
qualification follows. NATIVE_ALLOCATED_EVENTS.md now integrates native
Create/Overwrite reservations and trusted Cleanup trim into their SAME
transactions. Complete Windows FileInfo/physical backing reservations,
scalable allocation/reclaim, payload repair and actual driver/helper wiring
remain required.

## Executed hosted allocation checkpoint

Commit `de31aa86cf3d2782aa5ef9daa3fec87bd83290df` was implemented through native GitHub APIs and executed only on disposable GitHub-hosted VMs. The owner's PC was not accessed.

Windows native job 129705164863 passed all 94 CTest groups; raw Windows job 129705164706 and relocated native-codec Windows job 129705167291 each passed 88. Linux raw/codec and relocated jobs passed 84. The new allocation group emitted 180 invariant checks and eight sealed-publication cases. The full codec-enabled raw/automatic × fixed/compact × reserve/shrink/write-grow/EOF-grow matrix contained 16 complete traces and 1,698 faults. Raw-only contained 1,704: automatic fixed-layout shrink used 59 I/O steps there versus 57 with codecs. Every observed write/flush retained all three failure modes; these are not omitted fault points or physical-power tests.

The existing actual selected SDK groups retained initial-delete 22 cases/177 checks and provisional 32 cases/97 checks. Windows worker handle totals remained 94 before and after. No new Windows allocation callback, physical backing preallocation, writable mount or client qualification is implied by the native reservation component.

Legacy process-interruption campaigns completed 976 cases on each hosted OS, Windows basic metadata completed 712 and compact root-3 completed 172. Those old scopes remain unchanged; they are not new NS04/NS05 process-termination evidence. The official dependency audit passed with outdated_pins empty; this is not a new Dependabot alert result. Native RAM/digest comparisons and pinned reference tests passed without changing their benchmark scope.

Six fresh CPU-only CocoIndex routing queries bound this commit to source SHA-256 `2949cd87730735d784e1b7f759df4bad41f7229aafc929152363618cdc55872e` (job 129705165870). Current WinFsp release API and selected immutable allocation contract source were reviewed; no web-page reader or owner-host crawler was used.

Sanitizer job 129705164348 passed all 84 tests, including all 16 allocation traces/1,698 faults and 180 invariants/eight sealed cases, with ASan/UBSan/leak gates unchanged. All 11 workflows succeeded. Hosted read-only mount job 129708901933 completed normal, deliberately terminated-helper and recovery phases with confirmed exits 0, 1460 and 0. All three source-bound space reports reconciled; mapped reads, sparse tails, enumeration, write rejection and backing parity passed. Process peaks were 6,119,424 / 6,131,712 / 5,111,808 bytes under existing limits. This old read-only fixture does not execute NS05 allocation or qualify writable mounting, latest Windows 11 client behavior, physical power loss or AMD runtime benefit.
