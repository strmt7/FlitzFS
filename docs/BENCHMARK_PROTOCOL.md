# Objective FlitzFS benchmark protocol

Planning specification, 15 November 2026; no new benchmark result is claimed.
Use existing recognized tools where they measure the required property. Custom
tests fill explicit gaps; they do not replace an available standard workload.
All host execution follows current local-development authorization and protects
existing data, driver/system state, resources and owned-process retirement.

## Tools and roles

| Tool | Role | Important scope |
| --- | --- | --- |
| Microsoft DiskSpd | Sequential/random file I/O, queue depths, mixed read/write, latency and structured results | File targets only in disposable test volumes; never raw devices. Use features supported by every comparator. |
| fio | Reproducible job files, verification, mixed workloads, latency distributions and controlled data redundancy | Use a supported native Windows engine/build. Explicitly configure and disclose buffer reuse, compression and deduplication patterns. |
| WinFsp `fsbench` | Creates/opens/listing and other Windows filesystem operations | Use the current implementation against FlitzFS and baselines; preserve all results. |
| WinFsp `winfsp-tests`, `winfstest`, Windows FSX and applicable Microsoft IfsTest/HLK tests | Windows semantics, random read/write and mapped-I/O correctness | Correctness gates, not compression records. Faulty-driver/kernel campaigns use isolation, not the owner host. |
| lzbench | Matched in-memory CPU codec compression/decompression controls and exact readback | Raw codec result is separate from complete filesystem size; admit its whole-input memory requirement or partition every comparator identically. |
| Small FlitzFS-specific harness | Group discovery, protected dependencies, whole-image size, CPU/GPU fallback, migration, background interference and crash acknowledgement | These properties are not supplied by a throughput benchmark. Publish the gap, source and independent oracle. |

