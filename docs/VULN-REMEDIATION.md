# Apple Squeezer remediation report

This report records the remediation performed after the source-only audit in
`VULN-FINDINGS.md`. The original audit is intentionally preserved unchanged as
the review baseline.

## Formal findings

| Finding | Status | Remediation |
|---|---|---|
| F-001 | Fixed | ALAC table counts, allocation arithmetic, atom bounds and block indexes are validated. |
| F-002 | Fixed | DSD accepts only supported mono/stereo layouts and validates frame/block arithmetic. |
| F-003 | Fixed | WAV/AIFF fields are parsed into validated formats before conversion; unsupported channels are rejected. |
| F-004 | Fixed | Ogg comment output reserves its length prefix and validates nested packet lengths. |
| F-005 | Fixed | AIFF SSND offsets are bounded and ring advances use modular arithmetic. |
| F-006 | Fixed | SlimProto `setd` data is copied with an explicit packet bound and terminator. |
| F-007 | Fixed | Invalid/zero-length MP4 atoms are rejected and parser progress is enforced. |
| F-008 | Fixed | ALAC cannot become playable without a valid block-size source and complete sample tables. |
| F-009 | Fixed | PCM chunk lengths use checked arithmetic and reject zero-progress parsing. |
| F-010 | Fixed | Codec and resampler rate validation prevents zero divisors and invalid rate transitions. |
| F-011 | Fixed | HTTP headers reserve the terminator byte and return immediately at the limit. |
| F-012 | Fixed | SlimProto PCM indexes, widths, rates and alignment divisors are independently validated. |
| F-013 | Fixed | LMS ignores `SIGPIPE` locally, bounds helper execution and handles partial/failed DSP writes. |
| F-014 | Fixed | Crossfade pointers wrap modularly and mixing uses wide saturated arithmetic. |
| F-015 | Fixed | Every recognized SlimProto opcode has a minimum packet length before structure access. |

## Operational hardening

- CoreAudio render work uses a non-blocking lock attempt, emits silence on
  contention, and defers logging/recovery wakeups to the monitor thread.
- Hardware rate, buffer size, mute, volume and exclusive ownership are captured,
  verified and restored on close or failed open.
- Shared CoreAudio flags use atomic access; monitor creation and teardown are
  tracked; failed opens are retried without leaving an idle dead end.
- DSP configuration watching has a joined lifecycle. Configuration changes are
  prepared before use, invalid rate transitions preserve the last configuration,
  and long FIR response analysis uses prepared FFT partitions.
- PCM conversion covers arbitrary 4–32-bit signed input plus unsigned WAV 8-bit,
  both byte orders, without undefined shifts.
- Process-buffer allocation failures remain retryable; resampler failures cannot
  replay stale output; fades and headroom reject non-finite/zero-duration states.
- LMS process start/stop/restart is asynchronous, health-aware and supervised
  with bounded exponential backoff. Settings changes roll back on immediate
  lifecycle failure, helper processes have time/output limits, logs are bounded,
  and disconnected saved DACs remain selectable.
- The standalone plugin package stages the exact engine and DSP helper produced
  by the current Intel build.

## Automated evidence

- Intel macOS 11 target builds successfully with the native FLAC/Ogg, ALAC,
  SoXR, DSD/DoP, CoreAudio and DSP paths linked.
- Buffer wrap/resize, PCM conversion, native DSP and malformed DSP configuration
  tests pass.
- FLAC conversion passes for every input depth from 4 through 32 bits and the
  static Ogg FLAC linkage check passes.
- Buffer, PCM and DSP suites pass with AddressSanitizer and
  UndefinedBehaviorSanitizer.
- Clang Static Analyzer reports no remaining arithmetic, memory-safety or
  uninitialized-value finding in the release target. Its remaining diagnostics
  are conservative reports for non-blocking socket reads while the existing
  stream mutex is held.
- LMS Perl modules, command surface, lifecycle persistence, bounded helper
  behavior, manifest and bundled x86_64 executables pass `tools/check-plugin.sh`.
- The generated standalone ZIP passes a complete archive integrity test.

## Physical validation still required

Automated local tests cannot prove DAC behavior. Before publication, run the
consolidated Musicplayer/Mojo test to verify physical stream formats, DoP marker
transitions, disconnect/reconnect, sleep/wake, exclusive-access recovery,
underruns, LMS synchronization and an endurance soak on the target Intel Mac.
