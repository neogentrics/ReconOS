#include "recon_sniff.h"

#include <string.h>

/*
 * A signature: bytes that must be there, at a fixed offset.
 *
 * A table rather than a ladder of ifs, because the interesting property is
 * that no two entries can both match -- and a table can be read down and
 * checked for that, where a ladder hides it in control flow.
 */
struct signature {
    enum recon_format kind;
    size_t at;
    const uint8_t *bytes;
    size_t length;
};

static const uint8_t SIG_PNG[]  = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
static const uint8_t SIG_JPEG[] = { 0xFF, 0xD8, 0xFF };
static const uint8_t SIG_GIF7[] = { 'G', 'I', 'F', '8', '7', 'a' };
static const uint8_t SIG_GIF9[] = { 'G', 'I', 'F', '8', '9', 'a' };
static const uint8_t SIG_ICO[]  = { 0x00, 0x00, 0x01, 0x00 };
static const uint8_t SIG_ID3[]  = { 'I', 'D', '3' };
static const uint8_t SIG_ZIP[]  = { 'P', 'K', 0x03, 0x04 };
static const uint8_t SIG_GZIP[] = { 0x1F, 0x8B };
static const uint8_t SIG_PDF[]  = { '%', 'P', 'D', 'F', '-' };
static const uint8_t SIG_ELF[]  = { 0x7F, 'E', 'L', 'F' };

/* `ftyp` at offset 4 is what makes an MP4 an MP4; the four bytes before it are
 * the box length and are not fixed. */
static const uint8_t SIG_FTYP[] = { 'f', 't', 'y', 'p' };

static const struct signature SIGNATURES[] = {
    { RECON_FORMAT_PNG,  0, SIG_PNG,  sizeof(SIG_PNG) },
    { RECON_FORMAT_JPEG, 0, SIG_JPEG, sizeof(SIG_JPEG) },
    { RECON_FORMAT_GIF,  0, SIG_GIF7, sizeof(SIG_GIF7) },
    { RECON_FORMAT_GIF,  0, SIG_GIF9, sizeof(SIG_GIF9) },
    { RECON_FORMAT_ICO,  0, SIG_ICO,  sizeof(SIG_ICO) },
    { RECON_FORMAT_MP3,  0, SIG_ID3,  sizeof(SIG_ID3) },
    { RECON_FORMAT_ZIP,  0, SIG_ZIP,  sizeof(SIG_ZIP) },
    { RECON_FORMAT_GZIP, 0, SIG_GZIP, sizeof(SIG_GZIP) },
    { RECON_FORMAT_PDF,  0, SIG_PDF,  sizeof(SIG_PDF) },
    { RECON_FORMAT_ELF,  0, SIG_ELF,  sizeof(SIG_ELF) },
    { RECON_FORMAT_MP4,  4, SIG_FTYP, sizeof(SIG_FTYP) },
};

static bool at_offset(const uint8_t *bytes, size_t length,
        const struct signature *sig) {
    if (length < sig->at + sig->length) {
        return false;
    }
    return memcmp(bytes + sig->at, sig->bytes, sig->length) == 0;
}

/*
 * A bitmap, which needs more than its signature.
 *
 * "BM" is two ordinary letters. It was in the signature table, tested last, on
 * the reasoning that everything stronger came first -- and that was wrong,
 * because the whole table runs before the text test. **"BM is short for
 * basmati" was a bitmap.** A signature that any English sentence can carry is
 * not a signature; it needs corroborating.
 *
 * Two things corroborate it, and both are structure rather than content: bytes
 * 6 to 9 are reserved and are zero in every bitmap anything has ever written,
 * and the DIB header that follows declares its own size, which is one of a
 * short list of known values.
 */
static bool is_bmp(const uint8_t *bytes, size_t length) {
    if (length < 18 || bytes[0] != 'B' || bytes[1] != 'M') {
        return false;
    }
    if (bytes[6] != 0 || bytes[7] != 0 || bytes[8] != 0 || bytes[9] != 0) {
        return false;
    }

    uint32_t header = (uint32_t)bytes[14] | ((uint32_t)bytes[15] << 8) |
        ((uint32_t)bytes[16] << 16) | ((uint32_t)bytes[17] << 24);

    /* BITMAPCOREHEADER through BITMAPV5HEADER. */
    return header == 12 || header == 40 || header == 52 || header == 56 ||
        header == 64 || header == 108 || header == 124;
}

/* RIFF....WAVE -- the four bytes between are the size and vary. */
static bool is_wav(const uint8_t *bytes, size_t length) {
    return length >= 12 &&
        memcmp(bytes, "RIFF", 4) == 0 &&
        memcmp(bytes + 8, "WAVE", 4) == 0;
}

