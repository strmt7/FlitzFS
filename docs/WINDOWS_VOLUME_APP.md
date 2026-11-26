# Experimental Windows volume application

The helper supports the temporary editor and an experimental Explorer
attachment. Temporary volumes use the adaptive memory factory. The Explorer
variant uses the shared WinFsp controller, CPU recovery, and installed AMD
acquisition when a qualified package admits the work.
[Memory factory](WINDOWS_MEMORY_VOLUME_FACTORY.md) ·
[Windows background maintenance](WINDOWS_BACKGROUND_MAINTENANCE.md)

## Private development bundle

Configure the helper with `FLITZFS_PORTABLE_CPU=ON` and use matching
static-runtime LZ4 and Zstandard libraries if those codecs are enabled.
With Python available to CMake, build the packaging target from that
configuration.

```powershell
cmake --build build --config Release --target flitzfs_stage_volume_app
```

The target creates a new `flitzfs-volume-*` directory in the build
tree. It verifies the helper's AMD64 PE imports before staging. An
Explorer build includes the exact WinFsp DLL bound to the executable
and the approved AMD files, when configured, in the expected `amd`
layout. The completed directory contains `bundle.json` with import
and file digests. Each run creates a separate directory, so another
application's installation or package can stay under its original
ownership.

Run `flitzfs_volume_app.exe --check-installation` for an unattended
loader check. The Explorer executable checks the packaged WinFsp
identity and resolves its imports. The command exits with status 0
on success and 5 if the runtime check fails. A normal Attach still
goes through the volume controller and requires a matching registered
WinFsp driver.

