/*
 * What the parsers do with input that is wrong.
 *
 * Every other suite in this project asks whether a decoder gets the right
 * answer from a good file. This one asks what it does with a bad one, which is
 * a different question and the one that matters for anything a person can be
 * sent: a picture in an email, a page from a web server, a video off a stick.
 * The good file comes from a program that meant well. The bad one may not.
 *
 * --- What counts as a failure here ---
 *
 * Not "the answer is wrong". A truncated file has no right answer, and a
 * decoder that refuses it is behaving perfectly. The failures are:
 *
 *   * reading or writing outside the buffer -- caught by the address
 *     sanitizer, which is why this suite earns most of its keep under
 *     scripts/check.sh rather than on its own
 *   * an offset or length handed back that points outside the input, which is
 *     the same bug one step earlier: the decoder has not crashed, it has told
 *     its caller where to crash. Checked here directly, because the caller
 *     that would prove it is the media player and it needs a screen
 *   * not returning at all -- a loop that does not end on input that does not
 *     make sense. There is no timeout in here; a suite that hangs is a suite
 *     that failed, and the run says which case it was on
 *   * signed overflow, a shift past the word, a misaligned load: the
 *     undefined-behaviour sanitizer's business, same as above
 *
 * --- How the input is made wrong ---
 *
 * Three ways, all deterministic. A fuzzer that finds something at three in the
 * morning and cannot find it again has not found anything.
 *
 *   * **Truncated** at every length from nothing to the whole file. This is
 *     the one that finds the most: a header says a table is forty entries
 *     long, and the file stops after nine.
 *   * **One byte changed**, at every position, to three values that break
 *     different things -- 0x00, 0xFF, and the byte with its top bit flipped.
 *     Length fields are where this bites: 0xFF makes a small number enormous.
 *   * **Random**, from a generator written out below rather than from the
 *     library's, so the same seed gives the same bytes on every machine and a
 *     failure can be reproduced by anybody.
 *
 * Run with: cmake --build build && ./build/recon_malformed_tests
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_codec.h"
#include "recon_expr.h"
#include "recon_css.h"
#include "recon_cookie.h"
#include "recon_form.h"
#include "recon_html.h"
#include "recon_http.h"
#include "recon_ico.h"
#include "recon_mp4.h"

static int g_failures;
static int g_cases;

/* What was being fed in when something went wrong, so a failure names it. */
static char g_what[128];

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_what, sizeof(g_what), fmt, args);
    va_end(args);
}

static void fail(const char *why) {
    g_failures++;
    printf("  FAIL  %s -- %s\n", g_what, why);
}

/*
 * The generator, written here on purpose.
 *
 * xorshift64*, which is eight lines and has no state anywhere else. rand() is
 * allowed to differ between C libraries, so a case that fails on this machine
 * would not be the same case on another one -- and "reproducible" is the only
 * property that makes a random test worth more than a shrug.
 */
static uint64_t g_seed;

static uint32_t next_random(void) {
    g_seed ^= g_seed >> 12;
    g_seed ^= g_seed << 25;
    g_seed ^= g_seed >> 27;
    return (uint32_t)((g_seed * 2685821657736338717ull) >> 32);
}

/* --- What each parser is asked to do with the bytes --- */

/*
 * A parser under test: hand it `length` bytes and do whatever a caller would
 * do with what comes back. Returns false to report a finding of its own --
 * an offset outside the input, say -- having already called fail().
 */
typedef bool (*feeder)(const uint8_t *bytes, size_t length);

static bool feed_ico(const uint8_t *bytes, size_t length) {
    int w = 0, h = 0;
    unsigned char *pixels = recon_ico_decode(bytes, length, 32, &w, &h);
    if (pixels != NULL) {
        /*
         * It said yes, so the size has to be one that can be indexed. A
         * decoder returning pixels with a width of two billion has handed its
         * caller a multiplication that overflows.
         */
        if (w <= 0 || h <= 0 || (long)w * h > 64L * 1024 * 1024) {
            fail("decoded to an unusable size");
            free(pixels);
            return false;
        }
        /* Touch every pixel it claims to have made. The sanitizer says
         * whether they are all there. */
        volatile unsigned char sink = 0;
        for (long i = 0; i < (long)w * h * 4; i++) {
            sink = (unsigned char)(sink ^ pixels[i]);
        }
        (void)sink;
        free(pixels);
    }
    return true;
}