Current official release metadata checked on 15 November: DiskSpd **v2.3**
(4 September), fio **3.43** (23 September) and lzbench **2.4** (27 September).
The reviewed source pins and capture digests are in
[the review receipt](evidence/release-plan-review-2026-11-15.json). Recheck current
compatible versions before execution; this plan neither installs nor runs them.
[DiskSpd](https://github.com/microsoft/diskspd/tree/v2.3),
[fio](https://github.com/axboe/fio/tree/fio-3.43),
[lzbench](https://github.com/inikep/lzbench/tree/v2.4).

WinFsp published stable v2.2 on 19 November 2026. Use the checksum-pinned
v2.2 package and the matched correctness tools for that qualification lane.
The maintainer documents the WinFsp tests and the roles of Windows FSX and
IfsTest. [v2.2 release](https://github.com/winfsp/winfsp/releases/tag/v2.2) ·
[Testing strategy](https://github.com/winfsp/winfsp/blob/ada6de157df4fa99f5a866b6ac7af66918d6d838/doc/WinFsp-Testing.asciidoc).

The performance guide reviewed at v2.2B5 retains results from a Windows
11 21H2/WinFsp 2022 environment. Those timings are not current evidence. Its
best-of-three aggregation and asymmetric antivirus configuration are not this
protocol: keep distributions and equal security settings, with no owner-host
security changes or global cache purges.
[Guide reviewed for this limitation](https://github.com/winfsp/winfsp/blob/v2.2B5/doc/WinFsp-Performance-Testing.asciidoc).

## Comparison matrix

Run the same file workload, same corpus bytes, comparable resource envelope,
cache/durability semantics and security settings against:

- NTFS without compression and NTFS with supported native compression.
- ReFS where the Windows edition/configuration supports it; do not assume it.
- exFAT for meaningful common-feature throughput subsets, with its semantic
  differences explicit rather than treating it as an equal durability baseline.
- A current WinFsp passthrough/reference implementation to expose framework cost.
- FlitzFS CPU fallback, admitted GPU ingestion, optimized collective storage,
  and foreground I/O while each background activity runs.

Other OS filesystems can be secondary comparisons in an appropriately controlled
environment, but a different OS/cache/driver stack is a confound, not a fair
direct Windows ranking. Do not install unofficial filesystem drivers on the host.
Provisioning/formatting comparator volumes happens only in disposable isolated
test environments; it does not authorize formatting owner storage.

RAM-backed comparisons remain the development baseline. They measure engine,
namespace, memory movement and compression behavior; they do not establish SSD/
HDD performance, physical flush durability or device endurance. Physical-backend
qualification is a separate final gate, with the owner's existing storage
protected. Record unavailable comparators instead of manufacturing results.

## Corpus and workload design

Freeze a licensed, source-identified, SHA-256/BLAKE3-verified manifest before
testing. Use recognized compression corpora where suitable, plus held-out real
source trees, documents/PDFs, structured text/data, binaries, images/audio/video,
and bounded model/VM-data samples. Verify corpus licensing and current official
download provenance at execution. A long-established frozen corpus remains a
regression control, not sufficient proof of current desktop workloads.

Include exact duplicates, near-duplicate versions, insertions/deletions and
reordered content; dissimilar files; zeros/sparse ranges; already compressed
media; and high-entropy controls. Vary small/large files, directory skew and
file families. Separate training/tuning files from held-out files and report
per-family outcomes, weighted overall totals and adverse cases. Do not derive
weights after seeing which data wins. Stream/admit corpus sizes from current
host resources; never load the entire user filesystem or fill a drive.

Run data I/O, metadata and realistic workflows separately: small random reads,
sequential transfer, mixed read/write, create/open/list/rename/delete, sparse
growth/truncation, mapped I/O and representative application copy/open/save.
Test both single-threaded responsiveness and controlled concurrency. Directory
and extent scaling require populated named files/data, not only sparse sizes
or a million generic object keys.

fio documents that buffers may be reused unless requested otherwise and that
its compression controls synthesize mixtures of random/patterned bytes. Record
`refill_buffers`, patterns, deduplication/compression controls and seeds. Use
such patterns as labeled controls; real files determine realistic compression.
Never present zeros or repeating buffers as a typical-data compression record.
[Current fio controls](https://github.com/axboe/fio/blob/fio-3.43/HOWTO.rst).

## Measurements and anti-bias rules

1. Pin source, binary/tool/runtime hashes, parameters, seeds, corpus, Windows
   build and driver/adapter identities. Discover actual resources and record
   background contention; never assume one machine's specs.
2. Use identical source bytes and fresh uniquely owned outputs. Alternate or
   randomize comparator order, retain failed/slow trials, and repeat paired
   measurements. Start with a pilot and at least seven balanced pairs; increase
   samples when uncertainty can change the conclusion. Report median, tail
   latency and confidence intervals, not the best run. Do not call a difference
   inside host noise a win.
3. Distinguish decoder/model-cold, warm and filesystem/cache state. A RAM image
   does not simulate a cold physical device. Do not label caches purged when
   they were not. Keep comparable durability and supported I/O modes; for
   example, an NTFS-only bypass feature is not a cross-filesystem baseline.
4. Charge end-to-end GPU cost: setup, transfer, allocation, encoding/decoding,
   independent validation, publication and retirement. Show kernel-only timing
   separately. Use fresh per-trial owned process accounting; cumulative job
   peaks are not independent CPU/GPU memory comparisons.
5. Primary size is the complete storage footprint: data, metadata, coherence
   index, dictionaries/models, restart points, recovery protection, retained
   history and migration scratch. Also show compacted steady state and peak
   temporary space. A payload-only ratio is a secondary diagnostic.
6. Report read/write throughput, complete optimization time, small/random and
   whole-file p50/p95/p99 latency, read amplification, CPU/GPU time, RAM/VRAM,
   background interference and logical bytes rewritten per input/saved byte.
   RAM write counters do not measure actual SSD wear.
7. Verify every output byte through the independent CPU reader, including with
   GPU unavailable. Exercise corruption, interrupted regrouping/migration and
   restart separately from speed trials. Never trade integrity for a benchmark.
8. Publish all corpus families and configurations, including losses and skipped
   tools, exact manifests and raw structured logs. Compare strong high-ratio
   CPU codecs/solid grouping at practical settings as well as fast baselines.
   A record claim must name the exact dataset, rules, resources, access cost and
   accounting scope; one win cannot establish superiority on arbitrary files.

The methodology supports the primary objective of maximum net compression.
Latency and rewrite cost describe practical operating points rather than being
used to hide smaller representations. Background admission controls when work
can run; they do not permanently cap group scope or eliminate ambitious codecs.
