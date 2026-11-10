# Windows space reporting and allocation qualification

Experimental Windows-only integration work. This probe queries a read-only
synthetic volume on a disposable hosted VM. It does not implement allocation,
change compression settings or enable writable callbacks.

## Current primary evidence

The selected WinFsp v2.2B5 source at
14d6e0280ae15e2550c44d6dbc476be51649727f is the same reviewed 2026 candidate:

- [FileInformation query dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c)
- [Volume attributes](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/volinfo.c)
- [FileInfo ABI](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/fsctl.h)
- [Write/allocation contracts](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [Reference allocation implementation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)

The selected FSD returns STATUS_INVALID_PARAMETER for
FileCompressionInformation. It does not advertise FILE_FILE_COMPRESSION in its
volume-attribute construction. Its standard information reports one link.
This is a verified reporting gap, not a prohibition on compressed backing data.
It does not establish how GetCompressedFileSizeW or Explorer behaves; those
need actual Windows observations. No reference allocation code is copied into
the compression engine.

Current Microsoft declarations were captured through native repository APIs
at win32metadata commit 15fc870a5e96df7c560877ff30136c0fb5a21e9b,
whose head date was 6 November 2026:

- [GetCompressedFileSizeW and volume APIs](https://github.com/microsoft/win32metadata/blob/15fc870a5e96df7c560877ff30136c0fb5a21e9b/generation/WinSDK/RecompiledIdlHeaders/um/fileapi.h)
- [Information classes](https://github.com/microsoft/win32metadata/blob/15fc870a5e96df7c560877ff30136c0fb5a21e9b/generation/WinSDK/RecompiledIdlHeaders/um/minwinbase.h)
- [Standard/compression structures and query](https://github.com/microsoft/win32metadata/blob/15fc870a5e96df7c560877ff30136c0fb5a21e9b/generation/WinSDK/RecompiledIdlHeaders/um/WinBase.h)
- [Volume feature bits](https://github.com/microsoft/win32metadata/blob/15fc870a5e96df7c560877ff30136c0fb5a21e9b/generation/WinSDK/RecompiledIdlHeaders/um/winnt.h)

The actual hosted SDK compiles these calls and types. This does not select a
new product SDK/Windows release or qualify Windows 11 client behavior.

## Existing presentation is not physical compression accounting

The current read-only adapter sets each AllocationSize to its logical length
rounded to the configured 4096-byte allocation unit. It does not persist an
allocation reservation or measure compressed physical file usage.
The native read-only volume's total is its backing image length, with zero
advertised writable free space. These are current presentation contracts, not
the final writable volume's allocation/free-space implementation.

Keep the following facts separate:

- Logical file length includes sparse holes; it can exceed four GiB here.
- Native live decoded bytes include namespace metadata and padded extents.
- Stored payload bytes exclude record headers, sealing/padding, roots/manifests
  and retained history. A small payload count alone is not a physical ratio.
- Committed end and full image length describe the native container's byte
  layout/history, not allocated host clusters or physical-device usage.
- Backing FILE_STANDARD_INFO allocation is the host filesystem's report for
  the ordinary image file. It is separate from mounted per-file allocation and
  does not prove underlying VM/device allocation or per-inode attribution.

This intentionally sparse, compressible fixture is not a representative codec
benchmark. Do not report its sparse logical/image ratio as compression gain.

## Live read-only observations

The probe samples sample, empty and >4 GiB sparse files during normal mounting,
before deliberate helper termination and after independent recovery/remount.
Every file is independently resolved through the verified CPU native image.

Required observations are FileStandardInfo EOF/allocation/link count, volume
identity/name/flags, allocation geometry and free/total bytes. Standard allocation
must match the current rounded-logical presentation. The backing image's own
standard allocation is queried separately. Read-only/unsupported-compression
volume bits are reconciled against the current Microsoft definitions and
selected driver/adapter contract.

Secondary observations preserve the BOOL/error and meaningful output of
GetFileInformationByHandleEx(FileCompressionInfo), plus the raw low/high/
LastError triplet from GetCompressedFileSizeW. Missing compression information
must never become a successful zero-byte compression result. LastError after
a non-sentinel successful return need not be meaningful or phase-stable;
potentially undefined high-word failure output is retained without forcing
stability. The artifacts retain all phases. Direct raw NT queries and Explorer
UI behavior are not covered by this probe.

The new report does not require optional compression queries to succeed.
It requires their results/errors to be present and coherent. A report passing
reconciliation is evidence of these observations, not compression-reporting
support or full Windows compatibility.

## Ownership, source binding and reconciliation

Each child creates a NEW report inside its own pinned fixture. It is at most
8192 bytes, ASCII JSON, with exact source commit, native UUID/generation/
namespace sequence, volume facts and all three file records. No arbitrary
path, real user file or installed application is accepted. The supervisor
reads only the exact confirmed child's fixture; the recovery report belongs
to that same owned crash fixture. Reports are emitted as Windows space evidence
and retained as a commit-bound artifact.

verify_windows_space_reports.py is an explicitly hosted-Windows-only consumer.
Its pure validators reject duplicate JSON fields, wrong keys/types/commit/
UUID, missing/duplicate phases or files, changed native state, contradictory
EOF/geometry/volume facts, out-of-range values and fabricated failed-query
outputs. All three phases and complete records are mandatory. Raw optional
API observations stay distinct from supported-feature claims.

The regression suite uses clearly synthetic dictionaries, not fabricated live
Windows measurements. It checks incomplete/mixed/duplicated reports, identity/
size/query/type failures, raw error preservation and valid successful-query
outputs. Actual reports still require executed exact-commit inspection.

## Unchanged safety limits and build qualification

The existing 128 MiB process/job, 20% CPU, one-process, below-normal priority,
30-second normal/recovery and 15-second forced-exit limits stay unchanged.
No GPU initializes and no driver/security/boot policy changes are introduced.
Three reports add at most 24 KiB. The aggregate bound now explicitly includes
all three retained image copies (normal, crash and crash baseline), both inputs,
all reports and marker/sentinel allowance under four MiB of fixture file lengths.
Actual host cluster allocation is reported rather than assumed equal to length.
The existing free-space reserve and create-only fixture ownership remain.

The gated mount build now targets only flitzfs_mount_probe and its real library/
codec dependencies. It launches only that executable and its own children.
The prerequisite ordinary/native jobs still build and test ALL targets at the
same commit; no test is removed or accepted on earlier source.

A passing hosted Server probe does not qualify latest Windows 11 client,
writable allocation/security/FileInfo, application compatibility, power cuts,
live AMD benefit, production scaling/reclaim, payload replication or autonomous
learning. No owner-PC operation is authorized. Results must drive the next
allocation/driver decision rather than be replaced by guessed size semantics.