static bool feed_mp4(const uint8_t *bytes, size_t length) {
    struct recon_mp4 *mp4 = recon_mp4_open(bytes, length);
    if (mp4 == NULL) {
        return true;
    }

    bool ok = true;
    int tracks = recon_mp4_track_count(mp4);
    if (tracks < 0 || tracks > 4096) {
        fail("claimed an impossible number of tracks");
        ok = false;
        tracks = 0;
    }

    for (int t = 0; t < tracks && ok; t++) {
        struct recon_mp4_track track;
        if (!recon_mp4_track_at(mp4, t, &track)) {
            continue;
        }

        /*
         * Every sample, up to a limit.
         *
         * The limit is not squeamishness about time: a two-byte edit can turn
         * a sample count into four billion, and walking that is the test
         * hanging rather than the test finding something. Two thousand is
         * plenty to reach the end of a real file and short enough that a
         * corrupt count is reported as one.
         */
        for (int i = 0; i < 2000; i++) {
            size_t offset = 0, sample_length = 0;
            uint64_t when = 0;
            if (!recon_mp4_sample(mp4, t, i, &offset, &sample_length, &when)) {
                break;
            }

            /*
             * THIS IS THE CHECK THE WHOLE FILE EXISTS FOR.
             *
             * The header says a sample lives at some offset for some length,
             * and both numbers came out of the file being tested. A decoder
             * that passes them through without checking them against the size
             * of what it was given has not crashed -- it has told the media
             * player exactly where to read past the end, and the player is the
             * thing that will be blamed.
             */
            if (offset > length || sample_length > length ||
                    offset + sample_length > length) {
                fail("a sample points outside the file");
                ok = false;
                break;
            }
        }

        if (ok) {
            /* These answer with an index; it has to be one that can be asked
             * for, or -1. */
            int sync = recon_mp4_sync_sample(mp4, t, 10);
            if (sync < -1) {
                fail("sync_sample answered with a nonsense index");
                ok = false;
            }
        }
    }

    if (ok) {
        recon_mp4_first(mp4, RECON_MP4_VIDEO);
        recon_mp4_first(mp4, RECON_MP4_AUDIO);
        recon_mp4_duration(mp4);
        recon_mp4_sample_at_time(mp4, 0, 1.5);
    }

    recon_mp4_close(mp4);
    return ok;
}

static bool feed_html(const uint8_t *bytes, size_t length) {
    struct recon_html_document *doc =
        recon_html_parse((const char *)bytes, length);
    if (doc == NULL) {
        return true;
    }

    bool ok = true;
    int blocks = recon_html_block_count(doc);
    if (blocks < 0) {
        fail("counted a negative number of blocks");
        ok = false;
        blocks = 0;
    }

    /* Walked the way the viewer walks it, because a block index the viewer
     * would ask for and this would not is an index nothing has tested. */
    for (int i = 0; i < blocks && ok; i++) {
        const struct recon_html_block_entry *block = recon_html_block_at(doc, i);
        if (block == NULL) {
            fail("a block within the count was not there");
            ok = false;
            break;
        }
    }

    if (ok) {
        recon_html_title(doc);
        recon_html_needs_scripting(doc);
        recon_html_was_truncated(doc);
        /* One past the end, both ends, which a viewer reaches by scrolling. */
        recon_html_block_at(doc, -1);
        recon_html_block_at(doc, blocks);
    }

    recon_html_free(doc);
    return ok;
}

static bool feed_expr(const uint8_t *bytes, size_t length) {
    /*
     * The one text parser here, so it is handed a string rather than a buffer.
     * Copied and terminated: the grapher gets its text from an edit field,
     * which is always terminated, and testing it on something that is not
     * would be testing a situation that cannot arise.
     */
    char text[512];
    size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
    memcpy(text, bytes, n);
    text[n] = '\0';

    double value = 0;
    char why[160];
    recon_expr_eval(text, 1.0, &value, why, sizeof(why));
    return true;
}

