# Installed runtime metadata verification

The SDK worker has an explicit internal --verify-system-runtime mode. It requires
its existing exact 128 MiB/20 percent CPU job and one expected SHA-256. It does
not load the subject DLL, initialize HIP, enumerate devices, mount a filesystem
or select the installed runtime for normal compute. Default operations remain CPU-only.

VerifiedSystemModule discovers the architecture-correct Windows system directory
through GetSystemDirectoryW. It accepts a bounded lowercase DLL basename rather
than an arbitrary path. The worker supplies the basename from its selected SDK
manifest. The metadata verifier requires 64 lowercase hexadecimal hash digits
before file access, at most 32 directory levels and a regular file of at most
32 MiB. It pins direct ancestors against deletion/rename and holds a strictly
read-only file handle that denies write/delete sharing for its entire lifetime.

SHA-256 is streamed through the same 64 KiB scratch used by private-bundle
verification. The existing DirectoryPin and hash implementation are shared;
the authenticated private SDK route retains its previous file identity checks.
The expected hash comes from the trusted supervisor, not the subject's own
declaration. Hash equality alone does not authorize the module.

Signature verification uses the current reference mechanism: no UI,
cached-only URL retrieval, the generic Authenticode policy and an unconditional
CLOSE after every VERIFY. Only zero from both calls counts as success.
Missing cached trust or any nonzero status refuses the metadata. It cannot
trigger an automatic search, network retrieval, driver change or retry.
Windows cryptographic provider resources remain subject to the child job cap.

## Qualification

The hosted SDK probe checks real Windows kernel32 metadata without changing or
loading that subject; kernel32 is already part of the process. Its expected hash
comes from an independent bounded native file read. The child exercises invalid
names/hashes, a missing module, byte-identity mismatch and repeated handle cleanup,
and checks that the AMD runtime remains absent.

Typed fake WinVerifyTrust calls independently check exact action/policy flags,
VERIFY/CLOSE sequencing, positive and negative LONG failures and close failure.
They perform no file or signature operation. These are CPU policy tests.

The existing six worker admission/refusal cases remain mandatory. The metadata
child and two worker metadata refusal cases bring the confirmed-exit requirement
to nine. Inspect the matching commit's SDK receipt before calling qualification
complete. This is not AMD execution or a GPU performance result.

## Source evidence and remaining work

Isolated research run 42954040457 at 32dc1fd90945a5afdf09b957e23db1c622de70e7
captured all six requested Microsoft references successfully through the reviewed
Crawl4AI provider, with native content, publisher metadata and SHA-256 receipts.
The archive digest is
21e8afd6ef54265f2660c53299446371c0dc1583156fb088150002dea552dbd5.

Current-served references are not newly revised documents: preserved publisher
update dates range from 2024 to 2026. The 2026 process-termination reference
confirms that pending I/O and exact process exit remain distinct from requesting
termination. Keep the supervisor's existing confirmation gate and the unresolved
earlier hosted timeout record.

The installed-driver compute route still needs its own exact-hash argument,
kernel-only bundle, restricted module loading and complete retirement tests.
Metadata verification supplies a prerequisite; it does not enable that route
or authorize an owner-PC experiment. See GPU_DRIVER_RUNTIME_PLAN.md.