The staged development bundle supplies the required user-mode helper
dependency privately. Signed deployment, dependency notices, kernel
compatibility and transactional upgrade qualification remain release
work.
[MSVC runtime selection](https://cmake.org/cmake/help/v4.5/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html) ·
[WinFsp stable SDK](https://github.com/winfsp/winfsp/releases/tag/v2.2) ·
[Relocated bundle evidence](evidence/windows-private-volume-bundle-2026-11-26.json)

## Explorer attachment

Configure `FLITZFS_BUILD_EXPLORER_APP=ON` and provide the reviewed
`FLITZFS_WINFSP_SDK` directory when building the attachment variant. This
build stages the matching user-mode DLL beside the executable. Attach
checks its build-bound size and SHA-256, then looks up WinFsp through
its Windows installation registration. It loads the matching DLL from
WinFsp's resolved side-by-side deployment and resolves its dependencies
from System32. The runtime's deployment identity and the registered
driver's version must agree before Attach enters WinFsp preflight.
A missing, altered or incompatible runtime produces an inline status
message while the temporary editor stays available.
[Microsoft delay-load API](https://learn.microsoft.com/en-us/cpp/build/reference/understanding-the-helper-function?view=msvc-170) ·
[WinFsp registration](https://github.com/winfsp/winfsp/blob/master/doc/WinFsp-Registry-Settings.md) ·
[WinFsp side-by-side service selection](https://github.com/winfsp/winfsp/blob/ada6de157df4fa99f5a866b6ac7af66918d6d838/src/dll/fsctl.c#L353-L430)

Use `flitzfs_volume_app.exe --check-mount-support` to check the same
runtime and driver prerequisites without starting the driver or
creating a mount. It returns 0 when they match and 8 when a prerequisite
fails. The supported registration for this build uses the exact SDK
DLL in a WinFsp side-by-side deployment. This check establishes the
installed pairing. Mount and recovery qualification still require
separate tests.
[Installed pairing evidence](evidence/windows-winfsp-side-by-side-binding-2026-11-26.json)

Attach opens a new indexed volume or reopens the retained volume. It chooses
a fresh child name in the Windows temporary directory and holds its local
ancestor directories through retirement. WinFsp uses exclusive creation for
the junction. Open in Explorer uses the current attachment path. Detach joins
the dispatcher and workers while keeping the data in RAM. Another Attach
verifies that retained volume before opening it at a new path. Discard is
available after confirmed detachment. Window and tray notifications follow
the native lifecycle events.
[WinFsp directory creation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/util.c) ·
[Windows temporary directory API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppath2w)

The SDK fixture uses injected lifecycle operations and verifies file bytes
after attach, detach and reattach. GUI tests exercise the actual executable
with absent and altered dependencies. Interactive File Explorer integration
requires separate qualification.

### Persistent volume files

The Explorer build with `FLITZFS_EXPLORER_FILE_BACKING=ON` exposes **New volume
file** and **Open volume file** in the window and tray. The build uses local
NTFS storage independently qualified for 64 KiB failure units, flushes and
the applicable allocation operation. The helper checks the pinned volume's
allocation geometry before selecting a backing. New files use packed allocation
when the complete layout fits. A qualified sparse file can fit when there is
space for both images and the catalog but the packed directory units would
exceed the file's limit.

The Windows picker runs on the UI thread. The selected path and the create
or open action move to the existing attachment worker. Before acquiring a
backing, the worker checks the WinFsp mount location. Creation atomically
requires a new file. Capacity fits the current available growth and file-size
limit, including both image ranges, the reserved catalog and any packed
allocation metadata. Reopen derives sparse or ordinary capabilities from the
exclusive handle and checks the persisted volume identity and layout before
writable recovery. The window shows the selected file and its mount location.
**Open in Explorer** opens that location.

The same build accepts `--create-volume-file` or `--open-volume-file` with
one absolute path at launch. Quote paths containing spaces. The helper
opens its window and submits the explicit action through the same worker
and backing checks used by the picker. Creation still requires an unused
path. These arguments can be passed from a shortcut or another application.
[Windows command-line parsing](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw)

Detach retains the exclusive handle. **Reattach volume file** uses that
handle, and **Close volume file** releases it after confirmed retirement.
Exit can close a detached file. The next attachment then uses an explicit
file choice or the memory provider.
[Windows picker options](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions) ·
[File attachment contract](PERSISTENT_VOLUME_FILE.md#windows-helper-ownership) ·
[File selection validation](evidence/helper-file-selection-2026-11-21.json)
[Current attachment evidence](evidence/windows-tray-attachment-2026-11-18.json)

A signed WinFsp 2.2 Windows guest passed packed startup creation, live-handle
rename, detach and fresh-process reopen through the actual helper. The
independent CPU reader verified the reopened bytes. The separate same-SDK
fixture checked exclusive creation and writable opening of an older sparse
volume. A second mounted trial terminated the owner after a flushed overwrite
and rename. It recovered the target inode and all 1,024 neighboring files,
then verified a new flushed write with the independent CPU reader.
[Packed attachment receipt](evidence/windows-packed-explorer-2026-11-23.json)

The separate `--gui-mounted-packed-file-sandbox` qualification exercises
two physical allocation groups. It requires the writable publication probe
build and an isolated guest. A held untouched WinFsp open survives automatic
image selection. Provisional deletes and overwrites still defer handoff.
The file reclaimer releases eligible storage on an admitted idle opportunity
while optional compression waits. The mounted trial crossed the group
boundary after 60 revisions. It checked both anchors, the held bytes, orderly
detach, independent CPU readback and a fresh writable mount with a continued
write. Full settlement across repeated churn remains a release gate.
[Mounted handoff and SDK evidence](evidence/windows-mounted-packed-handoff-2026-11-23.json) ·
[Mounted backing release](evidence/windows-mounted-packed-retirement-2026-11-23.json)
· [Remaining reclamation checks](RELEASE_EXECUTION_PLAN.md#next-integrated-qualification)

### Initial Windows 11 driver trial

The mounted GUI fixture drives the real app through Attach, external Win32
write and flush, window close, Detach, and reattachment. It checks that the new
junction exposes the same volume identity and original bytes. It then verifies
removal of both junctions and exits through the app's normal controls. The
`--gui-mounted-sandbox` mode requires an admitted disposable system session and
an exclusively scoped guest mount parent.

Windows Sandbox supplied a headless Windows 11 Enterprise guest with a
read-only input mapping and a dedicated evidence output. Authenticode
verification of the pinned 2.2B5 package succeeded. Windows Installer stalled
at package policy verification in the offline and networked trials, before the
app mounted. The physical host retains its installed 2.1 driver.
The host's editor regression and disposable-guest admission check passed.
[Initial driver evidence](evidence/windows11-sandbox-mounted-tray-2026-11-18.json) ·
[Windows Sandbox CLI](https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-cli)

Using WinFsp's development registration and app-local Microsoft runtime
DLLs, the follow-up guest accepted the exact signed driver and matched DLL.
The sealed and writable memory mounts passed external Win32 byte checks.
The mounted app retained a flushed file across window close, detach and
reattach, preserved the volume serial, and removed both junctions before
discard and exit. The guest check drives the enabled controls on its
headless desktop. The regular GUI test still requires a visible window and
passed on a private desktop on the physical host.
[Windows 11 mounted evidence](evidence/windows11-sandbox-memory-mount-2026-11-18.json)

Service Control Manager refused the in-guest driver stop. The owned Sandbox
shut down, and the installed host driver stayed stopped. Supported installer
rollback, in-place driver retirement and interactive Explorer qualification
remain open.

Disposable guests are used for driver and recovery correctness. Performance
and compression comparisons run on the physical Windows host.

### Stable WinFsp 2.2 trial

The signed WinFsp 2.2 driver and its matching runtime passed the expanded
mounted workflow on Windows 11 Enterprise build 26100. Win32 clients resolved
Unicode aliases, rejected a case-insensitive collision and used paths beyond
`MAX_PATH`. A rename preserved bytes behind an open share-delete handle.
The moved directory and its file retained their spelling and bytes after
detach and reattach. A missing parent now reports `ERROR_PATH_NOT_FOUND`
and an absent file under an existing parent reports `ERROR_FILE_NOT_FOUND`.

The guest used WinFsp development registration. The official 2.2 MSI had a
valid signature and reached software restriction policy verification in a
separate guest. Its installer did not complete within the trial window.
The headless guest reported no active user logon. Installer rollback and
interactive File Explorer qualification remain open.
[Stable 2.2 validation](evidence/windows11-stable-winfsp-2.2-2026-11-20.json)

The current helper passed both backing workflows with that signed driver
and matching DLL in an offline Windows 11 Sandbox. A native client wrote,
flushed and moved files on the mounted file-backed volume. The helper
detached and closed it, refused a second create at the same path, and
reopened it in a fresh process with the same serial and bytes. Independent
CPU inspection verified the backing between processes. Both workflows
removed their mount points before the guest was disposed.
[Mounted volume-file activation](evidence/windows11-mounted-volume-activation-2026-11-21.json)

### Populated volume and access timestamps

The disposable guest harness's `--gui-mounted-populated-file-sandbox` mode
creates and flushes 1,024 files in one directory. It checks a refused
conflicting open, an overwrite that shrinks a file, a writable mapping across
two native extents, and a rename with a retained handle. The Windows reader
checks the directory inventory, file lengths and contents before detach,
after reattach and in a fresh helper. Independent CPU inspection verifies
the same bytes and records the recovered checkpoint and namespace sequence.

The writable Windows adapter coalesces automatic access-only timestamps.
It persists the current access time on Cleanup when it is at least one hour
newer than the stored value. Explicit timestamp changes, allocation trims,
deletion and Cleanup that requests write, change or archive metadata retain
their existing commit behavior.
[WinFsp Cleanup contract](https://winfsp.dev/doc/WinFsp-API-winfsp.h/) ·
[Windows file times](https://learn.microsoft.com/en-us/windows/win32/sysinfo/file-times)

On Windows 11 build 26100 with signed WinFsp 2.2.26323, the fresh
read-only helper changed 1,024 access timestamps and advanced the namespace
sequence by 1,028 in the control. Both counts were zero in the revised run.
The payload digest matched in every mounted and independent CPU pass.
[Populated mount and access-time evidence](evidence/windows11-populated-mount-access-time-2026-11-21.json)

### Recovery after an acknowledged flush

The disposable guest mode `--gui-mounted-flush-recovery-sandbox` extends the
populated volume test. An external client writes and flushes a new
131,209-byte file, then requests termination of the exact supervised helper.
The client keeps the file handle open until helper exit and supervisor
retirement are confirmed. The test checks that the old mount is absent after
the client releases the handle.

Independent CPU inspection verifies the original 1,024 files and the
acknowledged write before a fresh helper attaches the same backing. The new
mount verifies the directory and acknowledged file and flushes a second,
65,709-byte file. After normal detach and exit, the CPU reader verifies both
writes and the recovered checkpoint. Two Windows 11 build 26100 guests with
signed WinFsp 2.2.26323 passed this sequence. Physical power-loss and
interrupted device writes have separate release gates.
[Mounted interruption evidence](evidence/windows11-mounted-post-flush-recovery-2026-11-21.json)

Current backing creation is shared with the WinFsp composition through the
[adaptive memory factory](WINDOWS_MEMORY_VOLUME_FACTORY.md). The fixed capacity
and staged Optimize descriptions below apply to the labeled historical build.

The following f4928c2 qualification and fixed allocation/staging profile are
historical evidence for the earlier flat-volume application. They do not
override the current paged factory, dynamic capacity or idle-work contract.

At source **f4928c2d2510cacb55da664a0338c8592194d9a0**, the isolated Windows
application job and exact-binary owner-host model/GUI checks passed. All 13 existing core jobs also passed at that source.
[Source, hashes, limits, measurements and limitations](evidence/volume-app-f4928c2.json).

That build qualified the unmounted editor using the real CPU filesystem engine.
It created a temporary volume, reserved its optimization area, saved and
verified new root-level text files, performed staged recompression and retired
the volume. The physical and temporary backends share the
[filesystem engine](WINDOWS_VOLUME_BACKENDS.md).

## Provider ownership and accounting

The shared Windows memory provider computes the volume budget from current
host headroom. It uses VirtualAlloc and VirtualLock to commit and lock pages
as the stored image grows. Candidate images and retained readers use the
same domain. Final owner retirement restores the original working-set
settings. Discard checks every committed page before retiring the backing.
[Backing and reservation contract](WINDOWS_MEMORY_VOLUME_FACTORY.md)

The app reports file bytes, combined current image/head/advice extent and
the backing's locked RAM separately.

Flush is a serial ordering barrier without a full-image stable mirror or a copy
of the entire extent on every transaction. Prior bytes remain available within
the owning process lifetime. This temporary provider promises no process-crash
or power-loss survival. Stored encodings and independent CPU readers are unchanged.

## Controller, editor and tray

One owned worker serializes engine access, with one admitted command and bounded
owned result copies. Notifications contain no heap pointers. Compression,
headroom queries and metadata work run outside the window procedure.

Native controls use the system font. UTF-8 conversions are checked; text input
is capped at 16 KiB and names use the engine's conservative root-name rules.
Save creates a NEW name and never overwrites an existing file. Creation and
writing are separate transactions: a write failure may leave an empty new name;
the UI reports failure and retains ownership. Binary import and complete
Explorer/file-operation workflows remain future work.

Optimize is an explicit development operation: at most 257 bounded advances,
independent candidate verification and checked publication. Unchanged source
generations are suppressed. It is not an autonomous scheduler, learned policy
or proof of useful GPU compression. Reserving advisory state does not implement
an automatic learner.

Close retains a live temporary volume, hiding to the tray when registration
succeeds and keeping/restoring the window when it does not. Discard is a separate
explicit action. Normal Exit follows discard and confirmed engine retirement.
The controller joins its worker. The tray selects notification version 4 and
removes its icon on exit. Cleanup failure returns a nonzero app exit status.
There is no autostart, detached worker, service, scheduled task or registry edit.
Forced process termination necessarily loses temporary memory contents.

## Executed qualification

The OFF-by-default FLITZFS_BUILD_VOLUME_APP target builds on Windows x64/MSVC.
The hosted portable profile uses source-pinned static CPU dependencies and an
asInvoker/PerMonitorV2 manifest. A separate role-specific PE audit checks native
OS imports, subsystem, ASLR/high-entropy/NX and absence of delay imports. The
console CPU dependency policy remains unchanged. Experimental executable artifacts
include the project/static-library notices and an exact corresponding-source
reference. The notice-packaging follow-up requires its own hosted artifact check;
it changes no application runtime source.

| Check at f4928c2 | Disposable Windows VM | Owner Windows build 26300 |
|---|---|---|
| Provider/model/controller checks | 377 passed | 377 passed |
| Complete model ownership cycles | Three, each 113 to 113 handles | Three, each 179 to 179 handles |
| Working-set restoration and provider cleanup | Passed | Passed |
| Native GUI file workflow | Passed | Passed |
| GUI child peak committed bytes | 13,619,200 | 14,209,024 |
| Forced stop / unconfirmed exit | Neither | Neither |
| Remaining FlitzFS processes | Confirmed child retirement | None in postflight |

The workflow creates a real volume, saves and reads exact UTF-8 text including
a surrogate-pair character, checks its file list, retains the live owner on Close,
optimizes, explicitly discards and exits. GUI check counters were 38 in the VM
and 30 on the host because readiness/status observations vary; they are not
counts of distinct test cases.

The harness has 128 MiB process/256 MiB aggregate memory, two processes, 20% CPU
and ten user-CPU seconds. The GUI child additionally has creation-time one-process
containment, 128 MiB and a 25-second deadline. Host supervision added a 30-second
deadline, fresh headroom checks, immutable executable handles, exact artifact/PE
hashes and a child environment with System32 PATH and optional GPU variables
removed. It did not require installed codecs, HIP or another application's DLLs.

The host WinFsp driver remained Stopped/Manual; no driver/configuration action
was performed. No FlitzFS HIP compute ran. Ordinary Windows GUI compositing was
not measured. These checks are not full Windows 11 product, visual layout,
high-DPI, accessibility, mounted-volume, power-loss or universal resource-safety
qualification. No throughput/compression-superiority benchmark follows.

## Retained failed experiments

Cold platform initialization was separated from repeated ownership: native RNG
and window/thread primitives reproduced the initial handle increase, then every
measured sample stabilized. Three subsequent full app cycles required exact
balance, with no growth allowance. The initial one-call baseline was disproved.

The earlier failures remain in the evidence directory: console-only import
policy used for GUI ([477e688](evidence/volume-app-477e688.json)); unlocalized then
measured cold handle changes ([58fa7c3](evidence/volume-app-58fa7c3.json),
[5bdd166](evidence/volume-app-5bdd166.json)); incomplete RNG initialization
([7996266](evidence/volume-app-7996266.json)); startup/readiness and cached-null
control binding ([8540b35](evidence/volume-app-8540b35.json),
[e743108](evidence/volume-app-e743108.json),
[3eedfc7](evidence/volume-app-3eedfc7.json)).
The fixed GUI test binds status after observed readiness and queues final Exit,
then requires actual clean process retirement. It adds no arbitrary startup sleep.

W01 and the full product remain unfinished: mounted lifecycle, physical backend,
complete semantics/authorization, install/update/rollback, automatic low-priority
learning, useful AMD routing, scaling, repair and comparison gates remain required.

## Advisory recovery consumer

[ADVISORY_HELPER.md](ADVISORY_HELPER.md) describes the new maintenance-only audit
and one-attempt rebuild path. The helper retains all four bounded area mappings,
reports all advice extents and defers optional optimization at exhaustion while
keeping file access available. Source 9977674 passed 461 model assertions across
three balanced 113-to-113-handle VM cycles, the native GUI workflow and all 13
core/mounted regression jobs. Its healthy owner GUI also exited cleanly with no
remaining owned process or driver change. See
[source-bound evidence](evidence/advisory-helper-9977674.json). The new test-only
post-admission allocator cases need their own VM run; the healthy owner GUI is
not fault evidence. This adds no autonomous executor, learner, persistent wear
budget or physical mapping.

## NEW-volume workload accounting

The next [managed workload integration](WORKLOAD_HELPER.md) activates accounting
before exposing a newly created helper volume. It uses the same private provider
and serialized controller, adds no worker/UI policy and never upgrades a legacy
image during ordinary open. Saves count actual UTF-8 bytes; empty/rejected saves,
reads, advice repair and explicit optimization mint nothing. Existing budgets
and retirement checks remain. New-source model/GUI and full regression gates are
required; this is provenance, not automatic learning or persistent spending.
