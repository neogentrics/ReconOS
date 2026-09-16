/*
 * Keys, and the two questions anything asks about one.
 *
 * Freestanding by construction: `strcasecmp` is the only library call in the
 * file and `userland/libc` answers it. That is the point -- this file exists so
 * that twenty-four others stop needing a Linux keyboard library to say the
 * word "Escape".
 *
 * See include/recon_key.h for where the numbers came from and what holds them
 * to it.
 */

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "recon_key.h"

uint32_t recon_key_to_char(recon_keysym sym) {
    /*
     * Latin-1 is the identity, and it is the whole of the common case.
     *
     * X11 numbered the printable ASCII keysyms as their own character codes
     * and then did the same for the rest of Latin-1, which is why a table is
     * not needed for ninety-five of these and is needed for about thirty.
     */
    if (sym >= 0x20 && sym <= 0x7e) {
        return sym;
    }
    if (sym >= 0xa0 && sym <= 0xff) {
        return sym;
    }

    /*
     * Everything else Unicode has, in the range X11 set aside for it.
     *
     * Bounded at the top: 0x10ffff is the last codepoint there is, and a
     * keysym past that is a number in the Unicode range that is not a
     * character. Returning it anyway would put an impossible codepoint into a
     * document.
     */
    if (sym >= 0x01000000 && sym <= 0x0110ffff) {
        uint32_t code = sym - 0x01000000;

        /*
         * **A surrogate is not a character.**
         *
         * 0xd800 to 0xdfff are the two halves of a UTF-16 pair and mean
         * nothing on their own. Handing one back would put an unpaired
         * surrogate into a document -- text that is not valid Unicode, which
         * every later reader is entitled to refuse and most will instead
         * render as a replacement box a long way from here.
         *
         * Found by sweeping against xkbcommon, which returns zero for the
         * whole block. 292 disagreements, all of them this.
         */
        if (code >= 0xd800 && code <= 0xdfff) {
            return 0;
        }
        return code;
    }

    switch (sym) {
    /* The control keys that are characters, and only those. */
    case RECON_KEY_BackSpace:    return 0x08;
    case RECON_KEY_Tab:          return 0x09;
    case RECON_KEY_Linefeed:     return 0x0a;
    case RECON_KEY_Clear:        return 0x0b;
    case RECON_KEY_Return:       return 0x0d;
    case RECON_KEY_Escape:       return 0x1b;
    case RECON_KEY_Delete:       return 0x7f;

    /*
     * The keypad, as what it prints.
     *
     * Somebody pressing the 7 on the number pad meant a seven. A caller that
     * had to know the difference would get it wrong in one of the several
     * places that read a key, which is how a number pad ends up working in the
     * calculator and not in a form.
     */
    case RECON_KEY_KP_Space:     return ' ';
    case RECON_KEY_KP_Tab:       return 0x09;
    case RECON_KEY_KP_Enter:     return 0x0d;
    case RECON_KEY_KP_Equal:     return '=';
    case RECON_KEY_KP_Multiply:  return '*';
    case RECON_KEY_KP_Add:       return '+';
    case RECON_KEY_KP_Separator: return ',';
    case RECON_KEY_KP_Subtract:  return '-';
    case RECON_KEY_KP_Decimal:   return '.';
    case RECON_KEY_KP_Divide:    return '/';
    case RECON_KEY_KP_0:         return '0';
    case RECON_KEY_KP_1:         return '1';
    case RECON_KEY_KP_2:         return '2';
    case RECON_KEY_KP_3:         return '3';
    case RECON_KEY_KP_4:         return '4';
    case RECON_KEY_KP_5:         return '5';
    case RECON_KEY_KP_6:         return '6';
    case RECON_KEY_KP_7:         return '7';
    case RECON_KEY_KP_8:         return '8';
    case RECON_KEY_KP_9:         return '9';

    default:
        /* A key, and not a letter. Zero rather than a guess. */
        return 0;
    }
}

