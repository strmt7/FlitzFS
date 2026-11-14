# Native fast ingestion and verified LZ4 records — experimental

14 November 2026. The storage foundation passed all 13 core jobs at 0d96d903.
The native inode candidate hook passed all 13 core jobs at 3a797a3.
The actual AMD worker diagnostic at 979cd512 is separate evidence; this storage
change does not establish a GPU filesystem operation or end-to-end speed gain.

## Storage contract

Native object codec 4 is one complete independent LZ4 block, with no frame,
external dictionary, history, model or GPU requirement for reconstruction.
The existing object header supplies exact stored/logical lengths and separate
stored and decoded BLAKE3 digests. Logical size remains at most 1 MiB, and stored
size must be positive and strictly smaller. Unknown codecs still reject.
Historical raw/zero/pattern/Zstandard records and default writer bytes are
unchanged. Older readers reject codec 4 explicitly; they cannot read new LZ4
records. Do not enable this representation for a reader that lacks CPU recovery.

The independent reader uses the linked library's LZ4_decompress_safe and requires
exact logical output. Library versions below the qualified 1.11.1 minimum and missing support reject before decoded allocation
or exposure. Truncation, trailing bytes, wrong output lengths and stored/decoded
integrity faults remain errors, never a guessed fallback or rollback.

## CPU fallback and advisory candidates

StoreCompressionMode::fast is explicit. It retains exact zero/pattern detection,
then uses CPU LZ4 only if the complete result saves bytes. Without LZ4 or with
insufficient compressor-state allowance it keeps raw/pattern bytes. The normal
automatic mode retains its existing Zstandard policy; require_zstd with fast
mode rejects before storage creation.

The official external-state compressor uses caller-owned, at least 8-byte-aligned
memory. Its runtime-reported state size must be positive and at most 64 KiB;
actual rounded allocation must fit the supplied library-allocation allowance.
State is reused and reinitialized by LZ4_compress_fast_extState, with no streaming
dictionary. Output capacity is at most the original input size. There is no
worker launch, runtime discovery, thread or background task in this API.

A StoreMutation may additionally own one complete lz4_candidate block. The
encoder independently decompresses and compares every byte with its owned current
input before retaining candidate bytes. Invalid, stale, unavailable or non-saving
candidates select ordinary CPU encoding. Fast mode avoids redundant CPU compression
for a valid candidate while preserving cheaper zero/pattern choices. Automatic
mode compares with its ordinary CPU result. Raw mode preserves historical bytes
and ignores advisory content.

At most 4 MiB of original mutation bytes and a separate 4 MiB of advisory bytes are
admitted before I/O; each is bounded by the existing 1-MiB object limit. Erases may
not carry advisory data. Framing/resource/caller errors reject before writes.
Actual I/O/flush failures keep terminal writer poisoning and independent recovery.
The existing transactional publication, replicas and barriers are reused.

## Required evidence

- An independent hand-encoded block is first checked by the pinned public LZ4
  decoder, then by the object decoder. A hosted immutable 5da33dca baseline must
  reproduce the exact unknown-codec failure; current source must recover it.
- Enabled/raw builds check fast policies, bounded external state, incompressible
  fallback, stale/malformed/inferior advisory data and original byte preservation.
- Native file tests create compressed extents, overwrite across extent boundaries,
  retain the old file snapshot, truncate and independently reopen/read the file.
- Transaction tests independently reopen the stored image, retain old snapshots,
  and inject every observed write/barrier failure under all three existing RAM
  fault models. Recovery must return the last acknowledged or complete new object,
  never a mixture, with no source corruption.
- Existing codec/raw/sanitizer/native Windows and all 13 integration jobs remain
  required. These tests execute no GPU and establish no physical power-loss claim.

After qualification, connect the source-bound isolated worker through asynchronous
CPU selection to the native Windows write/store/read path, then measure complete
costs and useful benefit. Deep collective compression remains a separate size-first
requirement; this fast ingest codec does not constrain its algorithms or group size.

## Current primary API sources

