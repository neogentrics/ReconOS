/*
 * What a file actually is, against files whose answer is known.
 *
 * Two properties matter more than the individual formats, and both are easy to
 * get wrong in a way that passes a casual test:
 *
 *   * **No two signatures may both match.** "BM" is two ordinary letters, and
 *     an ftyp box sits four bytes in. A table that returns the first match is
 *     only correct if the order is, so the order is tested rather than trusted.
 *
 *   * **Agreement must not fire on ordinary files.** A warning that appears on
 *     every text file is one people learn to dismiss, which is worse than no
 *     warning at all.
 */

#include <stdio.h>
#include <string.h>

#include "recon_sniff.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
    checks++;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static bool sniffs(const uint8_t *bytes, size_t length,
        enum recon_format want, const char *what) {
    enum recon_format got = recon_sniff(bytes, length);
    if (got != want) {
        printf("        %s came back as %s, wanted %s\n", what,
            recon_sniff_name(got), recon_sniff_name(want));
        return false;
    }
    return true;
}

#define TEXT(s) ((const uint8_t *)(s)), strlen(s)

/* ------------------------------------------------------------------ */

static void test_the_formats(void) {
    printf("Formats with a signature\n");

    static const uint8_t PNG[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    check(sniffs(PNG, sizeof(PNG), RECON_FORMAT_PNG, "a PNG"), "a PNG");

    static const uint8_t JPEG[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F',
        'I', 'F', 0 };
    check(sniffs(JPEG, sizeof(JPEG), RECON_FORMAT_JPEG, "a JPEG"), "a JPEG");

    check(sniffs(TEXT("GIF89a....."), RECON_FORMAT_GIF, "a GIF"), "a GIF");
    check(sniffs(TEXT("GIF87a....."), RECON_FORMAT_GIF, "an older GIF"),
        "and the 1987 spelling of one");

    static const uint8_t ICO[] = { 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 32, 32 };
    check(sniffs(ICO, sizeof(ICO), RECON_FORMAT_ICO, "an icon"), "an icon");

    static const uint8_t WAV[] = { 'R', 'I', 'F', 'F', 0x24, 0, 0, 0,
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' };
    check(sniffs(WAV, sizeof(WAV), RECON_FORMAT_WAV, "a WAV"), "a WAV");

    static const uint8_t MP4[] = { 0, 0, 0, 0x20, 'f', 't', 'y', 'p',
        'i', 's', 'o', 'm', 0, 0, 2, 0 };
    check(sniffs(MP4, sizeof(MP4), RECON_FORMAT_MP4, "an MP4"),
        "an MP4, whose signature is four bytes in");

    check(sniffs(TEXT("ID3\x04\x00\x00\x00\x00\x00\x00"), RECON_FORMAT_MP3,
        "a tagged MP3"), "an MP3 with a tag on the front");

    static const uint8_t MP3[] = { 0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0 };
    check(sniffs(MP3, sizeof(MP3), RECON_FORMAT_MP3, "a bare MP3"),
        "and one that starts straight in with a frame");

    check(sniffs(TEXT("PK\x03\x04\x14\x00"), RECON_FORMAT_ZIP, "a zip"), "a zip");
    check(sniffs(TEXT("%PDF-1.7\n"), RECON_FORMAT_PDF, "a PDF"), "a PDF");

    static const uint8_t ELF[] = { 0x7F, 'E', 'L', 'F', 2, 1, 1, 0 };
    check(sniffs(ELF, sizeof(ELF), RECON_FORMAT_ELF, "a program"), "a program");

    /*
     * A whole bitmap header, because half of one is not one. The first version
     * of this fixture stopped after ten bytes and the sniffer refused it --
     * correctly, since "BM" alone is two ordinary letters and the check wants
     * the reserved bytes and the DIB header size before it will say bitmap.
     *
     * 14-byte file header, then a 40-byte BITMAPINFOHEADER: size, reserved
     * zeroes, pixels at offset 54, header size 40, 2x2, one plane, 24-bit.
     */
    static const uint8_t BMP[] = {
        'B', 'M',
        0x46, 0x00, 0x00, 0x00,     /* file size */
        0x00, 0x00, 0x00, 0x00,     /* reserved, and they must be */
        0x36, 0x00, 0x00, 0x00,     /* pixels begin at 54 */
        0x28, 0x00, 0x00, 0x00,     /* header size: 40 */
        0x02, 0x00, 0x00, 0x00,     /* width */
        0x02, 0x00, 0x00, 0x00,     /* height */
        0x01, 0x00,                 /* planes */
        0x18, 0x00,                 /* bits per pixel */
    };
    check(sniffs(BMP, sizeof(BMP), RECON_FORMAT_BMP, "a bitmap"), "a bitmap");

    /* And the corroboration is load-bearing: the same header with the reserved
     * bytes used for something is not a bitmap this will vouch for. */
    static uint8_t NOT_BMP[sizeof(BMP)];
    memcpy(NOT_BMP, BMP, sizeof(BMP));
    NOT_BMP[6] = 0x01;
    check(recon_sniff(NOT_BMP, sizeof(NOT_BMP)) != RECON_FORMAT_BMP,
        "and one byte into the reserved field is no longer one");
}

static void test_html_and_text(void) {
    printf("The two with no signature to speak of\n");

    check(sniffs(TEXT("<!DOCTYPE html>\n<html>"), RECON_FORMAT_HTML, "a page"),
        "a web page");
    check(sniffs(TEXT("\n\n   <HTML>"), RECON_FORMAT_HTML, "an indented page"),
        "and one behind whitespace, in capitals");

    check(sniffs(TEXT("Dear Aunt Mary,\n\nThank you for the socks.\n"),
        RECON_FORMAT_TEXT, "a letter"), "a letter is plain text");
    check(sniffs(TEXT("caf\xc3\xa9 na\xc3\xafve \xd0\xbf\xd1\x80\xd0\xb8"),
        RECON_FORMAT_TEXT, "another alphabet"),
        "AND SO IS TEXT IN SOMEBODY ELSE'S ALPHABET -- refusing that would be "
        "an insult dressed as a check");

    /*
     * A fragment is not a document. Calling it HTML would fire the warning on
     * things nobody would call a web page.
     */
    check(sniffs(TEXT("<p>hello</p>"), RECON_FORMAT_TEXT, "a fragment"),
        "a fragment of markup is text, not a web page");
}

static void test_nothing_matches_twice(void) {
    printf("What must not be mistaken for what\n");

    /*
     * The BMP signature is the two letters "BM". Every one of these begins
     * with them and none of them is a bitmap.
     */
    check(sniffs(TEXT("BM is short for basmati\n"), RECON_FORMAT_TEXT,
        "a sentence starting BM"),
        "A SENTENCE STARTING 'BM' IS NOT A BITMAP -- the weakest signature in "
        "the table, which is why it is tested last");

    /* An ftyp box lives at offset 4, so anything with those letters elsewhere
     * must not be read as an MP4. */
    check(sniffs(TEXT("ftyp is the box that names an MP4"), RECON_FORMAT_TEXT,
        "a sentence about ftyp"),
        "and a sentence about ftyp is not an MP4");

    /* Eleven set bits turn up in ordinary binary. The reserved fields are what
     * make the frame test more than a sync check. */
    static const uint8_t NOT_MP3[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    check(sniffs(NOT_MP3, sizeof(NOT_MP3), RECON_FORMAT_UNKNOWN, "all ones"),
        "a run of 0xFF is not an MP3, because its version and layer are "
        "reserved values");

    static const uint8_t SHORT[] = { 0x89, 'P' };
    check(sniffs(SHORT, sizeof(SHORT), RECON_FORMAT_UNKNOWN, "two bytes"),
        "a file too short to carry a signature is unknown, not guessed at");

    check(recon_sniff(NULL, 0) == RECON_FORMAT_UNKNOWN,
        "and nothing at all is unknown rather than a crash");
}

static void test_agreement(void) {
    printf("Whether a file is lying about itself\n");

    check(recon_sniff_agrees_with_name(RECON_FORMAT_PNG, "picture.png"),
        "a PNG called .png agrees");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_JPEG, "holiday.JPEG"),
        "and a JPEG called .JPEG, in capitals");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_JPEG, "holiday.jpg"),
        "and .jpg, because both spellings are ordinary");

    check(!recon_sniff_agrees_with_name(RECON_FORMAT_JPEG, "picture.png"),
        "A JPEG NAMED .PNG DOES NOT -- which is the whole point of this");
    check(!recon_sniff_agrees_with_name(RECON_FORMAT_ZIP, "report.pdf"),
        "and neither does a zip named .pdf");

    /*
     * The other half, and the one that decides whether anybody keeps the
     * warning switched on.
     */
    check(recon_sniff_agrees_with_name(RECON_FORMAT_TEXT, "notes.md"),
        "text called .md agrees, because a hundred extensions carry text");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_TEXT, "data.csv"),
        "and .csv");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_UNKNOWN, "thing.dat"),
        "bytes that said nothing cannot contradict a name");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_ELF, "Calculator.rex"),
        "and a program is named for what it does, not for its format");

    check(recon_sniff_agrees_with_name(RECON_FORMAT_PNG, "picture"),
        "a file with no extension makes no claim to contradict");
    check(recon_sniff_agrees_with_name(RECON_FORMAT_PNG, NULL),
        "and no name at all is not a disagreement");

    check(recon_sniff_agrees_with_name(RECON_FORMAT_ZIP, "sheet.xlsx"),
        "a zip called .xlsx agrees, because that is what those files are");
}

int main(void) {
    printf("What a file actually is\n\n");

    test_the_formats();
    test_html_and_text();
    test_nothing_matches_twice();
    test_agreement();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
