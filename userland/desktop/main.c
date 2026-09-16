/*
 * The ReconOS desktop, as a program on the volume.
 *
 * --- Which program this is ---
 *
 * `docs/ROLES.md` has five roles chosen at first boot. The workstation's first
 * program is `userland/init/recon_init.c`, which is inside the kernel image and
 * draws the first-boot screen. This is the other half of the split the kernel
 * session and this one agreed on: **the built-in stays `screen.c`, and the
 * copy on the volume becomes the desktop.**
 *
 * The reason for that split is worth restating, because the obvious
 * alternative is wrong in a way that is hard to see. Growing the built-in
 * program to be the desktop would put forty-odd sources inside the `.incbin`'d
 * fallback -- making the program that exists for *"the volume is unreadable"*
 * the one with the most code in it.
 *
 * --- What it does, and what it does not ---
 *
 * It asks the kernel how the screen is arranged, maps the framebuffer, wraps
 * it in a panel, and draws one frame with the desktop's own drawing layer.
 * Then it stays up.
 *
 * It is **not** the shell yet. `src/recon_shell.c` and `src/recon_desktop.c`
 * are still behind wlroots and are two of the twenty-seven sources
 * `scripts/check-userland.sh` cannot build. What this proves, the first time
 * it draws on a machine, is that the theme, the font loader, the text
 * rasteriser, the filesystem layer and the C library underneath all of them
 * work on ReconOS -- which is everything between the kernel and a window.
 *
 * --- Why it is written before it can run ---
 *
 * It cannot link today: `stat` does not exist, and `scripts/link-desktop.sh`
 * says so in one line. That is deliberate rather than premature. The allocator
 * was written the same way and the note in `docs/KERNEL-WANTS.md` records what
 * happened: *"Nothing in userland/ was rebuilt for it: mem_recon.c had been
 * sending that exact call since the allocator was written, so the day the
 * kernel accepted it, malloc worked."*
 *
 * The drawing is in `shell_frame.c`, which makes no system call, so the whole
 * picture is rendered and checked on the host today. What is untested here is
 * this file: eight calls, each of which fails with a distinct exit code.
 */

#include <recon.h>
#include <recon_machine.h>

#include <stdio.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_theme.h"
#include "recon_ui.h"

#include "../include/sys/input.h"

#include "keyboard.h"
#include "shell_frame.h"

/*
 * Exit codes, distinct on purpose.
 *
 * A program that fails before it can draw cannot say why on the screen, so it
 * says why in the one number it can still return, and the kernel prints it.
 * The same arrangement `recon_init.c` uses, and the reason either can be
 * debugged at all on a machine with no console.
 *
 * They start at 20 rather than at 1 so that a number on a screen says which
 * program produced it: `recon_init.c` uses 2 to 9.
 */
enum {
    DESKTOP_OK                = 0,
    NO_SCREEN_DESCRIBED       = 20,
    SCREEN_STRUCT_WRONG       = 21,
    SCREEN_MAKES_NO_SENSE     = 22,
    FRAMEBUFFER_CLOSED        = 23,
    MAPPING_REFUSED           = 24,
    NO_PANEL                  = 25,
    NO_FONT                   = 26,
};

/*
 * How much of what somebody types is kept.
 *
 * A line, and no more. This is a first frame and not a text editor: what the
 * field is for is showing that a key reached the screen, and a buffer that
 * scrolled would be the beginning of an editor nobody asked for. When it is
 * full the oldest character goes, which is the choice the kernel's input queue
 * makes and for the same reason -- the most recent thing somebody did is the
 * thing they are looking for.
 */
#define TYPED_MAX 64

/*
 * Put one keystroke into the line.
 *
 * Returns true when the line changed, because redrawing a whole screen for a
 * keystroke that changed nothing is how a machine comes to feel slow. Ctrl
 * held means the keystroke is a command and not text, which nothing here has
 * anything to do with yet -- but putting the letter in the field would be
 * wrong rather than merely unhelpful, because Ctrl-C is the letter C with Ctrl
 * held and a field that shows a "c" for it is lying about what happened.
 */