/*
 * Every key this system can name.
 *
 * A table rather than a generated hash: it is looked up when somebody types a
 * key name at the Terminal, which is not a path worth a data structure.
 */
static const struct {
    const char *name;
    recon_keysym sym;
} NAMES[] = {
    { "space", RECON_KEY_space },
    { "exclam", RECON_KEY_exclam },
    { "quotedbl", RECON_KEY_quotedbl },
    { "numbersign", RECON_KEY_numbersign },
    { "dollar", RECON_KEY_dollar },
    { "percent", RECON_KEY_percent },
    { "ampersand", RECON_KEY_ampersand },
    { "apostrophe", RECON_KEY_apostrophe },
    { "parenleft", RECON_KEY_parenleft },
    { "parenright", RECON_KEY_parenright },
    { "asterisk", RECON_KEY_asterisk },
    { "plus", RECON_KEY_plus },
    { "comma", RECON_KEY_comma },
    { "minus", RECON_KEY_minus },
    { "period", RECON_KEY_period },
    { "slash", RECON_KEY_slash },
    { "0", RECON_KEY_0 },
    { "1", RECON_KEY_1 },
    { "2", RECON_KEY_2 },
    { "3", RECON_KEY_3 },
    { "4", RECON_KEY_4 },
    { "5", RECON_KEY_5 },
    { "6", RECON_KEY_6 },
    { "7", RECON_KEY_7 },
    { "8", RECON_KEY_8 },
    { "9", RECON_KEY_9 },
    { "colon", RECON_KEY_colon },
    { "semicolon", RECON_KEY_semicolon },
    { "less", RECON_KEY_less },
    { "equal", RECON_KEY_equal },
    { "greater", RECON_KEY_greater },
    { "question", RECON_KEY_question },
    { "at", RECON_KEY_at },
    { "A", RECON_KEY_A },
    { "B", RECON_KEY_B },
    { "C", RECON_KEY_C },
    { "D", RECON_KEY_D },
    { "E", RECON_KEY_E },
    { "F", RECON_KEY_F },
    { "G", RECON_KEY_G },
    { "H", RECON_KEY_H },
    { "I", RECON_KEY_I },
    { "J", RECON_KEY_J },
    { "K", RECON_KEY_K },
    { "L", RECON_KEY_L },
    { "M", RECON_KEY_M },
    { "N", RECON_KEY_N },
    { "O", RECON_KEY_O },
    { "P", RECON_KEY_P },
    { "Q", RECON_KEY_Q },
    { "R", RECON_KEY_R },
    { "S", RECON_KEY_S },
    { "T", RECON_KEY_T },
    { "U", RECON_KEY_U },
    { "V", RECON_KEY_V },
    { "W", RECON_KEY_W },
    { "X", RECON_KEY_X },
    { "Y", RECON_KEY_Y },
    { "Z", RECON_KEY_Z },
    { "bracketleft", RECON_KEY_bracketleft },
    { "backslash", RECON_KEY_backslash },
    { "bracketright", RECON_KEY_bracketright },
    { "asciicircum", RECON_KEY_asciicircum },
    { "underscore", RECON_KEY_underscore },
    { "grave", RECON_KEY_grave },
    { "a", RECON_KEY_a },
    { "b", RECON_KEY_b },
    { "c", RECON_KEY_c },
    { "d", RECON_KEY_d },
    { "e", RECON_KEY_e },
    { "f", RECON_KEY_f },
    { "g", RECON_KEY_g },
    { "h", RECON_KEY_h },
    { "i", RECON_KEY_i },
    { "j", RECON_KEY_j },
    { "k", RECON_KEY_k },
    { "l", RECON_KEY_l },
    { "m", RECON_KEY_m },
    { "n", RECON_KEY_n },
    { "o", RECON_KEY_o },
    { "p", RECON_KEY_p },
    { "q", RECON_KEY_q },
    { "r", RECON_KEY_r },
    { "s", RECON_KEY_s },
    { "t", RECON_KEY_t },
    { "u", RECON_KEY_u },
    { "v", RECON_KEY_v },
    { "w", RECON_KEY_w },
    { "x", RECON_KEY_x },
    { "y", RECON_KEY_y },
    { "z", RECON_KEY_z },
    { "braceleft", RECON_KEY_braceleft },
    { "bar", RECON_KEY_bar },
    { "braceright", RECON_KEY_braceright },
    { "asciitilde", RECON_KEY_asciitilde },
    { "BackSpace", RECON_KEY_BackSpace },
    { "Tab", RECON_KEY_Tab },
    { "ISO_Left_Tab", RECON_KEY_ISO_Left_Tab },
    { "Linefeed", RECON_KEY_Linefeed },
    { "Clear", RECON_KEY_Clear },
    { "Return", RECON_KEY_Return },
    { "Pause", RECON_KEY_Pause },
    { "Scroll_Lock", RECON_KEY_Scroll_Lock },
    { "Sys_Req", RECON_KEY_Sys_Req },
    { "Escape", RECON_KEY_Escape },
    { "Delete", RECON_KEY_Delete },
    { "Home", RECON_KEY_Home },
    { "Left", RECON_KEY_Left },
    { "Up", RECON_KEY_Up },
    { "Right", RECON_KEY_Right },
    { "Down", RECON_KEY_Down },
    { "Page_Up", RECON_KEY_Page_Up },
    { "Page_Down", RECON_KEY_Page_Down },
    { "End", RECON_KEY_End },
    { "Begin", RECON_KEY_Begin },
    { "Insert", RECON_KEY_Insert },
    { "Menu", RECON_KEY_Menu },
    { "Print", RECON_KEY_Print },
    { "Help", RECON_KEY_Help },
    { "Break", RECON_KEY_Break },
    { "Num_Lock", RECON_KEY_Num_Lock },
    { "Caps_Lock", RECON_KEY_Caps_Lock },
    { "Shift_Lock", RECON_KEY_Shift_Lock },
    { "Shift_L", RECON_KEY_Shift_L },
    { "Shift_R", RECON_KEY_Shift_R },
    { "Control_L", RECON_KEY_Control_L },
    { "Control_R", RECON_KEY_Control_R },
    { "Alt_L", RECON_KEY_Alt_L },
    { "Alt_R", RECON_KEY_Alt_R },
    { "Super_L", RECON_KEY_Super_L },
    { "Super_R", RECON_KEY_Super_R },
    { "Meta_L", RECON_KEY_Meta_L },
    { "Meta_R", RECON_KEY_Meta_R },
    { "F1", RECON_KEY_F1 },
    { "F2", RECON_KEY_F2 },
    { "F3", RECON_KEY_F3 },
    { "F4", RECON_KEY_F4 },
    { "F5", RECON_KEY_F5 },
    { "F6", RECON_KEY_F6 },
    { "F7", RECON_KEY_F7 },
    { "F8", RECON_KEY_F8 },
    { "F9", RECON_KEY_F9 },
    { "F10", RECON_KEY_F10 },
    { "F11", RECON_KEY_F11 },
    { "F12", RECON_KEY_F12 },
    { "KP_Space", RECON_KEY_KP_Space },
    { "KP_Tab", RECON_KEY_KP_Tab },
    { "KP_Enter", RECON_KEY_KP_Enter },
    { "KP_Home", RECON_KEY_KP_Home },
    { "KP_Left", RECON_KEY_KP_Left },
    { "KP_Up", RECON_KEY_KP_Up },
    { "KP_Right", RECON_KEY_KP_Right },
    { "KP_Down", RECON_KEY_KP_Down },
    { "KP_Page_Up", RECON_KEY_KP_Page_Up },
    { "KP_Page_Down", RECON_KEY_KP_Page_Down },
    { "KP_End", RECON_KEY_KP_End },
    { "KP_Begin", RECON_KEY_KP_Begin },
    { "KP_Insert", RECON_KEY_KP_Insert },
    { "KP_Delete", RECON_KEY_KP_Delete },
    { "KP_Equal", RECON_KEY_KP_Equal },
    { "KP_Multiply", RECON_KEY_KP_Multiply },
    { "KP_Add", RECON_KEY_KP_Add },
    { "KP_Separator", RECON_KEY_KP_Separator },
    { "KP_Subtract", RECON_KEY_KP_Subtract },
    { "KP_Decimal", RECON_KEY_KP_Decimal },
    { "KP_Divide", RECON_KEY_KP_Divide },
    { "KP_0", RECON_KEY_KP_0 },
    { "KP_1", RECON_KEY_KP_1 },
    { "KP_2", RECON_KEY_KP_2 },
    { "KP_3", RECON_KEY_KP_3 },
    { "KP_4", RECON_KEY_KP_4 },
    { "KP_5", RECON_KEY_KP_5 },
    { "KP_6", RECON_KEY_KP_6 },
    { "KP_7", RECON_KEY_KP_7 },
    { "KP_8", RECON_KEY_KP_8 },
    { "KP_9", RECON_KEY_KP_9 },
};

