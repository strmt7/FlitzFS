# Process-isolated worker communication

Research and implementation date: 8 November 2026.
Baseline: `2996b7b319aa92d28e0df71b17d6bf1dda4f957e`.
This is initial development; the repository owner decides when to release.

## Goal and scope

Connect the bounded batch representation to a separately supervised process
without exposing application files, requiring a GPU, or adding a persistent
service. The existing CPU reader remains independent. This change does not
load HIP, execute a kernel, automatically route filesystem reads, or claim
GPU performance. The worker test explicitly executes CPU semantics.

## Plan and research decisions

1. Inspect the existing batch/gate contracts and Windows process supervisor.
   Reuse its creation-time Job Object, no handle inheritance, one-process limit,
   memory/CPU ceilings and confirmed termination result. Do not invent another
   process-launch path or silently relax limits.
2. Keep messages bounded and independent of native structs. Transfer only
   request identity, block descriptors, payload, statuses and output. The helper
   reconstructs its own PreparedGpuBatch and independently validates the frames.
3. Choose pagefile-backed mappings rather than temporary application files,
   inherited handles or an unbounded pipe reader. Use unpredictable local-session
   names, explicit current-user protected DACLs, non-inheritable handles and
   create-only collision checks. Do not use the privileged global namespace.
4. Publish a length only after bytes are written. The parent waits for confirmed
   successful process exit before inspecting the response; a ready flag cannot
   authorize concurrent reading or acceptance of a later-failing helper.
5. Before returning decoded bytes, check the requested execution backend, exact
   request identity, every status and byte equality with the retained independent
   CPU reference. Exercise partial publication, corrupt headers, stale identity,
   wrong backend, timeout, failure after publication and guard corruption.
6. Retain the existing no-GPU and no-mount default tests; add a real Windows
   child-process test on hosted CI. A CPU helper test is not a HIP-worker test.

Microsoft documents that CreateFileMapping may OPEN an existing named object;
ERROR_ALREADY_EXISTS must be rejected, not interpreted as exclusive creation.
The mapping uses INVALID_HANDLE_VALUE, so no existing application file can be
extended by mapping creation. PAGE_READONLY views prevent normal writes through
the helper's request view. Named mapping objects do not themselves provide a
synchronization wait; successful child termination supplies that boundary here.
Interlocked publication supplies a full memory barrier, but alone is not a
license to consume bytes from a still-running process.

AMD documents that runtime cleanup can synchronize with outstanding work.
The GPU result deadline is not a driver kill switch. A future HIP helper must
collect its OWN process-specific memory telemetry; the parent's DXGI usage is
not a substitute for the allocating child's usage. The current tests collect
no live GPU telemetry and do not weaken the existing result gate.

## Portable message contract

Both messages have a fixed 32-byte little-endian header. Requests use FLZREQ
and responses FLZRSP, each followed by two zero bytes. The next eight bytes
hold a nonzero request identity. Headers and block records reserve fields that
must remain zero. Unknown encodings, trailing bytes and truncated frames fail.
There are no raw pointers, paths, native structs or CPU-reference bytes on wire.

A request holds a block count, total payload/decoded extents, and one 12-byte
record per block: kind, fill, reserved bits, decoded length and stored length.
Payloads follow densely in record order. Before proportional allocation the
parser admits the complete framing and each extent. Preparation retains the
existing eight-block, 64 KiB/block, 256 KiB total-output ceilings and supported
six-codec subset. The maximum request frame is 262,272 bytes.

A response identifies CPU-semantic or HIP execution, task count, output length,
and one status per task before the output. The maximum response frame is
262,464 bytes. This execution tag is a declaration from a TRUSTED helper, not
cryptographic proof of hardware execution. accept_worker_reply rejects a backend
mismatch, stale identity, missing/pending/failed tasks, unequal bytes or malformed
framing before returning any candidate output. GPU acceptance additionally needs
the existing timing and hardware-budget gate; this change does not bypass it.

## Windows exchange and resource ownership

run_worker_exchange is an explicit internal API, not called by the CLI or mounted
filesystem. It starts the exact supplied trusted executable without a shell and
appends a fixed worker marker plus a random 128-bit channel identifier. No data
paths, credentials or private source content are added to the command line.
A process-wide try-lock refuses a second simultaneous exchange without queuing.

The two fixed-size, non-executable mappings occupy a total of 524,800 bytes
including transport headers/guards, before page rounding. This is separate from
private copies, runtime overhead and the child process limits. Admission checks
physical and commit headroom for the child allowance plus mappings and a 512 MiB
reserve before mapping creation. This snapshot is not a physical reservation.
The reused supervisor applies its default 128 MiB committed-memory and 20% CPU
limits, one active process, below-normal priority, bounded wait and up to two
additional seconds to confirm timeout termination. OS calls can still block.

The parent copies the request before launching; the helper receives a read-only
view and makes its own private copy. The response length starts at a pending
sentinel. Publication claims that sentinel atomically, copies within its fixed
capacity and finally publishes the length. Double publication is rejected.
No HIP cleanup runs in this transport. On timeout, unsuccessful exit or
unconfirmed termination, no response bytes are returned. For successful exits,
the parent checks transport headers, guards, request immutability and response
length before copying. The caller must still use accept_worker_reply.

Mappings, views, descriptors and process handles have scoped cleanup on success
and exceptional paths. A new explicitly requested exchange can run after a
failed CPU fixture; there is no automatic retry loop. This does not permit a
future poisoned HIP session to restart without its separate admission policy.

## Threat model and unfinished work

The executable and its containing directories must be trusted. The random
name and current-user DACL isolate ordinary accidental/cross-user access, not
hostile same-user processes, administrators, injected code or hostile drivers.
Tests deliberately demonstrate that same-user write access can reopen a named
object; unchanged-input/reference checks detect their test mutations. This is
not an encryption or security-sandbox feature. Pagefile-backed data may be paged
by Windows; memory-only here means no application-managed temporary data files,
not a guarantee that plaintext never reaches storage.

There is no public GPU worker binary, native runtime loader, hardware-budget
handshake, low-latency persistent worker, actual HIP dispatch, GPU-enabled mount,
or performance result. Process startup allowance is not the per-batch GPU
result deadline. Hardware execution and cleanup still require separate isolated
qualification; process resource limits cannot promise a healthy kernel driver.

## Executed checks before publication

The baseline passed its 16 portable CTest entries. The new wire suite passed
705 assertions, including exact six-codec/empty-payload/full-cap round trips,
all truncations of a small frame, every one-byte reply mutation, deterministic
request mutations, wrong backend/identity, over-limit sizes and failed statuses.
All 17 portable entries passed in codec-enabled, external-codec-disabled and
Clang AddressSanitizer/UndefinedBehaviorSanitizer builds: 51 test executions.
Hosted Windows outcomes require their matching CI run; no Windows success is
inferred from Linux tests.

The Windows test launches only its own executable. It checks cross-process byte
parity, actual read-only views and protected single-user DACLs, job settings,
invalid transport metadata, double publication, failure/timeout after publication,
new explicit exchange after timeout, rejection of malformed tokens and bounded
handle accounting. It creates no application fixture files and loads no GPU.

## Primary sources

- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw
- https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory
- https://learn.microsoft.com/en-us/windows/win32/memory/file-mapping-security-and-access-rights
- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile
- https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedexchange
- https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom
- https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___memory.html
- https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/stream_management.html

Related repository contracts: HIP_BATCH_PLAN.md, PROCESS_ISOLATION.md, and the
existing source-pinned FlitzZip study. No other repository is changed.
