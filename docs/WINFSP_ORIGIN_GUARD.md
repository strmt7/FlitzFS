# Create/Open host-origin admission (experimental)

The filesystem host must not acquire new client handles through its own mounted
namespace. Its normal client work belongs in a different process; backend and
control work use the independent internal APIs. This boundary prevents one known
class of self-referential handle ownership. It does not reconstruct the historical
D01 kernel state or guarantee shutdown under arbitrary driver/future bugs.

## Admission and provider contract

Both WinFspReadOnly and WinFspNativeWritable own a WinFspOriginGuard. Their
bind_callbacks method requires an explicit WinFspOriginQuery in addition to the
existing FSP_FILE_SYSTEM struct. Configure this binding before callbacks start;
never rebind a live adapter or race destruction/stop with control operations.

WinFspSessionApi now has a required eighth originating_process_id entry. Missing
entries reject construction before lifecycle provider calls. A live provider
supplies the pinned SDK FspFileSystemOperationProcessId query; no default query,
installation path, discovery scan, process reopen or fallback identity is chosen.
Direct CPU-only callback/lifecycle fixtures explicitly inject synthetic origins.
Those values are not valid substitutes for the SDK query in a mounted fixture.

The SDK permits the origin query during Create/Open and eligible Rename requests.
This implementation calls it ONLY for Create/Open. It never calls it from Read,
Write, Flush, Cleanup, Close, metadata or security callbacks. A missing/throwing
query or unavailable host PID denies admission. A request from the actual host PID
is denied before acquiring an open slot, preparing native mutation, allocating
an inode/token or exposing a context. Native denial also precedes the control
lock and foreground mutation wrapper. Original output clearing remains in place.

Foreign process origins and the SDK's kernel-origin zero continue through every
original namespace, rights, descriptor, storage and byte check. Existing read-only
write rejection retains its status precedence. This is additional lifecycle
admission, not replacement authorization, authentication or driver recovery.

An atomic diagnostic records actual self-origin rejection. It cannot authorize
file bytes or retries. Read-only sessions retain it with the adapter; native
sessions copy it before adapter retirement so an ordered stop does not erase
the observation. It is never persisted as a decoding dependency.

## Actual callback and fixture evidence

All mounted workload actors were separated before enabling this boundary.
Sourcefa9fe908ee42cb69104a1541c8f6d871c664992f passed all thirteen core jobs,
including the finite external RAM client, real CPU peer fault/handle tests and
all original mounted handoff/recovery/identity/cleanup oracles. Its RAM observation
retained65,609 user bytes, selected217 stored payload bytes after66,062 raw payload
bytes, confirmed source/client retirement and left zero contexts/locked bytes.
These fixed synthetic observations are not comparative throughput evidence.

The writable fixture wrapper no longer rejects its own origin before delegation.
It delegates to the product adapter, counts the actual ACCESS_DENIED result and
requires the adapter's self-rejection diagnostic. Each fresh/resumed source
checks unchanged native generation/extent/namespace/context state. Read-only
normal/kill/remount sources likewise attempt an actual own read-open of the
existing sample and require SDK-driver denial plus the product diagnostic.
All actual data operations still belong to the external controller.

New CPU-only integration tests cover read-only/native self-origin denial,
missing and throwing queries, cleared output, unchanged native generation/
extent/namespace/contexts, foreign/kernel byte parity and absence of out-of-scope
queries. Lifecycle fixtures test the missing eighth provider, propagation and
retained diagnostic after native stop. These tests initialize no dispatcher or
mounted filesystem and do not prove actual SDK-origin behavior by themselves.

The publishing source must pass its own complete CPU/raw/codec/sanitizer/Windows
and actual mounted qualification before this guard is described as qualified.
No owner mount, driver operation, privileged change or unchanged GPU rerun is
enabled by this change.

## Source and remaining limits

The selected v2.2B5 tag resolves to SDK source
14d6e0280ae15e2550c44d6dbc476be51649727f. The exact header and directory mount
implementation were inspected through native GitHub captures. Directory setup
opens the fresh parent directory before applying its reparse point, so no
synthetic-origin setup bypass is added. The dispatcher establishes operation
TLS before servicing requests. See evidence/origin-sources-fa9fe90.json for
tag/blob/digest binding, actual current-source semantic routing and the corrected
wrong-repository lookup.

This admission rule does not prevent another process from duplicating/inheriting
a handle into the host, indirect cross-process backing cycles, external filters,
arbitrary kernel faults or loss of all surviving verified copies. Those remain
explicit lifecycle/backing/failure-model concerns. Historical issue702 remains
an unconfirmed causal lead. Preserve all existing limits, immutable-source and
independent CPU recovery contracts; no passing subset completes the filesystem.

## First publishing-source qualification

At `ad9be7a`, Windows compiled successfully and 135 of 136 callback/CPU test
groups passed. The new origin fixture failed its kernel read because it supplied
zero disposition (`FILE_SUPERSEDE`) rather than high-byte `FILE_OPEN` to native
Open. The correction changes only that test request, retains the byte comparison
and adds unchanged-generation/exact-size checks. Product code, resource limits
and existing tests are unchanged. The failed native job is retained in
`evidence/origin-fixture-ad9be7a.json`; corrected-source qualification remains
required. This failure did not run or qualify the mounted guard jobs.
