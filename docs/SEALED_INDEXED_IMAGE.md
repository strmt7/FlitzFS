# Sealed indexed images

A detached, immutable FlitzFS image protects packed native data with checked
stripes. It keeps an exact CPU read and restore path while reducing the space
used by the mirrored payload layout. Its current entry point is
`NativeFilesystemSnapshot::create_sealed_indexed_copy`. Windows sessions
continue to use the writable indexed layout. A sealed image can be restored to
a new writable indexed image through `NativeFilesystemWriter::compact` or
`recompress`.

## Layout and interpretation

Use the explicit `NativeImageKind::sealed_indexed` read descriptor with an
admitted maximum physical size. All numeric fields are little-endian. The outer
extent must match the extent recorded in a valid root. The inner stream must be
an exact single-stream paged object store with the same identity. It presents
an indexed native namespace.

Two 128-byte roots start at offsets 0 and 65,536. The first stripe starts at
131,072. The write target supplies a qualified failure unit. The stripe unit is
the larger of that unit and 4,096 bytes. Supported units are powers of two
through 65,536 bytes.

| Root offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | `FLTXSP01` |
| 8 | 8 | Stripe unit |
| 16 | 8 | Inner stream length |
| 24 | 8 | Complete sealed length |
| 32 | 16 | Image identity |
| 48 | 48 | Reserved zeroes |
| 96 | 32 | BLAKE3 of bytes 0–95 |

Each stripe consists of up to eight data units, one descriptor unit and one
XOR parity unit. Only the final stripe may have fewer than eight data units.
The final logical unit is zero-padded. The parity unit is the XOR of all
data units and the descriptor. For `D = ceil(inner_length / unit)` data units,
complete length is `131072 + unit * (D + 2 * ceil(D / 8))`.

| Descriptor offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | `FLTXSD01` |
| 8 | 8 | Zero-based stripe ordinal |
| 16 | 16 | Image identity |
| 32 | 8 | Data unit count |
| 40 | 24 | Reserved zeroes |
| 64 | 32 per data unit | BLAKE3 of each full data unit |
| After unit digests | Through the last 32 bytes | Reserved zeroes |
| Unit size − 32 | 32 | BLAKE3 of the preceding descriptor bytes |

The reader verifies every fetched data unit. A small cache retains digests
from four verified descriptors. Recovery obtains the remaining units,
reconstructs the missing data or descriptor and verifies its digest before
returning bytes. An explicit audit rereads the roots and descriptors and
checks parity. A sound image can still be read if one root is lost. Two
intact roots must agree. The checksums detect damaged bytes and the parity
covers one lost unit per stripe. Recovered damage is reported through
`degraded_checkpoint`. The inner paged store keeps its checked metadata.

## Publication and restoration

1. Open the closed mirrored checkpoint. Admit a new identity and two separate
   empty backings for the packed workspace and sealed output.
2. Build the checked workspace with the same encoded objects, rebinding the
   native root identity. Verify it independently.
3. Write all stripes to the output. Flush, check all parity and compare the
   logical stream byte for byte to the workspace.
4. Write and flush each root in turn. Reopen the detached image and verify the
   native namespace. The original remains available throughout these steps.
5. Restore into a fresh mirrored target for writable operation. Keep the
   original selected until a future volume coordinator verifies and selects
   a candidate under its own recovered generation rules.

The fixed roots and immutable stripes avoid update-in-place parity after
publication. Separate physical failure domains or independent replicas are
needed for recovery after whole-device loss. Physical qualification must
establish the actual write-failure scope, flush persistence and backing
ownership. See the [storage execution plan](architecture/ADAPTIVE_STORAGE_EXECUTION.md#4-backing-provider-qualification).
[OpenZFS copy-on-write](https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Copy-on-write.html) ·
[Linux RAID parity consistency](https://docs.kernel.org/next/driver-api/md/raid5-ppl.html) ·
[Microsoft ReFS mirror-accelerated parity](https://learn.microsoft.com/en-us/windows-server/storage/refs/mirror-accelerated-parity)

## Verification on Windows

The test creates and reopens real indexed images on instrumented memory
backings. It recovered 109 individual root, data, descriptor or parity unit
losses across 4 KiB and 64 KiB geometries. Two lost data units in one stripe
failed closed. Of 51 interrupted write and flush cases, 43 detached outputs
had no selectable root and eight reopened with exact data. The prior image
remained recoverable in every case. The CPU restore test successfully appended
a write and reopened the result as a mirrored image.

| AIT 2026 files | Logical bytes | Mirrored image bytes | Sealed image bytes |
| --- | ---: | ---: | ---: |
| A–H | 13,136,308 | 15,998,976 | 10,240,000 |
| I–P | 25,153,011 | 35,635,200 | 22,523,904 |
| A–P together | 38,289,319 | 51,486,720 | 32,440,320 |

All nine Windows trials reopened every file byte for byte. For A–P, the
complete sealed allocation is 37.0% smaller than the already compacted
mirrored image. The change comes from protection and packing. Both images
use the same existing encoded file objects. On the pilot's small-read trace,
the verified descriptor cache lowered backing traffic from 53,272,576 to
36,024,320 bytes, versus 19,058,592 bytes for the mirrored image. The test
always reads that mirror first. The recorded elapsed times describe these
process-local runs. Mounted performance and physical durability require their
own measurements. Windows codec and raw-only suites passed 206 and 205 tests.
The focused ASan run passed the runtime preflight and both storage tests. A
separate ASan pass verified the complete AIT workload.
[Trial and source receipt](evidence/sealed-indexed-images-2026-11-22.json) ·
[AIT 2026 dataset](https://aitdcc.github.io/dataset.html)
