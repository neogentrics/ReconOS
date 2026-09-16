/*
 * The key table, held against the library it was copied from.
 *
 * `include/recon_key.h` exists so that twenty-four of the desktop's source
 * files stop needing a Linux keyboard library in order to say the word
 * "Escape". Its numbers are X11 keysym values and they were taken out of
 * xkbcommon once, by asking it.
 *
 * **A borrowed table with nothing checking the borrow is a copy that is right
 * on the day it is made.** This is the check: every name in the table is put
 * to xkbcommon and the two answers compared, and both conversions are swept
 * across the whole range rather than spot-checked.
 *
 * It is also the last place xkbcommon is allowed to appear on the desktop
 * side, which is deliberate: the one file that still links it is the one whose
 * job is to disagree with it.
 *
 * Run with: cmake --build build && ./build/recon_key_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <xkbcommon/xkbcommon.h>

#include "recon_key.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/*
 * Every name this system knows, and the value it is supposed to have.
 *
 * Written out rather than looped over the header, because a test that reads
 * the same table the code reads agrees with it by construction -- the fault
 * BG-182 was, in a smaller file. These names are typed here and the *values*
 * come from xkbcommon, so a wrong number in the header is a disagreement
 * between two independent sources.
 */
static const char *const EVERY_NAME[] = {
    /* the printable ones, by their X11 names */
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
    "ampersand", "apostrophe", "parenleft", "parenright", "asterisk", "plus",
    "comma", "minus", "period", "slash",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "colon", "semicolon", "less", "equal", "greater", "question", "at",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "bracketleft", "backslash", "bracketright", "asciicircum", "underscore",
    "grave",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "braceleft", "bar", "braceright", "asciitilde",

    /* the named ones */
    "BackSpace", "Tab", "ISO_Left_Tab", "Linefeed", "Clear", "Return",
    "Pause", "Scroll_Lock", "Sys_Req", "Escape", "Delete",
    "Home", "Left", "Up", "Right", "Down", "Page_Up", "Page_Down",
    "End", "Begin", "Insert", "Menu", "Print", "Help", "Break",
    "Num_Lock", "Caps_Lock", "Shift_Lock",
    "Shift_L", "Shift_R", "Control_L", "Control_R",
    "Alt_L", "Alt_R", "Super_L", "Super_R", "Meta_L", "Meta_R",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "KP_Space", "KP_Tab", "KP_Enter", "KP_Home", "KP_Left", "KP_Up",
    "KP_Right", "KP_Down", "KP_Page_Up", "KP_Page_Down", "KP_End",
    "KP_Begin", "KP_Insert", "KP_Delete", "KP_Equal", "KP_Multiply",
    "KP_Add", "KP_Separator", "KP_Subtract", "KP_Decimal", "KP_Divide",
    "KP_0", "KP_1", "KP_2", "KP_3", "KP_4",
    "KP_5", "KP_6", "KP_7", "KP_8", "KP_9",
    NULL,
};

static void test_every_name_agrees(void) {
    printf("every key this system names has xkbcommon's number\n");

    int names = 0;
    int wrong = 0;

    for (int i = 0; EVERY_NAME[i] != NULL; i++) {
        const char *name = EVERY_NAME[i];
        names++;

        recon_keysym ours = recon_key_from_name(name);
        xkb_keysym_t theirs = xkb_keysym_from_name(name,
            XKB_KEYSYM_NO_FLAGS);

        if (ours != theirs) {
            wrong++;
            if (wrong <= 5) {
                printf("  FAIL: %s is 0x%x here and 0x%x there\n",
                    name, ours, theirs);
            }
        }
    }

    check(names == 176, "the table holds every name the header declares");
    check(wrong == 0, "and every one of them has the same number");
    printf("  %d names, %d disagreements\n", names, wrong);
}

static void test_a_name_it_does_not_know(void) {
    printf("a key this system has no way to deliver\n");

    /*
     * xkbcommon knows several thousand keysyms and this knows a hundred and
     * seventy-six. The difference is not an oversight: there is no Hebrew
     * layout here and no media keys on this machine's idea of a keyboard, and
     * **accepting a name for a key that could never arrive is worse than
     * refusing it**, because the refusal happens where somebody typed the name
     * and the acceptance fails silently much later.
     */
    /*
     * Asked of xkbcommon rather than assumed. The first version used
     * `Hebrew_aleph`, and this build of xkbcommon does not know that name
     * either -- so the check meant to establish that there *is* a gap was
     * really checking that two libraries were both ignorant, and passed for
     * the wrong reason. `Greek_alpha` it knows: 0x7e1.
     */
    check(xkb_keysym_from_name("Greek_alpha", XKB_KEYSYM_NO_FLAGS) !=
        XKB_KEY_NoSymbol, "xkbcommon knows Greek_alpha");
    check(recon_key_from_name("Greek_alpha") == RECON_KEY_NoSymbol,
        "and this does not, and says so rather than guessing");

    check(recon_key_from_name("XF86AudioRaiseVolume") == RECON_KEY_NoSymbol,
        "nor a media key this machine has no idea about");
    check(recon_key_from_name("") == RECON_KEY_NoSymbol,
        "an empty name is no key");
    check(recon_key_from_name(NULL) == RECON_KEY_NoSymbol,
        "and neither is nothing at all");
}