recon_keysym recon_key_from_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return RECON_KEY_NoSymbol;
    }

    /*
     * **Exactly, first.**
     *
     * `a` and `A` are different keys whose names differ only in case, and a
     * case-insensitive scan returns whichever the table lists first -- the
     * capital, because the table is in value order and 0x41 comes before
     * 0x61. So `key a` at the Terminal injected a capital A, and every one of
     * the twenty-six letters was wrong the same way.
     *
     * Case-insensitivity is here so that somebody does not have to know X11
     * spelled it `Page_Up`. It was never meant to mean that a name matching a
     * key exactly can come back as a different key.
     */
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strcmp(NAMES[i].name, name) == 0) {
            return NAMES[i].sym;
        }
    }

    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strcasecmp(NAMES[i].name, name) == 0) {
            return NAMES[i].sym;
        }
    }

    return RECON_KEY_NoSymbol;
}

/* --- The machine's own keyboard ----------------------------------------- */

/*
 * One row of the layout: what a key produces, plain and with Shift.
 *
 * Indexed by USB HID usage code. The gaps are keys this layout has no meaning
 * for, and they stay `RECON_KEY_NoSymbol` -- **a real answer**, because a
 * keyboard can send a code for a key nobody here has and inventing a letter
 * for it would type something the person did not press.
 */
