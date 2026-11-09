# Native compressed-store experiment and corrective optimization

The first concrete compressed-transaction checkpoint is
56475daedbfc4b0c96683cf1faeaff5414479876. Its hosted
[comparison run](https://github.com/strmt7/FlitzFS/actions/runs/42614068539)
completed 90 rotating samples, six synthetic 4 MiB corpora, the pinned raw
baseline, current raw mode and automatic levels 1/6/9. All bytes were checked.
Two complete raw images matched the previous engine byte-for-byte and were
independently reopened by both readers.

This is a RAM-backed object-engine experiment. It does not measure physical
flushes, storage-device allocation, mounted filesystem I/O, steady state or
general file workloads. Creation includes two root zones and a sealed initial
segment. Physical image END counts those bytes, manifests and transaction
padding, but is not the OS's allocated-block measurement. Read/write times
include their specified CPU validation. No owner-PC process ran.

## Observed initial results

All four-object logical inputs were 4,194,304 bytes. Raw images were 4,456,448
bytes. Automatic images used 262,144 bytes for zeros/uniform/motif/numeric data,
327,680 for synthetic text, and 4,456,448 for noise. These synthetic reductions
do not establish a compression frontier or superiority to other filesystems.

Median automatic-level-6 writes versus raw mode (milliseconds):

| Synthetic corpus | Raw | Automatic 6 |
|---|---:|---:|
| Zeros | 12.56 | 12.97 |
| Uniform byte | 12.53 | 30.25 |
| Repeated motif | 12.56 | 33.07 |
| Numeric | 12.50 | 17.54 |
| Text | 12.37 | 18.77 |
| Noise | 12.68 | 17.09 |

Uniform/motif reads also slowed from about 7.9 to 17.0 milliseconds. Compression
reduced bytes while increasing CPU cost; no overall speedup is claimed from
this experiment. Three samples on one hosted VM are not broad statistical
qualification. Per-policy raw results and exact source/artifact identity are in
evidence/native-store-comparison-56475dae.json. Full samples remain in the
30-day hosted artifact (expiry 9 December 2026).

## Mechanism under correction

The pattern decoder and full-input pattern check performed a division/modulo
operation for each byte. The uniform/motif regressions motivated replacing
these loops with bulk operations:

- Verify full periodicity by comparing the input with its period-shifted prefix.
  Every byte after the first motif is still checked; there is no sample-only
  equality assumption.
- Materialize one-byte motifs using fill. Other motifs start with their original
  bytes, then repeatedly copy an initialized prefix, at most doubling it.
  Each copy length is <=the initialized prefix, so copy ranges do not overlap.
- Share this CPU helper with the internal extent mechanism decoder. Its contract
  still requires disjoint input/output and preserves the existing bytes.

Guard-byte tests cover zero/short/boundary outputs and motif lengths through
16,385 bytes, alongside existing malformed-input and compression fault tests.

The expanded hosted comparison builds BOTH pinned raw baseline 5c7d7a0a and
pre-optimization compressed engine 56475dae. It alternates prior automatic level
6 with current raw and automatic levels 1/6/9 on the SAME VM. It records active
payload separately from physical image end so alignment cannot hide codec
differences. Results must be inspected at the new exact commit; this description
does not assume the optimization wins before execution.

## Remaining product evidence

These experiments leave real filesystem namespace semantics, large-file extent
mapping, writable reopen/reclamation, interruption testing, broad holdout
corpora, multiple relevant comparator filesystems and actual AMD hardware work
unqualified. Standard benchmark suites with equivalent durability/resource
settings belong near product qualification on disposable machines.

At 56475daedbfc4b0c96683cf1faeaff5414479876, all five triggered workflows
passed, including the six-job CPU/native/mount pipeline, source evidence,
offline HIP compilation, independent mechanism checks and the RAM comparison.
Linux raw/codec/sanitizer lanes passed 21 tests each; Windows minimal passed 24,
native passed 26, and the synthetic mounted read/termination/recovery probe
passed. These checks do not qualify mounted writes or physical power loss.
