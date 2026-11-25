# Mirrored paged payloads (experimental)

`PagedObjectStoreWriter::create_mirrored` creates a new paged image with
capability 2 by default. Fresh indexed sessions select capability 3
when the qualified failure unit is larger than an index page.
Every logical object, including native roots, inodes, names and data,
has two independently checked encoded streams in separate qualified
allocation units. The CPU reader can select the surviving copy.
Ordinary `create` keeps capability 1. Reopen and rebuild preserve the
selected capability of an existing image.
Paged-storage capabilities (`FLTXPG01`) and native-namespace capabilities
(`FLTXPN01`) are separate. Mirrored storage can contain any of the three native
layouts; indexed names alone do not enable payload mirroring.

## Encoding and placement

Root/page magics, 4-KiB pages, root positions, reference fields, publication
barriers and record codecs retain the contracts in [paged storage](PAGED_OBJECT_STORE.md).
The page capability field is **2** for both mirrored layouts. The
root selects capability **2** for separate payload and index banks
or **3** for placement that can share a fresh pair. Both intact root
slots must agree on capability. Earlier readers reject an intact
unsupported root. A new reader can open all three capabilities.

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
agree before exposure.

Capability 2 places both copies before the leaf's primary page.
Capability 3 bounds each copy by its corresponding leaf page. The
primary payload and metadata occupy even banks, with their peers in
the following odd banks. The writer can place the first metadata
page after the payload in the same pair when a page fits after
alignment. The page stream can continue in additional pairs.
The writer zeroes the alignment gap and the final tails, reads both
payload copies independently and uses the existing root flushes.

The 64 KiB model saved one complete bank pair on the single-record
commit and on the 256-record scattered update. In the 4 KiB model
the requested and ordinary layouts produced identical persisted
images. The signed WinFsp guest replay verified the same external
operations and CPU file contents at lower backing allocation.
[Layout and recovery checks](evidence/windows-shared-mirrored-banks-2026-11-25.json)

Subsequent transactions never reuse acknowledged bank padding. Packing avoids per-object
rounding; redundancy still costs approximately two encoded streams plus headers,
bank tails and the larger index. Complete image/history bytes must always be
charged. `PagedStoreInfo::stored_payload_bytes` includes both payload copies;
individual `StoreObjectInfo` sizes describe one encoded stream. Neither field
includes record headers, index pages, padding or retained history: use the full
image size for comparisons. Logical object/file sizes and quotas do not double.

## CPU reads, scrub and repair

Normal reads verify the primary and try the peer only on a stored-copy failure.
Allocation failure propagates rather than repeatedly allocating another copy.
A successful fallback reports payload degradation. Explicit `verify_all` reads
both copies and checks each stored header and digest. A compressed peer shares
the decoded proof only after the primary has decoded successfully and the
two verified headers and encoded bodies compare byte for byte. A remaining
candidate is decoded on its own. The comparison retains at most one extra
encoded record within the existing object size bound. This still detects a
damaged peer hidden by a healthy primary and refuses access when both copies fail.

In 24 full audits of a 96-object Zstandard fixture, the median of four
matched Windows runs fell from 194.2 ms to 139.1 ms. Each run read 39,510,528
backing bytes, and fault checks detected damage to either copy and verified
the surviving bytes through the CPU reader.
[Mirrored audit evidence](evidence/indexed-audit-reuse-2026-11-20.json)

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

### Automatic integrity scan

On a populated indexed attachment the Windows owner also inspects replicas
that foreground reads have never needed. An admitted step shares a freshly
checked object-index traversal across several records. Each record still
checks both copies and independently validates its decoded bytes. A step
charges encoded bytes, headers and the maximum decoded work for both copies.
The owner checks for foreground demand and elapsed slice time before taking
another record. It retains no decoded pages or old image between steps.

The scan advances by key to the end captured at the start of the pass. A
foreground commit can supersede a record or add keys beyond the captured end.
A later pass covers that range. A complete in-process pass earns a checkpoint
observation only if each step saw the same checked generation and every
object passed. The final step freshly checks the roots and binds the
observation to the image identity, generation and digest of both roots.
Restored advisory cursors and passes across a generation change continue
scanning without producing this observation. The observation covers
paged-object verification. Writable attach keeps its full native audit.

