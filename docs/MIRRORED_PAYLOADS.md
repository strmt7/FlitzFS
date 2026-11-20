# Mirrored paged payloads (experimental)

`PagedObjectStoreWriter::create_mirrored` explicitly creates a new capability-2
paged image. Every logical object, including native roots, inodes, names and data,
has two independently checked encoded streams in separate qualified allocation
units. CPU reading can select the surviving copy. This provides a redundancy
primitive for future shared decoder dependencies; no grouped/model codec is added.
Ordinary `create` retains capability 1 and its existing bytes. No existing image
is upgraded, rewritten, selected or removed automatically.
Paged-storage capabilities (`FLTXPG01`) and native-namespace capabilities
(`FLTXPN01`) are separate. Mirrored storage can contain any of the three native
layouts; indexed names alone do not enable payload mirroring.

## Encoding and placement

Root/page magics, 4-KiB pages, root positions, reference fields, publication
barriers and record codecs retain the contracts in [paged storage](PAGED_OBJECT_STORE.md).
Root and page capability fields are **2** for mirrored images. New readers accept
both capabilities; old readers reject capability 2. Mixing the two in a root
pair or tree is invalid. This changes storage encoding, not a product version.

Mirrored leaves hold at most 55 entries, each 72 bytes; branches retain 35
references. Integers remain little-endian. An entry contains:

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0, 8 | 8 each | Object ID and generation |
| 16 | 8 | First primary-stream byte |
| 24, 28 | 4 each | Decoded length and encoded payload length per copy |
| 32 | 32 | Expected identity-bound record digest |
| 64 | 8 | First peer-stream byte |

The isolation unit U is the larger of 4,096 and the qualified backend failure
unit, as before. Logical record streams pack across alternating banks:

| Bank unit | Contents |
| --- | --- |
| Even | Primary stream bytes |
| Following odd | Identical peer stream bytes |
| Next even/odd pair | Continuation of those streams |

Records can share banks and their 128-byte headers can straddle banks. For
first byte A and logical byte offset J, its physical position is
`A - (A % U) + 2*U*((A % U + J)/U) + ((A % U + J) % U)`, using integer division
and checked arithmetic. A primary starts in an even unit; its peer starts at
`A + U`. The two streams therefore never share a failure unit, including when
one record spans many units. The peer pointer, full stream bounds, identity,
generation, lengths, codec, stored digest and independently decoded bytes must
agree before exposure. Both streams precede their owning leaf page.

Unused bank tails are zeroed and sealed before metadata publication. Subsequent
transactions never reuse acknowledged bank padding. Packing avoids per-object
rounding; redundancy still costs approximately two encoded streams plus headers,
bank tails and the larger index. Complete image/history bytes must always be
charged. `PagedStoreInfo::stored_payload_bytes` includes both payload copies;
individual `StoreObjectInfo` sizes describe one encoded stream. Neither field
includes record headers, index pages, padding or retained history: use the full
image size for comparisons. Logical object/file sizes and quotas do not double.

## CPU reads, scrub and repair

Normal reads verify the primary and try the peer only on a stored-copy failure.
Allocation failure propagates rather than repeatedly allocating another copy.
A successful fallback reports payload degradation. Explicit `verify_all` scrubs
both encoded and decoded copies, including a bad peer hidden by a healthy primary.
It retains access through one good copy; losing both refuses access, with no
silent rollback to an older namespace or fabricated reconstruction.

### Automatic repair after a verified read

A successful primary-to-peer fallback in a live indexed query records the
selected object descriptor. The initialized queue holds up to one ordinary
transaction's change count. If more distinct repairs arrive, the owner
uses a checkpoint-checked keyset sweep to inspect one record at a time.
The read returns verified data immediately. A write that encounters the same
recoverable degradation receives `STATUS_RETRY` before any publication.
The Windows owner schedules repair under its existing background priority
and fresh resource checks.

A repair step compares the observation to the current exact descriptor and
independently decodes both copies. A superseded record or a healthy retry
settles the observation without a write. With one verified copy, the writer
appends two verified copies and updates the index through a normal COW
checkpoint. The encoded payload, object generation, namespace sequence and
volume selection are preserved. Existing payload banks stay available to
pinned readers. An indeterminate write or checkpoint fences the writer.
If both copies fail verification, the operation refuses to publish bytes.
A capacity refusal suspends that queue at the same storage generation.

### Repair when the selected image cannot append

If append admission fails after verifying a surviving copy, the Windows
owner can stage an alternate image in its owned memory domain or the unused
range of a paired volume. The staged verifier confirms the failed replica
and its surviving peer against the captured checkpoint. It checks the
candidate footprint before writing and independently decodes both new
replicas before selection. A non-saving switch requires the same
capacity-blocked generation at publication. If the fault no longer
reproduces, publication still requires a smaller image.

An equal-size replacement can restore redundancy with zero free bytes
in the selected image. It still needs space for a verified second copy
and quiet maintenance opportunities. The provider attempts it once per
observed generation. Existing reader pins protect the old image through
selection and until retirement.
[Capacity repair evidence](evidence/indexed-capacity-repair-rotation-2026-11-20.json)

Writable resume fully scrubs and refuses known damaged payload replicas.
An explicit independent rebuild streams verified surviving objects into a
new image and verifies both new copies. Proactive scrubbing and automatic
offline repair remain open.
Metadata replicas retain their existing protection.
The defined separation protects against damage to a qualified write-failure
unit; both copies are still in one image. Common-mode errors, loss of the image/
device, dishonest flushes, lost RAM after power-off and arbitrary future bugs
are outside this evidence. Checksums are not authentication or lost-byte recovery.

## Executed scope and costs

The dedicated synthetic memory suite covers both 4-KiB and 64-KiB contracts,
96-object leaf splits, maximum-size records, deliberately straddling headers,
malformed replica descriptors, loss of every bank of a long record, loss of
native root/inode/name/data banks, surviving-copy reads, both-copy rejection,
source-preserving repair, staged live repair, sharing/private updates, independent
new-copy readback and all observed native transaction write/barrier cuts in three
failure modes. [Source-bound validation](evidence/mirrored-payloads-2026-11-15.json).

The live indexed repair checks preserve acknowledged bytes, pinned readers and
object generations. The 4 KiB and 64 KiB models exercise 360 isolated repair
interruptions. They also verify stale observations, transient reads, capacity
refusal and loss of both copies. The Windows SDK callback fixture verifies a
retryable write and an automatic repair with independent CPU reopen.
[Live repair validation](evidence/indexed-live-payload-repair-2026-11-20.json)

In the controlled 96-object raw fixture, one encoded stream contains 1,057,068
bytes at U=4,096; the complete mirrored image is 2,310,144 bytes. At U=65,536,
the fixture contains 1,118,508 encoded bytes and the image is 2,752,512 bytes.
The inputs deliberately differ at one boundary case. These counts expose the
redundancy/index/history cost; they are not compression or throughput records.
Native complete-Model/extent limits, automatic background selection, protected
group manifests, GPU benefit, mounted semantics, optional-codec breadth and
physical power-loss qualification remain open.

Current [OpenZFS mirror source](https://github.com/openzfs/zfs/blob/master/module/zfs/vdev_mirror.c)
was reviewed through its native repository API on 15 November 2026, file blob
`3db712dd5e1ac458f264468f8b2c4c3ef5db4b2f`. Its repair path explicitly preserves
known-good copies. FlitzFS uses a separately verified new image, not copied ZFS
code, its on-disk format or an assertion of equivalent device fault tolerance.
