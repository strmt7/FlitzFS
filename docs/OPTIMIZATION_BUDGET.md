# Input-funded advisory publication (experimental)

The controlled journal at `5db3ae7` passed its full source-bound qualification.
The next helper/pre-allocation integration requires its own qualification. It does not
launch background work, qualify a durable physical provider, authorize data-image
rewriting, or complete an autonomous learner.

## Persisted settlement and compatibility

Legacy FLTOP001/capability1 records and default writer calls remain unchanged.
Funded records use FLTOP002/capability2, the same two disjoint 64-KiB records and
the same 128-KiB area. Bytes52..55 hold little-endian funding marker1 and bytes
56..63 hold the unsigned settled-input position. All existing profile/history,
digest, sequence, identity, reserved-space and size rules remain.

A pair with mixed capabilities is invalid. Equal sequences must contain identical
state. Consecutive funded sequences must advance settlement by at least128 KiB;
an intact backward/uncharged position, unsupported or foreign peer supplies no
policy. Old readers reject capability2 explicitly. Ordinary filesystem encodings
and native workload capability6 do not change.

## Trusted control and costs

Only the managed-session funded methods can construct or mutate funded journals.
They require enabled native workload provenance and obtain current input from
a verified CPU snapshot under serialized control. Cached scalars may decline
work early; they never fund a write.

Each changed publication spends128 KiB of previously unsettled committed input
and writes two64-KiB records. Fresh-area creation writes four64-KiB records,
including initialization, and costs256 KiB. These are current journal byte-write
costs under an experimental one-to-one input policy, not optimal compression
policy, physical flash-wear measurements or a hardware guarantee. Future mapping
and image/work costs require separate admission.

The result explicitly distinguishes published, unchanged checked audit and
deferred. Callers cannot supply/reset a settlement. A legacy changed publication
cannot mutate funded state. Identical legacy audits remain zero-write checks.
There are still at most four publications per journal lease, four admitted areas
per session and512 KiB aggregate reservations. Credit never renews a healthy
exhausted allowance. Normal file operations never inspect advice.

Only actual NEW managed volume creation from empty head/image backends can
receive one initial fixed provision. That runtime claim is consumed before
advisory I/O, including failed creation. A resumed controller receives no new
bootstrap. Reopening healthy funded advice retains its settled position and
unspent entitlement; it cannot mint previously spent input.

## Loss, repair and failure

Unavailable/poisoned advice establishes a verified current-input baseline and
forfeits all old entitlement. Automatic replacement must wait for at least
256 KiB of genuinely new committed input before any new-area admission or write.
Repeated unchanged-input opportunities decline without candidate I/O or another
native snapshot once the baseline is known. Failed attempts retain their areas
and the existing alias exclusions.

A degraded funded resume retains the leased owner but exposes no policy and
writes no repair. It records a current-input repair floor, forfeits old unspent
entitlement and requires128 KiB of new input before paired repair. Repair advances
the record sequence and settlement, writing the observed peer first. A pending
repair cannot be replaced to bypass its charge.

Failure after advisory mutation makes that owner terminal and preserves user-data
ownership and contexts. No work is admitted by a failed or deferred result.
A fully persisted but unacknowledged update may be recovered with its already
advanced settlement; tests must show it cannot be spent again. Both-record loss
and fresh-area replacement start empty learned state without resetting input
history.

The budget bounds admitted journal publications within the qualified ownership
and persistence model. It is not authenticated anti-rollback protection or a
global physical-wear bound over arbitrary nonpersisted writes, controller crashes
and repeated external restarts. Durable mapping/ownership, supervisor policy,
authorization and actual media qualification remain required. Per-session limits
cannot substitute for those full-product gates.

## Qualification

The focused suite uses real managed synthetic RAM fixtures and independent CPU
data reopen. It covers actual native-input funding, unchanged/rejected policy,
healthy resume and repeated spent-credit checks, degraded repair, both-copy loss,
fresh-input replacement, the four-area ceiling and unchanged source/context
ownership. It injects each of the six creation and four update/repair write/flush
cuts under the three existing failure modes. Those counts follow the current
journal calls rather than guessed data-transaction padding counts.

Additional cases cover checksum-valid mixed/unknown/foreign/uncharged records,
future/overflowing positions, lease competition, legacy bypass attempts and
successful bytes followed by failed readback. Existing687 advisory assertions,
native workload, raw/codec, Windows/native, sanitizers, helper/GUI and all mounted
regressions remain required. No owner/GPU/driver/mount experiment is enabled.

The final publication readback now moves its validated owned state through a
statically checked nonthrowing move. The previous const temporary selected copy
assignment and could allocate after the paired writes; stored bytes and I/O
ordering remain unchanged.

## Helper pre-allocation admission

The explicit failed-funded-owner preflight can establish a checked loss baseline
without admitting an area. An unchanged opportunity declines through cached input
without another snapshot, candidate allocation or candidate I/O. A positive result
never grants write authority: the actual rebuild independently verifies current
input and all existing admission checks again. Healthy, staged, pending, unmanaged
and unfunded owners cannot use this path to reset or bypass journal limits.

The helper's new creation/replacement consumer is specified in
[ADVISORY_HELPER.md](ADVISORY_HELPER.md). It does not authorize image rewriting
or schedule background learning. Qualification must include the existing real
post-admission allocation failures after the newly added source checks.
