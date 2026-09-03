/*
 * Reading settings.
 *
 * Not a skin. A skin is about colour; this is about whether text can be read
 * at all -- how far apart the letters and lines sit, and which typeface is
 * used. Someone who needs letters further apart needs that under every skin,
 * and someone using the high-contrast skin for low vision is not necessarily
 * the same person.
 *
 * What is here is what the evidence supports. Extra letter spacing is the
 * best-supported single adjustment for dyslexic readers (Zorzi et al., 2012);
 * line spacing and a font of the reader's own choosing follow. A typeface
 * marketed for dyslexia is *not* built in as a fix, because controlled studies
 * have not found it to beat an ordinary sans-serif -- but the font is settable,
 * so anyone who finds one helps them can use it.
 *
 * The remaining piece, warm off-white instead of stark white, is a colour
 * question and so lives in the "Reading" skin instead.
 */

#ifndef RECON_ACCESS_H
#define RECON_ACCESS_H

struct recon_font;

/* Where the settings live, in the user's hive. */
#define RECON_ACCESS_LETTER_KEY "accessibility/letter-spacing"
#define RECON_ACCESS_LINE_KEY "accessibility/line-spacing"
#define RECON_ACCESS_FONT_KEY "accessibility/font"
#define RECON_ACCESS_FONT_SIZE_KEY "accessibility/font-size"

/* The default height text is drawn at. */
#define RECON_ACCESS_FONT_SIZE_DEFAULT 14

/*
 * Read the settings and apply them.
 *
 * `font` may be NULL, in which case only the spacing is applied -- useful
 * before there is a font to reload.
 */
void recon_access_apply(struct recon_font *font);

#endif
