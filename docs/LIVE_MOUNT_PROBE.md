> Current scope: native filesystem backing images only. The former archive-backed
> probe has been removed; any archive-specific assertions below describe historical
> runs. Current tests retain native image reads, write rejection, supervision and
> independent CPU recovery/remount. Apply current host/backing rules in AGENTS.md.
> This ordinary-file fixture requires disposable storage and an installed
> runtime. Host tests use the memory-backed provider. Driver provisioning
> follows the artifact qualification in [AGENTS.md](../AGENTS.md).

# Disposable real-mount integration gate

## Plan and acceptance conditions

The prior session and Job Object work is qualified by CI run 42380771960 at
`41cff2eea5d77f5b9582cca16c4b3c192f1e6a7b`: all five jobs passed. This milestone
advances from injected lifecycle operations to actual Windows filesystem I/O.
The probe is implemented. The historical qualification is in the matching
`windows-mount` CI job; new qualification must execute the actual source-bound
mounted path with the required backing/runtime/resource contracts. Linux or
callback-only success does not prove that property, regardless of location.

The optional `FLITZFS_BUILD_MOUNT_PROBE` executable accepts **no source or mount
paths**. It is never registered with CTest. Execution requires explicit mode and `FLITZFS_ALLOW_DISPOSABLE_MOUNT=1`,
qualified disposable backing, actual Windows/runtime support and resource limits.
Hosting environment values are not permission checks. The opt-in prevents
accidental invocation; it is not a security boundary or proof of safe backing.

### Resource and data boundaries

The supervisor starts its own trusted executable in the already tested Job
Object: 128 MiB process/job committed-memory cap, 20% CPU hard cap, one process,
below-normal priority, 30-second wall deadline, and a further two seconds to
confirm termination. The child verifies its job limits before creating fixtures.
No GPU runtime is loaded. There are two dispatcher workers and at most two
concurrent application-reader threads, with four bounded reads each.

A fresh fixture parent is created exclusively under the trusted runner temp
path. Existing ancestors and the new parent are checked as ordinary directories
and held without delete sharing. Reparse points, relative/UNC/device paths and
arbitrary user-supplied paths are rejected. A drive letter is never assigned.
The WinFsp directory leaf is created by its native create-only operation.

Fixture data consists of one 192 KiB + 17 byte deterministic file, one empty
file, their containers, and a two-byte collision sentinel. The probe confirms
actual Zstandard chunks, keeps the original inputs, and does not delete any
fixture files. The hosted VM disposes of them. Normal teardown removes only
its own WinFsp mount. A deliberately occupied directory and sentinel must remain
untouched. At least 1 GiB plus 4 MiB free storage is required before fixture work.

### Indexed writable memory fixture

The separate `--run-disposable-indexed-memory` invocation requires explicit
disposable mount and write admission. It uses the same owned Windows memory
volume and linked WinFsp provider as the application path. The external client
creates a directory and synthetic file, writes at a 32 MiB offset, flushes,
renames, deletes another fixture file, enumerates entries and verifies sparse
and populated reads. The owner joins the dispatcher, checks mount removal and
verifies the retained image through the independent CPU and sealed readers.
The storage image uses the shared locked-memory backing throughout. The
optional fixture runs under the existing disposable process and driver scope.
Run 44217384564 passed this profile on Windows Server 2025 with WinFsp
v2.2B5 at source `4523ce5`. [Validation scope](evidence/indexed-owned-windows-2026-11-18.json)

### Live assertions

1. Refuse an existing mount directory without changing its sentinel.
2. Start the real driver-backed read-only session.
3. Verify case-insensitive open, full reads, empty file and EOF behavior.
4. Verify a random read crossing the 128 KiB compression boundary.
5. Create a read-only file mapping, close the original file handle, then compare
   every mapped byte; do not confuse this with direct callback testing.
6. Enumerate root/nested directories and confirm Windows wildcard filtering.
7. Reject write access, creation, deletion, rename and directory creation.
8. Perform bounded concurrent application reads with exact byte parity.
9. Stop/join the dispatcher, repeat stop safely, and check mount removal.
10. Recheck original inputs, compressed backing bytes and the collision sentinel.

Failures return stage/detail information to the supervising process. A deadline
or unconfirmed exit is a failure, not a pass. Job limits do not constrain driver
or kernel memory and cannot guarantee recovery from a kernel fault. Real mounted
crash/cancellation recovery, Windows 11, cross-user ACLs, broad application
compatibility and long-term reliability remain independent gates.