struct layout_row {
    recon_keysym plain;
    recon_keysym shifted;
};

/*
 * The US layout, by HID usage code.
 *
 * The letters and digits are runs and are filled in by the lookup rather than
 * written out a hundred times -- `KEY_A` is 4 and `KEY_Z` is 29, contiguous,
 * which `kernel/include/recon/kernel/input.h` states by naming only the two
 * ends. What is written out is everything that is *not* a run, because that is
 * where a table earns its place: the punctuation, where plain and shifted have
 * no arithmetic relationship at all.
 */
#define HID_A            4
#define HID_Z            29
#define HID_1            30
#define HID_9            38
#define HID_0            39
#define HID_ENTER        40
#define HID_ESCAPE       41
#define HID_BACKSPACE    42
#define HID_TAB          43
#define HID_SPACE        44
#define HID_MINUS        45
#define HID_EQUAL        46
#define HID_LEFTBRACKET  47
#define HID_RIGHTBRACKET 48
#define HID_BACKSLASH    49
#define HID_NONUS_HASH   50
#define HID_SEMICOLON    51
#define HID_APOSTROPHE   52
#define HID_GRAVE        53
#define HID_COMMA        54
#define HID_PERIOD       55
#define HID_SLASH        56
#define HID_CAPSLOCK     57
#define HID_F1           58
#define HID_F12          69
#define HID_PRINTSCREEN  70
#define HID_SCROLLLOCK   71
#define HID_PAUSE        72
#define HID_INSERT       73
#define HID_HOME         74
#define HID_PAGEUP       75
#define HID_DELETE       76
#define HID_END          77
#define HID_PAGEDOWN     78
#define HID_RIGHT        79
#define HID_LEFT         80
#define HID_DOWN         81
#define HID_UP           82
#define HID_NUMLOCK      83
#define HID_KP_DIVIDE    84
#define HID_KP_MULTIPLY  85
#define HID_KP_MINUS     86
#define HID_KP_PLUS      87
#define HID_KP_ENTER     88
#define HID_KP_1         89
#define HID_KP_9         97
#define HID_KP_0         98
#define HID_KP_PERIOD    99
#define HID_LEFTCTRL     224
#define HID_LEFTSHIFT    225
#define HID_LEFTALT      226
#define HID_LEFTMETA     227
#define HID_RIGHTCTRL    228
#define HID_RIGHTSHIFT   229
#define HID_LEFTALT_GR   230
#define HID_RIGHTMETA    231

