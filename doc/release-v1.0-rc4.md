# Apple Squeezer Intel v1.0-rc4

Apple Squeezer Intel is a Squeezelite-derived player for Intel Macs using
CoreAudio. This release candidate targets x86_64 macOS 11 or newer and is
distributed through the Apple Squeezer Intel LMS plugin (v1.0-rc9, manifest
1.0.9). The binaries carry an ad-hoc code signature; they are not notarized.

## Changes since rc3

- Hardware volume and mute are written only when they are not already at
  unity, and before the Audio Unit starts. The Chord Mojo mutes a running DoP
  stream on any USB control write; PCM was unaffected, which is why FLAC played
  while DSD was silent with a white LED.
- A hardware-rate change is no longer given up after 200 ms. The request gets a
  500 ms settle window, is repeated once, and is finally made through the
  stream's physical format (`kAudioStreamPropertyPhysicalFormat`). A device
  that already runs at the requested rate is not written at all.
- A refused rate no longer triggers the shared Float32 fallback, which would
  fail the same way; the error names the cause when another application is
  running audio on the device (`kAudioDevicePropertyDeviceIsRunningSomewhere`).
- From the rc3 line: shared Float32 fallback when exclusive bit-perfect output
  cannot be satisfied (`strict` disables it), reopen retries backing off from
  1 s to 30 s instead of exiting, no `exit(1)` when the device is missing at
  startup, and the ALAC gapless rule that keeps the last packet of
  CoreAudio-encoded files.

## Validation

- Clean x86_64 build; `test-dsp`, `test-buffer`, `test-pcm` pass.
- Chord Mojo on musicplayer (macOS 12.7.6, Core i5-2435M), mode `dac-priority`:
  DSD64 as DoP at 176.4 kHz audible with white LED, 0 underruns, exclusive and
  physical format verified; FLAC 44.1 kHz and 192 kHz; ALAC.
- Offline DoP proof: the S32 stream written by `-o -` carries alternating
  0x05/0xFA markers on every frame and its 16-bit payload decodes with
  `dsdplay` (DSDIFF wrapper) to the same PCM as the source DSF within 0.01 dB.
- Not yet exercised on hardware: the physical-format route for a refused rate.
  It is reached only after the nominal-rate write fails twice; the earlier
  behaviour (backoff retry) remains as the last resort.

## Known limitations

- DSD is transported as DoP and only in modes that hold the device exclusively
  (`dac-priority`, `exclusive`, `audiophile`). Other modes convert DSD to PCM
  at 352.8 kHz; with the DSP or resampler active that path needs more CPU than
  a 2011 dual-core Intel Mac provides.
- Two players must not share one DAC. LocalPlayer's `squeezelite` on the same
  device blocks rate changes and, in exclusive modes, the open itself.
- The SqueezeDSP LMS plugin removes the native DSD conversion rules for every
  player; with it enabled LMS reports "Couldn't create command line for dsf
  playback" before this player is involved.
- Hashes of the signed binaries are published in `dist/SHA256SUMS.txt` of the
  release and in the plugin's `dist/SHA256SUMS.txt`.
