# Installed-runtime adapter identity diagnostic

Experimental, 11 November 2026. This diagnostic observes identity fields without
changing compute admission. It is separate from filesystem reads/writes, GPU
candidate replies and stored formats.

## Purpose

The owner-device decode probe passed HIP initialization, the properties query
and LUID comparison, then refused at the node-mask comparison. CPU bytes were
verified. Current FlitzZip device discovery records a nonzero properties LUID
without that additional node-mask requirement; its working GPU path does not
establish that FlitzFS's expected memory-budget node is correct.

The new mode reports the complete raw mask and LUID. It does not substitute a
guessed mask, infer a node from adapter order or authorize compute from a zero
or mismatched field. Correct admission still needs matched physical-node and
memory-budget evidence.

## Execution boundary

The internal parent mode is --inspect-driver-adapter WORKER ORDINAL LUID SHA256.
Its child mode is --inspect-system-adapter ORDINAL LUID SHA256 --worker-channel TOKEN.
Both are explicit diagnostics. Normal operations never invoke them.

Before loading HIP, the child validates arguments and the complete metadata
request, verifies the pinned installed System32 runtime with the existing
cached-only trust/byte checks, and admits the matched DXGI budget. It retains the
exact 128 MiB, 20 percent CPU and five-second worker job. The parent retains its
128 MiB per-process / 256 MiB aggregate limits and confirmed-exit exchange.

The child restricts module lookup, holds runtime file pins, calls hipInit and
hipGetDeviceProperties through the selected SDK's typed dynamic API, closes the
runtime before publication and returns only metadata. It requests no
hipSetDevice, priority query, stream, batch allocation, copy or kernel launch.
It loads no FlitzFS GPU kernel module and checks that absence before and after
the properties query. Runtime initialization can itself involve driver activity;
this is not a claim that no driver code executes.

The parent reports properties only after a successful confirmed exit and exact
frame validation. A failed request returns no frame or fabricated identity.
hip_initialization_confirmed=false means completion was not established; it does
not prove that a failing child never initialized HIP. Raw fields are observations,
not hardware attestation or permission to bypass a compute gate.

## Private frame layout

Request and reply are each exactly 40 bytes, little-endian.

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 8 | FLHID001 magic |
| 8 | 4 | Kind: request 1, reply 2 |
| 12 | 4 | Reserved, must be zero |
| 16 | 8 | Nonzero request ID |
| 24 | 4 | Ordinal, 0..63 |
| 28 | 4 | Request budget mask / raw observed reply mask |
| 32 | 8 | Request budget LUID / raw observed reply LUID |

Requests require a single-bit nonzero mask and nonzero LUID. Replies retain zero,
multibit or different observed fields for diagnosis. Request ID and ordinal must
match the parent's request. Wrong length, magic, kind, reserved bits or request
binding fails. The metadata format cannot pass the normal decode-reply acceptor.

## Qualification

Portable tests use independent golden bytes and malformed/cross-protocol cases.
Real Windows CPU helper tests exercise protected channel transfer, a zero mask,
failed-child empty output and unchanged handle accounting. Fake HIP API tests
prove that the identity-only query preserves raw masks without selecting a device
or querying stream priority. The SDK job retains all 17 existing admission cases
and adds a separate bad-runtime identity refusal with null observed fields.

Exact matching CI must pass before any owner diagnostic. Its artifact contains
only the existing own binaries and licenses, with no AMD runtime. Use the
qualified outer watchdog, current headroom and exact artifact/runtime hashes.
No driver install/update, filesystem mount, automatic retry, GPU speed or data
safety qualification follows from this diagnostic.

The qualified 7793100 properties-only owner run observed an exact LUID and raw
mask zero. See [GPU_NODE_PROOF.md](GPU_NODE_PROOF.md) for the subsequent explicit
OS proof and omission-only admission policy. This diagnostic continues to
report the raw mask without normalization.
