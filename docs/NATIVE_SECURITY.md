# Persistent Windows security descriptors (experimental)

This adds bounded descriptor persistence and read-only descriptor callbacks to
the Windows filesystem engine. It does not enable writable mounts, implement an
ACL evaluator or qualify Windows 11 client access checks. No owner-PC execution,
driver, service, GPU dispatch or dependency installation is authorized.

## Current primary source

The representation was checked through native repository API captures on
10 November 2026 against Microsoft's current published winnt.h change
c16762d41f1bd641ff8c4389ebf16ee50747129f (19 October 2026), and the selected
WinFsp v2.2B5 source 14d6e0280ae15e2550c44d6dbc476be51649727f
(published 13 October 2026):

- [Microsoft descriptor, SID, ACL and ACE layouts](https://github.com/microsoft/win32metadata/blob/c16762d41f1bd641ff8c4389ebf16ee50747129f/generation/WinSDK/RecompiledIdlHeaders/um/winnt.h)
- [WinFsp security callback contracts](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [Selected security helper implementation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/security.c)
- [Reference descriptor replacement](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)

Microsoft explicitly describes descriptors as opaque. Preserve the complete
self-relative bytes, ACE order, controls, owner/group and ACL contents. Do not
translate these into a home-grown permissions model. The selected helper uses
SetPrivateObjectSecurity, copies its input onto the proper process heap and
requires its matching FspDeleteSecurityDescriptor deallocator. New SDK tests
exercise that existing mechanism rather than inventing a patch/merge algorithm.
The selected driver remains a prerelease development candidate, not client or
production-driver qualification.

## Trust and validation

NativeSecurityDescriptor is an opaque byte vector with a 16,384-byte per-inode
cap. native_security_descriptor_framed performs allocation-free bounds/framing
checks only: revision/self-relative control, supported framing bits, owner/group
SID offsets/revision/count, ACL offsets/presence/revisions/lengths and bounded
ACE-header walks. Offsets are checked before reads; there are no pointer casts
or OS calls in this CPU path. ACL spare capacity and ACE payloads remain opaque.

Framing is deliberately NOT full Windows descriptor/ACE/claim validity,
authorization, inheritance, identity authentication or an access-control
decision. Trusted setters require a fully selected descriptor from the future
authorized Windows operation. Before callback exposure, the Windows adapter
additionally requires IsValidSecurityDescriptor. The SDK test validates actual
OS-produced simple/inherited/protected/audit/mandatory-label descriptors and the
selected helper's DACL merge. It also checks ABI offsets and distinguishes
absent, NULL and empty DACLs. None of this proves a live mounted access-denial
matrix, per-part authorization or privilege handling; those remain required.

## Atomic operations and context ownership

apply_with_metadata accepts file, full basic-metadata and full security
replacements with at most SIXTEEN operations TOTAL. File mutations run first,
then basic replacements and security replacements. Select either a stable inode
or a 1-based creation ordinal from this same batch. No separate unprotected
creation or partial data/basic/security state is published. The existing private
Stage, CPU verification and replicated THREE-barrier commit remain the only
publication path. Invalid framing/selectors/counts reject before backing reads
or writes; missing staged selectors or aggregate size rejection never write.

Existing apply/apply_with_basic signatures/layouts and default legacy bytes
are preserved. Generic writes, metadata effects, rename, deletion intent,
detachment, retirement and new-identity compaction preserve descriptor bytes.
Old snapshots retain their descriptors. Identical descriptor replacement is a
verified no-op. Failed storage publication fences; no accepted result/retry
escapes. Recovery and continuation use the independent CPU path.

NativeFileSession adds explicit already-authorized descriptor read and FULL
replacement grants. Data read/write, delete and basic-attribute grants do not
imply either security grant. A full replacement grant is not synonymous with
WRITE_DAC: owner/SACL/privilege/SECURITY_INFORMATION checks remain the future
Windows adapter's responsibility. No new API exposes the encapsulated writer.
Tokens, late contexts, Cleanup/Close, detached inode retention and failure
fencing remain unchanged. A trusted creation overload combines file/basic/
security in one transaction. The core invents neither inheritance nor defaults.

## Namespace capability FLTXNS04

This is a native namespace capability, separate from object root format 3,
codecs and product/release numbering. The first effective security replacement
upgrades to magic FLTXNS04 / format field 4. The capability is sticky, including
after all security-bearing children are retired. Legacy FLTXNS01/02/03 readers,
bytes and absence remain unchanged when security is unused. Old readers reject
the new capability; no silent downgrade removes descriptors.

The existing 64-byte namespace header and 88-byte basic fields retain meaning.
Format-4 inode headers are 96 bytes:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0..87 | 88 | Existing fields, with flag 0x08 indicating descriptor presence |
| 88 | 4 | Descriptor byte count, zero exactly when absent |
| 92 | 4 | Reserved zero |

After each header come the existing name bytes, then descriptor bytes when
present, then the existing 16-byte extent references. Basic presence remains
independent; absent basic fields 52..87 stay zero. Descriptor flag/length,
bounded framing, reserved bytes, graph/identity and complete catalog closure
are all checked before admission. Replicated current checkpoints are required.
This permits absent, NULL-DACL and empty-DACL descriptors as distinct states;
an empty vector is never an instruction to discard security.

## Resource and read-only integration

The namespace cap remains 65,536 bytes, including ALL headers, names, descriptors
and extent references. A descriptor may be at most 16 KiB, but all 256 inodes
cannot each consume that maximum. Aggregate capacity rejection occurs before
copying an oversized staged replacement and before backing writes. No inode,
extent, dirty-buffer, input, decoded-object, history or process/time cap grows.
Descriptors reside inside the existing namespace object and its compression/
integrity/publication path; there is no extra object per inode or GPU dependency.

ReadOnlyProjection returns present persisted bytes by stable inode.
WinFspReadOnly privately validates and caches descriptors while constructing its
immutable node table. GetSecurityByName and GetSecurity return the selected
node's descriptor, with bounded exact sizing and no partial short-buffer copy.
Legacy/unstamped nodes retain the pre-existing owner-only read-only policy;
present descriptors are never replaced with that default. SetSecurity and all
other write routes remain write-protected. OS/FSD access checking is separate
from directly calling a user-mode callback in an SDK test.

## Qualification gates

The four new CPU groups cover framing/truncation, legacy upgrade, exact
persistence/absence, atomic creation/basic/data/rename, no-op behavior, sticky
capability, malformed digest-valid namespace bytes, aggregate capacity, owning
grants/context lifetime, retained snapshots and source-preserving compaction.
Three fault groups enumerate every observed write/barrier under all three
existing persistence modes, raw/automatic and fixed/compact layouts, then
independently reopen the complete old/new state and continue CPU writes.

The Windows SDK group additionally creates a NEW ordinary image only in a
fresh owned hosted fixture, closes the writer, independently projects it, checks
root/file descriptor bytes through both actual callbacks, short buffers/stale
contexts/write rejection and unchanged source. Its file test is enabled only by
the existing GITHUB_ACTIONS=true / RUNNER_ENVIRONMENT=github-hosted gate. No driver/mount is started by it.

Configured tests require inspected exact-commit results. Existing 712/976/172
writer-termination campaigns keep their original scopes and cuts; they are not
silently relabelled as NS04 process-kill coverage. Dedicated NS04 real writer
termination and mounted ACL/access behavior remain required. Physical power,
Windows 11 client, writable allocation/FileInfo/helper, live AMD, capacity/reclaim,
redundant payload repair and low-priority autonomous learning remain open.

## Initial hosted invariant failure

At 5a8b43a35a149e4079411133cc719941762535c7 raw analysis job
129619894163 compiled the implementation and passed all three new fault
groups, but the new invariant fixture attempted compaction while retaining
its source writer. The existing object-store lease correctly rejected that
call before creating destination bytes. The initial invariant/portable run
is failed evidence, not a passing qualification.

The corrected fixture explicitly checks that live-writer rejection preserves
both source and empty destination, releases the source writer, then verifies
the permitted quiescent copy, exact descriptors and independent CPU reopen.
No production compaction ownership check is weakened. The portable workflow
also includes tests/** in its trigger because its qualification executes those
same tests; test-only corrections must produce a fresh qualification result.

## Executed corrected component checkpoint

At eedeb87db9656235389b00cf9b2dbb0dbf1471c4 Windows native job
129622022864 passed all 70 entries, including 67 actual Windows API/SDK
security checks, 192 native security invariants and 1,266 new modeled
publication failures. All twelve phase/encoding/layout traces were retained:
upgrade and replacement each used 414 failures, compound used 438.
Each observed write/barrier ran under all three existing persistence modes.
Worker handles stayed 94 before and after.

Windows raw job 129622023128 passed 66 entries with the same fault inventory.
Windows static/relocated job 129622022767 passed all 66, and the separate
analysis static/relocated job 129622023069 passed. Compression reference job
129622022220 passed. The corrected test explicitly retained live-writer
compaction rejection before verifying quiescent source-preserving copy and
independent reopen. The initial failed checkpoint remains documented above.

## Selected WinFsp transport bound

The native on-disk/CPU descriptor ceiling remains 16,384 bytes for compatibility.
The selected SDK defines a 16-KiB response INCLUDING its fixed header.
Its actual FSP_FSCTL_TRANSACT_RSP_BUFFER_SIZEMAX is therefore smaller.
FspFileSystemOpQuerySecurity supplies exactly that payload capacity and maps
a callback BUFFER_OVERFLOW to INVALID_SECURITY_DESCR. A direct callback test
using a larger vector would miss this mismatch.

Read-only adapter construction now rejects a present descriptor beyond the
selected SDK's actual response payload capacity BEFORE exposing callbacks.
This bound comes from the selected header, not a guessed constant or host
installation path. Native CPU admission/recovery and stored bytes remain
unchanged; no descriptor is truncated, defaulted or silently discarded.

The Windows SDK regression uses an OS-valid descriptor with retained opaque
padding to the largest four-byte-aligned admitted payload, then calls the REAL
FspFileSystemOpQuerySecurity dispatcher with its exact bounded response buffer.
It checks status, length and every returned byte. A separate NEW owned image
holds a descriptor up to the larger native limit: CPU readback must remain exact,
while adapter admission must fail with limit and preserve all source bytes.
No driver/mount starts in either case.

Sources: [selected protocol sizes](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/fsctl.h)
and [actual query dispatcher](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c).
The native storage cap does not establish transport, Windows request/creation
or full NTFS security-descriptor size compatibility. Future writable admission
must account for its actual request/name/response capacities too.

## Retained sanitizer timing failure

At eedeb87db9656235389b00cf9b2dbb0dbf1471c4 attempt-1 sanitizer
job 129622023097 passed 61 of 62 entries, including all four new security groups,
but the pre-existing bounded_native_filesystem hit its unchanged 45-second
deadline (45.07 seconds). The identical production revision at 5a8b43a had passed
that test in 43.98 seconds; the preceding qualified 1b81473 used 43.15 seconds.
This suggests timing variability but does not prove its cause. No sanitizer
memory/undefined-behavior diagnostic was reported by the failed attempt.

The failed attempt and its skipped dependent mount remain evidence. One
targeted retry of that failed sanitizer job was requested at the SAME eedeb87
commit, preserving all cases and limits. A retry is not proof that the timing
margin or its cause is resolved; inspect its exact outcome separately.

The unchanged targeted retry at eedeb87 completed successfully in job
129626620835 (attempt 2): all 62 entries passed, including the same 1,266
security failures/192 invariants. bounded_native_filesystem took 43.52 seconds;
the complete sanitizer test step took 440.21 seconds. No timeout was raised,
no case was removed and no production change was made for that retry.
The earlier timeout remains relevant evidence of a narrow timing margin.

The first transport regression build at
3e2b21e5c1e4a6332ec6cebc80ee87a0c619e5b4 compiled the production
adapter but failed to compile its new test in job 129630536134.
An aggregate-initialized FSP_FSCTL_TRANSACT_REQ contains the SDK's flexible
Buffer[] member, which MSVC rejected with C2466. The corrected fixture explicitly zeros the trivially copyable fixed request
header, as the existing Cleanup dispatcher test does, and keeps aligned bounded
response storage. It does not aggregate-initialize the flexible-buffer struct.
The real dispatcher, capacity-boundary/oversize cases and resource caps remain.

## Executed transport correction

At 0a9ba35fa6eb8f50095df1539776454efad59fb2 Windows native job
129632818210 passed all 70 entries. The 73 actual Windows/SDK checks
reported a 16,384-byte native descriptor cap, 16,232-byte request payload
and 16,256-byte response payload. The maximum admitted response passed the
real DLL dispatcher with exact bytes; the larger native descriptor remained
CPU-readable while adapter construction rejected it without source changes.

The same run retained all 192 native security invariants and all 1,266 modeled
failure cases. Worker handles stayed 94/94. These are exact-commit executed
VM/SDK results, not mounted access-policy, writable/client or live-AMD proof.

The same 0a9ba35 commit's sanitizer job 129632818277 passed all 62 entries
on its first attempt, including the same 192 invariants and 1,266 failures.
The existing bounded_native_filesystem took 31.30 seconds under its unchanged
45-second deadline; the full step took 322.30 seconds. Earlier failed and
retried attempts remain documented separately, without inferring their cause.

All four workflows triggered by 0a9ba35 succeeded. Mounted job 129635298668
passed the existing default-layout read-only path, deliberate confirmed helper
kill, unchanged source, independent CPU recovery and same-path remount.
Normal/recovery exits were zero; deliberate killed exit was 1460.
Peaks were 6,184,960 / 6,168,576 / 5,160,960 bytes. All three space reports
and strict reconciliation passed. This fixture still used the default image
(786,432 bytes / 6,416 live stored payload bytes), without persisted security,
so it is regression integration evidence and NOT mounted NS04/ACL qualification.

## Hosted writable security-update candidate

The opt-in hosted adapter now routes SetSecurity through the selected WinFsp
FspSetSecurityDescriptor helper, and frees every result through its matching
FspDeleteSecurityDescriptor. It captures original Create/Open granted access
and separately requires WRITE_DAC for DACL updates, WRITE_OWNER for owner/group,
and ACCESS_SYSTEM_SECURITY for SACL updates. Protection flags require the
matching ACL part; contradictory, empty and unsupported masks reject. No grant
is inferred from data/basic permissions. Modification and merged descriptors
retain the existing Windows validity, self-relative and transport/16-KiB caps.

A trusted session merge-input query requires an ACTIVE full replacement grant;
it does not authorize public descriptor reads. The adapter never exposes that
grant as an unrestricted callback. Merging/validation precede the existing
atomic replicated publication; success performs no post-publication I/O or
allocation. Identical replacements retain verified no-op behavior. Failed
publication fences and retains the context; no automatic retry is introduced.
Stored formats, default routes, process/image caps and driver policy are unchanged.

New tests are configured for actual user-mode SDK SetSecurity dispatch in raw
and automatic modes, per-part denied rights, invalid masks/null input, exact SDK
merge parity, preserved data/basic/reservation/inode, independent retired-reader
persistence and bounded-history failure. These require inspected CI results;
configuration is not a pass. Core fault campaigns remain mandatory. SDK tests
are trusted-transport tests, NOT live kernel authorization, caller privilege,
SACL disclosure, inheritance propagation, Windows11 client or power-loss proof.
No new mounted profile or owner-PC/GPU permission follows. General labels/claims/
resource-attribute security masks remain explicitly unsupported.