/* --- The three ways of being wrong --- */

static void truncations(const char *name, feeder feed, const uint8_t *good,
        size_t length) {
    for (size_t at = 0; at <= length; at++) {
        note("%s truncated to %zu of %zu bytes", name, at, length);
        g_cases++;
        if (!feed(good, at)) {
            return;
        }
    }
}

static void one_byte_changed(const char *name, feeder feed,
        const uint8_t *good, size_t length) {
    static const uint8_t VALUES[] = { 0x00, 0xFF, 0x80 };

    uint8_t *copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        return;
    }

    for (size_t at = 0; at < length; at++) {
        for (size_t v = 0; v < sizeof(VALUES); v++) {
            memcpy(copy, good, length);
            /* The third value flips the top bit rather than setting a
             * constant, so a byte that was already 0x80 still changes. */
            copy[at] = (v == 2) ? (uint8_t)(good[at] ^ 0x80) : VALUES[v];

            note("%s with byte %zu set to 0x%02X", name, at, copy[at]);
            g_cases++;
            if (!feed(copy, length)) {
                free(copy);
                return;
            }
        }
    }
    free(copy);
}

static void random_bytes(const char *name, feeder feed, int rounds) {
    uint8_t buffer[1024];

    for (int round = 0; round < rounds; round++) {
        size_t length = next_random() % sizeof(buffer);
        for (size_t i = 0; i < length; i++) {
            buffer[i] = (uint8_t)next_random();
        }

        note("%s from random round %d (%zu bytes, seed 1)", name, round,
            length);
        g_cases++;
        if (!feed(buffer, length)) {
            return;
        }
    }
}

/*
 * Random bytes that begin like the real thing.
 *
 * Pure noise is refused by the first check in every decoder -- the four
 * characters at the front are not "ftyp", so nothing after them is ever
 * reached, and a thousand rounds test one `if`. Keeping a real header and
 * making a mess of everything after it is what gets past the door and into the
 * part that walks tables.
 */
static void random_after_header(const char *name, feeder feed,
        const uint8_t *good, size_t keep, int rounds) {
    uint8_t buffer[1024];
    if (keep > sizeof(buffer)) {
        return;
    }

    for (int round = 0; round < rounds; round++) {
        size_t length = keep + next_random() % (sizeof(buffer) - keep);
        memcpy(buffer, good, keep);
        for (size_t i = keep; i < length; i++) {
            buffer[i] = (uint8_t)next_random();
        }

        note("%s with a real header and %zu random bytes, round %d", name,
            length - keep, round);
        g_cases++;
        if (!feed(buffer, length)) {
            return;
        }
    }
}

/* --- Something valid to start from --- */

static size_t make_ico(uint8_t *out, size_t max) {
    /* A 2x2 32-bit icon: ICONDIR, one ICONDIRENTRY, a BITMAPINFOHEADER, four
     * pixels, and the mask that readers expect to follow. */
    const int size = 2;
    size_t image = (size_t)size * size * 4;
    size_t mask = 4 * (size_t)size;          /* 4-byte-aligned rows */
    size_t total = 6 + 16 + 40 + image + mask;
    if (total > max) {
        return 0;
    }

    memset(out, 0, total);
    out[2] = 1;                              /* type: icon */
    out[4] = 1;                              /* one image */

    uint8_t *entry = out + 6;
    entry[0] = (uint8_t)size;                /* width */
    entry[1] = (uint8_t)size;                /* height */
    entry[4] = 1;                            /* planes */
    entry[6] = 32;                           /* bits */
    uint32_t bytes = (uint32_t)(40 + image + mask);
    memcpy(entry + 8, &bytes, 4);
    uint32_t at = 6 + 16;
    memcpy(entry + 12, &at, 4);

    uint8_t *dib = out + at;
    dib[0] = 40;                             /* header size */
    dib[4] = (uint8_t)size;                  /* width */
    dib[8] = (uint8_t)(size * 2);            /* height, doubled for the mask */
    dib[12] = 1;                             /* planes */
    dib[14] = 32;                            /* bits */

    uint8_t *pixels = dib + 40;
    for (size_t i = 0; i < image; i++) {
        pixels[i] = (uint8_t)(i * 7);
    }
    return total;
}

