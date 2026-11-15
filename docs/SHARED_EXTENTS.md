# Exact shared native extents (experimental)

`NativeFilesystemWriter::coalesce_equal_extents()` shares byte-identical native
extents across files or offsets in an explicit CPU maintenance transaction.
Names, inodes, file bytes, lengths, permissions and timestamps remain separate.
This is a native filesystem primitive, not an archive/extraction operation.
It is a foundation for collective storage optimization, not a learned codec,
automatic background scheduler, GPU speedup or general payload repair.

## Admission and publication

The existing single-control-thread writer stages a private namespace. It reads
and verifies each distinct referenced object through the independent CPU object
reader. Unkeyed BLAKE3 proposes equality candidates; a full byte comparison is
required before sharing. Hash equality alone never authorizes a reference.
There is no filename, mutable source file, external model or dependency chain.
After verifying equality, select the existing encoding with the fewest stored
payload bytes, breaking ties by object ID. The best encoding may appear later
in namespace order or use a different CPU-readable codec. A second bounded pass
redirects references to the final winner; no recompression or stream equality
is required.

The existing 64-KiB extent and object/namespace/transaction limits still apply.
At most one source and one representative payload are needed simultaneously;
maps are bounded by live objects. Representative comparisons have an explicit
extent-count budget, including adversarial hash buckets. Budget/allocation
failure discards staging before writes. Backing-integrity failure fences the
writer through its existing policy. There is no GPU runtime or device query.

One replicated object-store transaction publishes the updated namespace and
removes redundant object IDs from the live inventory. Redundant immutable
records remain in history. Consequently active-payload savings are not immediate
whole-image savings. Existing new-image compaction copies each live shared
object once, preserves the source and uses the existing publication contract.
The caller must still establish reader retirement before reclaiming old images.

## Namespace format 7

Format 7 uses magic `FLTXNS07`, format field 7 and the same 104-byte node headers,
16-byte extent descriptors and workload field as format 6. It permits multiple
inode/block descriptors to reference one verified data-object ID. Every ID must
remain in 2..kStoreMaxObjects, every live object must be referenced, every extent
must be exactly 64 KiB and sparse/EOF padding must remain canonical. Reserved
fields, ordering, identity/generation and replicated-checkpoint checks remain.

Formats 1..6 retain their original bytes and duplicate-ID rejection. Format 7
is emitted only when the explicit method finds new sharing. Empty/unique data
and already coalesced images cause no write or format upgrade. Old readers
reject the extension. Existing format-6 workload credit is preserved; upgrading
an older format starts credit at zero without crediting historical files.

## Copy-on-write and lifetime

A changed format-7 extent is detached before object-ID allocation. Partial writes,
truncate-tail zeroing and detached-inode writes all use this rule. A byte-exact
reversion may reattach only after independent comparison against its immutable
original, before IDs are allocated. A shared object cannot be overwritten through
another logical alias. Zero extents stay sparse.

Deletion, unlink/retirement and replacement remove only that inode's references.
The live object remains until its last reference disappears. Old snapshots keep
their old immutable record mappings through later writes and object-ID reuse.
Compaction/resume and the ordinary independent CPU reader accept format 7.

## Verification and remaining work

`bounded_native_shared_extents` covers cross-file and same-file sharing, unequal
and empty controls, repeated no-op maintenance, multi-mutation reversion/private
updates, shrink/regrow, deletion, detached lifetime, old snapshots, compact/resume,
selection of a later smaller encoding for identical decoded bytes,
unknown/legacy alias rejection, invalid dependencies/EOF padding and each modeled
publication fault before/after sharing. It compares complete freshly compacted
image costs against an otherwise equivalent unshared control.

Local raw-only qualification exercises both requested raw and automatic policies;
automatic uses CPU fallback when optional libraries are absent. Qualify codec-
enabled configurations separately. The same selected suites also pass under
MSVC AddressSanitizer. C++ exception unwinding is an explicit build requirement;
the store rejects an MSVC compile without it because RAII leases/rollback depend
on cleanup during exceptions. [Executed local evidence](evidence/local-native-shared-minimum-2026-11-15.json).
CPU tests are not GPU execution,
mounted Windows qualification or physical power-loss evidence.

This extension provides immutable references, independent verification and the
existing replicated metadata protocol. It does not add payload mirrors or
reconstruct corrupted/lost data. Automatic collective publication still needs
protected decoder-dependency redundancy/repair, resource-governed scheduling,
paged indices/reclamation and measured held-out benefit. Different/near-duplicate
data need stronger shared representations; exact equality alone is not the
owner's extreme collective-compression objective.
The deduplication extent size does not limit future compression groups: those
may contain one, two or thousands of files with separately indexed retrieval
units, paged metadata and incremental scanning over millions of files.
