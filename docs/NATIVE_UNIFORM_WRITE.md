# Controlled native uniform Windows writes (experimental)

ControlledNativeUniformWrite composes the resource-admitted uniform controller
with the checked NativeFileSession/Windows data bridge. It is explicit helper
infrastructure. Ordinary callbacks do not construct or launch it yet. This does
not mount a filesystem, load a driver, or establish GPU performance.

## Owned operation and automatic window selection

begin validates pure input/append/allocation-policy metadata, checks the exact
owned handle's active write grant and complete FileInfo prerequisites, and checks
current OS physical/commit headroom before retaining an immutable full write.
The existing native writer's 1 MiB operation bound remains a backend contract,
not an assumed RAM size or a restriction on logical file size.

For GPU advice, the coordinator finds complete interior 64-KiB native extents
inside the actual accepted write range. Prefix/tail and other extents retain
ordinary CPU encoding. It adds its full owned source and descriptor/result costs
to the qualified model, then obtains the largest currently admitted resource/
time window. It prepares every complete extent in that window, rather than the
old prototype's single extent. No manual batch/allocation size is required.

The real worker arguments are built from that exact shape, externally qualified
configuration identity, discovered ordinal, live LUID/node and authenticated
runtime digest. CPU-fixture arguments are used only with the explicit CPU test
backend. A differently keyed binding, unknown cost or preparation failure falls
back to CPU and does not fence a healthy storage session.

Admission is NOT a write acknowledgement. The external file context must stay
owned, the NativeFileSession must outlive the coordinator without moving, and
all calls run on one serialized control thread. Caller buffers can be released
or changed after successful begin; the coordinator owns the full original bytes.
The existing controller independently owns its in-flight subset.

## Reused finite-sweep dispatch

An optional NativeUniformSweepGrant on the qualified worker binding selects the
already authenticated finite reused worker. The coordinator derives canonical
quantum size, number of windows and peak slab from the complete immutable batch,
the caller's maximum work and device grant. It passes full request/reply capacities
together with the derived quantum/peak to --driver-uniform-lz4-sweep-adapter.
NativeUniformWriteStart reports that planned geometry without claiming execution.

Dispatch preparation is pure and validates grants for real and CPU-semantic
callers. Invalid or insufficient grants select CPU before a process can start.
Absent the optional grant, the original single-window command and geometry are
preserved. There is no host-specific allocation or two-chunk product default.

The complete source, result, transport, recovery and deadline admission remain
unchanged. They conservatively charge the whole canonical window, even though
the reused worker allocates only its peak slab. This change does not reduce those
bounds or qualify a new cost model. Each native operation still owns at most the
existing 1-MiB write; cross-operation coalescing remains unfinished.

The reused worker returns the same ordinary version-2 reply. The controller still
requires full independent CPU comparison, a fresh final observation and successful
exact child/supervisor retirement. No intermediate quantum becomes a candidate
or acknowledges bytes. All accepted extents enter the same checked native mutation
and commit-bound FileInfo/result path.

This connects the controlled write coordinator to the existing reused engine;
ordinary callbacks do not enable GPU automatically. It does not implement shared
dictionaries, collective/learned compression or GPU read acceleration. CPU fallback
may encode differently and produce a different size while preserving all bytes.

## Completion, failure and retirement

The two-stage controller poll retains independent CPU verification and a fresh
post-verification observation. use_cpu_fallback only selects the retained full
CPU source and never waits, signals, frees buffers or invokes a runtime.
Cancellation and exact worker/supervisor retirement remain separate.

finish is empty while waiting. On terminal GPU success it forwards every verified
extent candidate to dispatch_native_write_with_lz4 in the SAME native mutation.
The existing encoder checks its recovered bytes against the current CPU-merged
extent and chooses only valid, useful encodings. Stale append geometry or changed
file contents cannot redirect GPU bytes into the file. Count and all FileInfo
fields come from the actual committed bridge result, including append, clipping
and reservation behavior.

Returning a committed result performs only trivial state copies after bridge
publication: no backing query, allocation, process operation or context release.
Completion is idempotent. A rejected NEW begin invalidates the previous completion
cache, so a subsequent finish cannot acknowledge old bytes for a failed request.

Storage failure is distinct from GPU failure: the bridge returns zero accepted
count/FileInfo and fences without retry while preserving context ownership.
GPU fallback must never retry a failed storage commit. Buffers remain owned until
a later begin/destruction; borrow lifetimes and exact retirement still apply.

## Qualification boundary and next work

The new hosted-only SDK fixture joins actual sized-channel CPU helpers, complete
write ownership, two-extent advice, raw/fast and fixed/compact native records,
independent CPU recovery, prior snapshots, unrelated acknowledged data, corrupt
worker output, live-helper CPU fallback/cancellation, append, constrained and
unaligned writes, storage failure and rejected-admission/idempotence checks.
It uses synthetic RAM images and the user-mode SDK DLL. It does not create a
volume, dispatcher, mount, driver, persistent service or actual GPU request.

Source-matching Windows/Linux/raw/sanitizer/SDK checks remain required before
qualification. The controller at 4d1ef00 passed all 13 core jobs and full SDK
qualification, including live-helper CPU selection and late corruption quarantine.
The next required work is a separately admitted actual uniform GPU calibration
through the native write path, measured configuration-specific costs/grants and
controlled application integration. No useful GPU speed/total-size improvement,
Windows 11 client mount, physical-power-loss result or release is claimed here.

## Retained initial path-admission failure

At be5c284, native Windows passed 149 of 150 groups. The new controlled-write
fixture received a generic D:/... helper path from CMake, while the command
encoder required a backslash after the drive. The trusted helper therefore did
not launch. The original assertion remains required; CPU fallback is not counted
as GPU/worker success.

Microsoft's current filesystem implementation was inspected through its native
repository API at e1ed302683e5e822f663fbb0f44cee7e9ddc6af0 (29 October 2026).
Its path constructor preserves source separators, and make_preferred explicitly
replaces forward separators with backslashes. The shared command encoder now
normalizes separators before its existing root/component/argument validation.
It does not resolve dot components, aliases or relative paths, alter the caller's
string, invoke a shell, or remove any existing limit.

Portable tests cover generic/mixed separator equality, spaces, source immutability
and malformed/reserved/stream/UNC/relative spellings. The exact original failed
artifact, command and log digests remain in evidence/native-uniform-path-be5c284.json.
Corrected-source execution is required before qualification.

## Reused-dispatch validation candidate

The Windows SDK fixture adds a full 1-MiB, sixteen-extent owned write through a
CPU-semantic child that actually executes eight serial quanta and returns one
combined ordinary reply. Cases cover exact advice storage, independent current/
prior/unrelated CPU readback, caller-buffer mutation, corrupt combined output,
live held-child CPU selection/cancellation, and zero/insufficient work grants
that must not launch a process. Pure real-command checks bind the full capacities,
configuration/ordinal/LUID/runtime hash and derived quantum/peak; no HIP is loaded.

These checks require publishing-source execution before qualification. They do
not establish actual GPU execution through this coordinator, mounted GPU I/O or
performance benefit. The prior instrumented hardware evidence at 11fbf07 remains
scoped to its explicit diagnostic. Do not repeat that unchanged owner trial.
