# Objective FlitzFS benchmark protocol

Benchmark protocol and measured Windows pilots through 23 November 2026.
Use existing recognized tools where they measure the required property. Custom
tests fill explicit gaps; they do not replace an available standard workload.
All host execution follows current local-development authorization and protects
existing data, driver/system state, resources and owned-process retirement.

The [revised architecture](architecture/TARGET.md#8-compare-the-whole-product)
adds independent complete-size and operation-order qualification. DiskSpd 2.3
changes default core placement and I/O buffer separation. Use the same version
and explicit affinity, cache and data controls across paired trials.
[DiskSpd 2.3 release notes](https://github.com/microsoft/diskspd/releases/tag/v2.3)

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
[the review receipt](evidence/release-plan-review-2026-11-15.json). The Windows
pilot rechecked the DiskSpd and lzbench releases and verified their archive
hashes before running them.
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

### Plain and native-compressed NTFS

Prepare equivalent disposable targets with the same Windows build, backing,
capacity, allocation unit geometry and documented cache/flush settings. Check
that both handles identify NTFS volumes and that the compressed target supports
per-file compression. Use a freshly created fixture directory for each arm.

1. **Plain NTFS.** Require `FSCTL_GET_COMPRESSION` to return
   `COMPRESSION_FORMAT_NONE` for the fixture directory and its completed files.
2. **Windows' built-in NTFS compression.** Apply ordinary `compact /C` to the
   empty fixture directory or set `COMPRESSION_FORMAT_DEFAULT` with
   `FSCTL_SET_COMPRESSION` on its handle. This is the built-in file and folder
   compression setting, using LZNT1. Create files underneath that directory so
   they inherit its state. For a workflow that preserves source attributes,
   verify the resulting mode before including it in the comparison.
3. **Verify both arms.** Before measurement and after all writes, flush and
   query each completed file with `FSCTL_GET_COMPRESSION`. The compressed arm
   requires `COMPRESSION_FORMAT_LZNT1`. Record the compression attribute,
   queried format, logical bytes and content digest. Reject the pair if native
   compression is unavailable or any file has an unexpected mode. Keep the
   setup and verification times distinct from measured task times.

[Microsoft compact documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/compact) ·
[File and directory compression state](https://learn.microsoft.com/en-us/windows/win32/fileio/compression-state) ·
[FSCTL_SET_COMPRESSION](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression) ·
[FSCTL_GET_COMPRESSION](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_compression)

Record baseline and settled volume allocation and the sum of per-file
`GetCompressedFileSizeW` results as different measurements. Keep identical
logical files, verification and metadata workloads. Confirm the requested
DiskSpd caching and alignment options work on every included target before
running that comparison. Alternate across comparators and report cold and warm
trials separately. The 22 November host inventory found neither matched mount
available. The 23 November NTFS guest run exercised FlitzFS backing-file
reclamation and CPU recovery. The native compression comparison remains open.
[File storage size API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getcompressedfilesizew) ·
[Host and image evidence](evidence/sealed-indexed-images-2026-11-22.json) ·
[Guest recovery evidence](evidence/dense-cross-group-2026-11-23.json)

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

## Public compression workloads

### 2026 AIT Data Compression Challenge

Use the complete canonical **AIT Data Compression Challenge 2026 (AITDCC 2026)**
for contemporary general-purpose lossless compression. Preserve the published
split of eight training files (A–H) and eight held-out files (I–P) in addition
to all 16 files together. The organizers publish the complete bytes, SHA-256
hashes, evaluation rules and results. Report FlitzFS results as an independent
evaluation. The challenge's codec results cover different resource limits and
accounting from a complete mirrored filesystem.
[Official benchmark](https://aitdcc.github.io/) · [Dataset and attribution](https://aitdcc.github.io/dataset.html) · [Ribeiro et al. 2026](https://arxiv.org/abs/2606.17712)

Stage the original files and three packing fixtures in a new development
directory. The script checks the size and SHA-256 of every file against the
[pinned catalog](../tools/dev/aitdcc2026-corpus.json). Run the bounded benchmark
processes through the normal Windows development runner.

```powershell
$corpus = 'aitdcc2026-corpus'
$profile = '<path to flitzfs_native_group_maintenance_tests.exe>'
python -B tools/dev/stage_aitdcc2026.py $corpus
& $profile --profile-public-files "$corpus/all.flitz-fixture"
& $profile --profile-public-files "$corpus/test.flitz-fixture"
& $profile --profile-public-files "$corpus/train.flitz-fixture"
```

The profile uses ordinary indexed file writes and runs the collective controller
with modeled idle opportunities. It compactly rebuilds both the pre-maintenance
and post-maintenance images, reopens them on CPU, and compares the complete files.
Report the image and selector sizes, including protected stored objects, index
and metadata. This fixture measures collective packing. The idle cold-codec
rewrite, persistent coherence catalog and mounted I/O have their own test paths.

For standalone codec controls, pass the individual original files to
[lzbench v2.4](https://github.com/inikep/lzbench/releases/tag/v2.4) with
`-elz4,0/zstd,3,12,19/brotli,9 -t0,0 -i1,3 -o4`. Use `-elzma,6` in a
separate run with the same iteration and output settings. Keep each member
as an independent input. Alternate order and report A–H, I–P and A–P totals. Charge
filesystem protection and container overhead in their own measurements.

### Revision and wider workload coverage

- Keep paired **CPython 3.15.0 and 3.15.2** files intact to test regrouping
  across versions, and use the larger single-release tree for namespace scale
  and build workflows. The receipt pins the two releases, deterministic
  selection, file order and hashes. Compare repeated CPU and AMD trials
  before trusting automatic GPU admission for the observed class.
  [CPython 3.15.0](https://www.python.org/downloads/release/python-3150/) ·
  [CPython 3.15.2](https://www.python.org/downloads/release/python-3152/)
- Keep **Silesia** as a historical continuity control. lzbench 2.4 publishes
  results using the same tar corpus. A recent tool release does not change the
  age or coverage of its example inputs.
  [lzbench published comparisons](https://github.com/inikep/lzbench/tree/v2.4)
- Consider **Squishy-2026** for a wider codec study. Its checked edition lists
  26 scored entries and 11.17 GB of scored bytes, with per-file hashes.
  The pinned citation file has a placeholder for the archival DOI. Pin the
  exact inventory and confirm attribution per source before using individual
  entries. Reserve any full-edition score for the complete roster.
  [Pinned Squishy-2026 inventory](https://github.com/JackDanger/squishy-corpus/blob/64313dfa884da4654d423462f538770dcbf04247/build/meta/edition.json) ·
  [Pinned citation file](https://github.com/JackDanger/squishy-corpus/blob/64313dfa884da4654d423462f538770dcbf04247/CITATION.cff)
- Add recognized workload traces after native DiskSpd comparisons. UL's
  **3DMark Storage Benchmark** replays Windows game and recording traces.
  **MLPerf Storage v3.0** supplies AI training, checkpoint, vector database
  and KV-cache lanes. Its reference harness generates synthetic training
  files. Use that lane for storage behavior and separately test compressibility
  with verified public checkpoint bytes.
  [UL storage traces](https://support.benchmarks.ul.com/support/solutions/articles/44002277782-storage-benchmark-tests) ·
  [MLPerf Storage v3.0](https://mlcommons.org/benchmarks/storage/) ·
  [MLCommons reference harness](https://github.com/mlcommons/storage)

### First Windows observations

On the complete AIT input of 38,289,319 bytes, the standalone lzbench controls
ranged from 23,663,675 bytes (LZMA 6) to 31,204,855 bytes (LZ4). Zstandard 19
used 24,294,750 bytes. The compacted FlitzFS image and selector used 51,617,792
bytes. The collective scan
published zero groups and wrote zero bytes. Independent CPU opens recovered
all files exactly, including the separate training and held-out runs.

On 24 complete CPython files (12 changed paths, each at both revisions), both
CPU and the AMD copy-advice walk reduced the compacted image from 1,302,528 to
770,048 bytes and recovered every byte. The AMD trials launched HIP and used
validated advice. Both routes reached the same image size in three pairs.
Median process time, including startup and readback, was 0.25 seconds on CPU
and 3.25 seconds with the explicit AMD route.

DiskSpd 2.3 ran system inventory on this Windows host. Mounted FlitzFS and NTFS
I/O measurements remain a separate test with equivalent cache, allocation and
backing settings.
[Measured runs and source receipts](evidence/aitdcc2026-compression-baseline-2026-11-22.json)

### Sealed image pilot

The [sealed format report](SEALED_INDEXED_IMAGE.md#verification-on-windows)
records complete-image and small-read costs for the AIT 2026 split on Windows.
The cold-packing pilot isolates backing-byte changes in the native engine.
Use mounted DiskSpd and ordinary file operations for comparative latency and
throughput.

### Idle cold compression and unit size

Run the same staged fixtures through the WinFsp SDK controller test with
`--screened-cold-packed <fixture>` to exercise automatic cold recompression.

The Windows callback fixture uses the ordinary idle controller with a paired
memory provider. Its compact-only reference for A–P
used 51,486,720 bytes in the image. About 51.09 MB were stored mirrored
payload records, including encoded metadata objects. About 0.40 MB covered
the rest of the image. The selector, advisory area and catalog occupy their
own ranges.

When cold work could keep its write allowance, the training split's selected
image used 15,441,920 bytes, against 15,998,976 bytes for compaction alone.
The held-out and complete corpora received a read-only cold screen and kept
their previous image. All cases passed independent CPU readback. The receipt
records catalog writes, simultaneous old and new allocation, and zeroing done
by the memory retirement model.
[Cold rewrite and allocation evidence](evidence/aitdcc2026-cold-rewrite-2026-11-22.json)

Standalone lzbench 2.4 with Zstandard 19 used these amounts on the same input
files. Each column uses independent blocks of the stated maximum size.

| Input | 64 KiB | 256 KiB | 1 MiB |
| --- | ---: | ---: | ---: |
| A–H training | 7,604,575 | 7,492,766 | 7,395,790 |
| I–P held out | 17,357,411 | 17,194,158 | 17,060,974 |

The 256 KiB and 1 MiB blocks cut held-out codec bytes by 0.94% and 1.71%.
Before adopting either size, compare whole selected images and small-range
read costs with equivalent protection. Current research on compressed file
layouts also treats grouping and read amplification as coupled decisions.
[FAST 2026 RubikFS](https://www.usenix.org/conference/fast26/presentation/huang) ·
[EROFS compression design](https://erofs.docs.kernel.org/en/latest/design.html)

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