/*
 * An MP3 with no ID3 tag begins with a frame sync: eleven bits set.
 *
 * Checked more tightly than the sync alone, because eleven set bits turn up in
 * ordinary binary often enough to matter. The version and layer fields have
 * reserved values, and a frame claiming one of those is not a frame.
 */
static bool is_mp3_frame(const uint8_t *bytes, size_t length) {
    if (length < 3 || bytes[0] != 0xFF || (bytes[1] & 0xE0) != 0xE0) {
        return false;
    }
    unsigned version = (bytes[1] >> 3) & 0x03;
    unsigned layer = (bytes[1] >> 1) & 0x03;
    unsigned bitrate = (bytes[2] >> 4) & 0x0F;
    unsigned rate = (bytes[2] >> 2) & 0x03;

    /* 1 is a reserved version, 0 a reserved layer, 15 a reserved bitrate and
     * 3 a reserved sample rate. Any of them means this is not a frame. */
    return version != 1 && layer != 0 && bitrate != 0x0F && rate != 3;
}

/*
 * HTML, after any leading whitespace.
 *
 * Only the two openings that actually identify a document. A file starting
 * with a <p> is a fragment somebody pasted, and calling it HTML would make the
 * warning fire on things nobody would call a web page.
 */
static bool is_html(const uint8_t *bytes, size_t length) {
    size_t i = 0;
    while (i < length && (bytes[i] == ' ' || bytes[i] == '\t' ||
            bytes[i] == '\r' || bytes[i] == '\n')) {
        i++;
    }

    static const char *OPENINGS[] = { "<!doctype html", "<html" };
    for (size_t o = 0; o < sizeof(OPENINGS) / sizeof(OPENINGS[0]); o++) {
        size_t want = strlen(OPENINGS[o]);
        if (length - i < want) {
            continue;
        }
        size_t k = 0;
        while (k < want) {
            unsigned char c = bytes[i + k];
            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c - 'A' + 'a');
            }
            if (c != (unsigned char)OPENINGS[o][k]) {
                break;
            }
            k++;
        }
        if (k == want) {
            return true;
        }
    }
    return false;
}

/*
 * Text is what is left when nothing else matched.
 *
 * A NUL byte settles it: no text file has one, and every binary format that
 * got this far will. Beyond that, two conditions.
 *
 * The first version allowed any byte above 127 on the grounds that text in
 * another language is full of them and refusing to call it text would be an
 * insult dressed as a check. That is the right instinct and it was implemented
 * too generously: **six bytes of 0xFF passed as text**, and so did the first
 * two bytes of a truncated PNG. 0xFF is not a legal byte in UTF-8 at all.
 *
 * So high bytes are allowed *as valid UTF-8 sequences* rather than
 * individually. That keeps every real alphabet and rejects the runs of high
 * bytes that ordinary binary is made of. A sequence cut off by the end of what
 * was read is accepted, because reading stops at a fixed length and a
 * character straddling that boundary is a fact about the reading, not the file.
 */
static bool is_text(const uint8_t *bytes, size_t length) {
    if (length == 0) {
        return false;
    }

    size_t odd = 0;
    size_t i = 0;

    while (i < length) {
        uint8_t c = bytes[i];

        if (c == 0x00) {
            return false;
        }

        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' &&
                    c != '\f') {
                odd++;
            }
            i++;
            continue;
        }

        /* How many continuation bytes this lead announces. 0xC0 and 0xC1 are
         * overlong forms and 0xF5 upwards is past the last code point, so
         * neither is a lead byte -- and a continuation byte with no lead in
         * front of it is not text either. */
        size_t follow;
        if (c >= 0xC2 && c <= 0xDF) {
            follow = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            follow = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            follow = 3;
        } else {
            return false;
        }

        for (size_t k = 1; k <= follow; k++) {
            if (i + k >= length) {
                return true;    /* Cut off by the read, not by the file. */
            }
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += follow + 1;
    }

    return odd * 32 < length;
}

enum recon_format recon_sniff(const uint8_t *bytes, size_t length) {
    if (bytes == NULL || length == 0) {
        return RECON_FORMAT_UNKNOWN;
    }

    /* The two with a gap in the middle, before the fixed-offset table: both
     * would otherwise be caught by a weaker entry in it. */
    if (is_wav(bytes, length)) {
        return RECON_FORMAT_WAV;
    }
    if (is_bmp(bytes, length)) {
        return RECON_FORMAT_BMP;
    }

    for (size_t i = 0; i < sizeof(SIGNATURES) / sizeof(SIGNATURES[0]); i++) {
        if (at_offset(bytes, length, &SIGNATURES[i])) {
            return SIGNATURES[i].kind;
        }
    }

    if (is_mp3_frame(bytes, length)) {
        return RECON_FORMAT_MP3;
    }
    if (is_html(bytes, length)) {
        return RECON_FORMAT_HTML;
    }
    if (is_text(bytes, length)) {
        return RECON_FORMAT_TEXT;
    }

