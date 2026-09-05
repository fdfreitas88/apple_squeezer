# Apple Squeezer Intel v1.0-rc4

The product release name is **Apple Squeezer Intel v1.0-rc4**. LMS firmware
reporting retains the underlying Squeezelite protocol/source revision and adds
the product version suffix: `2.0-1595-apple-squeezer-intel-v1.0-rc4`.

This candidate tracks upstream Squeezelite revision 1595 and replaces the
PortAudio output path with Apple's AUHAL/CoreAudio APIs. It targets x86_64
macOS 11 or newer.

## Implemented audio path

- Native hardware sample-rate switching with read-back verification.
- AUHAL client-format and physical output-stream validation.
- LMS synchronization using CoreAudio device latency, safety offset, hardware
  buffer size, Audio Unit latency, and callback presentation timestamps.
- Device-list, default-device, alive-state, sample-rate, stream-format, and
  processor-overload listeners with automatic reopen attempts.
- `safe` (512 frames), `balanced` (256 frames), and `lowlatency` (64 frames)
  hardware-buffer profiles, clamped to the device's supported range.
- Underrun/processor-overload telemetry in the output log.
- Optional hog/exclusive access and fixed-volume bit-perfect processing.
- PCM, statically linked FLAC 1.5.0, Apple ALAC, and DSD-over-PCM (DoP).
- Native and chained Ogg FLAC decoding using statically linked libogg 1.3.6.
- Optional end-to-end FLAC MD5 integrity telemetry: set
  `SQUEEZELITE_FLAC_MD5=1` before starting the player. This adds CPU work and
  reports verification only after a complete stream; it is disabled by default.

DoP is transported as marker-framed PCM. The backend intentionally does not
claim native DSD because CoreAudio does not expose a portable native-DSD HAL
contract for USB DACs.

## Build

Initialize the pinned dependencies and build:

```sh
git submodule update --init --recursive
make -f Makefile.coreaudio-intel
```

CMake is required to build the pinned static FLAC library. The produced
`apple-squeezer-intel` has no Homebrew runtime dependencies.

Run the reproducible FLAC integration checks with:

```sh
tools/test-flac-coreaudio-intel.sh
```

They cover every legal FLAC PCM bit depth from 4 through 32 bits and verify
that native FLAC, Ogg FLAC, libogg, MD5, and decoder-finish support are
statically present in the Intel candidate.

## Modes

The deployment manager exposes five modes:

- `native`: CoreAudio native-rate output, balanced buffer.
- `bitperfect`: fixed-volume processing, strict physical-format checks,
  balanced buffer.
- `exclusive`: CoreAudio hog mode, balanced buffer.
- `audiophile`: fixed-volume plus hog mode, strict format checks, safe buffer.
- `pcm-studio`: VHQ linear-phase upsampling, shared access by default, 1 dB
  processing headroom, 24-bit TPDF dither, and a safe buffer.

PCM Studio uses the pinned, statically linked libsoxr engine at 28-bit
precision. It preserves clock families: 44.1 kHz sources are converted only to
44.1 kHz multiples and 48 kHz sources only to 48 kHz multiples, choosing the
highest synchronous rate the CoreAudio device reports. It is processed output
and therefore never claims bit-perfect operation. Native remains the default.

PCM Studio can also target an exact output rate. `auto` retains clock-family
matching; a manual rate converts all PCM sources to that rate when the device
reports it as supported. DoP bypasses PCM resampling.

```sh
tools/deploy-musicplayer-coreaudio-intel.sh upsample-rate auto
tools/deploy-musicplayer-coreaudio-intel.sh upsample-rate 192000
```

The installer preserves the configured CoreAudio device. Use `default` for the
current system output, or select a persistent name/ID explicitly:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh device default
tools/deploy-musicplayer-coreaudio-intel.sh install --device default
```

Native DSP configuration is versioned per player and controlled with:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh dsp-apply player-dsp.json
tools/deploy-musicplayer-coreaudio-intel.sh dsp-status --json
tools/deploy-musicplayer-coreaudio-intel.sh dsp-get
tools/deploy-musicplayer-coreaudio-intel.sh dsp-bypass on
tools/deploy-musicplayer-coreaudio-intel.sh dsp-rollback
```

`dsp-apply` validates the complete document before replacing the active file.
It retains the previous version and restores it automatically if the player
cannot restart with the new configuration.

Switch an installed candidate without reinstalling it:

```sh
tools/test-coreaudio-intel.sh mode native
tools/test-coreaudio-intel.sh mode pcm-studio
tools/test-coreaudio-intel.sh mode audiophile
```

Use `-a profile=lowlatency`, `-a profile=balanced`, or `-a profile=safe` for a
manual profile. Multiple options are separated with `:`, for example
`-a exclusive:bitperfect:profile=safe`. Additional candidate options are
`mode=native|pcm-studio`, `dither`, and `headroom=0..12`.

The output log periodically reports the signal path, hardware rate, buffer,
pipeline latency, processed frames, output starvation, HAL processor overloads,
automatic reopen count, and clipped samples. Inspect or validate it with:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh diagnostics
tools/deploy-musicplayer-coreaudio-intel.sh diagnostics --json
tools/deploy-musicplayer-coreaudio-intel.sh validate
tools/deploy-musicplayer-coreaudio-intel.sh mojo-report
```

## Test and rollback

Deploy to the configured Intel musicplayer:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh install
tools/deploy-musicplayer-coreaudio-intel.sh logs
```

The first install creates a rollback snapshot. Restore it with:

```sh
tools/deploy-musicplayer-coreaudio-intel.sh rollback
```

The Apple Squeezer player has a dedicated LMS MAC address and does not replace
the LocalPlayer Squeezelite binary, so both players remain selectable in LMS.

## Mojo validation checklist

For 44.1, 48, 88.2, 96, 176.4, and 192 kHz PCM, confirm that the log reports
the requested hardware rate and a matching physical stream. For DSD64, confirm
that LMS sends DSD and the log reports DoP at 176.4 kHz. Check that the Mojo's
sample-rate indication follows each transition and that the underrun count
does not rise during playback.

After the transition matrix passes, run a representative one-hour test with
`tools/deploy-musicplayer-coreaudio-intel.sh soak 3600`. Release candidates
should additionally complete a 24-hour run (`86400`) without new underruns,
HAL overloads, clipping, or a player exit. The soak command is observational:
it does not change the installed binary, mode, LMS playlist, or rollback state.

Once implementation is complete, collect every Musicplayer check and its recent
log in one timestamped report:

```sh
tools/run-musicplayer-test-suite.sh 300
```

The runner continues after individual failures so missing transition evidence
and health failures appear together. It saves the complete output under
`test-results/` and exits non-zero when any required section needs attention.