#define HID_MAX 232

static const struct layout_row US[HID_MAX] = {
    /*
     * The punctuation, where plain and shifted are unrelated numbers and a
     * table is the only honest way to say so.
     */
    [HID_MINUS]         = { RECON_KEY_minus,        RECON_KEY_underscore },
    [HID_EQUAL]         = { RECON_KEY_equal,        RECON_KEY_plus },
    [HID_LEFTBRACKET]   = { RECON_KEY_bracketleft,  RECON_KEY_braceleft },
    [HID_RIGHTBRACKET]  = { RECON_KEY_bracketright, RECON_KEY_braceright },
    [HID_BACKSLASH]     = { RECON_KEY_backslash,    RECON_KEY_bar },
    /*
     * The key a US keyboard does not have, and which arrives anyway.
     *
     * HID 50 is the extra key beside the left Shift on most of the world's
     * keyboards. On a US layout it produces what the backslash key does, which
     * is what every other system does with it -- and is better than nothing,
     * because somebody typing on a board that has it gets a character rather
     * than silence.
     */
    [HID_NONUS_HASH]    = { RECON_KEY_backslash,    RECON_KEY_bar },
    [HID_SEMICOLON]     = { RECON_KEY_semicolon,    RECON_KEY_colon },
    [HID_APOSTROPHE]    = { RECON_KEY_apostrophe,   RECON_KEY_quotedbl },
    [HID_GRAVE]         = { RECON_KEY_grave,        RECON_KEY_asciitilde },
    [HID_COMMA]         = { RECON_KEY_comma,        RECON_KEY_less },
    [HID_PERIOD]        = { RECON_KEY_period,       RECON_KEY_greater },
    [HID_SLASH]         = { RECON_KEY_slash,        RECON_KEY_question },

    /* The ones that are the same whatever is held. */
    [HID_ENTER]         = { RECON_KEY_Return,       RECON_KEY_Return },
    [HID_ESCAPE]        = { RECON_KEY_Escape,       RECON_KEY_Escape },
    [HID_BACKSPACE]     = { RECON_KEY_BackSpace,    RECON_KEY_BackSpace },
    [HID_SPACE]         = { RECON_KEY_space,        RECON_KEY_space },
    [HID_CAPSLOCK]      = { RECON_KEY_Caps_Lock,    RECON_KEY_Caps_Lock },

    /*
     * Tab, and the one key whose shifted form is a different key entirely.
     *
     * Shift-Tab is `ISO_Left_Tab`, not Tab -- which is how every toolkit
     * written since X11 tells "move to the next control" from "move to the
     * previous one". A layout that returned plain Tab for both would make
     * Shift-Tab walk forwards, and the fault would look like a focus bug
     * rather than a keyboard one.
     */
    [HID_TAB]           = { RECON_KEY_Tab,          RECON_KEY_ISO_Left_Tab },

    [HID_PRINTSCREEN]   = { RECON_KEY_Print,        RECON_KEY_Print },
    [HID_SCROLLLOCK]    = { RECON_KEY_Scroll_Lock,  RECON_KEY_Scroll_Lock },
    [HID_PAUSE]         = { RECON_KEY_Pause,        RECON_KEY_Pause },
    [HID_INSERT]        = { RECON_KEY_Insert,       RECON_KEY_Insert },
    [HID_HOME]          = { RECON_KEY_Home,         RECON_KEY_Home },
    [HID_PAGEUP]        = { RECON_KEY_Page_Up,      RECON_KEY_Page_Up },
    [HID_DELETE]        = { RECON_KEY_Delete,       RECON_KEY_Delete },
    [HID_END]           = { RECON_KEY_End,          RECON_KEY_End },
    [HID_PAGEDOWN]      = { RECON_KEY_Page_Down,    RECON_KEY_Page_Down },
    [HID_RIGHT]         = { RECON_KEY_Right,        RECON_KEY_Right },
    [HID_LEFT]          = { RECON_KEY_Left,         RECON_KEY_Left },
    [HID_DOWN]          = { RECON_KEY_Down,         RECON_KEY_Down },
    [HID_UP]            = { RECON_KEY_Up,           RECON_KEY_Up },
    [HID_NUMLOCK]       = { RECON_KEY_Num_Lock,     RECON_KEY_Num_Lock },

    /*
     * The keypad, as the keys it prints.
     *
     * Not as the navigation the keys *also* mean with Num Lock off -- this
     * machine has no Num Lock state to consult, and the kernel sends the same
     * code either way. Somebody who presses the 7 on the number pad meant a
     * seven; that is the same argument `recon_key_to_char` makes about the
     * same keys, and the two agree on purpose.
     */
    [HID_KP_DIVIDE]     = { RECON_KEY_KP_Divide,    RECON_KEY_KP_Divide },
    [HID_KP_MULTIPLY]   = { RECON_KEY_KP_Multiply,  RECON_KEY_KP_Multiply },
    [HID_KP_MINUS]      = { RECON_KEY_KP_Subtract,  RECON_KEY_KP_Subtract },
    [HID_KP_PLUS]       = { RECON_KEY_KP_Add,       RECON_KEY_KP_Add },
    [HID_KP_ENTER]      = { RECON_KEY_KP_Enter,     RECON_KEY_KP_Enter },
    [HID_KP_PERIOD]     = { RECON_KEY_KP_Decimal,   RECON_KEY_KP_Decimal },

    /* The modifiers themselves, which are keys too and have names. */
    [HID_LEFTCTRL]      = { RECON_KEY_Control_L,    RECON_KEY_Control_L },
    [HID_LEFTSHIFT]     = { RECON_KEY_Shift_L,      RECON_KEY_Shift_L },
    [HID_LEFTALT]       = { RECON_KEY_Alt_L,        RECON_KEY_Alt_L },
    [HID_LEFTMETA]      = { RECON_KEY_Super_L,      RECON_KEY_Super_L },
    [HID_RIGHTCTRL]     = { RECON_KEY_Control_R,    RECON_KEY_Control_R },
    [HID_RIGHTSHIFT]    = { RECON_KEY_Shift_R,      RECON_KEY_Shift_R },
    [HID_LEFTALT_GR]    = { RECON_KEY_Alt_R,        RECON_KEY_Alt_R },
    [HID_RIGHTMETA]     = { RECON_KEY_Super_R,      RECON_KEY_Super_R },
};

