# Input-funded advisory publication (experimental)

The controlled journal at `5db3ae7` and helper/pre-allocation consumer at
`99d5365` passed their full source-bound qualification. The latter includes all
13 integration jobs, the application model and a healthy owner GUI workflow;
see [funded-helper-99d5365.json](evidence/funded-helper-99d5365.json). It does not
launch background work, qualify a durable physical provider, authorize data-image
rewriting, or complete an autonomous learner.

The [managed paged-volume integration](MANAGED_PAGED_VOLUMES.md) now uses
this same journal with protected shared-base data, native file contexts and
image handoff/resume. It changes no journal bytes or costs. Advisory funding
still does not authorize or pay for data-image rewriting.

## Persisted settlement and compatibility

Legacy FLTOP001/capability1 records and default writer calls remain unchanged.
Funded records use FLTOP002/capability2, the same two disjoint 64-KiB records and
the same 128-KiB area. Bytes52..55 hold little-endian funding marker1 and bytes
56..63 hold the unsigned settled-input position. All existing profile/history,
digest, sequence, identity, reserved-space and size rules remain.

A pair mixing funded and unfunded records is invalid. Equal sequences must
contain identical state. Consecutive funded policy sequences must advance
settlement by at least 128 KiB. A cursor-only update can use fresh replica
verification as funding while preserving the settled input position. An
update with a cursor and insufficient settlement cannot change profiles,
benchmarks or group state. A backward position, an unsupported peer, or a
foreign peer supplies no policy. Old readers reject capability 2
explicitly. Ordinary filesystem encodings
and native workload capability6 do not change.

## Trusted control and costs

Only the managed-session funded methods can construct or mutate funded journals.
They require enabled native workload provenance and obtain current input from
a verified CPU snapshot under serialized control. Cached scalars may decline
work early; they never fund a write.

Each changed profile publication spends 128 KiB of previously unsettled
committed input and writes two 64 KiB records. Fresh-area creation writes four 64 KiB records,
including initialization, and costs256 KiB. These are current journal byte-write
costs under an experimental one-to-one input policy, not optimal compression
policy, physical flash-wear measurements or a hardware guarantee. Future mapping
and image/work costs require separate admission.

### Funded catalog updates

An indexed scan uses 4 MiB of unsettled input for its paired checkpoint. If its
dedicated catalog writer has a changed batch, paged planning calculates the
upper bound covering appended bytes, index pages, padding and both root writes.
Before the first catalog write, the same checkpoint settles an additional 32
input bytes per charged catalog byte. This uses the existing settlement field.
The verified journal write and readback finish before the catalog writes begin.
Insufficient additional credit leaves the independent catalog untouched and can
still fund the ordinary scan checkpoint. A failed catalog write keeps the
settlement charged across reopen.

First-time creation uses the same ordering. The planner includes the paired-root
area, typed catalog root, first useful postings, page pairs and padding in one
transaction. It checks the byte allowance and backend capacity before settlement.
No active catalog exists until that transaction and the typed-root read finish.
Changing the selected native image retires the old catalog without refunding
prior write charges.

A full catalog can reserve a separate packed replacement. The read-only
planning pass calculates its complete footprint. The advisory journal settles
4 MiB for the journal pair and 32 input bytes per byte of candidate write
allowance before the candidate's first write. The copy limits its extent to
the charged plan, which includes both root writes. The charge preserves any
recorded scan state and uses the selected catalog's image binding when the
journal has yet to record its first scan. An insufficient balance
leaves the candidate empty and quiescent. A paid interrupted copy preserves
the old selected catalog and keeps the charge spent.

For an identified provider, creation and rotation also need 4 MiB of
unsettled input for the separate selection pair. The planner includes that
cost before writing the candidate. Once the typed catalog is independently
verified, the journal settles the selection and reads back both slots
before the session adopts the new backing. A failed selection keeps the
old catalog active in the running session. Restart uses the checked journal
generation, which can select the new backing if its selection reached the
persistent view. Ordinary appends keep their existing charge.
[Catalog lifecycle](OPTIMIZATION_STATE.md#paged-coherence-catalog) ·
[Host validation](evidence/funded-paged-coherence-2026-11-19.json)

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

A degraded funded resume retains the leased owner, hides policy and records a
current-input repair floor. Paired repair can spend 128 KiB of genuinely new
committed input. A scrub cursor update instead earns its own write budget from
8 MiB of fresh verified records and advances settlement to at least the repair
floor. Both paths write the surviving peer's complement first and forfeit
prior unspent entitlement. A pending repair cannot be replaced to bypass its
charge.

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
