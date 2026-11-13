# `.flfs` v1 experimental chunk container

**Status: unstable development format; do not use as a sole archival or recovery copy.**
All integer fields are unsigned, little-endian. No implicit packing or machine-dependent structures are used. Metadata is checked before allocating block buffers. Data CRC32 is not cryptographic authentication.

## Byte layout

| Region | Byte count | Description |
|---|---:|---|
| Header | 40 | Magic and fixed-sized container metadata |
| Chunk payloads | Variable | Payloads are concatenated in ascending logical-chunk order |
| Index | `32 * chunk_count` | Fixed-size records pointing to independent chunk payloads |
| Footer | 32 | Commit marker and CRC32 for the complete index |

### Header (40 bytes)

| Offset | Width | Value |
|---:|---:|---|
| 0 | 8 | ASCII `FLFS0001` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Logical chunk size, 4 KiB–1 MiB, power of two |
| 16 | 8 | Original logical file length |
| 24 | 8 | Number of chunks = `ceil(logical_length / chunk_size)` |
| 32 | 8 | Reserved = 0 |

### Chunk-index record (32 bytes each)

| Offset within record | Width | Value |
|---:|---:|---|
| 0 | 8 | Offset of stored payload in the container |
| 8 | 4 | Stored length in bytes, 1..2 MiB |
| 12 | 4 | Decoded length, expected chunk size except the last chunk |
| 16 | 4 | CRC32 (IEEE 802.3 polynomial) of complete decoded chunk |
| 20 | 1 | Codec 0 = raw, 1 = Zstandard |
| 21 | 3 | Reserved = 0 |
| 24 | 8 | Reserved = 0 |

Payload records must be contiguous and ordered; no holes, overlaps, dangling payloads, or trailing bytes are accepted. Raw chunk length must equal decoded length. Zstd decoding requires exactly one complete frame whose declared decoded length equals the chunk index. Single-shot decompression is bounded by the index maximum of 1 MiB; `ZSTD_d_windowLogMax` does not cap the window in this mode.

### Footer (32 bytes)

| Offset | Width | Value |
|---:|---:|---|
| 0 | 8 | ASCII `FLFSIDX1` |
| 8 | 8 | Index start offset |
| 16 | 8 | Total index bytes |
| 24 | 4 | CRC32 of index bytes |
| 28 | 4 | Reserved = 0 |

An incomplete write without a valid footer will not be accepted as a container. The footer is **not a transactional filesystem journal** and does not make the new output automatically durable across every possible interruption.

## Bounded parsing invariants

- At most 1,048,576 chunks and 32 MiB of serialized index metadata.
- Input-derived sizes are checked before the corresponding buffers are allocated.
- Max decoded chunk = 1 MiB; max encoded chunk = 2 MiB; max single random read = 4 MiB.
- A header/index inconsistency is rejected before payload decoding.
- `info` is metadata validation only; `verify` is required to check all stored chunks.
- Zstd compression falls back to raw data whenever it does not save at least 17 bytes.

### Reader admission in release 0.2 (no wire-format change)

`Reader::open(path, max_index_bytes)` may impose a smaller serialized-index
allowance. The count is checked against it before the index buffer is read or
allocated. Empty indices can fit a zero allowance. Catalogs pass the remainder
of a 4 MiB aggregate allowance when opening each file. Parsed entry arrays and
other allocations are additional memory; this is not a total process-RAM cap.

## Compatibility and migration

This single-file container holds exactly one logical byte stream. It is **not** the `.flzip` native archive format, and it is not itself a directory tree. File paths, ACLs, links, timestamps, immutable object IDs, journaling, and encryption metadata belong to later format generations. Data must be migrated explicitly rather than silently reinterpreting v1 as a writable filesystem.

The 0.2 native callback backend supplies an in-memory namespace and synthetic
metadata around existing containers. It writes none of that metadata into v1,
and does not change its bytes, codec identifiers or integrity guarantees.

CPU workspace reuse does not introduce shared history, dictionaries or new
codec identifiers. Every Zstandard chunk remains a complete independent frame;
raw fallback still requires a saving of at least 17 bytes. The regression suite
compares each reused-context frame against the original one-shot encoder at
levels 1 through 5. Hosted comparison additionally requires identical complete
containers and extracted content; see `CPU_PIPELINE.md` for executed evidence.

## Historical external-format experiment (removed)

