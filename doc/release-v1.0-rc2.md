# Apple Squeezer Intel v1.0-rc2

Apple Squeezer Intel is a Squeezelite-derived player for Intel Macs using a
native AUHAL/CoreAudio output path. This is an opt-in, unsigned pre-release for
x86_64 macOS 11 or newer.

## Highlights

- Native CoreAudio device and sample-rate switching with physical-format readback.
- DAC Priority, Equalizer, OSF, CSF, native, exclusive, audiophile, and PCM Studio modes.
- Exclusive/hog access, unity hardware-volume verification, latency accounting,
  device listeners, automatic recovery, and underrun/overload telemetry.
- PCM, FLAC (including Ogg FLAC and 4–32-bit conversion), ALAC, and DSD64 over DoP.
- Statically linked FLAC, Ogg, ALAC, and SoXR dependencies.
- SoXR VHQ resampling with automatic or manual output rate and named filter presets.
- Native versioned per-player DSP: 12-band and parametric EQ, automatic headroom,
  true-peak telemetry/limiting, loudness, crossfeed, FIR room correction,
  fractional delay, balance, width, mono, polarity, stage bypass, response data,
  atomic updates, and configuration rollback.
- One-command Musicplayer deployment, diagnostics, physical transition tests,
  soak tests, and automatic installation rollback.

## Validation status

- Clean x86_64 macOS 11-targeted build: passed.
- Native DSP unit suite: passed.
- FLAC conversion and static-link checks: passed in earlier candidate runs.
- Physical CoreAudio playback at 44.1, 48, 88.2, 96, 176.4, and 192 kHz:
  passed on the Chord Mojo with physical-stream readback.
- Native DSD64/DoP at 176.4 kHz: passed; the CoreAudio signal path reported
  `S32 DoP -> CoreAudio 176400 Hz` in bit-perfect mode.
- Automated endurance playback processed millions of frames with zero
  underruns, CoreAudio overloads, or clipped samples.
- Automatic failure rollback and standalone LMS plugin restoration: passed.
- A 24-hour endurance qualification has not been completed.

This release must therefore be treated as a test candidate, not as a stable or
fully audiophile-qualified release.

## Installation and rollback

From a source checkout on the controlling Mac:

```sh
git submodule update --init --recursive
make -f Makefile.coreaudio-intel
tools/deploy-musicplayer-coreaudio-intel.sh install --device default
```

Restore the pre-installation snapshot with:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh rollback
```

The release binary is not signed or notarized. See
`doc/coreaudio-intel-candidate.md` for modes, diagnostics, testing, and detailed
operating instructions, and `THIRD_PARTY_NOTICES.md` for attribution.