static void test_names_are_case_insensitive(void) {
    printf("a person typing a key name does not have to know X11's spelling\n");

    check(recon_key_from_name("page_up") == RECON_KEY_Page_Up,
        "page_up is Page_Up");
    check(recon_key_from_name("RETURN") == RECON_KEY_Return,
        "RETURN is Return");
    check(recon_key_from_name("escape") == RECON_KEY_Escape,
        "escape is Escape");

    /*
     * **And an exact name always wins**, which is the half that was wrong.
     *
     * `a` and `A` are different keys whose names differ only in case, and a
     * case-insensitive scan returns whichever the table lists first -- the
     * capital, because the table is in value order. So `key a` at the Terminal
     * injected a capital A, and every letter was wrong the same way.
     *
     * The first version of this comment explained why that was fine. It was
     * not fine: it was a theory about code I had not read back, written beside
     * a check that passed because it asserted the theory.
     */
    check(recon_key_from_name("a") == RECON_KEY_a, "a is the letter a");
    check(recon_key_from_name("A") == RECON_KEY_A, "and A is the capital");
    check(recon_key_from_name("a") != recon_key_from_name("A"),
        "and they are not the same key");

    /* Every letter, both ways -- one of them being right by luck is exactly
     * what the broken version looked like. */
    int letters_wrong = 0;
    for (char c = 'a'; c <= 'z'; c++) {
        char lower[2] = { c, '\0' };
        char upper[2] = { (char)(c - 'a' + 'A'), '\0' };
        if (recon_key_from_name(lower) != (recon_keysym)c ||
                recon_key_from_name(upper) != (recon_keysym)(c - 'a' + 'A')) {
            letters_wrong++;
        }
    }
    check(letters_wrong == 0, "and all twenty-six behave the same way");
}

static void test_the_characters(void) {
    printf("what a key stands for, across the whole range\n");

    /*
     * Swept rather than spot-checked, over every keysym in the classic range
     * and the whole of the Unicode one -- with one deliberate exception, which
     * is counted rather than hidden. See below.
     */
    long compared = 0;
    long disagreed = 0;
    long we_say_nothing = 0;
    xkb_keysym_t first_disagreement = 0;

    for (uint32_t sym = 0; sym <= 0xffff; sym++) {
        uint32_t ours = recon_key_to_char(sym);
        uint32_t theirs = xkb_keysym_to_utf32(sym);

        if (ours == 0 && theirs != 0) {
            /*
             * **The deliberate gap.** xkbcommon maps the Greek, Cyrillic,
             * Hebrew, Arabic and Thai keysyms to their letters; this does not,
             * because nothing in ReconOS can produce one -- there is a single
             * keymap and it is `us`. Counted so the size of the gap is a
             * number somebody can look at rather than a surprise, and held
             * below to the rule that matters: it never contains a key this
             * system claims to know.
             */
            we_say_nothing++;
            continue;
        }

        compared++;
        if (ours != theirs) {
            if (disagreed == 0) {
                first_disagreement = sym;
            }
            disagreed++;
        }
    }

    check(disagreed == 0,
        "every keysym this system maps, it maps to the same character");
    if (disagreed != 0) {
        printf("        first at 0x%x: %u here, %u there\n",
            first_disagreement, recon_key_to_char(first_disagreement),
            xkb_keysym_to_utf32(first_disagreement));
    }
    printf("  %ld keysyms compared, %ld disagreements, %ld this system does "
        "not map\n", compared, disagreed, we_say_nothing);

    /*
     * And the gap must not contain a key that is in the table, which is the
     * property that makes the gap safe: it may only hold keys nothing here can
     * press.
     */
    int claimed_but_unmapped = 0;
    for (int i = 0; EVERY_NAME[i] != NULL; i++) {
        recon_keysym sym = recon_key_from_name(EVERY_NAME[i]);
        if (xkb_keysym_to_utf32(sym) != 0 && recon_key_to_char(sym) == 0) {
            claimed_but_unmapped++;
            printf("  FAIL: %s is in the table and has no character here\n",
                EVERY_NAME[i]);
        }
    }
    check(claimed_but_unmapped == 0,
        "and nothing in the gap is a key this system claims to know");
}

