# Apple Squeezer Intel v1.0-rc3

Apple Squeezer Intel is a Squeezelite-derived player for Intel Macs using
CoreAudio. This unsigned release candidate targets x86_64 macOS 11 or newer.

## Changes since rc2

- Correct and regression-test big-endian PCM conversion for asymmetric positive
  and negative 16-, 24-, and 32-bit samples.
- Use explicit S32-to-Float32 conversion for shared CoreAudio playback through
  the macOS mixer.
- Preserve the direct S32 integer and DoP path for manually selected exclusive
  modes.
- Make native shared access the default; exclusive access is never enabled
  implicitly by bit-perfect, EQ, OSF, CSF, or PCM Studio modes.
- Restrict DoP transport to modes that explicitly request exclusive access.
- Reopen the CoreAudio Audio Unit in the same controller pass when playback
  resumes from idle, avoiding an indefinite LMS `waitingToPlay` state.
- Retain device-format, volume, latency, underrun, overload, clipping, and
  rendered-frame telemetry.

## Validation

- Clean Intel CoreAudio build passed.
- PCM endian/signedness tests passed under AddressSanitizer and UBSan.
- Native DSP, buffer, plugin syntax, lifecycle, and packaging tests passed.
- The shared Float32 path was deployed to Musicplayer with a Chord Mojo;
  playback advanced with verified physical format and zero reported clipping.

This remains a test candidate. The binaries are not signed or notarized.
