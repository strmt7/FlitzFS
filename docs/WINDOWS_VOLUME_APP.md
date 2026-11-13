# Experimental Windows volume application

At source **f4928c2d2510cacb55da664a0338c8592194d9a0**, the isolated Windows
application job and exact-binary owner-host model/GUI checks passed. All 13 existing core jobs also passed at that source.
[Source, hashes, limits, measurements and limitations](evidence/volume-app-f4928c2.json).

This is a functional, unmounted application slice using the real CPU filesystem
engine. It creates a temporary volume, reserves its optimization area, saves NEW
root-level text files, lists and reads them, verifies saved bytes, performs
explicit staged recompression and retires the volume. It does not yet provide
an Explorer drive, physical provisioning, a complete helper/installer or a GPU
route. The final app must manage both memory and physical backends through the
[same engine](WINDOWS_VOLUME_BACKENDS.md).

## Provider ownership and accounting

The private Windows provider uses VirtualAlloc and VirtualLock, verifies initial
residency and keeps one domain lease per process until the last store retires.
A bounded registry tracks at most 64 stores. Status/discard checks verify every
current store's pages and the working-set reservation. No pageable fallback is
accepted. Allocation failure or unknown/low headroom refuses admission.

The provider ceiling is 40 MiB; this app reserves a 12 MiB experimental domain,
with a 4 MiB image, 128 KiB head and separately allocated 128 KiB advisory area.
Candidate images share that same aggregate limit. Capacity remains reserved
through backing-store ownership, including references surviving a wrapper.
Unconfirmed frees do not recycle their accounted bytes. Failed cleanup blocks
a new domain in that process. Prior working-set settings restore after the last
owner retires; no other process's settings are changed.

The physical/commit reserve is the greater of 768 MiB and one eighth of detected
physical memory. These are qualification limits, not final host sizing policy.
The app reports file bytes, combined current image/head/advice extent and all
provider allocations separately. It does not present active payload reduction
as total-space savings.

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