On a 16,384-object in-memory control, verifying up to 64 objects per step
reduced backing reads from 677,412,864 to 20,111,360 bytes. Every paired pass
checked the same checkpoint and the CPU reader independently recovered
every object. The Windows owner also applies its live time and foreground
checks. [Matched scan evidence](evidence/indexed-scrub-checkpoint-2026-11-23.json)

The scan queues repair only after decoding a surviving copy. A later
opportunity revalidates the descriptor before the usual append repair or
alternate-image handoff. An object with zero verified copies leaves its
bytes untouched and reports an integrity fault. An unverified index ends
the pass before it can offer repair from that index. The helper shows an
integrity warning for either observation.

Verification and repair use the Windows owner's foreground, CPU, memory
and worker-retirement checks. Scan age wakes the owner when a pass is due.
Clean passes schedule their next check after four weeks. A pass that saw
a fault revisits after one day, and a newly completed live repair can make
it due sooner. Each step still needs a resource-admitted opportunity,
and an unchanged empty attachment does no scan work.
[Windows owner scheduling](WINFSP_OWNED_CONTROLLER.md#content-events-and-bounded-work)
[Source-bound scrub validation](evidence/indexed-payload-scrub-2026-11-20.json)

### Persistent scan progress

An admitted Windows indexed scrub can save a verified prefix in the existing
paired advisory area. A pair of 64 KiB records is funded by 8 MiB of fresh checks
of both encoded payload copies and their headers. The scanner accumulates
credit only while that prefix is intact and checks for waiting foreground work
before issuing the paired write. A payload fault leaves subsequent work in
memory for the rest of that pass, and an index fault ends the pass. Neither can
advance the durable cursor past the unverified record.

The first admitted step after reopen can resume a saved prefix of the same
image. The cursor must fit the current image generation and key range.
Verification continues against the current index, so ordinary same-image
commits preserve usable progress. An unrelated image, future generation,
expired pass or backward clock starts a new pass. The pass start time limits
reuse to four weeks, including across repeated restarts. Changes past the
pass's captured end remain for the next pass.

Advisory failure leaves the namespace and acknowledged bytes available.
A restart can recover the old or new checked cursor. If one advisory peer
survives, fresh verification can fund repair of the pair while preserving
the foreground input settlement. Both-peer loss starts a fresh scan.
[Durable scrub validation](evidence/indexed-durable-scrub-2026-11-20.json)

### Repair during writable reopen

Closed indexed reopen verifies the selected native checkpoint after the
previous owning session has retired. A full CPU pass requires a verified
copy of every payload and healthy index pages before entering the repair.
The exclusive image writer then walks that immutable index and rechecks
each damaged descriptor against the current checkpoint. It appends verified
replicas using the same copy-on-write transaction as live repair.

Each successful repair preserves logical object generations, acknowledged
bytes, the namespace sequence and the selected image. The reopen checks the
result independently before recovering any retired file contexts or allowing
writable callbacks. An interrupted reopen can retry from the checkpoint
selected by the normal root protocol. Independently readable source data
stays available when append capacity is insufficient.

Direct paged-store resume still requires healthy payload copies. The
startup path requires closed indexed ownership and refuses unverified
payloads or a damaged index.
[Startup repair validation](evidence/indexed-startup-repair-2026-11-20.json)

When both copies of the head and the whole indexed source are healthy
and old lifetimes are resolved, the startup path acquires the selector
lease, checks the expected selection, and acquires the image writer lease.
The image writer checks the paged root and digest against the fully verified
initial view, flushes the image, and rechecks its root and extent. The
selector then flushes under both leases. The exact selection, image extent,
native inventory and every payload copy are checked before either owner
reaches the writable session.

The healthy path issues one image flush and one selector flush. A changed
extent, a same-generation fork or replica damage during either barrier stops
admission. A later reopen can use the ordinary repair path when valid
replicas survive.
[Matched reopen and fence validation](evidence/indexed-scale-reopen-2026-11-20.json)

If append repair runs out of space, the Windows paired-range and memory
providers can supply an alternate image. The closed owner verifies the
latest selected checkpoint again, including repairs that already committed.
It fences the selector, admits the packed target footprint and compares
the complete decoded inventories before publishing the new selector.
Detached lifetimes travel with the verified image and resolve after its
selection. An interruption leaves the selected source or the fully
verified replacement available to CPU recovery.
[Full startup repair validation](evidence/indexed-full-startup-repair-2026-11-20.json)

Index reconstruction and persistent repair intent remain open.
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
