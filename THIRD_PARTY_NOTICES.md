# Third-party notices

Apple Squeezer Intel v1.0-rc2 is derived from and statically links open-source
software. The complete corresponding source, pinned submodule revisions, and
build instructions are provided by this repository and release tag.

## Squeezelite

- Project: Squeezelite, upstream revision 1595
- Copyright: Adrian Smith (2012–2015), Ralph Irving (2015–2026), and contributors
- License: GNU GPL version 3 or later, with the OpenSSL linking permission in
  `LICENSE.txt`
- Source: <https://github.com/ralph-irving/squeezelite>

Apple Squeezer changes the CoreAudio output, decoding, resampling, DSP,
deployment, diagnostics, and validation paths. It remains a Squeezelite-based
player and retains the upstream copyright and license notices.

## FLAC and Ogg FLAC

- Projects: libFLAC and libogg
- Copyright: Xiph.Org Foundation and contributors
- Licenses: the applicable BSD/Xiph and related notices included under
  `third_party/flac/` and `third_party/ogg/COPYING`
- Sources: <https://github.com/xiph/flac> and <https://github.com/xiph/ogg>

## SoXR

- Project: SoX Resampler library
- Copyright: SoXR authors and contributors
- License: GNU LGPL version 2.1
- Source and full license: `third_party/soxr/` and
  `third_party/soxr/COPYING.LGPL`
- Upstream: <https://github.com/chirlu/soxr>

## Apple Lossless Audio Codec

- Project: Apple Lossless Audio Codec (ALAC)
- Copyright: Apple Inc.
- License: Apache License 2.0
- Source and full license: `third_party/alac/` and `third_party/alac/LICENSE`
- Upstream source used by this build: <https://github.com/macosforge/alac>

## dsd2pcm and DSD support

- dsd2pcm copyright: Sebastian Gesemann (2009, 2011)
- License: BSD-style license in `dsd2pcm/LICENSE.txt`
- The Squeezelite DSD path also contains work attributed by upstream to the
  Daphile Project (2013–2017); its notice is preserved in `LICENSE.txt` and
  `README.md`.

Apple, macOS, CoreAudio, Chord, Mojo, Logitech, Squeezebox, and other names are
trademarks of their respective owners. Their mention describes compatibility
only and does not imply endorsement.
