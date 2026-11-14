# Release readiness checkpoint — 14 November 2026

Stage: experimental prototype. The owner alone decides when to release.
No release, tag, package publication or version change is authorized.

Actual AMD GPU compression is now demonstrated through a native-file storage
operation, rather than only a standalone kernel. At a047952 the 65,536-byte
synthetic extent produced an 8,459-byte GPU payload that was stored exactly
through the inode hook and recovered by the independent CPU reader. The prior
acknowledged snapshot remained readable. The complete core matrix and SDK
qualification passed. [Retained source-bound evidence](evidence/native-lz4-composition-a047952.json)
includes artifact digests, current adapter/runtime admission, exact retirement,
restored working-set limits, zero remaining locked bytes and no owned processes.

That experiment used an explicit unmounted diagnostic. Ordinary Windows file
operations remain CPU-only. Its worker exchange took about 533 ms; active payload
equaled the CPU fixture result, and the complete image occupied 983,040 bytes
including history, metadata and alignment. There is no demonstrated end-to-end
speed or whole-image compression advantage. The unchanged trial must not be
repeated.

The native helper/editor/tray, bounded memory provider, native CPU storage and
namespace, and disposable-VM Windows mount/recovery fixtures exist within their
documented experimental scopes. They do not establish a production writable
filesystem or a completion percentage.

The new asynchronous LZ4 controller at f08e6b8 reuses bounded supervision and
evidence-aware recovery. Its terminal CPU selection does not wait for a worker;
late replies cannot replace selected CPU bytes. All 13 core jobs and full SDK qualification passed at that exact source.
CPU child fixtures are not actual GPU evidence.

Major unfinished release gates are:

- Complete ordinary Windows write/read integration with useful GPU compression,
  verified native encoding, independently tested immediate CPU fallback and
  exact worker/helper retirement.
- Whole-operation throughput and total stored-size gains on varied real data,
  including startup, transfers, validation, metadata, history and retained copies.
- Causal resolution of historical unconfirmed child retirement, plus complete
  promised Windows semantics, cancellation, shutdown and helper/driver lifecycle.
- Scalable bounded metadata and storage operations.
- Automatic resource governance, persistent workload learning and low-priority
  collective recompression/layout improvement with measured foreground impact.
- Acknowledged-write durability and recoverable corruption under an explicit
  fault model. Volatile RAM checkpoints and process-interruption tests do not
  establish physical power-loss survival.
- Physical/virtual volume creation integration, installer/update behavior,
  security review and independent filesystem comparisons.

## Development emphasis

The current direction has reached actual GPU-compressed native bytes and exact
CPU recovery. Complete the asynchronous failure/retirement and ordinary Windows
helper route before treating that as the requested file-operation slice.
Measure complete costs and gains; startup amortization is a real unresolved cost.
Do not remove CPU verification, resource limits, authentication or supervision to
make a benchmark appear faster.

Minimum total stored size remains the primary compression objective, subject to
practical retrieval latency and throughput. Later collective, learned or novel
cold-data compression must count models, dictionaries, indexes and redundancy,
retain independent lossless CPU recovery, and yield promptly to foreground work.
The initial LZ4 representation does not constrain that research or architecture.

Advance the full plan against explicit product exits. Assertion totals, document
volume and green infrastructure jobs are not product-readiness metrics.
Use recent primary-source research for concrete unresolved decisions, then
implement and validate the supported mechanism. Avoid unchanged research and
hardware reruns.

No reliable release date or completion percentage is established. This remains
substantial implementation and qualification work.
