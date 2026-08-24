# Plan C: native audiophile DSP

Status: approved for implementation

Implementation status: the C1-C6 feature set is implemented locally for the
Intel candidate. It includes the double-precision EQ and parametric graph,
response-derived headroom, ReplayGain ownership, true-peak protection, FIR
room correction with WAV/AIFF/REW-export import, fractional timing and spatial
tools, SoXR OSF/CSF modes, complete latency reporting, live validated JSON
updates with click-free swaps, rollback, and the Echo Classic control surface.
The remaining gate is C7 validation on Musicplayer and the physical Chord Mojo;
no physical result is claimed by the implementation-only milestone.

## Post-C6 hardening status

- The limiter now uses approximately 5 ms of stereo-linked lookahead, immediate
  attenuation, a 100 ms controlled release, and maximum gain-reduction and event
  telemetry. The 4x true-peak estimator still requires standards-corpus
  calibration before a standards-compliance claim.
- Loudness compensation now follows the effective LMS channel gain. Its
  low/high-frequency compensation increases as listening volume falls, while
  maximum configured compensation remains reserved by automatic headroom.
- This candidate still uses direct FIR convolution. Partitioned FFT processing,
  IR caching, and long-filter CPU acceptance tests are the next engine milestone
  before long REW filters are recommended for production use.

Plan C makes Apple Squeezer the audio-processing owner and keeps Echo Classic as
the control surface. Runtime playback must not depend on SqueezeDSP, a server-side
transcoder, or an intermediate lossy/encoded DSP stream.

## Non-negotiable behavior

- Existing `native`, `audiophile`, and `pcm-studio` configurations keep working.
- DSD/DoP never enters a PCM DSP stage. It is passed through or rejected with a
  clear reason when the selected mode requires PCM processing.
- The CoreAudio render callback performs no allocation, file I/O, configuration
  parsing, logging, or filter design.
- DSP configuration changes are prepared off the render path and swapped at a
  block boundary with a click-free ramp.
- Every processing mode reports the active signal path, source/output format,
  headroom, clipping, underruns, resampler state, and measured latency.
- “Bit-perfect” is reported only after the complete physical stream and volume
  path have been verified. Otherwise the UI says “direct path” and explains what
  remains unverified.

## Target signal path

```text
decoder -> PCM normalization -> preamp/headroom -> IIR EQ -> FIR/convolution
        -> balance/delay/width/crossfeed/loudness -> optional SoXR
        -> precision reduction/dither -> CoreAudio -> DAC
```

Inactive stages are true bypasses. DAC-priority mode bypasses the complete PCM
DSP graph, ReplayGain, fades, software volume, resampling, and dither.

## Delivery gates

### C1 — DSP engine foundation

- Replace the resampler-only processing dispatch with an ordered DSP graph.
- Use 64-bit floating-point working samples with saturating conversion at graph
  boundaries.
- Add immutable, versioned configuration and atomic block-boundary updates.
- Add preamp, automatic headroom, peak/clipping counters, bypass, flush, drain,
  and latency accounting.
- Add offline unit tests for silence, impulse, full-scale, channel isolation,
  bypass identity, clipping, and configuration swaps.

Acceptance: native/direct playback is bit-identical with all stages bypassed;
the decoder and CoreAudio threads contain no new allocation or blocking work.

### C2 — Native equalizer mode

- Add 12-band graphic EQ, parametric peak/notch, low/high shelf, and low/high
  pass filters using double-precision biquads.
- Smooth coefficient and gain changes to prevent zipper noise.
- Add global preamp, automatic headroom, per-stage bypass, presets, and response
  telemetry.
- Add a versioned JSON configuration file and atomic persistence.
- Add Echo Classic controls under Equalizer with an explicit DSP-owner selector.

Acceptance: measured magnitude/phase response matches the designed filters;
rapid preset changes are click-free; no clipping occurs with auto-headroom on.

### C3 — FIR and room correction

- Add partitioned FFT convolution with work performed outside CoreAudio's render
  callback and bounded queues into the output path.
- Import WAV/AIFF FIR filters and REW exports; validate channels, rate, length,
  normalization, delay, and checksum.
- Resample FIR coefficients offline when required and cache the result.
- Support stereo, dual-mono, and independent left/right filters.

Acceptance: impulse/null tests pass, declared FIR delay is included in LMS sync,
and filter replacement is click-free.

### C4 — Spatial, loudness, and timing tools

- Add fractional delay, balance, stereo width, mono, polarity, and configurable
  crossfeed.
- Add loudness compensation driven by the actual post-policy playback gain.
- Add contextual rules and gain-matched A/B slots.
- Include every stage's delay in a single LMS synchronization model.

Acceptance: channel/timing tests pass and synchronized players remain within the
agreed measured tolerance across sample-rate and mode transitions.

### C5 — DAC modes and advanced resampling

