# Protected shared-base groups

Experimental native representation, explicitly selected on a **new empty mirrored
paged store**. It is a CPU correctness and storage-accounting foundation for A04,
not automatic learning, GPU compression or a compression record.

[Indexed volumes](INDEXED_NATIVE_VOLUME.md#shared-base-extents) also
support this delta codec. Their extent frames, backreferences and
publication rules belong to the indexed format described there.

## Layout and compatibility

`initialize_shared_bases` selects native `FLTXPN01` capability 3. This requires
the separate paged-store capability 2 from [payload mirroring](MIRRORED_PAYLOADS.md),
indexed directory names and format-7 inode bodies. These capability numbers belong
to different layers. Ordinary creation and all earlier encodings are unchanged;
new readers retain earlier layouts and older readers reject capability 3.
There is no automatic conversion or product version change.

Ordinary native writes still store independent 65,536-byte logical extents.
`compress_shared_base` explicitly selects existing inode/extent pairs, includes
the base selection, deduplicates storage IDs and requires at least two unique
records. It privately encodes and independently CPU-compares every candidate.
It publishes the members, immutable base and namespace sequence in one existing
replicated COW transaction only when conservative complete-image bounds prove
a strict reduction. Invalid selections fail before writes; dense unrelated data
or an unprofitable complete layout can decline without a storage generation change.

An anchor is a raw logical 65,536-byte object with ID strictly between `2^61`
and `2^62`. It has no filename and never references another anchor/member.
Member IDs remain in the existing data domain 2..256. Their logical object
length, 128..65,535 bytes, distinguishes the representation from ordinary
65,536-byte extents. A full plain extent beginning with the member magic remains
plain file data. Normal paged codecs may further encode these object bodies.

## Exact member bytes

`FLTXGM01` has a 128-byte little-endian header:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | Magic `FLTXGM01` |
| 8 | 4 | Representation version 1 |
| 12 | 4 | Decoded extent length, exactly 65,536 |
| 16 | 8 | Immutable anchor object ID |
| 24 | 8 | Exact anchor object generation |
| 32 | 32 | BLAKE3 of decoded anchor bytes |
| 64 | 32 | BLAKE3 of decoded member bytes |
| 96 | 4 | Patch count |
| 100 | 4 | Bytes after header |
| 104 | 24 | Zero reserved bytes |

Each patch is `{offset:u32, length:u32, literal[length]}`. Lengths are positive,
ordered, nonoverlapping and within the decoded extent; the complete framing
must end exactly at the object boundary. Patch count is bounded by available
bytes before traversal. There is no recursive dependency or executable model.
The encoder records differences at matching byte positions, merging equal gaps
of at most eight bytes to avoid a larger patch header. Expansion declines.

Before exposing file bytes, native opening checks the exact reachable object
closure and every member's framing, anchor ID/generation/hash, reconstructed
hash and native extent/EOF invariants. Range reads decode at most one anchor
and one member per extent before slicing; they do not decode all group members.
The group transform needs neither an optional codec library nor a GPU. If the
underlying paged record uses an optional codec, its existing CPU decoder is still
required. BLAKE3 detects corruption;
it does not provide authentication against an adversary who can rewrite the image.

## Mutation, lifetime and recovery

Names, inode identities, attributes, permissions, workload accounting and file
modification sequences do not change during regrouping. Members may span files.
Deleting or modifying the file that supplied the base does not remove the anchor.
Private writes decode verified original bytes and detach to ordinary extents.
Detached/open-unlinked files retain their dependencies. The last live reference
retires the anchor from the new tree; pinned old snapshots retain old records.
Reused anchor IDs are bound by generation and decoded digest.

Both anchor and member object records inherit mirrored stream protection.
Single-copy reads and source-preserving new-image repair use the independent CPU
reader. Known degraded storage refuses writable resume; lost copies are restored
by rebuilding into a new image. Both missing copies are rejected, not reconstructed
from checksums. A dependency verification failure during preparation fences the
writer before later mutations; allocation pressure alone does not. Atomic
publication and terminal writer fencing use the existing
[persistence model](PERSISTENCE_MODEL.md); physical power loss, arbitrary bugs and
whole-device loss remain outside this modeled qualification.

Synchronous native compaction/recompression and the existing
[staged rebuild](STAGED_PAGED_REBUILD.md) preserve this layout, dependency object
generations and exact plaintext while rebinding only the image identity.

## Complete size accounting and limits

`rebuilt_size` is a pure no-I/O streaming-layout calculation. It charges the
128-KiB root area, record headers, both encoded streams, all tree levels and
isolation padding in the same order as new-image rebuilding. The admission
comparison uses a conservative before lower bound (zero root payload) and after
upper bound (raw changed records). It retains the current isolation unit and
protection policy. Untouched encoded streams remain unchanged during compaction.

Publication appends history and initially **grows the selected source image**.
Bounds describe a subsequent complete compacted image, not immediate physical
reclamation. Peak space must also charge the retained source, candidate and pinned
readers. Paged logical/stored payload counters describe the member/anchor object
representation; native file sizes describe plaintext. Neither is full image size.

The current native frontend still admits 256 nodes, 255 extents and 64-KiB
normalized metadata, with complete Model residency. Shared namespaces privately
admit at most 1,024 mutations and 4 MiB + 128 KiB input; public paged transactions
remain 256 operations/4 MiB. The synchronous wrapper uses the cooperative engine
below. These prototype bounds are not intended group or volume limits for the
finished filesystem.

## Cooperative preparation and foreground reconciliation

`prepare_shared_base` creates an owned `SharedBaseWork` from selected extent
coordinates and an immutable source root, with no backing I/O. One pending group per native
writer is admitted. This reservation is separate from the paged-rebuild candidate.
All calls are serialized on the existing control thread. No thread, timer,
service or automatic GPU work is created.

Each `advance_shared_base` either decodes the base or independently encodes and
CPU-compares one member. The caller can yield between every step. Encoding an
already grouped extent may also read its one-level dependency; this is a bound
on decoded extents, not a promise of one backend read. Preparation writes no
backing bytes. `progress()` reports phase, completed/total members and owned
payload bytes; that last counter excludes allocator, index, transient decoder
and process overhead and is not a host-memory measurement.

Private member frames initially use placeholder binding fields. At finish,
only the anchor ID and generation fields are rebound to the current transaction;
the independently verified patch/plaintext hashes remain unchanged. Placeholder
frames never become visible as stored members. No persisted encoding changes.

A weak owner identity survives writer/work moves but cannot revive after writer
retirement or reopening. The bounded legacy layout uses volatile per-object
revision stamps and cached inode/extent bindings. The indexed layout watches
only selected extent coordinates. A successful transaction that changes one
of those mappings or changes a selected inode's lifetime invalidates the work
before its next preparation read. Unrelated commits, selected-file renames and
writes to unselected extents of a selected file preserve the preparation.
Indexed finalization checks the live object identity and generation of every
member before publishing and verifies any reused anchor again.

`finish_shared_base` reconciles the **current** bounded namespace and complete
compacted-size bounds, then uses the existing atomic storage transaction. It
preserves unrelated acknowledged writes, current metadata and workload accounting.
Finalization keeps the independently checked member encoding. Live mapping
checks, layout accounting, record readback and commit barriers remain
synchronous. The finalization cost still needs measurement under contention.

Cancellation/destruction releases private payloads, source ownership and the group
reservation. Stale/foreign/incomplete attempts terminate without publication;
integrity/I/O failures fence the source writer. Duplicate work refuses before I/O.
An expired owner reports failed work. The caller can cancel or destroy it to
retire retained private resources. Cleanup and repeated control calls preserve
the first completed, cancelled or failed outcome. The next integration step is
to feed this primitive from durable indexed candidate discovery.

The [managed paged-volume path](MANAGED_PAGED_VOLUMES.md) now exposes these
control calls through NativeFileSession with typed head selection, funded advice
and normal file-context ownership. It supplies no automatic scheduling/grant.

## Evidence and next work

[Source-bound validation](evidence/shared-base-groups-2026-11-15.json) records the
selected Windows raw/AddressSanitizer suites, modeled interruptions, malformed
dependencies, retained snapshots, bank loss and complete-image measurements.
[Indexed preparation evidence](evidence/cooperative-indexed-shared-base-2026-11-18.json)
records selected-extent invalidation, interleaved foreground edits, scoped
memory I/O measurements and interruptions at publication.
Fixtures are synthetic related aligned extents and unrelated/dense rejection
cases. They establish neither representative compression ratios nor throughput,
latency, GPU benefit, mounted safety or superiority over other filesystems.

The subsequent [staged-work validation](evidence/staged-shared-base-2026-11-15.json)
qualifies the cooperative API, foreground reconciliation, stale/owner/cancel
guards, private budget refusal and interruption recovery separately from the
original representation. The 255-member case exercises the current native
frontend boundary; it is not a million-file or permanent group-limit result.

Current primary-source review of
[Zstandard's dictionary contract](https://github.com/meta/zstd/blob/dev/lib/zstd.h)
confirmed the need for identical immutable decoding dependencies. The API receipt
retains the exact file blob identity, source and digest behind this moving link. This
implementation does not call a Zstandard dictionary API or claim a novel patch
algorithm. Failed old-repository API reads and the canonical `meta/zstd` redirect
are retained in the evidence, rather than counted as successful source review.

Next: fresh contention/resource admission, durable restart progress and protected
paged coherence metadata;
evaluate shifted/fine-grained, dictionary, multi-reference and learned candidates
on held-out heterogeneous data; implement useful finite AMD work and ordinary
Windows routing; connect contention-aware learning, reader-safe selection and
reclamation. A04/S01/R01 and the full product goal remain open.