static void test_the_unicode_range(void) {
    printf("the range X11 set aside for everything else Unicode has\n");

    long compared = 0;
    long disagreed = 0;

    /* Every codepoint, in steps small enough to cross every boundary and
     * plane edge without spending a minute on it. */
    for (uint32_t code = 0; code <= 0x110fff; code += 7) {
        uint32_t sym = 0x01000000 + code;
        uint32_t ours = recon_key_to_char(sym);
        uint32_t theirs = xkb_keysym_to_utf32(sym);
        compared++;
        if (ours != theirs) {
            if (disagreed == 0) {
                printf("  FAIL: 0x%x is %u here and %u there\n", sym, ours,
                    theirs);
            }
            disagreed++;
        }
    }

    check(disagreed == 0, "the Unicode range agrees all the way across");
    printf("  %ld compared, %ld disagreements\n", compared, disagreed);

    /*
     * **And the top of it is bounded.** 0x10ffff is the last codepoint there
     * is; a keysym above that is a number in the Unicode range which is not a
     * character, and handing it back would put an impossible codepoint into
     * somebody's document.
     */
    check(recon_key_to_char(0x01000000 + 0x10ffff) == 0x10ffff,
        "the last real codepoint comes back");
    check(recon_key_to_char(0x01000000 + 0x110000) == 0,
        "and the first impossible one does not");

    /*
     * And the hole in the middle: the surrogate block, 0xd800 to 0xdfff.
     * These are the halves of a UTF-16 pair and are not characters on their
     * own, so returning one would put text into a document that is not valid
     * Unicode. The sweep above found this -- 292 disagreements, all of them
     * here.
     */
    check(recon_key_to_char(0x01000000 + 0xd800) == 0,
        "the first surrogate is not a character");
    check(recon_key_to_char(0x01000000 + 0xdfff) == 0,
        "nor is the last one");
    check(recon_key_to_char(0x01000000 + 0xd7ff) == 0xd7ff,
        "and the codepoint just below the block still is");
    check(recon_key_to_char(0x01000000 + 0xe000) == 0xe000,
        "as is the one just above it");
    check(recon_key_to_char(0xffffffff) == 0, "nor does nonsense");
}

static void test_the_keys_that_are_not_letters(void) {
    printf("a key that is not a character says so\n");

    /*
     * The reason this matters: a caller that inserts whatever comes back
     * without checking would put a NUL in the middle of a document every time
     * somebody pressed an arrow. Notepad, the Terminal and the text field all
     * read a key this way.
     */
    check(recon_key_to_char(RECON_KEY_Left) == 0, "Left is not a letter");
    check(recon_key_to_char(RECON_KEY_F5) == 0, "nor is F5");
    check(recon_key_to_char(RECON_KEY_Shift_L) == 0, "nor is Shift");
    check(recon_key_to_char(RECON_KEY_Home) == 0, "nor is Home");
    check(recon_key_to_char(RECON_KEY_NoSymbol) == 0, "and no key is no key");

    /* The ones that are. */
    check(recon_key_to_char(RECON_KEY_Return) == '\r', "Return is a return");
    check(recon_key_to_char(RECON_KEY_Tab) == '\t', "Tab is a tab");
    check(recon_key_to_char(RECON_KEY_BackSpace) == 8,
        "BackSpace is a backspace");
    check(recon_key_to_char(RECON_KEY_Escape) == 27, "Escape is an escape");
    check(recon_key_to_char(RECON_KEY_Delete) == 127, "Delete is a delete");
}

static void test_the_keypad(void) {
    printf("the number pad prints what it says on it\n");

    /*
     * Somebody pressing the 7 on the number pad meant a seven. If the caller
     * had to know the difference it would be got wrong in one of the several
     * places that read a key -- which is how a number pad ends up working in
     * the calculator and not in a form.
     */
    check(recon_key_to_char(RECON_KEY_KP_7) == '7', "KP_7 is a seven");
    check(recon_key_to_char(RECON_KEY_KP_0) == '0', "KP_0 is a zero");
    check(recon_key_to_char(RECON_KEY_KP_Add) == '+', "KP_Add is a plus");
    check(recon_key_to_char(RECON_KEY_KP_Decimal) == '.',
        "KP_Decimal is a point");
    check(recon_key_to_char(RECON_KEY_KP_Enter) == '\r',
        "and KP_Enter is a return, the same as the other one");

    /* But the navigation half of the keypad is still navigation. */
    check(recon_key_to_char(RECON_KEY_KP_Left) == 0,
        "KP_Left is not a character");
}

int main(void) {
    printf("ReconOS key tests\n\n");

    test_every_name_agrees();
    test_a_name_it_does_not_know();
    test_names_are_case_insensitive();
    test_the_characters();
    test_the_unicode_range();
    test_the_keys_that_are_not_letters();
    test_the_keypad();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