The former read-only FLZIP adapter described in `FLZIP_READONLY.md` was removed from product scope. It never changed
`.flfs` v1 bytes or codec identifiers. Its SUZP/SUZF framing and file-wide CRCs
are parsed independently, with verification before exposure. Never interpret
FLZIP as a `.flfs`, ZIP alias, or authenticated container.

## Test-only durability envelopes are a separate experiment

The RAM protocol model in PERSISTENCE_MODEL.md uses distinct test magic and
fixed envelopes to investigate publication ordering. It changes no v1 fields,
codec identifiers or migration rules and is not an accepted writable format.
Never interpret those model envelopes as .flfs containers.

## Separate transactional object experiment

OBJECT_STORE_FORMAT.md defines distinct FLTXR001/FLTXIDX1/FLTXDAT1 records for
an experimental native object transaction library. It does not modify .flfs v1,
its codecs, CRCs, CLI or callback route. No automatic format migration exists.

The separate transaction experiment now admits raw, zero, motif and bounded
Zstandard object records with decoded-content integrity. These are not .flfs
v1 codec changes; historical native image bytes and chunk semantics remain
unchanged. See OBJECT_STORE_FORMAT.md for the separate record contract.

## Native filesystem namespace is a separate client format

NATIVE_FILESYSTEM.md defines FLTXNS01 inode/extent metadata inside the separate
transactional object store. It does not reinterpret .flfs/.flcatalog as writable
namespace formats. No automatic migration or archive import exists.

Current .flfs and object-store Zstandard decoders require modern frame magic
0xFD2FB528 before calling library parsers. Writer-produced bytes remain unchanged;
legacy/skippable or unknown framing is rejected explicitly.

## Cleanup metadata has no new wire encoding

The requested Cleanup bridge in NATIVE_CLEANUP_METADATA.md uses the existing
optional FLTXNS03 tuple and replicated publication. It adds no container codec,
archive compatibility, namespace layout or capacity change. Legacy bytes and
read support remain; request sentinels are never persisted as timestamps.

Atomic stable-inode mutation metadata in NATIVE_MUTATION_METADATA.md reuses the
existing FLTXNS03 tuple and transaction. It adds no wire field, codec, format
version or capacity change; legacy readers/bytes and native product scope remain.

Commit-bound result values in NATIVE_MUTATION_RESULTS.md are in-memory API
output only. They add no wire field, layout, metadata format, codec or capacity
change. Modified namespace sequence is never a UTC time or allocation count.

Windows space reports in WINDOWS_SPACE_REPORTING.md are bounded hosted-test
artifacts, not filesystem wire data or a product-version change. They add no
codec, metadata format, allocation reservation or storage-capacity increase.

The transactional core's opt-in root field 3 in COMPACT_CHECKPOINT_METADATA.md
is a separate internal capability. It changes replicated catalog placement,
not this legacy container format or product numbering. Existing native
namespace/data/codec formats and default root bytes remain supported.


NATIVE_SECURITY.md specifies the separate experimental FLTXNS04 namespace
capability: bounded opaque self-relative security descriptors, 96-byte inode
headers, sticky format and legacy-byte preservation. It changes neither this
container encoding nor object root/record/codec formats.


CPU_DIGEST_ACCELERATION.md changes only the implementation selected for the
existing BLAKE3 calculation. All 32-byte digests and stored formats must remain
byte-identical across scalar and runtime-dispatched CPU builds. It adds no
new codec, format capability, authentication or encryption.


Atomic native create/open ownership introduces no encoding or format change.
The explicit new path uses the existing FLTXNS04 basic/security fields and
existing lifecycle flags; optional delete-on-close intent shares its initial
transaction. Older creation paths preserve their prior capability/bytes.
NATIVE_CREATE_OPEN.md specifies result ownership and configured fault checks.


The explicit native overwrite event changes no encoding/capability. EOF/extent
references and existing basic attrs/times share one ordinary replicated
publication; original creation time and optional security bytes remain.
Historical APIs and wire bytes are unchanged. See NATIVE_OVERWRITE.md.


Provisional overwrite phases are bounded session memory only; they introduce
no wire capability or record/root change. Healthy cancellation clears a context
without deleting or rolling back its inode. See NATIVE_PROVISIONAL_OPEN.md.


Initial delete-on-close introduces no record/header/capability change. Eligible
Cleanup uses existing pending/unlinked/basic fields atomically. Nonempty
directory pending authority is session-only, preserving on-disk children and
safe CPU recovery. Historical durable-intent API bytes remain unchanged.
NATIVE_INITIAL_DELETE.md distinguishes runtime state from persisted flags.