static bool remember(char *line, size_t *length,
             const struct recon_keystroke *stroke)
{
    u32 c;

    if ((stroke->modifiers & (RECON_MOD_CTRL | RECON_MOD_ALT)) != 0) {
        return false;
    }

    if (stroke->symbol == RECON_KEY_BackSpace) {
        if (*length == 0) {
            return false;
        }
        line[--(*length)] = '\0';
        return true;
    }

    /*
     * Only what can be drawn. `recon_key_to_char` answers 0 for a key with no
     * character -- an arrow, a function key -- and those have somewhere to go
     * later and nowhere to go now.
     *
     * The ASCII bound is this program's, not the layout's: the font can draw
     * far more, and the line below writes one byte per character. A UTF-8
     * encoder here would be the second one in the tree.
     */
    c = recon_key_to_char(stroke->symbol);
    if (c == 0 || c < 32 || c > 126) {
        return false;
    }

    if (*length + 1 >= TYPED_MAX) {
        /* Oldest out, which keeps the end of the line -- the part somebody is
         * looking at -- rather than the beginning. */
        memmove(line, line + 1, *length - 1);
        (*length)--;
    }
    line[(*length)++] = (char)c;
    line[*length] = '\0';
    return true;
}

/* A size in bytes, as something a person reads. */
static void say_bytes(char *into, size_t room, u64 bytes)
{
    if (bytes >= (u64)1024 * 1024 * 1024) {
        u64 tenths = (bytes * 10u) / ((u64)1024 * 1024 * 1024);

        snprintf(into, room, "%llu.%llu GiB",
             (unsigned long long)(tenths / 10u),
             (unsigned long long)(tenths % 10u));
        return;
    }
    snprintf(into, room, "%llu MiB",
         (unsigned long long)(bytes / (1024u * 1024u)));
}

