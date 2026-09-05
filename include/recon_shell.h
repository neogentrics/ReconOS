/*
 * The ReconOS desktop shell: taskbar and apps menu.
 *
 * Drawn by the compositor itself rather than by a separate bar process. The
 * shell is part of the system, not an application that happens to run on it,
 * so there is no second process to schedule, no protocol between them, and
 * nothing to keep running when the desktop is idle.
 */

#ifndef RECON_SHELL_H
#define RECON_SHELL_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;
struct recon_shell;

/*
 * Build the shell for a screen of the given size. Returns NULL if it could
 * not be created; the compositor still runs, just without a taskbar.
 */
struct recon_shell *recon_shell_create(struct recon_server *server,
    int screen_width, int screen_height);

void recon_shell_destroy(struct recon_shell *shell);

/* Re-lay-out for a new screen size. */
void recon_shell_resize(struct recon_shell *shell, int screen_width, int screen_height);

/* Redraw the taskbar. Call when the window list or focus changes. */
void recon_shell_refresh(struct recon_shell *shell);

/*
 * Redraw everything, because the colours changed.
 *
 * Separate from refresh: a refresh redraws what the shell owns, while this
 * also reaches every window. A skin that only took effect on the windows you
 * happened to touch afterwards would look like a bug rather than a setting.
 */
void recon_shell_restyle(struct recon_shell *shell);

/*
 * Sign out: hide the desktop and put the login screen back up.
 *
 * Windows are left open rather than closed. Signing out is not shutting down,
 * and a half-written note should still be there when the same person signs
 * back in.
 */
void recon_shell_sign_out(struct recon_shell *shell);

/*
 * Cover the desktop with the login screen without ending the session.
 *
 * The account stays signed in and its windows stay where they are; signing
 * back in as the same person returns to them. Signing in as somebody else is
 * a switch, and that does clear the desktop.
 */
void recon_shell_lock(struct recon_shell *shell);

/*
 * Put up setup or the login screen, whichever applies. Called once the rest of
 * the system is running: the gate is the last thing built and the first thing
 * seen.
 */
void recon_shell_begin_session(struct recon_shell *shell);

/*
 * Bring a freshly built shell to whatever state the machine is already in.
 *
 * For a shell restart. Somebody signed in stays signed in and lands on their
 * desktop: restarting the shell is a repair, and being asked for a password
 * because a taskbar needed rebuilding would cost more than the fault did.
 * With nobody signed in it does what a first start does and asks who is
 * there.
 */
void recon_shell_resume_session(struct recon_shell *shell);

/* Whether setup or the login screen is up, which is when nothing else is. */
bool recon_shell_session_active(struct recon_shell *shell);

/*
 * The session, for the one thing that has to reach past the shell to get at
 * it: the screen that says the system has stopped. The session owns a panel
 * covering the whole display, which is what a stop needs, and there is no
 * sense in a second one existing only for the case where everything else has
 * already failed.
 */
struct recon_session *recon_shell_session(struct recon_shell *shell);

/* What the gate is showing, for diagnosis from outside. */
/* Where an account's tile is on the login screen, in screen coordinates, so
 * that choosing one can be driven from outside. False when the grid is not
 * showing or there is no such account. */
bool recon_shell_account_at(struct recon_shell *shell, const char *name,
    int *x, int *y);

void recon_shell_describe_session(struct recon_shell *shell,
    char *out, size_t size);

/*
 * Offer a click at layout coordinates to the shell.
 *
 * Returns true if the shell consumed it, in which case the compositor should
 * not pass it to a window.
 */
bool recon_shell_handle_click(struct recon_shell *shell, double lx, double ly,
    bool pressed);

/* Height reserved at the bottom of the screen, so windows can avoid it. */
int recon_shell_reserved_bottom(struct recon_shell *shell);

/* True if the pointer is over shell chrome rather than the desktop. */
bool recon_shell_contains_point(struct recon_shell *shell, double lx, double ly);

/* Keep the shell above application windows. */
void recon_shell_raise(struct recon_shell *shell);

/*
 * The Ctrl+Alt+Del dialog: a small box asking what the user wants to do.
 *
 * Kept because it is the gesture people already know for "something is wrong",
 * and it should reach the task manager without going through a desktop that
 * may be the thing misbehaving.
 */
void recon_shell_toggle_security(struct recon_shell *shell);
bool recon_shell_security_open(struct recon_shell *shell);

/* Open the task manager directly. */
void recon_shell_open_taskmgr(struct recon_shell *shell);

/*
 * The built-in windows, for anything that needs to enumerate open windows --
 * the task manager's Applications view, for one.
 */
/* The font the shell draws with, so anything it hands work to draws the same. */
struct recon_font *recon_shell_font(struct recon_shell *shell);

int recon_shell_app_count(struct recon_shell *shell);
struct recon_appwin *recon_shell_app_at(struct recon_shell *shell, int index);

/*
 * The icon an application uses, found by its title. Lets a shortcut show the
 * icon of what it points at rather than deriving a name from the title, which
 * only works while the two happen to match.
 */
