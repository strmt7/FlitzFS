# Atomic native data and automatic metadata (experimental)

This Windows write-engine component supplies stable-inode data plus requested
automatic metadata in one durable operation. It does not enable writable
callbacks, allocation-size handling, driver installation or an app.

## Current contract evidence

The selected 2026 WinFsp v2.2B5 header at
14d6e0280ae15e2550c44d6dbc476be51649727f was read again through the native
repository API for this change:
[Write and SetFileSize contracts](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h).

Write selects the current EOF for append, constrained I/O cannot extend EOF,
and success returns accepted bytes and file information including times and
attributes. SetFileSize separately distinguishes EOF from allocation size.
These are integration requirements. They do not specify a complete automatic
time policy for this engine. The actual adapter must decide effects from
qualified Windows events and per-open suppression state. This change does not
infer that every write must stamp every time or grant allocation support.

## Shared staging, no second commit

NativeAutomaticMetadata is an alias of the existing NativeCleanupMetadata.
The historical type, layout, functions and Cleanup signatures remain intact.
The new name reuses its bounded requested archive/access/write/change effects
and strict nonnegative 100-nanosecond UTC clock. Creation is never changed.
An unused clock must be zero; requested zero is a real time, never a sentinel.

write_inode_with_metadata and truncate_inode_with_metadata use the existing
stable-inode selector and private Stage. After staging data/EOF, the same
Stage copies an existing tuple and changes only requested fields. The usual
replicated three-barrier commit publishes data, namespace, size and metadata
together. No intermediate acknowledged data-only or metadata-only state exists.

Meaningful effects need a present tuple; the engine never fabricates original
times/attributes for a legacy inode. Missing tuples, invalid clocks, type,
bounds and input admission fail before backing writes. Invalid clocks reject
before any backing read. Pure admission failure leaves the writer healthy.
Storage failure fences through the existing terminal/indeterminate rules.

Old write_inode/truncate_inode APIs supply no effects and retain their behavior.
Append selects current acknowledged EOF inside the single controlled call.
The full supplied input remains at most 1 MiB, even when clipped. Zero accepted
write bytes apply no effects and publish nothing. Nonempty identical accepted
bytes still advance metadata and execute all three barriers. An explicitly
requested truncate effect may update a tuple even when EOF is unchanged;
identical EOF plus identical fields remains a no-op.

Accepted offset/count and transaction results escape only after durable success.
Existing result layouts and nonthrowing post-publication moves are unchanged.
No extra snapshot/path query is used to form the result. Truncate/regrow keeps
tails zero. Original snapshot data and tuples remain immutable.

## Ownership and authorization boundary

NativeFileSession::write_with_metadata and truncate_with_metadata select the
owned context's stable inode and require data-write access. They accept only
trusted automatic effects, not a full user tuple or an explicit SetBasicInfo
permission. The explicit setter still requires separate WRITE_ATTRIBUTES.
A read-only/stale/foreign context cannot gain mutation access via these APIs.

Contexts remain valid through Cleanup and late paging until Close, including
detached files. Automatic effects follow that identity after rename/path reuse;
they cannot redirect to a new same-name file. Existing per-open delete intents,
latches, cleaned flags, context counts and final retirement rules are unchanged.
A later explicitly requested data effect is independent of Cleanup duplicate
delivery; it does not resample or replay the prior Cleanup event.

The future callback layer must authenticate contexts, serialize the full call,
keep input stable, choose qualified events/clock and form truthful FileInfo.
This is not an ACL/share/cache evaluator, an implementation of SetFileTime
-1/-2 suppression, a writable callback table or supervisor-offline integration.

## Resource and recovery limits

No encoding, format capability, object count, namespace size, decoded-live,
dirty-extent, input, snapshot/read, physical-history or queue cap increases.
The alias adds no stored/session state; requested effects are bounded stack
values and reuse the existing tuple-copy code. Existing CPU integrity and
recovery paths remain independent of any GPU. No background worker starts.