/*
 * A tiny but *complete* MP4: four samples of eight bytes, with the five tables
 * that say where they are.
 *
 * The first version of this was a header and an empty moov, and it tested
 * nothing at all. That was not noticed by reading it. It was noticed by
 * deleting the bounds check in recon_mp4_sample on purpose and watching this
 * suite report five thousand cases and no failures -- no fixture ever produced
 * a sample, so the check was never reached and the walk below never walked.
 *
 * With the tables here, the same deletion is caught: "MP4 with byte 276 set to
 * 0xFF -- a sample points outside the file". That is the whole reason this
 * function has the shape it does, and the reason to break a check on purpose
 * before believing a test that passes.
 *
 * mdat comes before moov so the chunk offset is a number known while writing
 * rather than one that has to be patched afterwards. Both orders are legal and
 * both occur in the wild.
 */

/* Four bytes, big-endian, which is the only endianness this format has. */
static void put32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

/* A box header, with its size left to be filled in when the end is known. */
static size_t open_box(uint8_t *out, size_t at, const char *name) {
    memcpy(out + at + 4, name, 4);
    return at + 8;
}

static void close_box(uint8_t *out, size_t opened_at, size_t now) {
    put32(out + opened_at, (uint32_t)(now - opened_at));
}

#define SAMPLES 4
#define SAMPLE_BYTES 8

