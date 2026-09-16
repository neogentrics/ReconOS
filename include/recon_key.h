/*
 * The keys, as ReconOS's own.
 *
 * --- why this file exists -----------------------------------------------
 *
 * `include/recon_ui.h` included `<xkbcommon/xkbcommon.h>` for exactly one
 * thing: the typedef `xkb_keysym_t`, which is a `uint32_t`. That one include
 * put **twenty-four of the desktop's source files** out of reach of a compiler
 * with no Linux underneath it -- the file manager, the terminal, Notepad, the
 * browser window, Help, the Control Panel, the theme engine, the widget layer.
 * Not because any of them talk to a keyboard library, but because they name a
 * key.
 *
 * xkbcommon is a Wayland dependency of the compositor. A program on the
 * ReconOS kernel will not have it, and the kernel's keyboard will hand up
 * something of its own.
 *
 * --- borrowed once, owned afterwards ------------------------------------
 *
 * The numbers are X11 keysym values, which is what xkbcommon uses, and they
 * were copied out of xkbcommon once by asking it rather than by reading a
 * table into this file by hand. **The same shape as the icons and the trusted
 * roots: borrowed at the boundary, owned afterwards.**
 *
 * Keeping the numbering is deliberate, not laziness. The compositor half still
 * runs on wlroots today and hands these values straight up, so a second
 * numbering would mean a translation layer in the one place a mistake is
 * invisible -- a key that does the wrong thing looks like a key that does
 * nothing.
 *
 * **What stops it drifting is `tests/test_key.c`**, which holds every constant
 * in this file against xkbcommon's answer for the same name, and sweeps both
 * conversions across the whole keysym range. A borrowed table with nothing
 * checking the borrow is a copy that is right on the day it is made.
 */

/*
 * The guard is not `RECON_KEY_H`, and that is not a style choice.
 *
 * This file declares a constant for every letter, so `RECON_KEY_H` is the H
 * key -- and an include guard by that name is a macro that expands inside the
 * enum it is guarding. The compiler says `expected identifier before '='` on
 * the line for H and nothing at all about the guard.
 */
#ifndef RECON_KEY_H_INCLUDED
#define RECON_KEY_H_INCLUDED

#include <stdint.h>

/*
 * A key, as a symbol rather than as a position.
 *
 * The distinction matters: a *keycode* is which switch on the board went down
 * and means nothing without a layout, and a *keysym* is what that key means
 * once the layout has been applied. Everything above the compositor wants the
 * second one -- a shortcut is Ctrl and the letter C, on whatever board.
 */
typedef uint32_t recon_keysym;

/* No key. Zero, so a zeroed structure holds one by default. */
#define RECON_KEY_NoSymbol ((recon_keysym)0)

