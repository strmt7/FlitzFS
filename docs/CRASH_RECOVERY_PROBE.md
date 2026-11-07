# Bounded process-crash and remount qualification

## Plan and scope — 7 November 2026

The real read-only mount passed its first hosted-Windows run 42384272839 at
`913b9e2f980a85b7564a23bd2b825ee7dae51f3c`. That proved normal operation and
orderly teardown in that synthetic test, not survival of abrupt termination.
This next test makes the latter scenario explicit without owner-PC access.

## Research rounds and design decisions

1. Microsoft documents that `TerminateJobObject` terminates member processes
   without letting them postpone termination. Do not simulate a crash by
   calling ordinary session stop or allowing C++ destructors to run.
2. Windows signals process termination and closes process-owned handles;
   other processes' surviving handles may still retain kernel objects. WinFsp
   describes crash cleanup via handle lifetime and create-only directory
   mounts. Test the behavior, do not assume cleanup because the API returned.
3. Separate the killed process from the recovery process. Require a readiness
   receipt written only after actual mounted reads, mapping and mutation
   rejection have passed. A helper killed before reaching that point must fail
   qualification, not be reported as a recovered filesystem.

## Implemented test sequence

The existing hosted-only executable now runs three **sequential** supervised
children. Every child verifies the same 128 MiB committed-memory, 20% CPU,
one-active-process Job Object before creating or reading its synthetic fixture.
No subagents or GPU workloads are launched.

- The normal child repeats mounted reads, EOF, mapping, enumeration, mutation
  rejection, concurrent reads and clean teardown (30-second supervisor cap).
- The crash child repeats those checks through the mounted/concurrent-read
  stage, writes a small synthetic compressed-byte snapshot and three-byte
  readiness receipt, then sleeps. Its supervisor forcibly terminates the job
  at 15 seconds and must confirm exit with the expected timeout status.
- A distinct recovery child uses only the PID returned for that launched
  child to identify its known synthetic fixture (no path argument). It checks
  readiness, observes mount disappearance for at most two seconds, verifies
  original synthetic data and the compressed-byte snapshot, and uses the CPU
  reader to decode/check all chunks. It then remounts the same backing data at
  the same now-absent directory, checks normal and mapped reads, and unmounts
  cleanly under a 30-second cap.

The subprocess result now exposes the launched PID for correlation. A PID is
not an authorization token or a persistent process identity. These fixture
lookups rely on a trusted, isolated runner. Exclusive creation rejects stale
name collisions; recovery never deletes stale mountpoints or repairs namespaces.
A readiness file is a synthetic execution receipt, not cryptographic attestation.
The fixture parent is pinned and checked as before. No user data is accepted.

## Limits and reporting

This tests termination of an otherwise responsive read-only filesystem after
its application reads complete. It is not a power cut, kernel hang, GPU reset,
writable transaction recovery, in-flight application cancellation, cross-user
ACL test, Windows 11 qualification, or a promise of no possible system freeze.
The data files and snapshot are not durable-write evidence; there are no writes
through the mounted volume. No cleanup deletes fixture data. The VM is discarded.

Normal Linux and Windows CPU unit tests remain mount-free. The initial live
crash/recovery assertions passed in run 42385143601. Later persistent-manifest
integration needs its own matching `windows-mount` result; prior successes do
not qualify subsequent changes.

## Primary references

- https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject
- https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process
- https://github.com/winfsp/winfsp/wiki/WinFsp-Design
- https://github.com/winfsp/winfsp/wiki/Frequently-Asked-Questions
- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/mount.c