static size_t make_mp4(uint8_t *out, size_t max) {
    if (max < 512) {
        return 0;
    }
    memset(out, 0, max);
    size_t at = 0;

    /* ftyp */
    size_t ftyp = at;
    at = open_box(out, at, "ftyp");
    memcpy(out + at, "isom", 4);      at += 4;
    put32(out + at, 0x200);           at += 4;
    memcpy(out + at, "isommp41", 8);  at += 8;
    close_box(out, ftyp, at);

    /* mdat, and the four samples inside it. */
    size_t mdat = at;
    at = open_box(out, at, "mdat");
    size_t payload = at;
    for (size_t i = 0; i < SAMPLES * SAMPLE_BYTES; i++) {
        out[at++] = (uint8_t)(i + 1);
    }
    close_box(out, mdat, at);

    /* moov > trak > mdia > minf > stbl > the tables */
    size_t moov = at;  at = open_box(out, at, "moov");
    size_t trak = at;  at = open_box(out, at, "trak");
    size_t mdia = at;  at = open_box(out, at, "mdia");

    /* mdhd: a timescale, so a sample has a moment as well as a place. */
    size_t mdhd = at;
    at = open_box(out, at, "mdhd");
    put32(out + at, 0);               at += 4;   /* version and flags */
    put32(out + at, 0);               at += 4;   /* created */
    put32(out + at, 0);               at += 4;   /* modified */
    put32(out + at, 1000);            at += 4;   /* timescale */
    put32(out + at, SAMPLES * 100);   at += 4;   /* duration */
    put32(out + at, 0);               at += 4;   /* language, quality */
    close_box(out, mdhd, at);

    /* hdlr: what kind of track this is. */
    size_t hdlr = at;
    at = open_box(out, at, "hdlr");
    put32(out + at, 0);               at += 4;   /* version and flags */
    put32(out + at, 0);               at += 4;   /* predefined */
    memcpy(out + at, "soun", 4);      at += 4;
    memset(out + at, 0, 12);          at += 12;  /* reserved */
    close_box(out, hdlr, at);

    size_t minf = at;  at = open_box(out, at, "minf");
    size_t stbl = at;  at = open_box(out, at, "stbl");

    /* stsd: one description, of a kind the player has a name for. */
    size_t stsd = at;
    at = open_box(out, at, "stsd");
    put32(out + at, 0);               at += 4;   /* version and flags */
    put32(out + at, 1);               at += 4;   /* one entry */
    size_t entry = at;
    at = open_box(out, at, "mp4a");
    memset(out + at, 0, 20);          at += 20;
    close_box(out, entry, at);
    close_box(out, stsd, at);

    /* stts: how long each sample lasts. One run covers all four. */
    size_t stts = at;
    at = open_box(out, at, "stts");
    put32(out + at, 0);               at += 4;
    put32(out + at, 1);               at += 4;   /* one run */
    put32(out + at, SAMPLES);         at += 4;   /* this many samples */
    put32(out + at, 100);             at += 4;   /* each this long */
    close_box(out, stts, at);

    /* stsc: which samples live in which chunk. All four in the one chunk. */
    size_t stsc = at;
    at = open_box(out, at, "stsc");
    put32(out + at, 0);               at += 4;
    put32(out + at, 1);               at += 4;   /* one run */
    put32(out + at, 1);               at += 4;   /* from chunk one */
    put32(out + at, SAMPLES);         at += 4;   /* this many per chunk */
    put32(out + at, 1);               at += 4;   /* description one */
    close_box(out, stsc, at);

    /*
     * stsz: how big each sample is.
     *
     * Written the long way -- a zero in the "everything is this size" field
     * and then a table -- rather than the short way, because the table is what
     * the mutations below get to corrupt. A single number has one byte worth
     * changing and a table has sixteen.
     */
    size_t stsz = at;
    at = open_box(out, at, "stsz");
    put32(out + at, 0);               at += 4;
    put32(out + at, 0);               at += 4;   /* not a uniform size */
    put32(out + at, SAMPLES);         at += 4;
    for (int i = 0; i < SAMPLES; i++) {
        put32(out + at, SAMPLE_BYTES); at += 4;
    }
    close_box(out, stsz, at);

    /* stco: where the one chunk starts. This is the number that, changed,
     * would send a reader outside the file. */
    size_t stco = at;
    at = open_box(out, at, "stco");
    put32(out + at, 0);               at += 4;
    put32(out + at, 1);               at += 4;   /* one chunk */
    put32(out + at, (uint32_t)payload); at += 4;
    close_box(out, stco, at);

    close_box(out, stbl, at);
    close_box(out, minf, at);
    close_box(out, mdia, at);
    close_box(out, trak, at);
    close_box(out, moov, at);

    return at;
}

static size_t make_html(uint8_t *out, size_t max) {
    static const char PAGE[] =
        "<html><head><title>A page</title></head><body>"
        "<h1>Heading</h1><p>Some <b>bold</b> text and a "
        "<a href=\"http://example.com/\">link</a>.</p>"
        "<ul><li>one</li><li>two</li></ul>"
        "<script>never run</script>"
        "</body></html>";
    size_t length = sizeof(PAGE) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, PAGE, length);
    return length;
}

/*
 * --- A form ---
 *
 * The markup this is built from is somebody else's, and so is every value in
 * it. What comes out the far end is a request this machine sends, so a fault
 * here is not a crash on a bad page -- it is a bad page deciding what bytes
 * leave.
 *
 * The seed is the HTML5 standard's example form, so a mutation starts from
 * something with a control of every kind in it rather than from nothing.
 */
static size_t make_form(uint8_t *out, size_t max) {
    static const char PAGE[] =
        "<form method=\"post\" action=\"/post\">"
        "<input name=\"custname\" value=\"Ada\">"
        "<input type=password name=\"pw\">"
        "<input type=radio name=size value=\"small\">"
        "<input type=radio name=size value=\"large\" checked>"
        "<input type=checkbox name=topping value=\"bacon\" checked>"
        "<input type=hidden name=\"token\" value=\"abc\">"
        "<select name=\"where\"><option value=\"door\">Door"
        "<option value=\"desk\" selected>Desk</select>"
        "<textarea name=\"comments\">note</textarea>"
        "<button name=\"act\" value=\"order\">Send</button>"
        "</form>";
    size_t length = sizeof(PAGE) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, PAGE, length);
    return length;
}

