# Owned deferred Windows writes (experimental)

The explicit hosted-only native adapter can now own one pending write. Its CPU/SDK and disposable mounted response paths passed at a4ad516.
Automatic ordinary GPU I/O remains unqualified. Default application/filesystem paths remain CPU-only and create no service.

Source d384b6c passed all thirteen core jobs, all 53 prescribed SDK commands and
one fixed six-request owner comparison through this service. All three GPU
requests verified and stored sixteen candidates; every CPU/GPU request passed
independent current/prior CPU readback, one injected response and retirement.
Median request times were 24.2311 ms for CPU and 513.918 ms for GPU. Each produced
the same 1,310,720-byte image and 135,648 active payload bytes from the same 1 MiB
input. This case showed no speed or stored-size advantage.
[All trials and scope](evidence/owned-service-comparison-d384b6c.json). The response
sink remains injected; real GPU SendResponse/Explorer delivery and automatic
useful routing are still unqualified.

The explicit --deferred-native-cpu-once invocation uses the same owned-service
source, initial storage, cleanup and timing scope
without launching a worker or querying the GPU. The original
--deferred-native-cpu-fixture remains hosted-only; the SDK gate checks rejection
when that child's hosted flag is absent. Receipts identify hosted_cpu,
explicit_cpu or gpu invocation. Changed source still requires exact-source
qualification before owner use or a matched performance claim.

The first CPU-mode qualification at 94af72c passed all thirteen core jobs and all
52 prescribed SDK commands, including both invocations and legacy-guard rejection.
One bounded explicit CPU invocation on Windows 11 also passed with complete
readback and retirement. [Evidence](evidence/owned-deferred-cpu-94af72c.json).
Its request reports of
0/16 ms exposed the existing GetTickCount64 granularity; they do not establish
zero execution time. The qualified d384b6c fixture records owned_request_ns with the existing
steady-clock facility over the same scope. Legacy milliseconds remain for
historical readers and timeout control is unchanged.
Both modes also report an unkeyed BLAKE3 source fingerprint, calculated before
the request timing scope. It binds comparison inputs without requiring identical
CPU/GPU compressed streams and does not authenticate the data. The full-process
scope includes that calculation.

The explicit --gpu-deferred-native-compare mode uses the same qualified worker,
ordinal, LUID, node and runtime-digest arguments as the single GPU invocation.
Its fixed order is CPU/GPU, GPU/CPU, CPU/GPU: six requests in one parent process,
each with new identical volume state. CPU trials clear GPU bindings and perform
no GPU query or worker launch. A refusal stops the series immediately; no later
request or automatic retry follows. Before each request, a five-second series
admission window is checked. This is not a five-second completion guarantee: each
request retains its deadlines, and the reviewed outer supervisor still enforces
the complete process wall limit. Use that supervisor, not an unsupervised launch.

Each row retains independent CPU readback and complete retirement checks. The
controller's peak commit figure is cumulative over the shared process job; it
is not a separate peak for each trial. Compare per-request owned_request_ns and
actual image/payload sizes. The one outer-process time covers the whole series
and cannot be assigned to individual CPU/GPU requests. The CPU-only
--deferred-native-cpu-series mode qualifies six successive ownership lifetimes
on the disposable SDK runner without hardware execution. Its source fingerprints,
storage accounting, response and cleanup must match the single CPU case. This
comparison passed the bounded owner case at d384b6c. Three trials per backend on
one fixed synthetic source do not qualify a reusable production cost model.

The observed CPU range was 20.0793–28.1673 ms; the GPU range was
505.7415–713.1048 ms. Each GPU request used a fresh worker. The request clock
includes input copy/enqueue through service stop/join, excluding initial volume
setup and final independent CPU readback. The complete six-request parent took
1,911 ms. Final locked bytes were zero, every transport was quiescent, and the
postflight found no named FlitzFS processes. Driver status was OK and the runtime
digest was unchanged. The bounded System warning/error query found no matching
GPU providers; this does not establish absence of every possible driver fault.

