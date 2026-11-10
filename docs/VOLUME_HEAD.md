# Fixed-size native volume head (experimental)

This is a CPU-only custom-backend authority for selecting an active native image
after verified compaction. It is not physical reclamation, a file-provider,
mounted handoff, background service, production startup, GPU execution or a
power-loss qualification. No owner-PC operation or deletion is enabled.

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

## Fixed encoding

Exactly 131072 bytes: two disjoint 65536-byte slots at offsets 0 and 65536.
Each slot contains the following little-endian fields:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | FLTVH001 |
| 8 | 4 | Internal encoding capability 1 |
| 12 | 4 | Physical slot index, 0 or 1 |
| 16 | 8 | Positive handoff epoch |
| 24 | 16 | Stable external volume identity |
| 40 | 16 | Active image identity |
| 56 | 16 | Previous image identity; zero only at epoch 1 |
| 72 | 8 | Minimum active image checkpoint-group generation |
| 80 | 8 | Minimum native namespace sequence |
| 88 | 8 | Parent checkpoint-group generation; zero at epoch 1 |
| 96 | 65408 | Reserved zero |
| 65504 | 32 | Existing unkeyed BLAKE3 integrity over bytes 0..65503 |

This is a separate internal control encoding, not a release/product-number
change. Existing object, root, namespace, codec and default-route bytes/APIs
retain their behavior. Integrity is NOT authentication or anti-replay protection.

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

## Publication and recovery

The backend contract explicitly requires successful flush persistence and
disjoint power-of-two write-failure units no larger than 65536 bytes. Different
head slots and the data images must have qualified nonaliasing failure domains.
This is a BACKEND ASSUMPTION, never inferred device geometry.

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