/*
 * The digits, whose shifted forms are the one part of a US keyboard with no
 * pattern at all: 1 gives !, 2 gives @, 3 gives #, and so on with no rule
 * anybody could derive. Written down in the order the keys sit.
 */
static const recon_keysym DIGIT_SHIFTED[10] = {
    RECON_KEY_exclam,      /* 1 */
    RECON_KEY_at,          /* 2 */
    RECON_KEY_numbersign,  /* 3 */
    RECON_KEY_dollar,      /* 4 */
    RECON_KEY_percent,     /* 5 */
    RECON_KEY_asciicircum, /* 6 */
    RECON_KEY_ampersand,   /* 7 */
    RECON_KEY_asterisk,    /* 8 */
    RECON_KEY_parenleft,   /* 9 */
    RECON_KEY_parenright,  /* 0 */
};

recon_keysym recon_key_from_hid(unsigned keycode, unsigned modifiers)
{
    bool shift = (modifiers & RECON_MOD_SHIFT) != 0;
    bool caps = (modifiers & RECON_MOD_CAPS) != 0;

    /*
     * Letters, where Shift and Caps Lock combine by *exclusive or*.
     *
     * Caps on and Shift held gives a small letter, which is what a keyboard
     * does and what somebody holding Shift to "undo" Caps Lock expects. A
     * layout that treated them as "either one capitalises" would make Caps
     * Lock impossible to type around.
     */
    if (keycode >= HID_A && keycode <= HID_Z) {
        unsigned n = keycode - HID_A;
        return (shift != caps)
            ? (recon_keysym)(RECON_KEY_A + n)
            : (recon_keysym)(RECON_KEY_a + n);
    }

    /*
     * Digits, where Caps Lock does nothing.
     *
     * Shift-2 is an at-sign whether or not Caps Lock is on. Everybody who has
     * used a keyboard knows this and almost nobody writes it down, which is
     * exactly why it is written down here -- it is one `||` away from being
     * wrong, and the fault would be somebody's password typing as `@` when
     * they meant `2`.
     */
    if (keycode >= HID_1 && keycode <= HID_0) {
        unsigned n = (keycode == HID_0) ? 9 : (keycode - HID_1);
        if (shift) {
            return DIGIT_SHIFTED[n];
        }
        return (keycode == HID_0)
            ? RECON_KEY_0
            : (recon_keysym)(RECON_KEY_1 + n);
    }

    /* The keypad digits, which are their own keys and not the number row. */
    if (keycode >= HID_KP_1 && keycode <= HID_KP_9) {
        return (recon_keysym)(RECON_KEY_KP_1 + (keycode - HID_KP_1));
    }
    if (keycode == HID_KP_0) {
        return RECON_KEY_KP_0;
    }

    /* The function keys, which are a run like the letters. */
    if (keycode >= HID_F1 && keycode <= HID_F12) {
        return (recon_keysym)(RECON_KEY_F1 + (keycode - HID_F1));
    }

    if (keycode >= HID_MAX) {
        return RECON_KEY_NoSymbol;
    }

    return shift ? US[keycode].shifted : US[keycode].plain;
}