int main(void)
{
    struct recon_screen screen;
    struct recon_machine machine;
    struct recon_shell_facts facts;
    char machine_line[120];
    char display_line[120];
    char volume_line[120];
    char memory[32];
    i64 answer;
    i64 fd;
    i64 mapped;

    /* --- The screen --- */

    answer = recon_screen(&screen, sizeof(screen));
    if (answer < 0) {
        return NO_SCREEN_DESCRIBED;
    }
    if ((u64)answer != sizeof(screen)) {
        /*
         * The kernel's description is a different size from the one this was
         * built against. **Refused rather than used**: the fields are laid
         * out by agreement, and a disagreement about the size is a
         * disagreement about the layout -- which would draw a picture into
         * whatever memory the wrong `pitch` pointed at.
         */
        return SCREEN_STRUCT_WRONG;
    }
    if (screen.width == 0 || screen.height == 0 ||
        screen.pitch < screen.width * 4u) {
        return SCREEN_MAKES_NO_SENSE;
    }

    fd = recon_open("/dev/fb0", 8, OPEN_WRITE | OPEN_READ, 0);
    if (fd < 0) {
        return FRAMEBUFFER_CLOSED;
    }

    mapped = recon_map((int)fd, screen.bytes);
    if (mapped < 0) {
        return MAPPING_REFUSED;
    }

    /* --- The machine, as words --- */

    memset(&machine, 0, sizeof(machine));
    machine.size = (u32)sizeof(machine);
    answer = recon_machine_facts(&machine, sizeof(machine));

    if (answer < 0) {
        /*
         * Not fatal, and this is the one place that decision is worth making
         * loudly. Everything above is needed to draw at all; this is needed
         * only to say what the machine is. A desktop that refuses to appear
         * because it could not count the processors is a black screen, and a
         * black screen is the outcome nobody can diagnose.
         */
        snprintf(machine_line, sizeof(machine_line),
             "the machine would not describe itself");
    } else {
        say_bytes(memory, sizeof(memory), machine.memory_bytes);

        /*
         * Found and online, both, because they are not the same number and
         * the difference is the interesting one: a machine with eight
         * processors running on one is a machine somebody wants to know
         * about. `recon_machine.h` keeps them as separate fields for exactly
         * this reason and printing only one of them throws that away.
         */
        snprintf(machine_line, sizeof(machine_line),
             "%s, %u of %u processors, %s",
             machine.architecture[0] != '\0' ? machine.architecture
                             : "an unnamed architecture",
             machine.processors_online, machine.processors_found, memory);
    }

    snprintf(display_line, sizeof(display_line),
         "%u x %u, %u bytes a row", screen.width, screen.height,
         screen.pitch);

    /* --- The volume --- */

    /*
     * Asked, not assumed. This is the first thing in the program that touches
     * `src/recon_fs.c`, and therefore the first thing that would notice a
     * filesystem layer which believes every path is outside the root -- which
     * is exactly what `realpath` returning NULL did until v0.4.53.
     */
    if (recon_fs_exists(NULL, "/System/Fonts")) {
        snprintf(volume_line, sizeof(volume_line),
             "System volume mounted, fonts present");
    } else {
        snprintf(volume_line, sizeof(volume_line),
             "System volume mounted, no fonts on it");
    }

    /* --- The drawing layer --- */

    recon_theme_init();

    struct recon_panel *panel = recon_panel_on_screen(
        (void *)(unsigned long)mapped, (size_t)screen.pitch,
        (int)screen.width, (int)screen.height,
        0, 0, (int)screen.width, (int)screen.height);

    if (panel == NULL) {
        return NO_PANEL;
    }

    /*
     * Asked for before drawing, and refused if absent.
     *
     * A frame drawn with no font is a frame with rectangles and no words on
     * it, which looks like a drawing fault. An exit code says which. The
     * fonts are on the medium and laid down at `/System/Fonts` -- see
     * `scripts/make-medium.sh` and the eleventh entry of the volume layout.
     */
    if (recon_font_system(14) == NULL) {
        return NO_FONT;
    }

    facts.version = "ReconOS " RECONOS_VERSION;
    facts.machine = machine_line;
    facts.display = display_line;
    facts.volume = volume_line;
    facts.note = "The desktop's own drawing layer, on its own kernel.";

    char typed[TYPED_MAX];
    size_t typed_length = 0;

    typed[0] = '\0';
    facts.typed = typed;

    recon_shell_first_frame(panel, (int)screen.width, (int)screen.height,
        &facts);
    recon_panel_commit(panel);

    /*
     * --- And then it listens ---
     *
     * `/dev/input` is opened *after* the first frame is on the screen, and
     * that order is deliberate: a machine with no keyboard driver still shows
     * a desktop, and shows it before finding out. The other order gives a
     * black screen on a machine whose only fault is that nothing is plugged
     * in, which is the least diagnosable outcome there is.
     *
     * So a failure here is reported on the screen that already exists rather
     * than through an exit code nobody will see.
     */
    i64 keyboard_fd = recon_open("/dev/input", 10, OPEN_READ, 0);

    if (keyboard_fd < 0) {
        facts.typed = NULL;
        facts.note = "No keyboard: /dev/input could not be opened.";
        recon_shell_first_frame(panel, (int)screen.width,
            (int)screen.height, &facts);
        recon_panel_commit(panel);

        for (;;) {
            recon_yield();
        }
    }

    struct recon_keyboard keys;
    memset(&keys, 0, sizeof(keys));

    for (;;) {
        /*
         * Several events at a time. A key pressed and released is two, and a
         * repeat arrives faster than a screen is drawn -- so reading one at a
         * time would redraw between the halves of every keystroke.
         */
        struct recon_input_event batch[16];
        i64 got = recon_read((int)keyboard_fd, batch, sizeof(batch));

        if (got <= 0) {
            /*
             * Nothing, or an error. Yielding rather than spinning, and not
             * giving up: a read that failed once is not a machine with no
             * keyboard, and a desktop that exited on one would take the
             * screen with it.
             */
            recon_yield();
            continue;
        }

        /*
         * A length that is not a whole number of events means this program
         * and the kernel disagree about how big one is -- which is the
         * failure `sys/input.h` is arranged around, and the only place it can
         * be noticed at run time. The events are dropped rather than
         * misparsed: half an event read as a whole one is a keypress that
         * never happened.
         */
        u64 count = (u64)got / sizeof(struct recon_input_event);

        if ((u64)got % sizeof(struct recon_input_event) != 0) {
            facts.note = "The keyboard sent something this program cannot read.";
            count = 0;
        }

        bool changed = false;

        for (u64 i = 0; i < count; i++) {
            struct recon_keystroke stroke;

            if (!recon_keyboard_event(&keys, &batch[i], &stroke)) {
                continue;
            }
            if (remember(typed, &typed_length, &stroke)) {
                changed = true;
            }
        }

        if (!changed) {
            continue;
        }

        recon_shell_first_frame(panel, (int)screen.width,
            (int)screen.height, &facts);
        recon_panel_commit(panel);
    }
}