const char *recon_shell_icon_for_app(struct recon_shell *shell, const char *title);

/*
 * Find a built-in application by name, or -1.
 *
 * Callers used to hardcode the position in the list, which is only ever right
 * by luck: the order is decided by the order things are constructed, and one
 * application failing to start shifts every index after it. Asking by name
 * cannot drift.
 */
int recon_shell_app_index(struct recon_shell *shell, const char *title);

/* Open a built-in application by name. Does nothing if there is no such one. */
void recon_shell_open_named(struct recon_shell *shell, const char *title);

/*
 * Open the help at whatever the window in front is about, or at its first
 * page when nothing is in front or nothing says.
 *
 * What F1 does. An application declares its page in its `recon_appwin_impl`,
 * and one with views of its own moves it as it moves between them.
 */
void recon_shell_open_help(struct recon_shell *shell);

/* Open a built-in window by its registration index. */
void recon_shell_open_app(struct recon_shell *shell, int index);

/*
 * Open a file with whatever handles it. False when nothing does, or when the
 * application refused -- Notepad refuses while it holds unsaved work.
 */
bool recon_shell_open_file(struct recon_shell *shell, const char *path);

/*
 * Move the keyboard to the next window, over everything the taskbar lists.
 *
 * What Alt+Tab does. Minimized windows are skipped rather than restored:
 * Alt+Tab moves between what is in front of you, and a cycle that reopens
 * windows makes it impossible to get past them.
 */
void recon_shell_cycle_windows(struct recon_shell *shell);

/* --- Desktops --- */

/*
 * Show one desktop and hide the rest. Its windows come back, the others go
 * away, and the taskbar lists what is here.
 */
void recon_shell_set_desktop(struct recon_shell *shell, int desktop);
int recon_shell_desktop(struct recon_shell *shell);
int recon_shell_desktop_count(void);

/*
 * Send the window that has the keyboard to another desktop, and follow it.
 *
 * Following is the point: moving a window somewhere and staying behind means
 * watching it disappear and then switching to check it arrived.
 */
void recon_shell_move_to_desktop(struct recon_shell *shell, int desktop);

/* Take focus away from every built-in window, when it goes elsewhere. */
void recon_shell_clear_app_focus(struct recon_shell *shell);

/*
 * --- Blanking the screen when nothing is happening ---
 *
 * Not the display's power state, which needs a kernel: this covers the screen
 * with black. What it saves is the picture, not the watt -- an OLED panel does
 * not burn an image of a taskbar into itself, and nobody walking past reads a
 * document that is not on screen.
 *
 * Which is the honest description, so it is the one the Control Panel gives.
 * Calling this "the display turns off" would be a claim about hardware ReconOS
 * cannot make.
 *
 * Two settings, in the user's hive: how long, and whether waking asks who you
 * are. Both are per-account, because both are about the person rather than the
 * machine.
 */
#define RECON_BLANK_AFTER_KEY "display/blank-after"
#define RECON_BLANK_LOCK_KEY "display/lock-on-wake"

/* Any input at all. Restarts the countdown, and wakes a blanked screen --
 * which is the same signal, so it is one call. Returns true if it woke the
 * screen, in which case the input was spent on waking and should go no
 * further: the first key after a blank should not also be typed into
 * whatever had focus. */
bool recon_shell_note_input(struct recon_shell *shell);

/* Blank it now, without waiting. */
void recon_shell_blank(struct recon_shell *shell);

bool recon_shell_is_blanked(struct recon_shell *shell);

/* Re-read the two settings, after the Control Panel has changed one. */
void recon_shell_blank_reload(struct recon_shell *shell);

/*
 * The most windows ReconOS will draw its own frames around at once.
 *
 * Named rather than left as a number in one array declaration, because
 * several places have to agree on it and the one that did not agree was the
 * one that silently dropped a window.
 *
 * Thirty-two rather than eight: an application that opens a window per thing
 * you are working on -- which the Control Panel now does -- reaches eight
 * with the built-ins alone.
 */
#define RECON_SHELL_WINDOWS_MAX 32

/*
 * Take on a window an application built for itself.
 *
 * Most windows arrive through the application registry, one per installed
 * application, and the shell finds them on its own. This is for the other
 * kind: an application that opens a window per thing you are looking at, so
 * that two of them can be open side by side. The Control Panel does this --
 * a wallpaper and a set of colours are two windows, and both are still the
 * Control Panel.
 *
 * Without this the window exists and is drawn and is reachable by nothing:
 * the taskbar does not list it, clicks are not offered to it, and Alt+Tab
 * steps straight past. Returns false only when there is no room left.
 *
 * Adopting the same window twice is harmless and does nothing.
 */
bool recon_shell_adopt_window(struct recon_shell *shell,
    struct recon_appwin *win);

/*
 * Let go of one, without ending it.
 *
 * For an application closing a window it opened. The shell stops listing it
 * and stops offering it input; whoever adopted it still owns it and must
 * destroy it. Call this *before* destroying, or the shell is left holding a
 * pointer to freed memory and the next taskbar draw reads it.
 */