Four sequential test groups keep the existing 45-second per-process limit.
Raw/automatic invariants cover all sixteen effect masks, creation/untouched
fields, zero/invalid clocks, clipped/empty writes, identical-write barriers,
unstamped legacy admission, size/input/type/grant/stale-context rejection,
same-EOF effects, rename/path reuse, detached late paging and zeroed tails.

Append, constrained write and truncate groups fault every observed write and
barrier under all three existing persistence modes. Each uses detached owned
contexts and an unrelated acknowledged file. A failed call must return no
success, retain the exact context state, fence without retry, preserve an old
snapshot and independently reopen a complete old/new data/basic/lifetime state.
Only after the original owning session is destroyed does qualified closed-session
recovery retire detached data and continue combined writes to the keep file.

Configured checks require exact-commit executed evidence before passing claims.
The existing metadata/legacy process-kill, sanitizer, CPU/reference, relocated
package and read-only mount lanes remain required. Modeled faults and real
process kills are not physical power-cut proof. Windows Server tests are not
Windows 11 client qualification. Owner-PC execution remains prohibited.

## Remaining product work

Complete per-open metadata policy, persistent security/ACLs, real allocation,
Create/Overwrite/cache/cancellation behavior, writable adapter/supervisor/helper,
latest Windows 11 client, actual AMD runtime benefit, production capacity and
reclamation, redundant payload repair, autonomous low-priority learning and
representative filesystem benchmarks remain open.

## Executed hosted checkpoint

At c70a2ed0fc9fe8eb804042abfd064e381dc69631 Windows native job
129555011584 passed all 54 entries; sanitizer job 129555012119 passed all 47.
Both completed 264 mutation-metadata invariants and all 1,074 modeled failures:
each raw/automatic append trace had 61 I/O steps and 183 faults, while each
constrained/truncate trace had 59 steps and 177 faults. The new sanitizer
fault groups took 19.11, 17.42 and 17.33 seconds, below the unchanged
45-second per-process bound. These are bounded test durations, not filesystem
performance measurements.

The same Windows run retained the actual basic API oracle, all nine vendor
Cleanup dispatcher cases, 209 Cleanup invariants and 1,026 Cleanup failures.
Earlier lifetime/context/basic/Flush cases remained in the complete suite.
Worker handle totals were 94 before and after.

Windows metadata writer job 129555011844 completed all 712 termination cases
in two 356-case partitions (38.0/37.934 seconds). Legacy job 129555013409
completed all 976 cases in two 488-case partitions (64.801/64.46 seconds).
These are exact-commit process-kill results, not physical power cuts.

Relocated native Windows job 129555012367 passed 51 CTest entries, its real
private-job retirement test and all 21 package commands, with zero retries
and supervisor-only before/after accounting. Each command incremented total
assigned processes exactly once. No limit was raised or owner-PC action taken.

Read-only mount job 129558751121 passed actual native reads/mapping/enumeration,
write rejection, source parity, confirmed helper termination and independent
CPU recovery/remount. Normal/recovery exits were zero; the deliberate kill
returned 1460 with exit confirmed. Peak commit bytes were 6,172,672 /
6,168,576 / 5,214,208, inside the existing process bound. All nine triggered
workflows succeeded. The selected driver candidate still explicitly reports
windows11_policy_qualified=false. No writable mount, client, live GPU or
owner-PC qualification follows.

## Commit-bound output

[NATIVE_MUTATION_RESULTS.md](NATIVE_MUTATION_RESULTS.md) adds captured scalar
inode/basic/lifetime facts returned after success, without a post-commit query.
Original result layouts remain intact. The existing fault cases now check the
new output directly; sealed-backend tests separately exercise result construction
when all backing I/O becomes unavailable after the final publication barrier.
No complete Windows allocation/FileInfo/write-callback qualification follows.

## Compact layout fault qualification

The same native mutation invariants and append/constrained/truncate fault
oracles additionally run against explicitly compact 4 KiB checkpoint metadata.
Evidence labels fixed versus compact layouts; all fixed cases remain.
The RAM fault unit is explicit and matches each modeled contract. See
COMPACT_CHECKPOINT_METADATA.md for persistent admission and remaining
root-3-specific real process/power-loss coverage.
