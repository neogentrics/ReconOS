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

## System libraries

Linked at build time, not distributed with ReconOS.

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