void recon_shell_forget_window(struct recon_shell *shell,
    struct recon_appwin *win);

/*
 * Bring one to the front and give it the keyboard.
 *
 * Raising a window and focusing one are two different things: raising puts it
 * in front, focusing decides where typing goes and which title bar is drawn
 * as the active one. An application that opens a window and only raises it
 * hands somebody a window on top that their keyboard is not talking to.
 *
 * Does nothing for a window the shell has not adopted.
 */
void recon_shell_focus_window(struct recon_shell *shell,
    struct recon_appwin *win);

/* Offer a key to whichever built-in window has focus. */
bool recon_shell_handle_key(struct recon_shell *shell, uint32_t sym,
    uint32_t modifiers);

/* The cursor the shell wants shown at this point, or NULL for the default. */
const char *recon_shell_cursor_at(struct recon_shell *shell, double lx, double ly);

/* Report pointer motion, so a window drag or resize can follow it. */
void recon_shell_handle_motion(struct recon_shell *shell, double lx, double ly);

/* Offer a scroll to the shell. Returns true if consumed. */
bool recon_shell_handle_scroll(struct recon_shell *shell, double lx, double ly,
    double delta);

/*
 * What a right click landed on, which decides what the menu offers.
 */
/*
 * Report what the shell currently has open -- focus, windows, and the entries
 * of any menu that is up, with where each one is.
 *
 * The point is to be able to look rather than infer. A menu entry that does
 * nothing when clicked is a different problem from one that is not where it
 * appears to be, and from outside the two are indistinguishable without this.
 */
/* --- Asking the user something --- */

/*
 * A question with buttons, drawn over everything and answered before anything
 * else happens.
 *
 * The system needs one of these because the alternative is what ReconOS did
 * before: a Delete button that relabelled itself to "Confirm Delete". That
 * makes the button change width, so the second click can miss it entirely,
 * and it puts a question in a place nobody reads. Destructive things should
 * be asked about plainly.
 *
 * The answer is the index of the button chosen, or -1 if it was dismissed.
 */
typedef void (*recon_answer_fn)(void *user, int choice);

#define RECON_DIALOG_BUTTONS_MAX 3

void recon_shell_ask(struct recon_shell *shell, const char *title,
    const char *message, const char *const *buttons, int button_count,
    recon_answer_fn answer, void *user);

bool recon_shell_dialog_open(struct recon_shell *shell);

/* Drop a pending question without answering it. Used when whatever asked is
 * going away, so its callback is never reached with a dead pointer. */
void recon_shell_cancel_dialog(struct recon_shell *shell, void *user);

void recon_shell_describe(struct recon_shell *shell, char *out, size_t size);

/* The focused built-in window, or NULL. */
struct recon_appwin *recon_shell_focused_app(struct recon_shell *shell);

/*
 * The middle of a named entry in the open context menu, in screen
 * coordinates. False when no menu is open or nothing matches.
 *
 * Lets a test click "Rename" instead of clicking (137, 163) and hoping. The
 * click that follows is a real one through the real hit test -- only the
 * arithmetic of locating the entry is skipped, not the path being tested.
 */
bool recon_shell_context_entry_at(struct recon_shell *shell, const char *label,
    int *x, int *y);

/*
 * The middle of a named entry in the open Start menu, in screen coordinates.
 *
 * The menu is a shell panel rather than an application window, so the
 * per-window instrument cannot see it. Without this the only way to press
 * "Sign Out" from outside is to work out where the footer put it, which tests
 * arithmetic rather than the menu.
 */
bool recon_shell_menu_entry_at(struct recon_shell *shell, const char *label,
    int *x, int *y);

/* The middle of a named button in the open dialog, in screen coordinates. */
bool recon_shell_dialog_button_at(struct recon_shell *shell, const char *label,
    int *x, int *y);

enum recon_context_kind {
    RECON_CONTEXT_DESKTOP,
    RECON_CONTEXT_DESKTOP_ITEM,
    RECON_CONTEXT_TASKBAR_WINDOW,
    /* The bar itself, not a window button on it. */
    RECON_CONTEXT_TASKBAR,
    RECON_CONTEXT_WINDOW,
    /* A menu the application under the pointer asked for. Its entry ids mean
     * whatever that application decided they mean. */
    RECON_CONTEXT_APP,
    /* An application in the Start menu, pinned or not. */
    RECON_CONTEXT_MENU_APP,
};

/* Offer a right click to the shell. Returns true if it opened a menu. */
bool recon_shell_handle_right_click(struct recon_shell *shell, double lx, double ly);

void recon_shell_close_context(struct recon_shell *shell);

/* Close the apps menu if it is open. */
void recon_shell_close_menu(struct recon_shell *shell);

/* Open it if it is not. For driving the desktop from the control socket:
 * "choose this from the Start menu" should not require knowing where the
 * Start button happens to be on this screen. */
void recon_shell_open_menu(struct recon_shell *shell);

#endif