unsigned recon_key_modifiers_after(unsigned held, unsigned keycode,
    bool pressed)
{
    unsigned bit = 0;

    switch (keycode) {
    case HID_LEFTCTRL:
    case HID_RIGHTCTRL:
        bit = RECON_MOD_CTRL;
        break;
    case HID_LEFTSHIFT:
    case HID_RIGHTSHIFT:
        bit = RECON_MOD_SHIFT;
        break;
    case HID_LEFTALT:
    case HID_LEFTALT_GR:
        bit = RECON_MOD_ALT;
        break;
    case HID_LEFTMETA:
    case HID_RIGHTMETA:
        bit = RECON_MOD_LOGO;
        break;

    case HID_CAPSLOCK:
        /*
         * **A latch, not something held.** It turns over on a press and does
         * nothing on a release -- which is the whole difference between it
         * and Shift, and the reason it is a separate case rather than another
         * bit in the list above.
         */
        return pressed ? (held ^ RECON_MOD_CAPS) : held;

    default:
        /* Not a modifier. Nothing about what is held has changed. */
        return held;
    }

    /*
     * A release clears the bit whether or not a press was ever seen.
     *
     * The kernel's own queue makes the same choice about a release for a key
     * nobody saw pressed -- *"a no-op, never a decrement"* -- and the reason
     * is the same one: the machine's idea of what is held has to be able to
     * recover from having missed something. A Shift that is stuck on because
     * one event was lost is a keyboard that types in capitals until it is
     * rebooted.
     */
    return pressed ? (held | bit) : (held & ~bit);
}
