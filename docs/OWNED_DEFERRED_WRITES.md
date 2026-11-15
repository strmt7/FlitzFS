# Owned deferred Windows writes (experimental)

The explicit hosted-only native adapter can now own one pending write. Its CPU/SDK and disposable mounted response paths passed at a4ad516.
Automatic ordinary GPU I/O remains unqualified. Default application/filesystem paths remain CPU-only and create no service.

## Ownership and ordering

Configuration is allowed only before callback binding. The callback validates
the handle and complete metadata prerequisites, copies immutable input, and
captures the opaque WinFsp request Hint while its operation context is valid.
It retains no callback buffer, byte-count, FileInfo or thread-local request
pointer. An accepted request returns STATUS_PENDING. One owned control thread
alone accesses the persistent GPU coordinator.

Following native callbacks request CPU fallback and drain pending publication
before accessing the session. An already terminal verified result may complete
normally. This serializes append, overlapping writes, reads,
truncate, rename/delete, flush, cleanup and close. Callbacks wait for CPU
publication, never worker/driver retirement. The shared session lock protects
begin/finish and native mutation; worker polling/verification and retirement run
outside it. One outstanding request bounds the queue. A retiring worker owns
bytes only and cannot mutate the session after publication.

Each completion carries the original Hint, committed status/count and FileInfo.
Publication, response submission and worker retirement are separate facts.
FspFileSystemSendResponse returns no delivery status; a failed delivery must
never replay a mutation. Counters describe attempts, not acknowledged delivery.

## GPU evidence and fallback

The control thread consumes a copied health mailbox. Neither it nor callbacks
discover adapters or query GPU drivers. An independently supervised collector
must supply fresh qualified configuration/resource evidence. After CPU result
validation, a later observation is required before advice can publish. Missing
or stale evidence, unqualified work/costs, busy ownership, cancellation or stop
selects CPU behavior. CPU encoding may differ from GPU encoding; stored bytes
must remain independently CPU-readable.

GPU admission also requires an explicit trusted assertion that the helper and
its dependencies are independently available outside this volume. This flag is
not learning state or proof of automatic dependency validation. Configuration
and helper paths are immutable during service lifetime. Automatic trustworthy
helper/cost acquisition and ordinary GPU activation remain unfinished.

Shutdown first selects CPU completion, sends the single response, cancels and
observes worker retirement, then joins the owned control thread before stopping
the dispatcher/deleting its filesystem. Cleanup does not depend on mutable
hosted-environment flags. Unconfirmed retirement disables the service; it cannot
silently retry or authorize recovery. Existing isolated worker supervision may
retain its own transport frame until actual child exit; it retains no filesystem
reference. No perpetual service, scheduled task or driver change is added.

## Driver-independent control target

The owned service now builds as flitzfs_native_write_control. It consumes the
pinned WinFsp ABI headers without linking its runtime library or calling SDK
functions. Direct construction requires both a hint reader and a response sink
before starting a thread. Missing either rejects before storage access.

The actual adapter supplies its SDK functions when callers leave those options
empty. Existing mounted behavior and explicit fixture injection are preserved.
This separation prepares GPU service qualification without loading the WinFsp
DLL or a driver in the diagnostic process. The changed source still requires
exact-source qualification; it is not new hardware evidence or owner-trial
permission.

## Qualification

Source [a4ad516](https://github.com/strmt7/FlitzFS/commit/a4ad5166d5063d820cfdd704a600ea20a23f1d93)
passed all 13 [core jobs](https://github.com/strmt7/FlitzFS/actions/runs/43633235875),
154 native test groups (54 new deferred checks) and the complete
[ROCm SDK gate](https://github.com/strmt7/FlitzFS/actions/runs/43633243098).
The real CPU-only RAM-handoff mount passed and retired: zero contexts and locked
bytes, observed client flush, exact independent readback. Digest-verified
[receipts](evidence/owned-deferred-a4ad516.json) retain the detailed scope.


The direct SDK test uses four bounded synthetic CPU cases: deferred completion,
CPU-semantic verified worker advice, held-worker fallback triggered by a read,
and queued shutdown with concurrent stop callers. It checks immutable input, exact response identity/facts,
current/prior independent readback, one completion and joined resource ownership.
It does not execute a GPU or create a mount.

The existing disposable hosted RAM-handoff mount scenario explicitly enables
CPU-only deferred writes. Its real client/SDK response path must complete,
retire and preserve recovery; its report schema and resource limits are unchanged.
All other existing scenarios remain default CPU behavior.

Primary API semantics and exact captures are recorded in
[the research receipt](evidence/native-async-write-research-2026-11-14.json).
The historical [bridge plan](ASYNC_NATIVE_WRITE_PLAN.md) describes the preceding
design boundary. Finite cross-operation batching, automatic costs, actual GPU
callback qualification, scalable metadata, collective compression, persistent
learning and release qualification remain required. No owner execution or
production data-safety claim follows from this qualification.
