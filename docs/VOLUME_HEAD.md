# Native volume selector

The [managed paged layouts](MANAGED_PAGED_VOLUMES.md) bind paged image kind
and capacity. The [paired file layouts](PERSISTENT_VOLUME_FILE.md#paired-sparse-image-ranges)
also bind the selected range and the geometry of a sealed image.

The selector identifies the active native image during startup, compaction
and recovery. The CPU reader checks both selector copies and verifies
the selected image. Provider durability and allocation behavior have
their own qualification.

## Authority and ownership

The volume has a stable external identity, separate from its active image's
identity. A qualified provider must retain an immutable, durable, globally
unambiguous image-ID mapping, never reuse an image ID, exclude aliased backends
and all competing volume controllers, serialize calls and retain every source.
Per-instance StoreByteIo writer leases are enforced, including while poisoned;
they do NOT authenticate a global provider or exclude different storage facades.
No PID, boolean, filename, pointer declaration or elapsed delay proves retirement.

create requires a NEW empty head and a quiescent, fully verified native image.
resume requires the exact observed volume identity/epoch and selected image,
actual prior-owner retirement, resolved file-context state and restored image
metadata redundancy. It cannot select an older image to bypass damage.
The head lease must govern future active-image writers as well; the generic
underlying image APIs alone are not a volume-controller security boundary.
No ordinary-file head factory is exposed. Namespace/name persistence and
provider-wide file identity/ownership still require separate qualification.

## Legacy and paired-file encoding

The legacy encoding occupies 131,072 bytes. Its two 65,536-byte slots
start at offsets 0 and 65,536. Each slot contains the following
little-endian fields.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | FLTVH001 |
| 8 | 4 | Internal encoding capability |
| 12 | 4 | Physical slot index, 0 or 1 |
| 16 | 8 | Positive handoff epoch |
| 24 | 16 | Stable external volume identity |
| 40 | 16 | Active image identity |
| 56 | 16 | Previous image identity; zero only at epoch 1 |
| 72 | 8 | Minimum active image checkpoint-group generation |
| 80 | 8 | Minimum native namespace sequence |
| 88 | 8 | Parent checkpoint-group generation; zero at epoch 1 |
| 96 | 65408 | Capability-specific fields followed by reserved zero |
| 65504 | 32 | Existing unkeyed BLAKE3 integrity over bytes 0..65503 |

Capability 1 reserves all bytes after offset 95. Capabilities 2–7 store the
image kind at offset 96 and the indexed byte limit at offset 104. Offset
100 remains reserved. Capabilities 6 and 7 extend 4 and 5 respectively with
two little-endian 64-bit fields. The packed base length starts at offset
112 and the corresponding end of the folded indexed address space starts
at offset 120. The base starts at the beginning of the selected paired
range and the tail follows it. The tail binding must confirm both lengths
and the selected image identity. Earlier capabilities reserve all bytes
after their last defined field. Current readers accept those earlier layouts.

During a mixed mirror/sealed handoff, adjacent epochs keep the same outer
paired layout and capacity. Each peer independently describes its own
image. The higher bound epoch selects its complete image. Older readers
reject an intact sealed capability. A fresh verified mirrored successor
uses capability 4 or 5 again.
[Paired sealed layout](PERSISTENT_VOLUME_FILE.md#sealed-paired-images)

The selector has its own encoding. The selected image retains its own
object, root, namespace and codec versions. The unkeyed checksum
detects altered selector bytes.

Both copies at the same epoch must agree on every selection field. Adjacent
epochs may coexist only as a bound source-to-target transition; select the
higher and report degradation. A missing/bad-integrity peer may be repaired.
Intact unsupported fields, conflicting equal epochs, wrong volume identity,
nonadjacent epochs or inconsistent parent binding reject rather than roll back.
Both copies lost means explicit failure, never guessing an image.

The minimum generation is the FIRST generation of a replicated image checkpoint
group. Losing its second root must not reject the complete surviving first root.
Later normal image commits may advance the namespace/checkpoint while the head
stays fixed. Reader binding requires the selected UUID and both minimum floors;
there is no alternate-image fallback on missing, stale, corrupt or unresolved
selected data.

## Compact external encoding

New indexed Windows memory volumes request compact selectors. Creation
chooses a slot width from the provider's qualified write-failure unit,
with a minimum width of 4,096 bytes. Each slot occupies one or more
complete failure units. The two slots start at zero and at the selected
slot width. A 65,536-byte width uses the legacy encoding.

A narrower slot uses the magic `FLTVH002` and capabilities 1–3. The
selection fields retain their offsets. The final 32 bytes hold the
BLAKE3 digest over all preceding bytes, including reserved zero fields.
The reader derives the slot width from the exact twice-slot extent and
checks both copies independently before resolving an image. Supported
narrow widths are 4,096, 8,192, 16,384 and 32,768 bytes.

The reader accepts existing selectors and reads a compact selector on a
provider with a larger failure unit. Writable reopen checks that the
provider can still separate the two slots before any image or selector
barrier. A reader built for the fixed 131,072-byte extent rejects a
narrower selector at its extent check. Creation of a paired file uses
the legacy width required by its own range addresses.
[Paired-file layout](PERSISTENT_VOLUME_FILE.md) ·
[Windows memory commitment](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)

## Publication and recovery

The backend contract requires successful flush persistence and disjoint
power-of-two write-failure units no larger than the selected slot width.
Selector slots and data images require qualified nonaliasing failure
domains. The memory provider derives its unit from Windows page
geometry. Persistent provider qualification establishes its own
failure and flush behavior.

1. Acquire the exclusive head lease. Require actual source/target writer
   quiescence with their own per-instance leases and the provider contract.
2. Reopen latest native snapshots, verify payloads, root/checkpoint health and
   resolved state. Reject stale supplied snapshots and unhealthy target metadata.
3. Compare the entire canonical namespace with ONLY internal image identity
   rebound, then every non-namespace object ID/generation and decoded payload.
   Times, ACL bytes, reservations, sparse layout, allocator high-water marks,
   names and inode IDs must match. Compare bounded live objects, NOT a sparse
   file's potentially 1-TiB logical range.
4. Flush and fully revalidate the target; recheck the unchanged source. Publish
   the new self-contained selector in the first slot and flush.
5. Publish the identical selector in the other slot and flush. Independently
   read/check both slots before returning authority. Epoch increments once.
6. Any error after target fencing begins makes the writer terminal and retains
   its head lease. It exposes no usable cached selection and retries no I/O.
   Preserve source, candidate and partial control bytes for independent recovery.

Initial creation fences the initial image before writing a new head. Its two
zeroed slots establish the full extent before selector publication. Resumption
fences the selected image; repair writes/flushed the missing/stale peer FIRST,
preserving the chosen survivor, then refreshes the other copy. Repair does not
change the selection epoch or manufacture an old source as current.

An interrupted handoff can recover either the previous source or the complete
new candidate. Those have identical native state at publication. Foreground
writes may continue ONLY on the independently recovered selected image under
the qualified volume owner. After acknowledged selection, loss of either head
copy must still select the SAME active image, including its later commits.
The old image remains available to already-owned snapshots.

## Current tests and remaining integration

Four configured CTest groups cover invariants and every observed creation,
publication and repair write/flush step under all three existing RAM fault modes,
raw/automatic CPU encoding and fixed/compact image metadata. Repair covers both
head peers. Independent CPU selection must preserve all acknowledged contents
and continue writing; a stale/foreign image or conflicting intact head rejects.
Tests include metadata/security, stable inode IDs, a near-1-TiB sparse file,
unknown contracts, exact epoch leases/exhaustion, source preservation, live-writer
exclusion, poison-held leases and terminal zero-I/O retries.

Each baseline source+candidate+head is bounded to 3 MiB; invariant checks create
additional separately bounded synthetic RAM copies. No files, GPU or mount are
created; all pre-existing process/memory/time limits and all old oracles remain.
These are configured tests until exact-commit results are inspected.

This implements durable selection under the stated model, not source deletion.
Reader pins/retirement, safe reclamation, resumable low-priority compaction,
provider namespace durability and Windows helper/callback integration remain
required before automatic maintenance. Global writer/clone exclusion, latest
Windows11 client behavior and physical power-cut evidence remain unqualified.