### Explicit native allocation reservation capability

[NATIVE_ALLOCATION.md](NATIVE_ALLOCATION.md) specifies opt-in FLTXNS05: format-5 inode headers are 104 bytes, preserve format-4 fields, add presence flag 16 and a u64 aligned reservation at offset 96 (zero if absent). Regular files only; present zero differs from absence. The 64-byte namespace header, all old bytes/readers, roots/manifests/codecs and aggregate caps remain. New-capability decoding rejects inconsistent flags/framing/alignment/EOF/capacity; normal metadata/security/lifecycle/copy operations retain format 5. Reservations count worst-case native live slots, not host allocation or compressed payload bytes.

### Atomic allocation events

NATIVE_ALLOCATED_EVENTS.md adds APIs and captured state, not another format. Create/Overwrite may include an explicit reservation, and trusted Cleanup may trim an existing tracked reservation, within their SAME private Stage and three-barrier publication. FLTXNS05/104-byte inode headers, old formats, sticky capability, reserved-zero rules and every aggregate limit remain unchanged. These are native live-slot reservations, not measured host or compressed allocation.

### Windows FileInfo/data bridge

NATIVE_WINDOWS_IO.md introduces no stored format or codec change. It translates existing captured FLTXNS05/basic/inode facts into the current 72-byte SDK FileInfo only for explicitly admitted cache/reservation presentation. Original layouts, all old bytes/readers, mutation barriers and bounds remain; native capacity is still separate from measured physical allocation.


### Unmounted native adapter policy

The hosted writable adapter candidate (NATIVE_WRITABLE_ADAPTER.md) uses the
existing native object/namespace encodings unchanged. Windows-global versus
historical aggregate deletion is runtime policy, not a stored format revision.
Its explicit live-quota/cache allocation must not be relabelled host clusters,
compressed physical bytes or host preallocation. No production mounted-write
route or new public existing-image writer factory is enabled.


### Bounded writable VM trial

WRITABLE_MOUNT_TRIAL.md adds no stored format revision. Its per-instance image
history cap is runtime admission, separate from native live reservations,
logical EOF and physical allocation. Existing factory defaults/wire bytes and
three publication barriers remain. An explicit mounted Flush is required before
the trial asserts data durability; CPU reopen is independent after writer exit.


### Extended writable evidence

The new writable mutation/termination reports use test-evidence schema 2;
no filesystem wire format changes. Data and security use the existing unkeyed
record digest (integrity, not authentication). Independent recovery must retain
exact acknowledged namespace/generation, full primary basic tuple/reservation/
security and bytes; report reconciliation rejects mixed, omitted or changed
facts. A helper kill is not a physical power cut.


### Hosted writable restart

No stored filesystem encoding changes. The hosted resume path keeps the
original identity, volume creation time, inode IDs and descriptors, exact
recovered-generation lease and uncertain-tail append floor. It rejects
unresolved old-session state rather than synthesizing Cleanup. Restart evidence
appends a receipt to the existing source report within the same 2048-byte cap;
it does not replace the original acknowledgement or increase image/report caps.


The opt-in hosted writable adapter now has a bounded SetSecurity candidate using
WinFsp's merge helper and original per-part granted-access checks. SDK/CPU tests
are configured; inspect exact-commit CI before claiming a pass. Stored formats and historical bytes are unchanged; bounded descriptor replacements use the existing FLTXNS04/05 atomic metadata transaction.
See [NATIVE_SECURITY.md](NATIVE_SECURITY.md) for its contract and limits.


The hosted writable candidate now rejects all owner/group-bearing security
updates atomically before merge/adoption/storage access because the selected
WinFsp request does not carry the current requester's token. Original handle
rights and equal SIDs do not supply that authority. DACL/SACL behavior and stored
formats remain unchanged. Full owner/group support remains required through a
separately qualified authenticated request path; it is not complete. See
NATIVE_SECURITY.md and WINDOWS_OWNERSHIP_SOURCES.json for current-source
evidence, regression requirements and driver/host boundaries.