enum {
    /* --- printable, where the value is the character --- */
    RECON_KEY_space            = 0x00000020,
    RECON_KEY_exclam           = 0x00000021,
    RECON_KEY_quotedbl         = 0x00000022,
    RECON_KEY_numbersign       = 0x00000023,
    RECON_KEY_dollar           = 0x00000024,
    RECON_KEY_percent          = 0x00000025,
    RECON_KEY_ampersand        = 0x00000026,
    RECON_KEY_apostrophe       = 0x00000027,
    RECON_KEY_parenleft        = 0x00000028,
    RECON_KEY_parenright       = 0x00000029,
    RECON_KEY_asterisk         = 0x0000002a,
    RECON_KEY_plus             = 0x0000002b,
    RECON_KEY_comma            = 0x0000002c,
    RECON_KEY_minus            = 0x0000002d,
    RECON_KEY_period           = 0x0000002e,
    RECON_KEY_slash            = 0x0000002f,
    RECON_KEY_0                = 0x00000030,
    RECON_KEY_1                = 0x00000031,
    RECON_KEY_2                = 0x00000032,
    RECON_KEY_3                = 0x00000033,
    RECON_KEY_4                = 0x00000034,
    RECON_KEY_5                = 0x00000035,
    RECON_KEY_6                = 0x00000036,
    RECON_KEY_7                = 0x00000037,
    RECON_KEY_8                = 0x00000038,
    RECON_KEY_9                = 0x00000039,
    RECON_KEY_colon            = 0x0000003a,
    RECON_KEY_semicolon        = 0x0000003b,
    RECON_KEY_less             = 0x0000003c,
    RECON_KEY_equal            = 0x0000003d,
    RECON_KEY_greater          = 0x0000003e,
    RECON_KEY_question         = 0x0000003f,
    RECON_KEY_at               = 0x00000040,
    RECON_KEY_A                = 0x00000041,
    RECON_KEY_B                = 0x00000042,
    RECON_KEY_C                = 0x00000043,
    RECON_KEY_D                = 0x00000044,
    RECON_KEY_E                = 0x00000045,
    RECON_KEY_F                = 0x00000046,
    RECON_KEY_G                = 0x00000047,
    RECON_KEY_H                = 0x00000048,
    RECON_KEY_I                = 0x00000049,
    RECON_KEY_J                = 0x0000004a,
    RECON_KEY_K                = 0x0000004b,
    RECON_KEY_L                = 0x0000004c,
    RECON_KEY_M                = 0x0000004d,
    RECON_KEY_N                = 0x0000004e,
    RECON_KEY_O                = 0x0000004f,
    RECON_KEY_P                = 0x00000050,
    RECON_KEY_Q                = 0x00000051,
    RECON_KEY_R                = 0x00000052,
    RECON_KEY_S                = 0x00000053,
    RECON_KEY_T                = 0x00000054,
    RECON_KEY_U                = 0x00000055,
    RECON_KEY_V                = 0x00000056,
    RECON_KEY_W                = 0x00000057,
    RECON_KEY_X                = 0x00000058,
    RECON_KEY_Y                = 0x00000059,
    RECON_KEY_Z                = 0x0000005a,
    RECON_KEY_bracketleft      = 0x0000005b,
    RECON_KEY_backslash        = 0x0000005c,
    RECON_KEY_bracketright     = 0x0000005d,
    RECON_KEY_asciicircum      = 0x0000005e,
    RECON_KEY_underscore       = 0x0000005f,
    RECON_KEY_grave            = 0x00000060,
    RECON_KEY_a                = 0x00000061,
    RECON_KEY_b                = 0x00000062,
    RECON_KEY_c                = 0x00000063,
    RECON_KEY_d                = 0x00000064,
    RECON_KEY_e                = 0x00000065,
    RECON_KEY_f                = 0x00000066,
    RECON_KEY_g                = 0x00000067,
    RECON_KEY_h                = 0x00000068,
    RECON_KEY_i                = 0x00000069,
    RECON_KEY_j                = 0x0000006a,
    RECON_KEY_k                = 0x0000006b,
    RECON_KEY_l                = 0x0000006c,
    RECON_KEY_m                = 0x0000006d,
    RECON_KEY_n                = 0x0000006e,
    RECON_KEY_o                = 0x0000006f,
    RECON_KEY_p                = 0x00000070,
    RECON_KEY_q                = 0x00000071,
    RECON_KEY_r                = 0x00000072,
    RECON_KEY_s                = 0x00000073,
    RECON_KEY_t                = 0x00000074,
    RECON_KEY_u                = 0x00000075,
    RECON_KEY_v                = 0x00000076,
    RECON_KEY_w                = 0x00000077,
    RECON_KEY_x                = 0x00000078,
    RECON_KEY_y                = 0x00000079,
    RECON_KEY_z                = 0x0000007a,
    RECON_KEY_braceleft        = 0x0000007b,
    RECON_KEY_bar              = 0x0000007c,
    RECON_KEY_braceright       = 0x0000007d,
    RECON_KEY_asciitilde       = 0x0000007e,