/*
 * --- A cookie ---
 *
 * `Set-Cookie` is a header a server writes and this machine keeps and hands
 * back, so every byte of it is somebody else's and every byte of it decides
 * what is sent on the next request. The seed carries every attribute at once,
 * so a mutation starts from something with all of them rather than from
 * nothing.
 */
static size_t make_cookie(uint8_t *out, size_t max) {
    static const char HEADER[] =
        "session=abc123; Path=/app; Domain=.example.com; Max-Age=3600; "
        "Expires=Wed, 09 Jun 2021 10:18:14 GMT; Secure; HttpOnly; "
        "SameSite=Lax";
    size_t length = sizeof(HEADER) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, HEADER, length);
    return length;
}

static size_t make_expr(uint8_t *out, size_t max) {
    static const char TEXT[] = "3 * sin(x) + (x ^ 2) / (1 - x)";
    size_t length = sizeof(TEXT) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, TEXT, length);
    return length;
}

/*
 * Parse whatever this is, then send every form it turned out to have.
 *
 * Every control is asked for at each of several states rather than only its
 * default -- a checkbox that is only ever off never exercises the branch that
 * sends it, and an option index is exactly the kind of thing a mutated page
 * can put out of range.
 */
static bool feed_form(const uint8_t *bytes, size_t length) {
    struct recon_html_document *d =
        recon_html_parse((const char *)bytes, length);
    if (d == NULL) {
        return true;                   /* no memory is not a failure */
    }

    int count = recon_html_field_count(d);
    struct recon_form_value *values = NULL;
    if (count > 0) {
        values = calloc((size_t)count, sizeof(*values));
        if (values == NULL) {
            recon_html_free(d);
            return true;
        }
    }

    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < count; i++) {
            const struct recon_html_field *f = recon_html_field_at(d, i);
            values[i].text = (pass == 0) ? f->value
                : (pass == 1 ? "" : "a value with spaces & symbols=1");
            values[i].on = (pass != 1);

            /*
             * Deliberately out of range on the last pass. A viewer keeps this
             * index and a page can change under it -- a reload with fewer
             * options is the ordinary way it happens -- so reading the option
             * table with it must be bounded here rather than at every caller.
             */
            values[i].chosen = (pass == 2) ? f->option_count + 3 : 0;
        }

        int forms = recon_html_form_count(d);
        for (int form = -1; form < forms; form++) {
            for (int who = -1; who < count && who < 4; who++) {
                int sent = 0;
                bool secret = false;
                char *body = recon_form_body(d, form, who, values, count,
                    4096, &sent, &secret);
                if (body != NULL) {
                    /* What comes back has to be a string, and has to fit. */
                    if (strlen(body) >= 4096) {
                        printf("  form body ran past its ceiling\n");
                        free(body);
                        free(values);
                        recon_html_free(d);
                        return false;
                    }

                    struct recon_http_url where;
                    if (recon_http_parse_url("https://example.com/s", NULL,
                            &where)) {
                        recon_form_get_address(&where, body);
                    }
                    free(body);
                }
            }
        }
    }

    free(values);
    recon_html_free(d);
    return true;
}

/*
 * Store whatever this is, then ask what would be sent, on several hosts and
 * paths -- including the ones the header names, which is where a matching
 * rule that was too generous would show.
 */