## Explicit provisioning boundary

`prepare_winfsp_sdk.ps1` is extraction-only. The disposable mount jobs ask it
to stage the exact AMD64 driver from the same MSI File and Media tables as
the SDK. Its receipt binds the driver path, byte size and SHA-256 to the
verified MSI and current commit. Ordinary SDK staging selects four user-mode
files. A separate installer rechecks the receipt, MSI digest and signature
before use. The mount jobs follow Windows native checks. The portable and
sanitizer lanes provide independent qualification evidence. The installer
rejects an existing WinFsp installation and requires administrator context.
The official installer uses quiet/no-restart options and a 90-second wait.
A timeout or restart-required result fails the job; there is no reboot/retry.
Only this disposable VM receives driver/service installation. Never run this
provisioner on the owner's PC or on a self-hosted runner.

## Research rounds (7 November 2026)

- Round 1: WinFsp's native directory mount contract and v2.1 `mount.c`/`util.c`
  show directory leaves use `FILE_CREATE`, not open-existing. Retain that
  create-only boundary and the separate collision regression.
- Round 2: Microsoft directory-handle sharing and reparse-point documentation
  motivate holding validated ancestors. This is scoped to trusted isolated
  fixtures, not advertised as a sandbox for adversarial Windows namespaces.
- Round 3: Windows mapping lifetime documentation requires keeping the mapping
  while closing the original file handle. Test that real paging path directly.
  WinFsp documents that the kernel always filters directory patterns; retain
  callback behavior and verify actual wildcard results through FindFirstFile.
- Round 4: Microsoft Installer documentation requires explicit no-restart with
  quiet installation. Preserve the checksum pin and keep installation out of
  default builds, CTest and the ordinary native-callback lane.

Primary sources:
- https://winfsp.dev/doc/WinFsp-API-winfsp.h/
- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/mount.c
- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/util.c
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga
- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile
- https://learn.microsoft.com/en-us/windows/win32/msi/standard-installer-command-line-options

The `.flfs` v1 wire format is unchanged. Encryption, HIP, a general-purpose mount
application, writable transactions and sole-copy storage safety are not provided.


## Current source-bound driver candidate

SDK extraction and this VM-only installer now share tools/winfsp-sdk-lock.json.
The development candidate is WinFsp v2.2B5, explicitly prerelease, with current
upstream security/deadlock fixes; historical v2.1 results do not qualify it.
Require fresh callback, mounted read/mapping/write-rejection, shutdown and
crash/remount evidence. Current Windows 11 driver-policy/signing acceptance is a
separate client-VM gate even if a Windows Server runner passes. Never apply
policy-removal, Secure Boot/HVCI changes or test-signing workarounds to the owner.


## Native transactional image probe

The current probe uses a NEW native transactional image with verified directory/
data creation and a committed rename/removal. It closes the writer before the
read-only source opens, requires the exact UUID and actual compressed payloads,
and exposes persisted inode IDs. It no longer constructs a collection manifest
as the mounted backing source. Original inputs, the complete image, crash
baseline and collision sentinel remain owned synthetic VM fixtures.

New checks exercise 64 KiB extent crossings and mounted sparse offsets beyond
4 GiB. No large file is materialized; the complete on-disk fixture remains below
4 MiB. The no-driver native projection tests separately check the core's 1 TiB
logical sparse bound. Forced termination and independent CPU native
namespace/identity/content verification precede same-path remount and mapped/
sparse parity. Existing process/job/dispatcher/cancellation budgets are unchanged.
This still tests mounted READS and write rejection, not mounted durability.

## Source-bound Windows space observations

WINDOWS_SPACE_REPORTING.md adds normal/crash/recovery standard/compression/
volume API observations, separate backing-image allocation and three complete
source-bound reports. Optional API errors remain explicit. Required presentation
facts and phase identity/record closure are strictly reconciled; configured
checks need exact executed evidence. No compression toggle, allocation mutation
or raw volume handle is added. All retained source/baseline/input/report file
lengths now share an explicit <4 MiB aggregate bound. Existing job/time/CPU/
memory/driver safety boundaries remain. The mount-only build targets its probe;
ordinary/native prerequisite jobs still build/test all targets.


## Separate writable trial

The old read-only normal/kill/recovery probe and its byte/process/oracle limits
are unchanged. WRITABLE_MOUNT_TRIAL.md specifies a separate OFF-by-default target
and gated disposable-VM job. Do not relabel read-only kill/recovery as writable
crash coverage or transfer either driver experiment onto the owner's PC.
