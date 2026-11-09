# Windows native metadata research and implementation boundary

Experimental. Windows 11 AMD64 is the sole product target. These notes do not
enable a writable callback, driver, mount, helper service or owner-PC experiment.

## Verified current source

The selected WinFsp development candidate remains v2.2B5, with source
`14d6e0280ae15e2550c44d6dbc476be51649727f`. Exact native repository API
reads on 10 November 2026 (Europe/Berlin) inspected its
`inc/winfsp/winfsp.h`, `src/dll/fsop.c`,
`src/sys/fileinfo.c` and `tst/memfs/memfs.cpp`. The source pin is the
selected package identity, rather than an assumption about the newest branch.

The header's SetBasicInfo callback uses INVALID_FILE_ATTRIBUTES to preserve
attributes and zero to preserve each timestamp. The selected FSD source first
converts a zero incoming attributes field to that sentinel; otherwise it
clears NORMAL/DIRECTORY and restores DIRECTORY from the actual node kind.
It rejects TEMPORARY on directories. It converts incoming -1 times to zero
before the user-mode callback. The callback therefore has a different
representation from the incoming Windows structure. Passing raw kernel flags
and time sentinels directly into persisted fields would be incorrect.

That source explicitly special-cases -1; this inspection does not establish
complete -2 restore behavior or Windows 11 client compatibility. Those need
current primary-reference review and executed boundary tests. A value's fitting
in UINT64 is insufficient evidence that it is a valid persisted timestamp.

The user-mode dispatcher forwards SetBasicInfo results into the response's
FileInfo. Its sample memfs mutates attributes/times in memory and returns updated
information. The sample does not provide FlitzFS transactional durability,
failure fencing or a production metadata design.

The header's allocation contract requires allocation-unit alignment, allocation
at least file size, automatic allocation growth for file extension, and
truncation when allocation is reduced below EOF. This is a Windows-facing
contract; it does not make compressed payload bytes or the whole image's
historical length an interchangeable allocation figure.

The sample SetSecurity calls FspSetSecurityDescriptor, obtains the replacement
descriptor length, copies the result and releases the helper allocation with
FspDeleteSecurityDescriptor. That mechanism is a source-bound candidate for
the future adapter. It does not establish FlitzFS authorization, inheritance,
descriptor validation, persistent security storage or memory budgets.

Primary source:
[callback header](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h),
[FSD metadata](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c),
[user-mode dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c),
[memfs example](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp).

## Hosted research campaign

The reviewed isolated Crawl4AI lane now requests twelve Microsoft primary pages
in three bounded groups: basic timestamps/attributes, security/inheritance,
and current protocol/allocation references. Search results were used only for
URL discovery. No search snippet is accepted as implementation evidence.

Every group requires all native captures to succeed. Empty/challenged/wrong-
scope pages remain errors; there is no alternate reader or challenge bypass.
Native URL/status/error/text/provider/digest receipts and complete captures are
retained. Publisher revision metadata is separately extracted from captured
HTML with bounded parsing/logging. Missing metadata is explicit. Every such
metadata record keeps revision_verified=false: a publisher field alone does
not prove a recent compatible contract. Fetch time and content revision must
not be conflated. No results from this newly configured campaign are claimed
here before the exact hosted job is inspected.

Current-served pages with old revision dates may supply navigation/background.
Important behavior changes must be bound to recent reputable primary source
and current selected SDK/FSD source, then verified on the appropriate Windows
environment. A Windows Server hosted result is not a Windows 11 client result.

## Required next implementation properties

Existing born/modified namespace sequences are transaction counters, not UTC
timestamps. Preserve their semantics. The read-only adapter's synthetic times
and rounded logical allocation are not persisted Windows metadata.

Persisted Windows metadata needs explicit capability negotiation, strict
canonical decoding, stable inode association and no silent downgrade.
Historical namespace readers must reject unsupported capabilities explicitly.
Keep the current 64 KiB namespace, 256-inode, 255-nonzero-extent, 16 MiB decoded
object and transaction staging limits until a separately reasoned and tested
capacity design is qualified. Security descriptors need both individual and
aggregate bounds; do not smuggle unbounded allocations into per-inode fields.

Updates must publish attributes/times/security and associated data or namespace
changes atomically where the Windows operation requires it. Authorization and
per-open suppression state belong at the correctly synchronized Windows/session
boundary. Advisory clocks/models must not affect identity, transaction sequence
or independent CPU recovery. Allocation reservation, logical EOF, stored live
bytes and historical image length need distinct meanings.

Failure tests must preserve all acknowledged metadata and file bytes, retain
open contexts and refuse retries after uncertain completion. New compatibility
work needs real Windows API/callback tests, current source review and explicit
remaining client/ACL/cache/sharing limitations. Nothing here permits owner-PC
execution or makes an impossibility guarantee about future bugs.