static bool feed_cookie(const uint8_t *bytes, size_t length) {
    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return true;
    }

    /* A string, because a header is one. The bytes may contain a NUL and
     * that is worth feeding: a parser reading past one has been handed a
     * shorter header than the caller thinks it gave. */
    char header[2048];
    size_t take = length < sizeof(header) - 1 ? length : sizeof(header) - 1;
    memcpy(header, bytes, take);
    header[take] = '\0';

    static const char *const HOSTS[] = {
        "example.com", "www.example.com", "other.example", "co.uk", "",
    };
    static const char *const PATHS[] = {
        "/", "/app", "/app/deep/page.html", "/appliance", "",
    };

    const time_t now = 1782000000;

    for (size_t h = 0; h < sizeof(HOSTS) / sizeof(HOSTS[0]); h++) {
        recon_cookie_set(jar, HOSTS[h], PATHS[h % 5], h % 2 == 0, header,
            now);
    }

    for (size_t h = 0; h < sizeof(HOSTS) / sizeof(HOSTS[0]); h++) {
        for (size_t p = 0; p < sizeof(PATHS) / sizeof(PATHS[0]); p++) {
            char out[4096];
            size_t used = recon_cookie_header(jar, HOSTS[h], PATHS[p],
                p % 2 == 0, now, out, sizeof(out));
            if (used != strlen(out)) {
                printf("  the cookie header said one length and wrote "
                    "another\n");
                recon_cookie_jar_free(jar);
                return false;
            }
        }
    }

    /*
     * A cookie is never sent to a host that did not set it. Checked here
     * rather than only in the suite, because the suite uses headers somebody
     * wrote and this uses headers nobody did -- and the interesting failure is
     * a malformed Domain or Path that widens the match.
     */
    for (int i = 0; i < recon_cookie_count(jar); i++) {
        struct recon_cookie_view view;
        if (!recon_cookie_at(jar, i, &view)) {
            continue;
        }
        char out[4096];
        recon_cookie_header(jar, "somewhere.else", "/", true, now, out,
            sizeof(out));
        if (out[0] != '\0' && strcmp(view.host, "somewhere.else") != 0) {
            printf("  a cookie reached a host that never set one\n");
            recon_cookie_jar_free(jar);
            return false;
        }
    }

    recon_cookie_sweep(jar, now + 100000);
    recon_cookie_forget_host(jar, "example.com");
    recon_cookie_forget_all(jar);
    recon_cookie_jar_free(jar);
    return true;
}

/* --- Running one parser through all of it --- */


/*
 * --- A stylesheet ---
 *
 * Fetched from whatever server the page named, parsed, and then consulted for
 * every element on that page. It is as much somebody else's file as the
 * markup is, and it was not in this suite.
 */
static bool feed_css(const uint8_t *bytes, size_t length) {
    struct recon_css_sheet *sheet = recon_css_new();
    if (sheet == NULL) {
        return true;
    }

    recon_css_add(sheet, (const char *)bytes, length);

    bool ok = true;
    int rules = recon_css_rule_count(sheet);
    if (rules < 0) {
        fail("counted a negative number of rules");
        ok = false;
    }

    /*
     * Matched against an element, which is the only thing a sheet is for and
     * the path where a bad rule would actually be walked. A sheet that parsed
     * without crashing and then reads outside itself on the first match is a
     * sheet this would otherwise call fine.
     */
    if (ok) {
        struct recon_css_element stack[3] = {
            { "html", NULL, NULL },
            { "body", "main", "wide dark" },
            { "p", NULL, "lead" },
        };
        for (int depth = 1; depth <= 3; depth++) {
            struct recon_css_style style;
            memset(&style, 0, sizeof(style));
            recon_css_match(sheet, stack, depth, &style);

            /* A size is a percentage and is applied by multiplying. One that
             * came back enormous or negative would be a heading of four
             * million pixels, or of minus one. */
            if (style.size_percent < 0 || style.size_percent > 100000) {
                fail("a font size came back outside anything usable");
                ok = false;
                break;
            }
        }
    }

    /* And an inline style, which is the other way in and does not go through
     * recon_css_add at all. */
    if (ok) {
        struct recon_css_style inline_style;
        memset(&inline_style, 0, sizeof(inline_style));
        recon_css_inline((const char *)bytes, length, &inline_style);
    }

    recon_css_free(sheet);
    return ok;
}

static size_t make_css(uint8_t *out, size_t max) {
    static const char SHEET[] =
        ":root { --ink: #123456; --paper: #08090c }\n"
        "body { background: var(--paper); color: var(--ink) }\n"
        "h1, h2 { font-size: 180%; text-align: center }\n"
        ".lead p, #main .wide { display: block; color: rgb(20, 30, 40) }\n"
        "@media (min-width: 40em) { p { display: none } }\n"
        "/* a comment */ em { font-style: italic }\n";
    size_t length = sizeof(SHEET) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, SHEET, length);
    return length;
}