- `dac-priority`: source rate and direct integer PCM/DoP path where supported.
- `equalizer`: source-rate PCM through the native DSP graph.
- `osf`: curated SoXR VHQ oversampling with clock-family-aware target selection.
- `csf`: named linear, intermediate, minimum-phase, gentle, steep, and
  apodizing-style presets plus guarded expert controls.
- Apply dither only when real precision reduction occurs.

Acceptance: unsupported rates/formats fail safely; all physical stream fields
and device controls are verified and displayed; transitions are muted/ramped.

### C6 — Echo Classic integration and migration

- Put mode, sample-rate, filter, EQ, FIR, crossfeed, loudness, telemetry, and A/B
  controls inside Equalizer settings.
- Scope settings per LMS player and expose capability-based controls only.
- Import the supported subset of SqueezeDSP presets with a migration report.
- Keep a one-click SqueezeDSP fallback until native results are accepted.

Acceptance: UI/API tests pass; rollback restores the previous player binary,
launch configuration, Echo Classic files, and DSP-owner setting.

### C7 — Audiophile validation and release candidate

- Run automated frequency response, impulse, phase, THD+N proxy, clipping,
  bypass-null, resampler, FIR, channel, and malformed-config tests.
- Run FLAC conformance and long-duration stability tests.
- Test real Chord Mojo transitions for 44.1/48 kHz families, high-rate PCM,
  sleep/wake, disconnect/reconnect, hog-mode loss, DSD/DoP, underruns, and LMS
  synchronization.
- Complete third-party notices, source-offer/license packaging, operator guide,
  rollback guide, checksums, and signed/notarized release artifacts where used.

Acceptance: 24-hour playback has no crash, leak, unhandled device loss, or
unexplained underrun; measurements and known limitations ship with the RC.

## Release sequence

1. `Apple Squeezer Intel v1.0-dev`: C1 foundation and test harness.
2. `Apple Squeezer Intel v1.0-alpha`: C2 native EQ, opt-in through local configuration.
3. `Apple Squeezer Intel v1.0-beta`: C3–C5 engine complete and exposed in Echo Classic.
4. `Apple Squeezer Intel v1.0-rc2`: current release candidate; C6 migration plus
   C7 automated and Mojo validation must be complete before publication.
5. `Apple Squeezer Intel v1.0`: only after rollback, licensing, stability, and
   listening/measurement acceptance are signed off.

## First implementation slice

The first coding slice is C1 plus the minimum C2 path: DSP graph interfaces,
double-precision PCM conversion, preamp/headroom, one reusable biquad engine,
telemetry, configuration parsing, and offline tests. It deliberately excludes
FIR and Echo Classic UI until the real-time and bypass guarantees are proven.

For development builds, native DSP is enabled with `-Q`. Parameters are
semicolon-separated, for example:

```text
-Q preamp=-2;headroom=auto;lowshelf=90:0.707:2;peak=2800:1.0:-1.5
```

The `eq` value contains 12 comma-separated gains for 31.25, 62.5, 125, 250,
500, 1000, 2000, 4000, 8000, 12000, 16000, and 20000 Hz. Supported parametric
filters are `peak`, `notch`, `lowshelf`, `highshelf`, `lowpass`, and `highpass`.
Gain filters use `frequency:Q:gain_db`; pass/notch filters use `frequency:Q`.

`-Q @/absolute/player.json` loads schema version 1 or 2. Version 2 adds
per-stage/per-band bypass, FIR import and normalization, spatial controls,
true-peak limiting, and ReplayGain-aware headroom. Required fields are
`version`, `player_id`, `bypass`, `preamp_db`, `headroom_db` (number or `null`
for automatic), `graphic_eq_db` (exactly 12 gains), and `parametric` (the
semicolon-separated filter expressions above). Persistence validates the full
replacement, fsyncs a temporary file, and retains the previous document as
`.bak`; rollback restores it and retains the rejected document as `.rejected`.

FIR preparation uses 512-frame partitioned FFT convolution. Version 2 may set
`fir_max_taps`, `fir_trim_db` (trailing coefficients relative to the impulse
peak), `fir_channel_map` (`stereo`, `swap`, `left`, `right`, or `mono`), and
`fir_latency_reference` (`peak`, `start`, or `center`). Telemetry publishes the
effective tap and partition counts plus a checksum of the prepared coefficients.

The Musicplayer manager exposes this stable command contract for Echo Classic:

```text
device NAME_OR_ID
dsp-status [--json]
dsp-get
dsp-apply < config.json
dsp-bypass on|off
dsp-rollback
```

Configuration replacement is validated by the same native parser used by the
player. The player watches the versioned configuration and queues a validated
replacement for an atomic block-boundary crossfade. The manager retains the
previous document for explicit rollback; mode changes that alter CoreAudio or
the resampling path still use a controlled player restart.