    /* --- named --- */
    RECON_KEY_BackSpace        = 0x0000ff08,
    RECON_KEY_Tab              = 0x0000ff09,
    RECON_KEY_ISO_Left_Tab     = 0x0000fe20,
    RECON_KEY_Linefeed         = 0x0000ff0a,
    RECON_KEY_Clear            = 0x0000ff0b,
    RECON_KEY_Return           = 0x0000ff0d,
    RECON_KEY_Pause            = 0x0000ff13,
    RECON_KEY_Scroll_Lock      = 0x0000ff14,
    RECON_KEY_Sys_Req          = 0x0000ff15,
    RECON_KEY_Escape           = 0x0000ff1b,
    RECON_KEY_Delete           = 0x0000ffff,
    RECON_KEY_Home             = 0x0000ff50,
    RECON_KEY_Left             = 0x0000ff51,
    RECON_KEY_Up               = 0x0000ff52,
    RECON_KEY_Right            = 0x0000ff53,
    RECON_KEY_Down             = 0x0000ff54,
    RECON_KEY_Page_Up          = 0x0000ff55,
    RECON_KEY_Page_Down        = 0x0000ff56,
    RECON_KEY_End              = 0x0000ff57,
    RECON_KEY_Begin            = 0x0000ff58,
    RECON_KEY_Insert           = 0x0000ff63,
    RECON_KEY_Menu             = 0x0000ff67,
    RECON_KEY_Print            = 0x0000ff61,
    RECON_KEY_Help             = 0x0000ff6a,
    RECON_KEY_Break            = 0x0000ff6b,
    RECON_KEY_Num_Lock         = 0x0000ff7f,
    RECON_KEY_Caps_Lock        = 0x0000ffe5,
    RECON_KEY_Shift_Lock       = 0x0000ffe6,
    RECON_KEY_Shift_L          = 0x0000ffe1,
    RECON_KEY_Shift_R          = 0x0000ffe2,
    RECON_KEY_Control_L        = 0x0000ffe3,
    RECON_KEY_Control_R        = 0x0000ffe4,
    RECON_KEY_Alt_L            = 0x0000ffe9,
    RECON_KEY_Alt_R            = 0x0000ffea,
    RECON_KEY_Super_L          = 0x0000ffeb,
    RECON_KEY_Super_R          = 0x0000ffec,
    RECON_KEY_Meta_L           = 0x0000ffe7,
    RECON_KEY_Meta_R           = 0x0000ffe8,
    RECON_KEY_F1               = 0x0000ffbe,
    RECON_KEY_F2               = 0x0000ffbf,
    RECON_KEY_F3               = 0x0000ffc0,
    RECON_KEY_F4               = 0x0000ffc1,
    RECON_KEY_F5               = 0x0000ffc2,
    RECON_KEY_F6               = 0x0000ffc3,
    RECON_KEY_F7               = 0x0000ffc4,
    RECON_KEY_F8               = 0x0000ffc5,
    RECON_KEY_F9               = 0x0000ffc6,
    RECON_KEY_F10              = 0x0000ffc7,
    RECON_KEY_F11              = 0x0000ffc8,
    RECON_KEY_F12              = 0x0000ffc9,
    RECON_KEY_KP_Space         = 0x0000ff80,
    RECON_KEY_KP_Tab           = 0x0000ff89,
    RECON_KEY_KP_Enter         = 0x0000ff8d,
    RECON_KEY_KP_Home          = 0x0000ff95,
    RECON_KEY_KP_Left          = 0x0000ff96,
    RECON_KEY_KP_Up            = 0x0000ff97,
    RECON_KEY_KP_Right         = 0x0000ff98,
    RECON_KEY_KP_Down          = 0x0000ff99,
    RECON_KEY_KP_Page_Up       = 0x0000ff9a,
    RECON_KEY_KP_Page_Down     = 0x0000ff9b,
    RECON_KEY_KP_End           = 0x0000ff9c,
    RECON_KEY_KP_Begin         = 0x0000ff9d,
    RECON_KEY_KP_Insert        = 0x0000ff9e,
    RECON_KEY_KP_Delete        = 0x0000ff9f,
    RECON_KEY_KP_Equal         = 0x0000ffbd,
    RECON_KEY_KP_Multiply      = 0x0000ffaa,
    RECON_KEY_KP_Add           = 0x0000ffab,
    RECON_KEY_KP_Separator     = 0x0000ffac,
    RECON_KEY_KP_Subtract      = 0x0000ffad,
    RECON_KEY_KP_Decimal       = 0x0000ffae,
    RECON_KEY_KP_Divide        = 0x0000ffaf,
    RECON_KEY_KP_0             = 0x0000ffb0,
    RECON_KEY_KP_1             = 0x0000ffb1,
    RECON_KEY_KP_2             = 0x0000ffb2,
    RECON_KEY_KP_3             = 0x0000ffb3,
    RECON_KEY_KP_4             = 0x0000ffb4,
    RECON_KEY_KP_5             = 0x0000ffb5,
    RECON_KEY_KP_6             = 0x0000ffb6,
    RECON_KEY_KP_7             = 0x0000ffb7,
    RECON_KEY_KP_8             = 0x0000ffb8,
    RECON_KEY_KP_9             = 0x0000ffb9,
};

/*
 * The character a key stands for, or zero when it does not stand for one.
 *
 * Zero for Left, F5, Shift and the rest: they are keys, and they are not
 * letters. A caller that inserts whatever comes back without checking would
 * otherwise put a NUL in the middle of somebody's document.
 *
 * The keypad answers as the character it prints -- KP_Enter is a carriage
 * return, KP_7 is a seven -- because that is what somebody pressing it meant.
 */
uint32_t recon_key_to_char(recon_keysym sym);

/*
 * The key with this name, or RECON_KEY_NoSymbol.
 *
 * Case-insensitive, because a person typing a key name at the Terminal should
 * not have to know that X11 spelled it `Page_Up`.
 *
 * **It knows the keys in this file and no others**, which is a smaller set
 * than X11's several thousand -- there is no Hebrew_aleph here, and no
 * XF86AudioRaiseVolume. A name outside it comes back as NoSymbol and the
 * caller says it does not know that key, which is true and is better than
 * accepting a name for a key this system could never deliver.
 */
recon_keysym recon_key_from_name(const char *name);

#endif /* RECON_KEY_H_INCLUDED */