The experimental custom-backend native volume head adds a fixed 128-KiB,
replicated authority for selecting a verified compacted image while retaining
the source and a stable external volume identity. Its publication/recovery,
CPU comparison, minimum-checkpoint binding and fault-test contract are in
VOLUME_HEAD.md. Existing encodings/routes/caps remain unchanged. The CPU model tests passed at
dd388157456d0ae6ed9735c0c16a5e83826b37b7; see VOLUME_HEAD_VALIDATION.md.
This does not qualify mounted head integration. No file-provider, mounted handoff,
source deletion, automatic reclaim/worker, GPU or owner-PC operation follows.


The owner explicitly includes their PC in carefully bounded RAM-only testing.
The optional locked-RAM CPU probe and immutable real-file corpus policy are in
HOST_RAM_TESTING.md. It requires VM-qualified exact provenance, page locking,
hard process budgets and confirmed cleanup, with no WinFsp/driver/GPU invocation.
Small source/tool/report staging on the verified HDD is separately capped; it
never supplies filesystem image backing. Configuration is not a host execution
or mounted/GPU/idle-recompression qualification. Existing formats/routes stay
unchanged. Dependabot OPEN-alert access was successfully verified (0 on the
complete10-November snapshot); see DEPENDENCY_AUDIT.md for scope.

The explicit CPU post-compression primitive converts a preserved raw/fast
source into a separately verified NEW image. It preserves native state and
object generations, streams bounded objects and compares decoded source/target
bytes before replicated publication. See RECOMPRESSION.md. Automatic
idle learning/scheduling and live GPU execution remain unimplemented. The first
six-file owner-PC unmounted RAM CPU result is in HOST_RAM_VALIDATION.json;
its complete image remained larger than the tiny corpus despite compressed
payload reduction. New re-encoding tests require their own exact-commit pass.

Managed native volume sessions now connect fixed-head image handoff to the
existing context-owning I/O layer. Explicit compaction/recompression preserves
live resolved handles, access grants, Cleanup state and stable volume identity.
A failed transition retains both images and the context table for one verified
CPU recovery attempt. See VOLUME_SESSION.md. This remains custom-backend
engine work, with new exact-commit tests pending; no automatic maintenance,
mounted/GPU integration, physical reclamation or power-loss claim follows.

Native transactional-store compression match proposals are private encoder inputs. They introduce no codec ID, frame variant, persistent hint, root or container-format change. Accepted results use the existing bounded Zstandard store record and CPU reader.

Worker failure-stage diagnostics change only the supervised process exit code.
They add no container fields, codec identifiers, authentication or durable-write
guarantees. Existing stored bytes and worker request/reply frames are unchanged.

Native compression-profile measurements and their source tuples are in-memory
advice only. They add no stored codec, container field, dictionary dependency or
worker frame. Existing CPU decoding and historical stored bytes are unchanged.


The separate experimental advisory optimization area uses FLTOP001 in two
fixed 64-KiB slots, bound to the external volume identity. Its complete encoding
is in OPTIMIZATION_STATE.md. It changes no existing container/object/root/codec/
worker frame, and no user-data reader depends on it. It is not integrated into
the native provider or an automatic learner. New-source qualification is required.


Volume-owned advisory control changes no stored encoding. The same FLTOP001
records and existing user-image/namespace/head/codec bytes remain readable.
The new session methods add ownership and failure isolation in memory only.

The optional native application and process-lifetime memory provider introduce
no stored encoding, codec, head, advisory-state or worker-frame change. Existing
CPU readers remain independent. The volatile provider's successful-flush scope
is its owning process lifetime; no physical-persistence claim follows.

Input-only parent match preparation changes no request/reply or stored bytes.
The opt-in FLHPH001 256-byte diagnostic metadata is a separate message type,
never a compressed-data reply or file-format field. Its complete layout and
binding/seal/reserved rules are in GPU_PHASE_PROFILE.md. Historical readers and
the independent CPU recovery path remain unchanged.

The coalesced GPU match upload is transient host/device scratch, not a stored
format or IPC change. Its initialized alignment padding is included in admission;
input, little-endian predecessor links and sentinel output share one allocation.
No file bytes, codec identifiers, dictionary dependencies or recovery encoding
change. See GPU_PHASE_PROFILE.md for the checked ownership/qualification boundary.

The ordered basic/mutation/delete mounted fixtures extend the existing bounded
external-client diagnostic report variant to those profiles. The original schema1/
schema2 storage-observation fields remain readable, while the current live gate
requires distinct source/client/verifier actors and phase-correct observations.
This changes no native container, checkpoint, codec, dictionary or worker-frame
encoding. It grants no new write/recovery or physical durability guarantee.
