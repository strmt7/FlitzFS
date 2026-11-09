# Replicated current checkpoints

Experimental CPU storage protocol. Native file transactions and positive-
generation compacted copies now use two current roots and two separately sealed
manifest replicas. This strengthens recovery of acknowledged namespace/object
references. File payloads are NOT replicated by this mechanism, and arbitrary
media/device/firmware failures or universal future-bug immunity are not claimed.

## Compatibility and API

Existing record, manifest, namespace and codec encodings remain unchanged.
Legacy root format field 1 stays readable. Generic create/commit on a legacy
object store preserves its historical bytes. commit_replicated explicitly
upgrades publication; once upgraded, commit preserves replicated publication.
Native initialize/apply use it by default. Native resume upgrades a legacy
namespace with unchanged logical namespace bytes/sequence before returning.

Root format field 2 uses the existing FLTXR001 magic and 4096-byte root size.
This is an internal format capability, not a product release/version bump.
Older readers that support only root field 1 reject it explicitly.

StoreCommitInfo reports replicated_checkpoint, degraded_checkpoint and
checkpoint_generation. The latter is the FIRST publication generation of the
current pair; it stays the same when either root is lost. A repair creates a new
pair and therefore a new checkpoint_generation without changing object contents.
It is not a replacement for the native namespace's persistent logical sequence.
Root generation remains a publication/lease counter and may be one lower after
loss of the second root while every acknowledged byte is still present.

## Root fields and independent metadata units

The existing root fields through offset 95 remain as documented in
OBJECT_STORE_FORMAT.md. Root format 2 adds:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 8 | 4 | Root format field = 2 |
| 96 | 8 | Peer manifest byte offset |
| 104 | 8 | First publication generation P, positive, less than UINT64_MAX |
| 112 | 3952 | Zero, followed by the existing final 32-byte root digest |

Each root generation is P or P+1, with the existing parity-slot rule.
The first manifest is at align_up(data_end, 65536), and the second is in the
next 64 KiB unit. Both fit within their own unit; the committed end seals the
second unit. A root at P names the lower manifest offset; P+1 names the higher.
Both bind identical object IDs, object generations, offsets, lengths, stored
digests, store identity and decoded-byte sum. Only manifest generation differs.

A root's own manifest digest also verifies its peer: require the peer's expected
generation, normalize only that generation field to the root's generation, and
compare the complete manifest digest. Then run ordinary structural admission.
Two intact same-pair roots must agree with these exact digests/locators. Intact
unsupported/conflicting semantics are rejected, never treated as harmless tears.
If the selected manifest is damaged, only its bound same-state peer is admitted.
Loss of both copies fails; no older object set is silently substituted.

## Publication and health fencing

For previous publication generation G:

1. Admit arguments, decoded/staging/history budgets and two generation numbers.
2. Re-read current roots/manifests and require exact agreement with the writer's
   known healthy state before any replicated mutation. Newly lost/degraded
   checkpoint metadata terminally fences the writer; destroy it and recover
   independently before writing a peer.
3. Privately encode/CPU-verify candidates, append records and prepare manifests
   at G+1 and G+2. Seal their distinct metadata units.
4. Flush dependencies. Publish root G+1 in the peer of the old selected root,
   flush, then publish G+2 in the other root unit and flush.
5. Acknowledge only after all three barriers. Both roots now describe the same
   current objects, including the newest overwrites/deletions.

Before the first new root is durable, the previous selected root survives a
failure confined to the write's admitted units. After it is durable, it protects
the complete candidate while the second root is updated. A failed commit remains
indeterminate and its writer stays poisoned. Old snapshots retain immutable
record references.

A root loss or manifest-unit loss AFTER an acknowledged replicated commit can
recover the same current object set from its surviving metadata. Missing current
roots, mixed old/new publication roots or a damaged manifest copy are reported
as degraded_checkpoint. Payload integrity still precedes exposure. Metadata
preflight reads at most 8 KiB of root bytes plus two 16,448-byte manifests; it does
not re-read every unchanged file payload during write staging.

## Repair before further mutation

Qualified custom-backend resume fully CPU-verifies selected content, checks the
identity/generation lease and fences the observed baseline. For a degraded
replicated checkpoint, it appends a NEW isolated manifest pair beyond the aligned
physical extent/orphan tail, publishes two new roots with the same objects, and
reopens/verifies the complete pair before returning. It does not overwrite a
damaged manifest unit, reclaim data, truncate tails or bypass a live writer.
Interrupted repair follows the same survivor rule and can be recovered again.

A healthy format-2 checkpoint keeps its mode across later ordinary commits.
Legacy format-1 generic resume retains its original protocol; the stronger
post-acknowledgement metadata-loss guarantee does not retroactively apply to
unupgraded images. Native writable admission explicitly upgrades legacy state.

## Cost and limits

Object/inode/live decoded/dirty-buffer/codec allocation ceilings are unchanged.
Two 64 KiB manifest units are retained per replicated publication, plus the
data-to-manifest alignment gap. The protocol adds a root write/flush and
additional metadata I/O versus legacy single publication. Physical history,
padding, orphan tails and both copies count against the existing 64 MiB image
cap. This is a durability improvement, not a claimed throughput/ratio gain.
Efficient grouping, scalable allocation and safe reclamation remain required.

## Fault scope and qualification

RAM tests remove either current root OR either current manifest unit after
different acknowledged transactions, require exact latest bytes/deletions,
repair, continue writing and independently reopen. They inject every observed
replicated write/flush and repair operation with three abstract fault modes,
test unnoticed-root-loss fencing, unsupported intact fields, both-copy loss,
payload corruption, old snapshots and legacy native upgrade.

Hosted Linux/Windows interruption tests retain legacy object cases and update
native transaction/compaction generation oracles. A new checkpoint phase seeds
loss of the selected owned catalog after acknowledgement and forcibly stops
each repair boundary; recovery must retain all acknowledged bytes and continue.
The harness now caps 1024 cases, 256 events, 256-byte lines, eight queued events,
15 seconds per child, 180 seconds total and 3 MiB combined owned image files.
Per-process memory/CPU and 2 MiB individual POSIX file ceilings are unchanged.

These are CPU/RAM/process experiments, not physical power cuts, dishonest flush,
whole-device loss, writable mount/application, GPU-runtime or background-load
qualification. File payload replicas, verified reconstruction, independent
failure domains, automatic handoff/reclamation and low-priority learning-driven
maintenance remain unfinished.


Native file-lifetime transitions use this SAME publication path. Namespace
FLTXNS02 requires replicated current checkpoints and rejects a digest-valid
downgrade to historical single publication. Pending/detached state remains
fully CPU-readable, but ordinary native resume refuses it until the caller
selects separately qualified closed-session recovery. That recovery cannot
infer old context retirement from root generation or elapsed time. See
NATIVE_FILE_LIFETIME.md.
