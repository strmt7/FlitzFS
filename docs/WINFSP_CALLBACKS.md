# Native read-only callback milestone (0.2.0)

## Plan and acceptance gates

1. Reproduce and repair virtual-name validation after the first dot.
2. Admit each container's serialized index against the remaining catalog budget
   before allocation; preserve format v1 and independent CPU decoding.
3. Implement actual WinFsp 2.1 callback signatures behind an OFF-by-default build
   option. Do not add a mount command, filesystem creation, dispatcher, driver
   installation, service, device writes, or GPU work in this milestone.
4. Test callbacks against the real SDK and DLL in disposable hosted Windows CI.
   Independently qualify CPU Zstandard there, not merely raw-only code.
5. Keep mount integration, Windows application compatibility, kernel failure
   behavior, encryption and HIP performance as separate unpassed gates.

## Architecture and resource contract

`WinFspReadOnly` owns an immutable `ReadOnlyProjection`, stable file metadata and
an owner-only security descriptor. `bind_callbacks` only sets `UserContext` in
an existing user-mode `FSP_FILE_SYSTEM` struct. Neither construction nor tests
call `FspFileSystemCreate`, mount-point assignment or dispatcher startup.

Backing data is verified before exposure, with an aggregate **32 MiB logical**
cap. Existing limits remain: 16 backing files, 240-character virtual paths,
100-character ASCII components, depth 16, and 4 MiB aggregate serialized index.
`Reader::open(path, index_budget)` rejects excessive index size **before** its
index buffer and entry array are allocated. This budget concerns serialized
index size, not a complete process-memory accounting limit.

There are at most 257 immutable nodes, 256 open contexts and two simultaneous
decodes. Each read is at most 4 MiB; each reader caches at most one decoded
chunk, at most 1 MiB. Excess read concurrency returns `STATUS_DEVICE_BUSY`;
a future dispatcher must have at most two workers. Handles use monotonically
assigned opaque tokens, preventing a stale close from closing a reused slot.
No contexts are released at Cleanup: paging reads can arrive before Close.

The volume parameters advertise read-only behavior, ASCII names, no hardlinks,
streams or reparse points, and per-open contexts (`UmFileContextIsUserContext2`).
All implemented mutation callbacks return `STATUS_MEDIA_WRITE_PROTECTED`.
Native positive-length reads at EOF return `STATUS_END_OF_FILE`; the portable
projection still intentionally returns an empty vector. Directory pagination
uses case-folded ASCII markers and the SDK's own record-packing helper.

The process user's SID is used to create a protected, single-ACE read/execute
DACL. This is **not** preservation of each source file's ACL, metadata encryption,
or proof of authorization behavior through a mounted Windows filesystem.
Timestamps represent adapter construction because format v1 stores no original
timestamps. Allocation sizes are logical sizes rounded to 4096, not compressed
backing-store allocation. File IDs are stable only within the adapter lifetime.
Destruction must occur after all callbacks stop; no implicit lifecycle worker
is started.

## Build and test

Normal builds stay CPU-only without a WinFsp dependency. To explicitly compile
callbacks on 64-bit Windows, provide an SDK layout containing:

- `include/winfsp/winfsp.h` and `include/winfsp/fsctl.h`
- `lib/winfsp-x64.lib` and `bin/winfsp-x64.dll`

```powershell
cmake -S . -B build-native -A x64 -DFLITZFS_BUILD_WINFSP=ON -DFLITZFS_WINFSP_SDK="C:/path/to/sdk"
cmake --build build-native --config Release --parallel 2
ctest --test-dir build-native -C Release --output-on-failure --timeout 45 --parallel 1
```

There is no mount operation in these tests. They use synthetic files in a fresh
unique temporary directory, and remove only their own enumerated fixture files.
`FLITZFS_REQUIRE_ZSTD=ON` makes a missing codec a configure failure rather than
silently qualifying a raw-only build. The Windows native CI lane also asserts
that its compressible fixture actually produced Zstandard chunks.

The SDK staging script refuses non-GitHub-hosted/non-Windows execution. It
limits the download to 4 MiB/60 seconds, checks the pinned release SHA-256, reads
MSI tables in read-only mode, and extracts files without executing the installer.
It selects only the two headers, import library and user-mode DLL. No driver
or service is installed or started. Extracted packages are not redistributed.

## Research rounds and decisions — 7 November 2026

**Round 1: ABI and lifetimes.** Read the official WinFsp native API and v2.1
headers for callback signatures, Open/Close context semantics, Cleanup lifetime,
security-size negotiation, directory markers and readonly flags. Chose direct
native callbacks rather than a new FUSE/ProjFS layer so contracts can be tested
without mounting.

- https://winfsp.dev/doc/WinFsp-API-winfsp.h/
- https://github.com/winfsp/winfsp/blob/v2.1/inc/winfsp/winfsp.h
- https://github.com/winfsp/winfsp/blob/v2.1/inc/winfsp/fsctl.h

**Round 2: security and dependency isolation.** Use Windows security descriptor
APIs rather than hand-encoding ACLs. Use MSI database mode 0 and cabinet
extraction, not installer execution. Preserve read-only workflow permissions,
full-SHA action pins, no persistent credentials, bounded build/test jobs.

- https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-string-format
- https://learn.microsoft.com/en-us/windows/win32/msi/installer-opendatabase
- https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/expand
- https://github.com/winfsp/winfsp/releases/tag/v2.1

WinFsp dependency: `winfsp-2.1.25156.msi`, SHA-256
`073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`.
Zstandard native-CI source: `meta/zstd` (formerly `facebook/zstd`), v1.5.7 commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`. These are verified pins, not a claim
that newer versions do not exist. Keep upstream licenses with any future
redistribution; this repository does not vendor the SDK binaries.

**Round 3: inspect implementation when prose is ambiguous.** The v2.1 header
comment calls MaxComponentLength a byte length, but `src/sys/volinfo.c` exports
it directly as MaximumComponentNameLength and directory code multiplies it by
sizeof(WCHAR). The adapter therefore advertises **100 characters**, matching
its validated namespace, rather than 200. This correction follows source
semantics, not the misleading comment.

- https://github.com/winfsp/winfsp/blob/v2.1/src/sys/volinfo.c
- https://github.com/winfsp/winfsp/blob/v2.1/src/sys/dirctl.c

Prior cross-repository lessons remain in `RESEARCH_2026-11-07.md`: use bounded
resources, differential parity tests, independent evidence, and no claims of
performance or security that the executed tests cannot support. No other user
repository was modified and no private repository source was copied.

## Validation scope

The new path regression failed before the fix and passed afterwards. Portable
core, catalog and projection tests passed locally in GCC/Zstd, GCC/raw-only and
Clang ASan/UBSan configurations. Native Windows qualification is determined by
the corresponding commit's `windows-native` Actions job, **not** by those Linux
results. A green callback job still does not qualify mounted I/O, memory-mapped
files, driver reset/shutdown recovery, Windows 11, personal-PC/GPU behavior,
cryptographic security, or writable durability.


## Current candidate migration

The historical v2.1 observations above are not today's dependency selection.
Current hosted development selects source/asset-bound v2.2B5 through
tools/winfsp-sdk-lock.json because the newer upstream prerelease lists important
security/deadlock fixes. Its exact Product.wxs preserves the four public SDK
component identities used here. Callback signatures and lifecycle must pass
fresh native tests before this candidate is called qualified. Beta status stays
explicit; the normal owner-PC/no-driver boundary is unchanged.