    return RECON_FORMAT_UNKNOWN;
}

const char *recon_sniff_name(enum recon_format kind) {
    switch (kind) {
    case RECON_FORMAT_PNG:  return "a PNG image";
    case RECON_FORMAT_JPEG: return "a JPEG image";
    case RECON_FORMAT_GIF:  return "a GIF image";
    case RECON_FORMAT_BMP:  return "a bitmap image";
    case RECON_FORMAT_ICO:  return "an icon";
    case RECON_FORMAT_WAV:  return "a WAV sound";
    case RECON_FORMAT_MP3:  return "an MP3 sound";
    case RECON_FORMAT_MP4:  return "an MP4 video";
    case RECON_FORMAT_ZIP:  return "a zip archive";
    case RECON_FORMAT_GZIP: return "a gzip archive";
    case RECON_FORMAT_PDF:  return "a PDF document";
    case RECON_FORMAT_ELF:  return "a program";
    case RECON_FORMAT_HTML: return "a web page";
    case RECON_FORMAT_TEXT: return "plain text";
    case RECON_FORMAT_UNKNOWN:
    default:              return "not a kind this knows";
    }
}

/*
 * The extensions a kind is ordinarily called.
 *
 * More than one where more than one is ordinary -- .jpg and .jpeg are the same
 * thing and warning about either would be wrong.
 */
struct expected {
    enum recon_format kind;
    const char *extensions;     /* lower case, dotted, space separated */
};

static const struct expected EXPECTED[] = {
    { RECON_FORMAT_PNG,  ".png" },
    { RECON_FORMAT_JPEG, ".jpg .jpeg .jpe" },
    { RECON_FORMAT_GIF,  ".gif" },
    { RECON_FORMAT_BMP,  ".bmp .dib" },
    { RECON_FORMAT_ICO,  ".ico .cur" },
    { RECON_FORMAT_WAV,  ".wav .wave" },
    { RECON_FORMAT_MP3,  ".mp3" },
    { RECON_FORMAT_MP4,  ".mp4 .m4a .m4v .mov" },
    { RECON_FORMAT_ZIP,  ".zip .rpk .jar .odt .docx .xlsx .pptx" },
    { RECON_FORMAT_GZIP, ".gz .tgz" },
    { RECON_FORMAT_PDF,  ".pdf" },
    { RECON_FORMAT_HTML, ".html .htm" },
};

const char *recon_sniff_extension(enum recon_format kind) {
    for (size_t i = 0; i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        if (EXPECTED[i].kind != kind) {
            continue;
        }
        /* The first is the ordinary spelling: .jpg before .jpeg, .mp4 before
         * .mov. Static because the caller gets a pointer and the list holds
         * several. */
        static char first[16];
        size_t used = 0;
        const char *at = EXPECTED[i].extensions;
        while (*at != '\0' && *at != ' ' && used + 1 < sizeof(first)) {
            first[used++] = *at++;
        }
        first[used] = '\0';
        return used > 0 ? first : NULL;
    }
    return NULL;
}

bool recon_sniff_agrees_with_name(enum recon_format kind, const char *name) {
    if (name == NULL) {
        return true;
    }

    /*
     * Nothing to disagree with.
     *
     * UNKNOWN means the bytes said nothing, TEXT means they said only that it
     * is text -- and a hundred extensions legitimately carry text. ELF is here
     * too: a program is named for what it does, not for its format, and
     * ReconOS's own modules end .rex and .rts.
     */
    if (kind == RECON_FORMAT_UNKNOWN || kind == RECON_FORMAT_TEXT ||
            kind == RECON_FORMAT_ELF) {
        return true;
    }

    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        /* No extension is no claim, so there is nothing to contradict. */
        return true;
    }

    char lower[32];
    size_t used = 0;
    for (const char *c = dot; *c != '\0' && used + 1 < sizeof(lower); c++) {
        char ch = *c;
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        lower[used++] = ch;
    }
    lower[used] = '\0';

    /* An extension longer than the buffer is not one of the ones below, and
     * treating it as a disagreement would warn about a file whose name is
     * merely unusual. */
    if (used + 1 >= sizeof(lower)) {
        return true;
    }

    for (size_t i = 0; i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        if (EXPECTED[i].kind != kind) {
            continue;
        }

        /* Word by word, so ".m4a" inside ".m4a1" cannot match. */
        const char *at = EXPECTED[i].extensions;
        while (*at != '\0') {
            while (*at == ' ') {
                at++;
            }
            const char *end = at;
            while (*end != '\0' && *end != ' ') {
                end++;
            }
            if ((size_t)(end - at) == used && strncmp(at, lower, used) == 0) {
                return true;
            }
            at = end;
        }
        return false;
    }

    /* A kind with no expected extensions listed cannot disagree with one. */
    return true;
}
