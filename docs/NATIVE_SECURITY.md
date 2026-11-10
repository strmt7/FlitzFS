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
