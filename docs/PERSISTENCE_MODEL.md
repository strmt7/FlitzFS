# Bounded executable commit-persistence model

Experimental protocol experiment, 9 November 2026. This does not implement a
filesystem, accepted disk format, writable CLI, encryption or GPU execution.
Existing v1 bytes and all product paths are unchanged.

## Question and independent acceptance oracle

Compare recovery obligations of one/two-barrier COW publication before choosing
the writable format. One abstract opaque object is updated three times using
alternating recovery-root slots. A separate oracle retains submitted inputs
and durable acknowledgements. After a modeled crash, recovery must produce an
exact submitted version at least as new as the last acknowledgement and no
newer than the current candidate. A complete unacknowledged candidate is allowed.
Missing recovery, rollback past acknowledgement and fabricated bytes fail.

The oracle does not trust a root's declared generation as durability proof:
recovered bytes must match retained inputs. Four intentionally broken controls
must yield counterexamples so an always-passing checker cannot satisfy the test.

## Assumptions and scoped faults

- Start with an ALREADY durably initialized image. File creation, directory
  persistence, initialization publication and real OS storage are outside scope.
- A successful truthful flush persists all preceding pending writes. Durable
  bytes remain stable except within a currently written failure unit. A dishonest
  successful flush is a negative control, not an achievable software guarantee.
- Model 512/4096/8192-byte units. Correctly separated roots use different units;
  each immutable object version gets its own unit.
- At write/barrier/acknowledgement checkpoints enumerate whole-write subsets,
  each individual torn prefix/suffix in both application orders, plus destruction
  of a pending write's ENTIRE unit, including collateral bytes.
- This is not every simultaneous torn-write combination, arbitrary corruption,
  all input bytes, concurrent transactions, GC/reuse, namespace changes or
  physical power failure. Repeated equivalent cases are counted as executions,
  not unique states or a probability estimate.
- At most two pending 128-byte writes, three 64-byte versions and 40,960 image
  bytes. Cap execution at 60,000 cases and a checked 20-second wall budget;
  CTest adds 30 seconds. Image bytes stay in process RAM. Builds/logs remain
  confined to disposable hosted machines under the owner's restriction.

The small test-only envelopes have little-endian fields, canonical offsets,
generation binding, reserved-field checks and CRC32. CRC32 only exercises
torn-record rejection; it is NOT authentication or a selected production hash.
Model magic is distinct from .flfs; never implicitly migrate or expose it.

## Candidate protocols

1. Flush data, publish root, flush root, acknowledge; validate dependencies
   independently on recovery.
2. Write data plus root, flush once, acknowledge; FULL dependency validation
   and fallback to a prior valid root during recovery.
3. Two barriers as (1), but validate only root metadata before selection.

Test these rather than assuming an extra barrier is always necessary or fewer
barriers are free. Candidate (2) can require more recovery work. The finite
model cannot prove a general filesystem protocol or choose an efficient policy
for large dependency graphs.

Negative controls must detect acknowledgement before the root flush, dishonest
flush, one-barrier publication WITHOUT validating unpersisted dependencies, and
roots colocated in one failure unit. These are deliberately broken model
variants, not reported historical FlitzFS product bugs.

## Sources, provider restoration and next gates

The source-reviewed design in RESILIENT_STORE_DESIGN.md identified ordering,
CPU recovery, generation binding and root failure domains. New native research
at 740a12dff0cea9e42b16be973959063ed3b6ead4 adds details from
[Linux fsync](https://man7.org/linux/man-pages/man2/fsync.2.html) and
[SQLite WAL](https://www.sqlite.org/wal.html). File flushing alone does not ensure
a Linux directory entry persists. SQLite documents that NORMAL synchronous mode
can roll back transactions after power loss; comparisons must name durability.

The [hosted research run](https://github.com/strmt7/FlitzFS/actions/runs/42599540602)
used reviewed FlitzZip commit 4e6a1bb0d144611a09dcbc65276faa676e923882,
provider 0.9.5+flitzzip.portable2, its unchanged hash lock, 45-second page
deadlines, robots checks, no external LLM/persistent profile and explicit
3-GiB/two-CPU/256-task cgroup limits. Native captures/failures are retained for
30 days. This restores actual provider use off-host; no owner-PC process ran.

Five native source captures passed scope checks. Relevant Windows/OpenSSL API
bodies still need deeper review: their initial automatic excerpts were
navigation/copyright material and are NOT used to select crypto or assert
Windows namespace durability. Compact receipts are retained in
evidence/hosted-durability-research-740a12d.json.

Next: select a concrete journal/object format and integrity scheme, implement
an independent parser and bounded storage interfaces, then acknowledged-history
fault tests before attaching ordinary-file I/O. File/name durability, full disk,
process/power-loss tests and mounted writes remain separate gates. No sole-copy
storage or writable mount is enabled by this experiment.

## Validation state

Hosted Windows/Linux/raw/codec/sanitizer results are pending at initial
publication. Passing this model will not establish actual filesystem durability
or physical power-loss survival.
