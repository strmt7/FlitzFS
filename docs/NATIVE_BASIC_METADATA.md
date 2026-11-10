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

## Executed checkpoint

At 324a15c7e1ed09da1e204c75332751ef1c896e8a, all eight triggered
hosted workflows succeeded. Native Windows job 129519108702 passed 45 CTest
entries; ASan/UBSan job 129519108869 passed 39. Both emitted 400 basic
invariants and the complete 1,038 basic fault cases: raw/automatic upgrade and
update each covered 57 I/O steps times three failure modes; compound
data/rename/creation/basic transactions each covered 59 steps times three.

The combined-maximum fixture admitted all 256 inodes, 255 nonzero extents,
100-byte non-root names and stamped tuples, with exactly 52,172 serialized
namespace bytes under the unchanged limits. Windows passed the actual
ordinary-file basic API oracle, native projection/image parity and SDK callback
tests, including preservation of present attributes without fabricated READONLY.
The unchanged suites retained 2,064 lifetime faults, 1,026 owning-context faults
and 271 Flush checks. Native worker handle totals remained 94 to 94.

Hosted mount job 129522214230 queried persisted attributes and all four times
through GetFileInformationByHandleEx both normally and after deliberate helper
termination/remount. Source bytes remained identical. Normal/recovery exits
were zero; intentional timeout/kill confirmed exit. Observed process peaks
were 6,090,752, 6,164,480 and 5,206,016 bytes, within the existing 128 MiB bound.
These were read-only Windows Server integration results, not writable or
Windows 11 client qualification or a kernel resource bound.

The existing Windows process campaign completed all 976 legacy-format cases
in two 488-case partitions (69.46/58.639 seconds). Its scope was not silently
changed into metadata-specific process-kill coverage. A dedicated format-3
writer interruption campaign remains required. Modeled basic failures and
read-only helper termination do not substitute for that campaign or physical
power-loss qualification. No owner-PC execution or live GPU occurred.

## Dedicated metadata writer process campaign

The separately scoped basic-store-interruption workflow now configures three
real writer phases: FLTXNS01-to-FLTXNS03 upgrade, existing-tuple replacement,
and compound file data/rename/creation/basic changes. The legacy 976-case
campaign remains unchanged. Each raw/automatic phase keeps its own complete
fresh baseline trace before selecting cuts; every write-before/after,
flush-before/after and acknowledgement event is included.

Two decoded-state closure digests bind the acknowledged baseline and complete
candidate. They cover ordered live object IDs, decoded lengths and complete
independently verified decoded bytes, including the namespace and file data.
The digest scratch is capped at 512 KiB; no owner-file or installed-app path
is read. Partial first-root and complete two-root publication of the SAME
candidate have the same decoded closure. Historical/orphan physical bytes are
retained and are intentionally not mistaken for current live state.

After confirmed TerminateProcess, a fresh CPU process verifies every expected
inode, name, data byte, present/absent tuple and namespace sequence. Its closure
must equal the appropriate full baseline/candidate digest. It then commits a
combined update to the unrelated keep file and its tuple, closes the writer
and independently reopens/validates the continuation. No GPU or mounted write
is involved. This is process interruption, not physical power loss.

Schema-3 reports additionally require the exact basic campaign/scope, all six
raw/automatic phase traces, ordered paired I/O, three barriers, matching
acknowledgement floors/digests, unchanged resource limits and a complete
nonoverlapping cut union. Missing, relabelled, mixed-state, duplicate, false
termination and incomplete reports fail. Existing schema-2 legacy reports and
fault cases remain supported.

Each campaign keeps the existing 1024 planned-case, 256-event/256-byte-line,
eight-event queue, 15-second/768-MiB/20% Windows child and 3-MiB combined-image
limits. Windows partitions retain 180 seconds each, 360 seconds maximum per
complete campaign, on separate disposable hosted VMs from the legacy workflow.
Peak resources are not multiplied within a test case. Configured tests still
require exact-commit job inspection before passing evidence is claimed.

## Executed metadata writer process campaign

At 6cc05e654b006bd0a13b2a0bdaefc695bfcbe63d, job 129528969148
completed all 712 real Windows metadata writer termination cases. The 356-case
partitions finished in 33.658 and 43.991 seconds, each below 180 seconds.
The strict schema-3 union accepted every required cut and state digest,
acknowledged-history recovery and independently reopened combined continuation.
The legacy Windows campaign also retained all 976 cases at that commit.

This closes the separately configured process-kill test at that exact source;
it does not prove physical power loss or writable mounted applications.
The overall CPU workflow at that commit had a separate raw portability
CreateProcess quota failure, so do not describe all its workflows as green.
HOST_INDEPENDENCE.md preserves that failure and the accounting-based
qualification-harness correction. No owner-PC or live GPU execution occurred.