The current release was verified through the native GitHub API as LZ4 v1.11.1,
published 10 November 2026. Source pinned to
aa4647fad575d01a874aaaadcdbe8e6a40e072f5:

- [Official public API](https://github.com/lz4/lz4/blob/aa4647fad575d01a874aaaadcdbe8e6a40e072f5/lib/lz4.h):
  exact block lengths, safe decoder error contract, caller-owned state size and
  8-byte alignment, and extState initialization.
- [Official block format](https://github.com/lz4/lz4/blob/aa4647fad575d01a874aaaadcdbe8e6a40e072f5/doc/lz4_Block_format.md):
  independent literal/match sequences and final-literal constraints.

These were exact source/API reads, not search snippets or a performance claim.

## Retained first-build failure

At 8685f12, Linux enabled/raw builds stopped in the new test: its automatic
encoder omitted the required policy argument. Pass an explicit empty policy;
keep the production constructor and all decoder/fault gates unchanged.
[Exact failure](evidence/native-fast-build-8685f12.json). This was a build result,
not a passed storage or GPU qualification.

## Executed storage qualification at 0d96d903

[Exact source-bound evidence](evidence/native-fast-ingestion-0d96d90.json)
records all 13 jobs in run 43575751686. Linux enabled/raw and ASan/UBSan each
passed 127 CTest entries; Windows native passed 146. The new enabled fast-ingestion
suite passed 119 checks, including native file partial-write/truncate/reopen.
Its replicated transaction campaign covered 55 operation boundaries under three
fault models (165 cases); raw fallback covered 57 (171 cases). Existing checksum,
allocator and object-store assertions remain separately identified.

The immutable 5da33dca source failed the independent valid-block oracle at exactly
the missing native decoder; current source recovered that same block. Archive
and native log digests bind the retained results. No GPU or owner execution
occurred in this storage qualification, and modeled flushes are not physical
power-loss evidence.

## Native inode candidate hook — qualified at 3a797a3

write_inode_storage_with_lz4 reuses the ordinary inode write's committed count,
offset and metadata result. Borrowed NativeLz4ExtentCandidate spans must remain
valid and immutable on the same serialized control thread until return.
Hints address extent indexes only within the explicitly selected inode.

The bridge forwards one unambiguous saving candidate only for a changed,
nonsparse extent. Namespace records, other files, zero extents and reused
unchanged records receive none. Duplicate, stale and non-saving advice selects
CPU encoding; the storage encoder still compares every recovered byte with the
final CPU-merged extent. Partial writes therefore cannot publish a candidate for
a different old/full extent.

Keep the existing 63-dirty-extent count, 1-MiB per-advisory and 4-MiB aggregate
advisory budgets. Budget/caller errors reject before backing I/O; they do not
poison a healthy volume. There is no device query, worker, clock, scheduler or
automatic GPU dispatch in this hook. Existing request/mutation layouts and the
ordinary public write signature remain unchanged.

The new tests use an independently constructed two-match LZ4 representation
distinct from CPU output, then check candidate selection, stale/duplicate
fallback, cross-extent merging, retained snapshots, zero-byte constrained writes
and rejection before I/O. The source-bound [native inode qualification](evidence/native-inode-lz4-3a797a3.json) passed all 13 core jobs, including enabled/raw, Windows native, sanitizers and seven hosted writable mount scenarios. These cases execute CPU code.
## Native compression composition diagnostic

The separate experimental [native LZ4 composition fixture](NATIVE_LZ4_COMPOSITION.md) adds hosted CPU candidate/fallback verification and an explicit GPU-to-native-file diagnostic. It requires matching qualification before owner execution. Normal filesystem I/O does not launch this probe. Its exact-source SDK and 13-job core qualification passed at a047952. One actual GPU payload was stored and independently CPU-recovered; useful end-to-end benefit and physical durability remain unproved.

The new [asynchronous compression controller](ASYNC_LZ4_CONTROLLER.md) reuses bounded worker supervision and evidence-aware recovery while making CPU source selection terminal and independent of worker retirement. Its publishing-source tests are pending; ordinary Windows I/O remains CPU-only.