The next implementation must amortize setup over finite useful work and advance
protected collective compression. Do not repeat this unchanged comparison or
enable this slower small-request route automatically. CPU/GPU encoders and sizes
may differ; qualification compares independently decoded source bytes and full
stored cost, rather than requiring identical compressed streams.

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

## Optional advice receipts and driver-free composition

NativeUniformWorkerBinding.capture_advice_receipt enables a compact integrity
receipt for verified encoded advice before storage publication. It records the
first payload's length and unkeyed BLAKE3 digest and whether every offered
payload has that same representation. It neither authenticates data nor changes
the stored format. Disabled normal writes perform no additional payload hashing.
Nonuniform representations remain fully supported; their receipt simply cannot
establish the repeated-source diagnostic's single-fingerprint comparison.

The service retains the committed result through completion_receipt_locked().
Callers must own its control lock. Accepting a new request clears the preceding
result. These fixed-size advisory facts do not retain callback buffers or
decoder dependencies. Their retained copies are included in resource accounting.

The existing optional runtime probe adds --deferred-native-cpu-fixture and
--gpu-deferred-native-once. Both exercise one owned 1-MiB synthetic request,
overwrite the caller's original buffer after enqueue, require one commit-bound
response, join the service, and independently recover current and prior bytes.
The GPU mode additionally compares stored encoded payloads against the observed
advice fingerprint. A codec identifier alone is insufficient: CPU fallback can
also encode LZ4. These are unmounted diagnostics, not ordinary Explorer I/O.

The explicit GPU fixture supplies a manually reviewed configuration-bound grant
for the same finite two-block/64-KiB quantum and sixteen-block source shape.
It is not a reusable measured cost model; cost_profile_qualified stays false.
Its values cannot become automatic routing policy or authorize other workloads.
Fresh telemetry uses the supplied verified LUID/node, never adapter-order scans.
Only the diagnostic supervisor samples DXGI; the service can select CPU without
waiting for that sampling code. General product collectors still require their
own supervised lifecycle and qualified costs.

The private SDK fixture stages pinned ABI headers on a disposable runner and
links only the control target. Loader metadata must contain no WinFsp or AMD
runtime dependency in the parent probe; the runtime presence check also refuses
an unexpectedly loaded WinFsp DLL. No WinFsp runtime is packaged. The exact
upstream license is retained alongside the existing dependency notices.

A verified GPU result and matching stored payload are distinct report fields.
The owned_request_ms scope begins before input copying/enqueue and ends after
confirmed control-thread exit. It excludes volume setup and the final independent
readback. Image and payload accounting occurs after native context cleanup/close.
Unknown hardware execution after a rejected child is null, not a fabricated
false. CPU fallback means controller source selection; storage can independently
choose a smaller CPU encoding. Source-specific core and full SDK qualification
are mandatory before any owner experiment. The narrow source-d384b6c hardware
comparison is qualified; this is not an automatic GPU route or a general
filesystem performance result.

## Qualification

Source [94aaa2a](https://github.com/strmt7/FlitzFS/commit/94aaa2a772e03689044df4c0b7504903faa9d553)
passed all thirteen [core jobs](https://github.com/strmt7/FlitzFS/actions/runs/43692752566),
154 native groups (57 deferred and 226 controlled-write checks), and the full
[SDK gate](https://github.com/strmt7/FlitzFS/actions/runs/43692765002).
The new driver-free owned-service CPU fixture passed on the disposable runner.
The one bounded owner GPU case encoded 1 MiB into 135,344 candidate bytes,
matched their stored digests, independently recovered current/prior data, sent
one injected response and joined the service. Locked memory returned from 4 MiB
to zero; package inputs remained unchanged and no named FlitzFS processes remained.
The request scope took 641 ms; the complete 1,310,720-byte image exceeded the input.
No competitive speed or total-size advantage follows from this synthetic case.
The [receipt](evidence/owned-deferred-gpu-94aaa2a.json) retains source, package,
telemetry-failure, adapter-identity, retirement and postflight evidence.

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
learning and release qualification remain required. The one separately recorded
owner GPU case does not establish production data safety or arbitrary-failure
survival.
