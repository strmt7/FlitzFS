# Bounded adapter identity

The experimental diagnostic supports --describe-amd-adapters before a compute
experiment. It uses Windows DXGI metadata rather than a registry layout,
performance-counter name, application installation or assumed HIP device order.

The diagnostic retains its existing self-imposed 128 MiB per-process,
256 MiB aggregate, two-process, below-normal and 20 percent CPU limits. It
inspects at most 64 adapters through EnumAdapters1 and GetDesc1. Enumeration
error, cap exhaustion, zero identity or duplicate selected identity refuses the
whole result. All COM references are scoped and released before reporting.

Only nonsoftware AMD adapters with nonzero dedicated video memory are returned.
An empty selected list is valid. The LUID is encoded from the actual eight-byte
Windows structure into the same byte order consumed by the worker. PCI vendor/
device IDs and dedicated capacity are numeric metadata; capacity is not free
memory or a reservation. The worker must still collect fresh matched budgets.

A returned dxgi_ordinal is not a HIP ordinal. A later explicit compute probe
must independently match its selected HIP ordinal's properties LUID and actual
node mask before device selection or compute. The reported probe_node_mask=1
is the initial single-node selector, not a claim to have queried a node count.
A mismatch selects no candidate and does not trigger another device search.

This discovery mode makes no HIP call, initializes no HIP context and performs
no GPU compute. It creates no filesystem backing or mount. Runtime trust/hash
verification and actual hardware qualification remain separate steps. Normal
filesystem operations still do not launch a diagnostic.

## Source and validation

Microsoft's current win32metadata snapshot
15fc870a5e96df7c560877ff30136c0fb5a21e9b is dated 6 November 2026.
Its generation/WinSDK/RecompiledIdlHeaders/shared/dxgi.h blob is
ca732a2232a195ee837eec168320fcb32f031525 (103895 bytes). The native repository
API verified the typed EnumAdapters1 and GetDesc1 declarations before this
change. The existing matched-budget collector already uses the same factory
and descriptor family and supplies the AMD/hardware/dedicated-memory policy.

The isolated Crawl4AI workflow additionally captures the current-served
Microsoft enumeration and adapter-description references, retaining their
native statuses, text, digests and publisher dates. A current fetch is not a
claim of a new document revision.

The complete-SDK lane must execute this mode without loading HIP and validate
all returned fields, uniqueness and resource accounting. It accepts a genuinely
empty AMD result rather than fabricating a test device. Inspect that matching
receipt before running the exact diagnostic on the owner PC. No GPU result or
speed follows from adapter metadata.
