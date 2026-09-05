# Third-Party Components

Everything ReconOS uses that it did not write, and what each is used for. This
is the source of truth for the credits shown in the system itself.

The line ReconOS draws: libraries may parse formats and talk to hardware.
Everything above that — layout, drawing, window management, the shell — is
written here.

## Vendored

Included directly in the repository under `third_party/`.

### stb_image

- **Author:** Sean Barrett and contributors
- **License:** Public domain (dual-licensed MIT)
- **Source:** https://github.com/nothings/stb
- **Used for:** decoding wallpaper and icon files into pixels.

### stb_truetype

- **Author:** Sean Barrett and contributors
- **License:** Public domain (dual-licensed MIT)
- **Source:** https://github.com/nothings/stb
- **Used for:** rasterizing font glyphs. ReconOS does its own glyph caching,
  text layout, and drawing on top; stb_truetype only turns a character into a
  coverage bitmap.

### minimp3

- **Author:** lieff and contributors
- **License:** CC0 1.0 (public domain)
- **Source:** https://github.com/lieff/minimp3
- **Used for:** decoding MP3 into samples. `minimp3.h` and `minimp3_ex.h`; the
  `_ex` layer builds an index on open, which is what makes seeking land on the
  right sample rather than on a guess from the average bitrate.

  Squarely on the "may parse formats" side of the line above, and the same
  category as stb_image: it turns a file into numbers and does nothing else.
  ReconOS keeps it behind `src/recon_codec.c`, which is also where the WAV
  decoder is -- and that one is written here, because WAV is a header and then
  the samples.

  Writing an MP3 decoder here was considered and rejected. The format is a
  hundred pages of psychoacoustics, and a decoder that is ninety-five per cent
  correct does not sound nearly right, it sounds broken.

## System libraries

Linked at build time, not distributed with ReconOS.

### ALSA

- **License:** LGPL-2.1
- **Source:** https://www.alsa-project.org/
- **Used for:** playing sound. It is the lowest thing on Linux that is still an
  interface -- a thin layer over the ioctls a driver exposes -- which is why it
  was chosen over a sound server: the shape of `include/recon_audio.h` is close
  to the shape a real driver has, so replacing it when ReconOS has one of its
  own is replacing a file rather than rethinking an abstraction. A sound server
  would have been easier to get working and would have put a daemon, a protocol
  and a mixing policy inside the thing being replaced.

  This is the "talk to hardware" half of the line above. Optional at build
  time: without it ReconOS builds and says it cannot play anything, which is
  the same path a machine with no sound card takes.

### mbedTLS

- **License:** Apache-2.0
- **Source:** https://github.com/Mbed-TLS/mbedtls
- **Used for:** the encryption on the network control port -- the TLS protocol
  itself, certificate parsing and writing, and the primitives underneath.
  ReconOS keeps it behind `src/recon_tls.c`; nothing else in the tree includes
  an mbedtls header.

  This is the second place the line above applies. TLS is a wire format that
  other people's clients have to understand, so it is parsing, not policy.
  The policy -- no certificate authority, an identity the machine asserts for
  itself, a fingerprint a person checks once -- is in `include/recon_tls.h`
  and is ReconOS's.

  Version 2.28, the long-term-support branch. 3.x moved several of the calls
  used here.

### wlroots

- **License:** MIT
- **Source:** https://gitlab.freedesktop.org/wlroots/wlroots
- **Used for:** the compositor foundation — DRM/KMS output, input devices,
  the Wayland protocol implementations, and the scene graph.

### Wayland

- **License:** MIT
- **Source:** https://gitlab.freedesktop.org/wayland/wayland
- **Used for:** the display server protocol and its reference implementation.

### libxkbcommon

- **License:** MIT
- **Source:** https://github.com/xkbcommon/libxkbcommon
- **Used for:** turning key codes into characters according to the keyboard
  layout.

### Pixman

- **License:** MIT
- **Source:** https://gitlab.freedesktop.org/pixman/pixman
- **Used for:** the damage region arithmetic that decides what to redraw.

## Assets

### Wallpaper

`assets/wallpaper.jpg` is a placeholder photograph, not an original work, and
should be replaced before ReconOS is distributed.

### Fonts

No font is bundled. ReconOS loads one from the host system at runtime, so no
font license applies to this repository. `RECONOS_FONT` overrides the choice.
When ReconOS ships its own font, it belongs here.

## Cursor themes

No cursor theme is bundled either; the system theme is used, overridable with
`RECONOS_CURSOR_THEME`.
