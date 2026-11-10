# Native Windows FileInfo and data bridge (experimental)

This is the synchronous Windows SDK boundary for admitted native facts and
Write, SetFileSize, GetFileInfo and Flush. It advances the writable adapter
without enabling a mounted-write callback table or helper/driver product.

## Current primary contracts

The current release API still selects WinFsp v2.2B5, published 13 October 2026,
source 14d6e0280ae15e2550c44d6dbc476be51649727f. These immutable native source
bytes were reviewed for this change:

- [FileInfo ABI](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/fsctl.h):
  72 bytes, attributes/reparse/allocation/EOF/four times/inode/HardLinks/EaSize.
  HardLinks is explicitly unimplemented and must be zero.
- [Native callback contracts](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h):
  WriteToEndOfFile uses offset UINT64_MAX; constrained writes must not extend
  EOF. SetAllocationSize is aligned and can truncate EOF.
- [Actual SDK dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c):
  derives append from the sentinel, copies accepted count/FileInfo only on
  successful Write, maps information classes 19/20 to SetFileSize, and calls
  query/Flush synchronously.
- [Kernel cache sizing](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c):
  clamps allocation to at least EOF before cache-manager sizing.
- [Reference data implementation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)
  establishes its data/count rules; its no-op Flush and detached slow-I/O
  examples are not used. FlitzFS retains its real barrier and bounded ownership.

## Explicit allocation presentation

NativeFileInfoAllocation has no silently accepted default. The only admitted
mode is native_reserved_cache: AllocationSize presents the native aligned
live-capacity reservation for cache/protocol qualification. It is NOT measured
compressed physical allocation or host clusters, and the declaration is not
backing preallocation, driver/cache conformance or external attestation.

A production mounted adapter still needs its real backing/capacity/free-space
policy qualified before selecting this mode. WINDOWS_SPACE_REPORTING.md retains
the selected FSD's compression-information gap and live read-only observations.
No compressed-size feature/flag or Explorer physical-size claim is introduced.

## Complete output for the admitted profile

encode_native_file_info clears all 72 bytes and validates bounded scalar facts.
Regular files require present canonical basic metadata and a present aligned
reservation at least EOF within the existing capacity. Present zero is valid;
absence is not silently converted. Directories require zero EOF and no
reservation. Invalid IDs/sequences/attributes/times/flags or contradictory
pending/detached state reject with a zero output.

Success preserves exact persisted attributes and four timestamps and stable
inode identity. It does not synthesize NORMAL, ARCHIVE, original times or a
path. ReparseTag, HardLinks and EaSize are explicitly zero for this admitted
profile. Static offset/size assertions establish that all ABI bytes are
accounted for. There is no backing/path/clock access or heap allocation in this
translation, so captured Create/Overwrite/data facts can be translated without
a post-publication query once their caller's policy prerequisites are admitted.

## Synchronous operation boundaries

mutation_info verifies an active owned data-write grant before backing I/O.
The write/size bridges preflight this owned state and complete-output
prerequisites before mutation. Read-only, foreign/stale or provisional contexts
cannot become writable through a metadata query. Explicit basic/security rights
remain independent. Absent metadata/reservation requires explicit writable
admission/migration elsewhere; these bridges do not upgrade it as a side effect.

Write validates the complete supplied length, pointers, append sentinel and
requested automatic clock, then uses write_storage_result. SetFileSize selects
reserve_result or truncate_storage_result. Existing append, constrained count,
zero/no-op, tail zeroing, stable/detached identity and three-barrier publication
remain. Return construction uses captured facts only. Count is assigned after
complete FileInfo succeeds. Any unexpected post-commit fact failure returns
failure, zero output/count and a fenced engine rather than false success.

Queries use the verified owned view. File Flush preflights output, performs
the existing real barrier and namespace verification, and returns its preflight
facts without an extra post-return query. Whole-volume Flush performs the real
barrier and returns zero FileInfo, as the ABI marks it valid only for file Flush.
Flush still MUST verify after its barrier; denied post-barrier verification is
a terminal error, not a successful flush.

Every bridge is noexcept. Logical admission errors return native failure with
zero output/count and preserve a healthy owner. Backing/corruption/terminal or
unknown failures fence and require supervisor offlining. Poisoned calls do no
further backing I/O and release no contexts. The future adapter must serialize
all callbacks/control requests on the same session and offline outside a
callback; no worker, thread, service, scheduled task or retry is created here.

## Configured SDK and fault gates

The hosted-only fixture uses actual FspFileSystemOpWrite, SetInformation,
QueryInformation and FlushBuffers exports. It verifies all output fields,
append/constrained/empty/clipped cases, EOF/allocation shrink, zeroed regrowth,
real file/volume Flush, late detached paging and context retirement.
Pure rejected policy/input/clock and grant/provisional/stale cases must return
zero count/FileInfo without writes; invalid pure admission and denied write
grant must do no backing reads either.

Twenty-four success cases deny EVERY backing operation after the third
publication barrier across both CPU modes/layouts and six write/size variants.
Return count/FileInfo must finish without another query. Four further Flush
cases deny post-barrier verification and must fail/fence. Repeated positive
fixtures verify backing ownership expires and OS handles match before/after.
Those checks are not universal heap/VRAM/kernel leak proof.

Four separately bounded fault groups cover actual SDK write, EOF, reservation
and Flush dispatch. Each enumerates all observed writes/barriers under all
three existing failure modes, raw/automatic and fixed/compact. Recovery requires
complete old/new bytes/EOF/reservation/basic/security, unrelated acknowledged
data, zero false acknowledgement and independent CPU continuation.
Existing tests/oracles/45-second limits, formats and resource ceilings remain.

Configured tests need exact-commit execution. Actual SDK dispatch is not
kernel/mounted adoption, writable application/ACL/shared-disposition/cache
qualification, physical power loss or latest Windows 11 client proof. Driver/
helper lifecycle, truthful physical space, scalable storage/reclaim/repair,
live AMD and bounded adaptive maintenance remain required. The latest
RAM-only/no-driver-change owner permission does not bypass those gates;
development and tests here remain on disposable hosted VMs.
