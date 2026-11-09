# Persistent Windows basic metadata

Experimental CPU persistence and read-only Windows projection. Writable
callbacks, helper application, automatic Windows timestamp policy, security
descriptors, real allocation accounting and Windows 11 client qualification
remain unfinished. No owner-PC operation or live GPU route is added.

## Source and representation

The current selected WinFsp v2.2B5 source is
14d6e0280ae15e2550c44d6dbc476be51649727f. Its callback header,
src/sys/fileinfo.c, src/dll/fsop.c and current native tests were inspected
through the native repository API. These are the selected 2026 package's
actual contracts, distinct from older Microsoft documents found by the
publisher-date audit in WINDOWS_NATIVE_METADATA.md.

The FSD transforms incoming basic-information requests before invoking
SetBasicInfo: zero attributes become INVALID_FILE_ATTRIBUTES; otherwise it
strips NORMAL/DIRECTORY and restores the actual directory bit. It strips
-1 times to zero; the callback uses zero times as preserve requests. The
persistence library accepts an explicit canonical replacement tuple, not
that request representation. A future adapter must resolve preservation,
per-open update suppression, -2 behavior and authorization correctly before
passing values. Do not persist request sentinels as timestamps.

NativeBasicMetadata holds four explicit 100-ns UTC values and canonical
attributes. Timestamps range from zero through INT64_MAX. Zero is a genuine
stored value here, not an implicit preserve operation or "unknown". Values
are not ordered against each other or forced monotonic: an authorized caller
may intentionally set older times. Existing created/modified sequence fields
remain logical transaction counters and are never interpreted as UTC.

Supported attributes are READONLY, HIDDEN, SYSTEM, DIRECTORY, ARCHIVE,
TEMPORARY and NOT_CONTENT_INDEXED (mask 0x2137). DIRECTORY must match the actual
inode kind; TEMPORARY is forbidden on directories. NORMAL is represented by
zero on regular files. Unsupported bits fail, including claims of NTFS
compression, encryption, reparse or sparse-control capabilities. This bounded
initial subset is not the final Windows compatibility scope.

Absent metadata is explicit through optional<NativeBasicMetadata>. Legacy and
unstamped inodes never acquire invented original times. An explicit tuple of
all zero times differs from absence. Read-only callbacks preserve present
attributes/times verbatim; the read-only volume/callback boundary independently
denies mutation. It does not OR a fabricated READONLY attribute into present
tuples. For absent tuples, the historical adapter's synthetic read-only/time
behavior remains explicit and unchanged.

[Selected header](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h),
[FSD transformation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c),
[user-mode dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c).

## Atomic mutations and ownership

apply_with_basic consumes file operations followed by full metadata
replacements. At most sixteen COMBINED operations/replacements are admitted.
Each replacement selects either a stable live inode or a 1-based inode
creation ordinal from this same batch; selecting neither/both or a missing/
removed creation fails. Rename keeps the inode. A final replacement can target
a retained detached inode. This API does not manufacture a surrogate path.

All file changes and replacements use one privately staged model, one namespace
object and the existing replicated three-barrier publication. No metadata-only
side write can acknowledge a different state from file data. Existing put,
input, dirty-extent, object, history and decoded-memory limits stay unchanged.
A changed tuple advances the inode's modified sequence and one namespace
sequence; setting an identical tuple alone is a no-op. An old snapshot retains
its original tuple/data. Compaction rebinds only volume identity and preserves
present/absent tuples.

Generic file operations preserve basic fields; there is no hidden library
clock, read-triggered timestamp mutation or automatic ARCHIVE policy. The
future Windows write/create/rename/cleanup adapter must supply authorized
automatic updates in the SAME transaction. Two separate calls are not an atomic
Windows data-plus-metadata update. This prerequisite alone is not completed
Windows timestamp behavior.

The owning session supports explicit stamped creation and per-context tuple
queries/updates. A separate NativeBasicAccess grant is supplied at open; the
historical open overload grants no basic-metadata write access. Data-write
access does not imply attribute-write access, and attribute-write access does
not imply data-write access. These grants are already-authorized inputs from
the future OS adapter, not an ACL implementation. Context identity/lifetime,
Cleanup retention, final Close retirement and failure fencing still apply.
A failed update retains the context and cannot retry backing I/O.

## Namespace capability FLTXNS03

The first actual tuple replacement selects magic FLTXNS03 with format field 3.
The choice is sticky across lifecycle changes, inode removal and compaction.
Ordinary FLTXNS01/02 bytes remain unchanged. Format 3 requires replicated
current checkpoints. An old reader rejects its distinct magic/format rather
than silently skipping metadata.

The 64-byte namespace header and 16-byte extent entries stay unchanged.
Under format 3 each inode header is 88 bytes; formats 1/2 retain 64 bytes.
Existing inode offsets 0..51 retain their meanings. Flags at offset 20 add
bit 2/value 4 for a present basic tuple; lifecycle bits 0/1 remain mutually
exclusive. A root may have basic metadata, but never lifecycle flags.

| Inode offset | Width | Field |
|---:|---:|---|
| 52 | 4 | Canonical attributes |
| 56 | 8 | Creation time |
| 64 | 8 | Last access time |
| 72 | 8 | Last write time |
| 80 | 8 | Change time |

When presence is clear, ALL bytes at offset 52 through the selected header end
must be zero. A presence bit under formats 1/2, unsupported flags/magic/format,
invalid tuple, mismatched node kind, truncation, bad graph/count/reference or
nonreplicated publication fails full admission. Public snapshots/open/resume
still verify every live object. Writer queries compare namespace bytes with
the last admitted model before returning tuples.

The conservative static metadata ceiling is 52,272 bytes for 256 possible
100-byte names and all 255 extents, below 64 KiB. The legal combined maximum,
with the root's required empty name, is 52,172 bytes. Live decoded data plus
that namespace remains below the unchanged 16 MiB aggregate limit. This is no
increase in file/inode/extent or transaction capacity. ACL/security/allocation
storage needs a separate reasoned capacity/compatibility design.

## Configured verification

Four bounded CTest groups retain raw and automatic-codec paths: invariants,
upgrade failures, existing-tuple update failures and compound data/rename/
creation/metadata failures. The latter groups discover every write/barrier
step and inject each existing failure mode. Recovery must expose a complete
old or new transaction, preserve every earlier acknowledgment, retain old
snapshots, fence failed writers, perform no retries and continue through the
independent CPU reader/writer. No modeled error may justify older-data loss.

Digest-valid malformed namespace tests cover capability/presence/reserved
bytes, unsupported attributes, timestamp sentinels, graph kind, truncated
headers and publication downgrade. Either current root/manifest unit loss
must preserve and repair the latest tuple. Capacity coverage combines all
256 inodes, 255 nonzero extents, maximum names and stamped tuples. Context
tests separate access grants and retain ownership through failure.

Hosted Windows projection/SDK tests check persisted tuples and absence,
unaltered legacy images, attributes without an injected READONLY bit, denial
of writes on the read-only volume, full image parity and actual ordinary-file
Flush. A fresh exclusive ordinary-file oracle exercises Windows
SetFileInformationByHandle/GetFileInformationByHandleEx and zero-preserve
behavior, with SDK constants checked at compile time. The separate mounted
probe queries all four persisted times/attributes through the real Windows
API, then retains the existing crash/remount/parity/resource tests.

Configured tests are not passing evidence. Inspect exact hosted jobs before
claiming this behavior validated. Hosted Windows Server does not substitute
for the latest Windows 11 client, and no process test proves physical
power-cut durability or immunity to every future failure.
