# In-session advisory rebuild (experimental)

13 November 2026. This is a controlled shared-engine recovery operation, not an
automatic learner, a provider mapping implementation or a power-loss claim.
Publishing-source qualification is required.

## Contract and ownership

The original create_optimization_area and resume_optimization_area still admit
one initial binding attempt. rebuild_optimization_area is a separate serialized
control operation. It requires a managed healthy data session, no staged
recompression, and an existing unavailable or poisoned advisory owner. Healthy
advice cannot be replaced, including when its publication allowance is exhausted.
A caller flag, timer or new confidence score cannot manufacture failed status.

The provider supplies a NEW, independently reserved/nonaliasing backend and the
existing publication allowance of 1..4. Reject null, current/retained advice,
head/active/candidate aliases, invalid allowance, unbound state and the aggregate
limit before backend access. The existing journal validates the storage contract
and empty extent; an admitted but invalid/partial candidate remains owned.

Allocate its ownership record before changing bindings. Retain the former owner
and any poisoned writer lease, then admit the new area through the unchanged
two-slot creation protocol. A successful rebuild starts at sequence 1 with EMPTY
advice. Never copy cached confidence, old policy or an unsupported record into
the replacement. Old owned snapshots remain unchanged.

Success requires both flushed slots and independent readback through the
existing journal. No allocating/I/O diagnostic follows success. Failure retains
the attempted area, exposes no policy and leaves the data writer, volume head,
open/cleaned handles, grants and original source unchanged. Allocation failure
before ownership admission preserves the prior binding; failure during journal
creation consumes the admitted binding. There is no automatic retry.

## Resource and reclamation boundary

The fixed experimental ceiling is four admitted areas in one session: the
initial area and at most three distinct rebuild attempts. Every attempt reserves
128 KiB in accounting, including empty, partial and poisoned attempts. Thus
owned_area_bytes is at most 512 KiB. reserved_bytes still describes the current
128 KiB reservation; binding_attempts includes all admitted areas. These scalar
getters perform no allocation or backing access.

All former backends remain excluded from immediate and staged image maintenance,
even when empty and otherwise eligible. Actual session destruction releases their
writer owners before the volume-head owner. Moves and image handoff preserve the
same advisory ownership. Reclamation/mapping must not be inferred from pointer
inequality or a per-instance lease.

The provider must independently retain/publish the selected-area mapping and
globally exclude other storage facades. A successful journal creation does not
attest to that mapping. A later controller may resume the selected new area only
after actual previous-owner retirement and exact sequence observation. No data
head, user namespace or serialized encoding is changed by this API.

This per-session ceiling and existing per-lease publication allowance are NOT a
persistent lifetime rewrite budget. No repeated discovery, background process,
GPU call, host test or general area recycling is enabled. Provider-managed
mapping/reclamation, workload-derived persistent write credit, controlled helper
authorization and the automatic learning consumer remain required for L01/A01.

## Configured tests and source basis

The expanded optimization_state_crash_model retains every existing case and adds
23 compound replacement scenarios: every creation write/flush boundary in three
persistence modes, final readback, allocation refusal, pressure, unsafe contract
and a nonempty candidate. Independent CPU user-data recovery and continued file
writes must succeed after advisory failure.

Additional cases cover both-lost detection by the real advisory writer, poisoned
lease retention, new-area lease exclusion, invalid aliases/allowances, staged
deferral, a healthy exhausted owner, all retained empty-area target exclusions,
the aggregate cap, empty-policy initialization, owned snapshot stability, live and
cleaned handles, image handoff, move/destruction and independent new-controller
resumption. They use bounded deterministic RAM backends, no paths/mount/GPU.
The existing 30-second test timeout and all other qualification limits are intact.

Primary-source native captures and actual semantic routing are bound in
[evidence/advisory-rebuild-sources-053ade8.json](evidence/advisory-rebuild-sources-053ade8.json).
The inference from SQLite's crash/compound-failure testing is to enumerate every
mutation boundary and combine recovery with I/O/allocation failure. Its reader
lifetime discussion supports retaining old ownership until safe retirement;
this API does not adopt SQLite's storage format or depend on SQLite. Microsoft
file-caching guidance keeps buffered completion distinct from durable flushing.
The custom backend's explicit flush/failure-unit contract remains the authority.

At parent 053ade8, all 13 core jobs passed, including the existing advisory model,
136 Windows native groups, 117 sanitizer groups and actual mounted origin denial.
The VM app also passed its 377 model checks and GUI workflow; its GUI observation
counter is not a distinct-case count. Exact evidence is
[evidence/origin-guard-053ade8.json](evidence/origin-guard-053ade8.json).
Those results do not qualify this new rebuild implementation or close the
unexplained historical D01 failure, useful-GPU benefit or full-product gates.

The retained-empty-area exclusion oracle gives those areas the same 4-MiB image
capacity as a successful fresh control. Resource refusal therefore cannot hide
a missing alias check. Separate cases reject reuse of retired empty advice while
the current advice is unavailable and the binding cap has not been reached.