/*
 * --- An address ---
 *
 * Every one a viewer handles came off somebody else's page. This is the
 * boundary between their bytes and where the machine connects to.
 */
static bool feed_url(const uint8_t *bytes, size_t length) {
    /*
     * A URL is a string, and the fuzzer hands over bytes -- including NULs,
     * which is itself worth feeding: a parser that reads past one has been
     * handed a shorter string than the caller thinks it gave.
     */
    char text[2048];
    size_t take = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
    memcpy(text, bytes, take);
    text[take] = '\0';

    struct recon_http_url base;
    bool have_base = recon_http_parse_url("https://example.com/a/b.html",
        NULL, &base);

    struct recon_http_url url;
    for (int with_base = 0; with_base < 2; with_base++) {
        if (!recon_http_parse_url(text, (with_base && have_base) ? &base : NULL,
                &url)) {
            continue;
        }

        /*
         * What a caller then does with it: the path is put in a request line
         * and the host in a Host: header, so both have to be strings and the
         * path has to start with the slash every caller assumes.
         */
        if (strlen(url.host) >= sizeof(url.host)) {
            fail("a host filled its buffer with no terminator");
            return false;
        }
        if (strlen(url.path) >= sizeof(url.path)) {
            fail("a path filled its buffer with no terminator");
            return false;
        }
        if (url.path[0] != '/') {
            fail("a path came back not starting with a slash");
            return false;
        }
        if (url.port < 0 || url.port > 65535) {
            fail("a port came back outside the range a port has");
            return false;
        }
        if (strchr(url.path, '#') != NULL) {
            fail("a fragment was left in the path, which is sent to a server");
            return false;
        }

        char formatted[RECON_HTTP_URL_MAX * 2];
        recon_http_format_url(&url, formatted, sizeof(formatted));
    }
    return true;
}

static size_t make_url(uint8_t *out, size_t max) {
    static const char URL[] = "https://example.com:8080/a/b.html?q=1#notes";
    size_t length = sizeof(URL) - 1;
    if (length > max) {
        return 0;
    }
    memcpy(out, URL, length);
    return length;
}

static void sweep(const char *name, feeder feed,
        size_t (*make)(uint8_t *, size_t), size_t header_bytes) {
    printf("%s\n", name);

    uint8_t good[1024];
    size_t length = make(good, sizeof(good));
    if (length == 0) {
        printf("  FAIL  could not build something valid to start from\n");
        g_failures++;
        return;
    }

    int before = g_failures;

    /* The valid one first. If a parser cannot read what it is supposed to
     * read, nothing this suite says about the broken ones means anything. */
    note("%s, unmodified", name);
    g_cases++;
    feed(good, length);

    truncations(name, feed, good, length);
    one_byte_changed(name, feed, good, length);
    random_bytes(name, feed, 400);
    if (header_bytes > 0 && header_bytes <= length) {
        random_after_header(name, feed, good, header_bytes, 600);
    }

    if (g_failures == before) {
        printf("  ok    nothing crashed, nothing pointed outside itself\n");
    }
}

int main(void) {
    printf("Malformed input\n\n");

    /* One, written down, so a failure can be reproduced by running this
     * again rather than by being lucky twice. */
    g_seed = 1;

    /* The first eight bytes of an MP4 are its length and "ftyp", and of an ICO
     * the two-byte zero, the type and the count. Keeping them is what gets a
     * random file past the door and into the tables. */
    sweep("ICO", feed_ico, make_ico, 6);
    sweep("MP4", feed_mp4, make_mp4, 8);
    sweep("HTML", feed_html, make_html, 0);
    sweep("CSS", feed_css, make_css, 0);
    sweep("Addresses", feed_url, make_url, 0);
    sweep("Forms", feed_form, make_form, 0);
    sweep("Cookies", feed_cookie, make_cookie, 0);
    sweep("Expressions", feed_expr, make_expr, 0);

    printf("\n%d cases, %d failures\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
