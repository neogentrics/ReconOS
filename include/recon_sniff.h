/*
 * What a file actually is, read from its first bytes.
 *
 * ReconOS decides what a file is by its name everywhere else, which is right:
 * a name is what somebody chose and is the thing they can change. This answers
 * the other question -- what the bytes say it is -- and the two disagreeing is
 * worth telling somebody about.
 *
 * --- What this can and cannot honestly answer ---
 *
 * It can say **what format a file is in**, when the format begins with
 * something that identifies it. That is evidence, not a guess: a file starting
 * with the eight bytes PNG defines is a PNG, or was made by something
 * pretending to be one, and either way an image viewer will read it as one.
 *
 * It cannot say **whether a particular program will open it**. Only the
 * program knows that, and nothing here asks it. So `recon_props_claims` still
 * decides that question by the declared extension list, and this is used to
 * improve what the warning *says* rather than to replace the decision.
 *
 * It also cannot identify a format with no signature. Plain text has none --
 * it is recognised by the absence of anything else, which is why
 * RECON_FORMAT_TEXT is decided last and is the weakest answer here. CSV, Markdown
 * and a shopping list are all text, and no amount of looking will separate them
 * from each other; that is a real limit and not a gap to be filled later.
 *
 * Kept out of recon_props for the reason recon_expr is kept out of the
 * Calculator: it touches no screen and can be asked questions with known
 * answers.
 */

#ifndef RECON_SNIFF_H
#define RECON_SNIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How many bytes are worth reading to decide. Every signature here lives in
 * the first few, and the text test wants a run long enough to be convincing
 * without reading a whole film.
 */
#define RECON_SNIFF_BYTES 512

enum recon_format {
    RECON_FORMAT_UNKNOWN = 0,

    RECON_FORMAT_PNG,
    RECON_FORMAT_JPEG,
    RECON_FORMAT_GIF,
    RECON_FORMAT_BMP,
    RECON_FORMAT_ICO,

    RECON_FORMAT_WAV,
    RECON_FORMAT_MP3,
    RECON_FORMAT_MP4,

    RECON_FORMAT_ZIP,
    RECON_FORMAT_GZIP,
    RECON_FORMAT_PDF,

    RECON_FORMAT_ELF,
    RECON_FORMAT_HTML,

    /* Decided last, and only because nothing else matched. */
    RECON_FORMAT_TEXT,
};

/*
 * What these bytes are.
 *
 * `length` may be shorter than RECON_SNIFF_BYTES; a file too short to carry a
 * signature is UNKNOWN rather than guessed at.
 */
enum recon_format recon_sniff(const uint8_t *bytes, size_t length);

/* A name for it, for putting in a sentence: "a PNG image", "plain text". */
const char *recon_sniff_name(enum recon_format kind);

/*
 * Whether a file of this kind would ordinarily be called `name`.
 *
 * True when the kind cannot be pinned down (UNKNOWN, and TEXT, which any of a
 * hundred extensions legitimately carries) -- because the caller uses this to
 * warn, and a warning that fires on every text file is one people learn to
 * dismiss. **The question it answers is "is this file lying about itself",
 * and the honest answer to that is usually no.**
 */
bool recon_sniff_agrees_with_name(enum recon_format kind, const char *name);

/*
 * The extension a file of this kind would ordinarily be given, with its dot,
 * or NULL when there is no single answer.
 *
 * For asking a *different* question of the extension machinery: not "does this
 * program open files called this", but "does it open files that really are
 * this". A JPEG named .png is refused by the first and accepted by the second,
 * and the second is the true answer.
 */
const char *recon_sniff_extension(enum recon_format kind);

#endif
