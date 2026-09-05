/*
 * The Control Panel. See include/recon_control_panel.h.
 *
 * Four pages down the left, whichever is chosen on the right. Everything it
 * changes is a setting something else already reads -- the skin the shell
 * draws with, the spacing the text layer uses, the accounts the login screen
 * offers -- so nothing here has state of its own to fall out of step.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_access.h"
#include "recon_appwin.h"
#include "recon_avatar.h"
#include "recon_control_panel.h"
#include "recon_clock.h"
#include "recon_display.h"
#include "recon_firewall.h"
#include "recon_fonts.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_help.h"
#include "recon_explorer.h"
#include "recon_modules.h"
#include "recon_package.h"
#include "recon_net.h"
#include "recon_procinfo.h"
#include "recon_registry.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_tls.h"
#include "recon_ui.h"
#include "recon_users.h"
#include "recon_wallpaper.h"

/*
 * --- The front page ---
 *
 * Icons with their names under them, the way the desktop does it, because
 * that is what somebody arriving at a Control Panel is looking for: not a
 * list of words down the side of something already open, but a set of things
 * to go into. Clicking one opens it in a window of its own, so the wallpaper
 * and the colours can be worked on at the same time.
 */
/* The Themes / Colours / Wallpapers bar. */
#define SECTION_HEIGHT 26

#define TILE_W 150
#define TILE_H 96
#define TILE_ICON 32
#define TILE_GAP 6
#define ROW_HEIGHT 26
#define HEADER_HEIGHT 34
#define PADDING 12
#define BUTTON_HEIGHT 24
#define FIELD_HEIGHT 26
#define STATUS_HEIGHT 24

#define COLOR_BG THEME(WINDOW_FRAME)
#define COLOR_PANEL THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_BUTTON_ACTIVE THEME(BUTTON_ACTIVE)
#define COLOR_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_WARNING THEME(WARNING)
#define COLOR_ROW_ALT THEME(SURFACE_ALT)

/* How many folders the Storage page will account for: the fixed few, plus
 * one for each account, plus the bin. */
#define STORAGE_ROWS_MAX 24

/* Hit ids. */
#define HIT_PAGE_BASE (RECON_APPWIN_HIT_USER + 10)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)
#define HIT_ACTION_BASE (RECON_APPWIN_HIT_USER + 200)
#define HIT_FIELD_BASE (RECON_APPWIN_HIT_USER + 300)
#define HIT_PENDING_BASE (RECON_APPWIN_HIT_USER + 400)
#define HIT_AVATAR_BASE (RECON_APPWIN_HIT_USER + 500)
#define HIT_WALLPAPER_BASE (RECON_APPWIN_HIT_USER + 600)
/* The icons on the front page. Separate from HIT_PAGE_BASE, which belonged to
 * a sidebar a page window does not have. */
#define HIT_TILE_BASE (RECON_APPWIN_HIT_USER + 700)
/* The Themes / Colours / Wallpapers bar across the top of Appearance. */
#define HIT_SECTION_BASE (RECON_APPWIN_HIT_USER + 800)
/* The firewall's preset rules, on the Add Rule page. */
#define HIT_PRESET_BASE (RECON_APPWIN_HIT_USER + 900)
/* The System / Programs / User selector on the Storage page. */
#define HIT_VOLUME_BASE (RECON_APPWIN_HIT_USER + 950)
/* Installed / System Apps, at the top of the Programs page. */
#define HIT_PROGRAMS_TAB_BASE (RECON_APPWIN_HIT_USER + 1100)
#define HIT_NET_TAB_BASE (RECON_APPWIN_HIT_USER + 1140)
#define HIT_NET_ROW_BASE (RECON_APPWIN_HIT_USER + 1160)
#define HIT_FONT_BASE (RECON_APPWIN_HIT_USER + 1200)
#define HIT_MODE_BASE (RECON_APPWIN_HIT_USER + 1240)
#define HIT_ZONE_BASE (RECON_APPWIN_HIT_USER + 1280)
/* The tick boxes on the Disk Cleanup page. */
#define HIT_CLEAN_BASE (RECON_APPWIN_HIT_USER + 1000)

/* What the buttons on each page do. */
enum action {
    ACTION_NONE,
    /* Accounts */
    ACTION_ADD_USER,
    ACTION_REMOVE_USER,
    ACTION_TOGGLE_ROLE,
    ACTION_SET_PASSWORD,
    ACTION_NEXT_AVATAR,
    ACTION_CHOOSE_AVATAR,
    /* Programs and modules */
    ACTION_INSTALL_PROGRAM,
    ACTION_REPAIR_PROGRAM,
    ACTION_DISABLE_PROGRAM,
    ACTION_REMOVE_PROGRAM,
    ACTION_CONFIRM_INSTALL,
    ACTION_LOAD_MODULE,
    ACTION_UNLOAD_MODULE,
    /* Registry */
    ACTION_UNLOCK_REGISTRY,
    ACTION_LOCK_REGISTRY,
    ACTION_REGISTRY_HIVE,
    ACTION_REGISTRY_EDIT,
    ACTION_REGISTRY_ADD,
    ACTION_REGISTRY_SAVE,
    ACTION_REGISTRY_CANCEL,
    ACTION_REGISTRY_REMOVE,
    /* Network */
    ACTION_TEST_NETWORK,
    ACTION_OPEN_FIREWALL,
    ACTION_TOGGLE_NET_APP,
    ACTION_REFRESH_NETWORK,
    /* Reading */
    ACTION_SPACING_LESS,
    ACTION_SPACING_MORE,
    ACTION_LINES_LESS,
    ACTION_LINES_MORE,
    ACTION_SIZE_LESS,
    ACTION_SIZE_MORE,
    ACTION_RESET_READING,
    ACTION_SET_RESOLUTION,
    ACTION_CHECK_TIME,
    ACTION_TOGGLE_24H,
    ACTION_ADD_FONT,
    ACTION_REMOVE_FONT,
    ACTION_USE_FONT,
    ACTION_DEFAULT_FONT,
    /* Storage */
    ACTION_MEASURE_STORAGE,
    ACTION_CLEAN_NOW,
    ACTION_CLEAN_VIEW,
    ACTION_CLEAN_SYSTEM_FILES,
    ACTION_EMPTY_BIN,
    /* Firewall */
    ACTION_FIREWALL_TOGGLE,
    ACTION_FIREWALL_DEFAULT_IN,
    ACTION_FIREWALL_DEFAULT_OUT,
    ACTION_FIREWALL_RULE_TOGGLE,
    ACTION_FIREWALL_RULE_ACTION,
    ACTION_BLANK_LONGER,
    ACTION_BLANK_SHORTER,
    ACTION_BLANK_LOCK,
    ACTION_FIREWALL_ADD,
    ACTION_FIREWALL_ADD_CUSTOM,
    ACTION_FIREWALL_ADD_CANCEL,
    ACTION_FIREWALL_CUSTOM_DIRECTION,
    ACTION_FIREWALL_CUSTOM_PROTOCOL,
    ACTION_FIREWALL_CUSTOM_ACTION,
    ACTION_FIREWALL_CUSTOM_CONFIRM,
    ACTION_FIREWALL_REMOVE,
    ACTION_FIREWALL_RULE_UP,
    ACTION_FIREWALL_RULE_DOWN,
    /* Skins */
    ACTION_USE_SKIN,
    ACTION_ADD_WALLPAPER,
    ACTION_REMOVE_WALLPAPER,
    ACTION_COPY_SKIN,
    ACTION_NEW_SKIN,
    ACTION_BEGIN_NAMING_SKIN,
    ACTION_CONFIRM_COPY_SKIN,
    ACTION_EDIT_SKIN,
    ACTION_SKIN_DONE,
    ACTION_SKIN_SET,
    ACTION_SKIN_CANCEL,
    ACTION_SKIN_FLAT,
    /* Update */
    ACTION_SHOW_CHANGES,
};

enum page {
    PAGE_ACCOUNTS,
    PAGE_APPEARANCE,
    PAGE_CLOCK,
    PAGE_DISPLAY,

    PAGE_PROGRAMS,
    PAGE_MODULES,

    PAGE_NETWORK,
    /*
     * Its own page rather than a section of Network, because it is the page
     * somebody comes to the Control Panel *for* -- and because it is meant to
     * be replaced. A firewall with per-program rules and its own history
     * belongs in an application; when that application arrives it takes this
     * page's place, and a page is a cleaner thing to replace than half of
     * another one.
     */
    PAGE_FIREWALL,
    PAGE_POWER,
    PAGE_STORAGE,
    PAGE_CLEANUP,
    PAGE_UPDATE,

    PAGE_TROUBLESHOOT,
    PAGE_RECOVERY,
    PAGE_REGISTRY,

    PAGE_ABOUT,
    PAGE_COUNT,
};

static const struct {
    const char *label;
    const char *icon;
    /*
     * Three or four words under the name on the front page. Not the page's
     * own heading repeated -- that says what the page is called, and this has
     * to say what it is for, to somebody who does not already know.
     */
    const char *summary;
} PAGES[PAGE_COUNT] = {
    { "Accounts", RECON_ICON_APP, "Who may sign in" },
    { "Appearance", RECON_ICON_APPEARANCE, "Skins and wallpaper" },
    { "Date and Time", RECON_ICON_CLOCK, "The clock and its zone" },
    { "Display Settings", RECON_ICON_NOTEPAD, "Text, size, resolution" },

    { "Programs", RECON_ICON_PROGRAMS, "What is installed" },
    { "Modules", RECON_ICON_MODULES, "Code the system loads" },

    { "Network", RECON_ICON_NETWORK, "What it talks to" },
    { "Firewall", RECON_ICON_FIREWALL, "What may be opened" },
    { "Power", RECON_ICON_SHUTDOWN, "Left alone, and asleep" },
    { "Storage", RECON_ICON_EXPLORER, "Where the room went" },
    { "Disk Cleanup", RECON_ICON_TRASH, "Free some of it up" },
    { "Update", RECON_ICON_UPDATE, "What version this is" },

    { "Troubleshoot", RECON_ICON_TERMINAL, "When it goes wrong" },
    { "Recovery", RECON_ICON_RECOVERY, "Back to what worked" },
    { "Registry", RECON_ICON_NOTEPAD, "Every setting, raw" },

    { "System Information", RECON_ICON_TASKMGR, "What this machine is" },
};

/*
 * --- Settings that are not built yet ---
 *
 * These pages are here on purpose, empty of function and honest about it.
 *
 * The argument for leaving them out was that a button which does nothing is
 * worse than no button. The argument against, which won, is that a gap is
 * worse still: nobody can see a gap, so nobody remembers it was meant to be
 * filled, and the shape of the system stops being visible to the person
 * building it. A control that says plainly what it will do and why it cannot
 * yet is a note to ourselves that is also useful to a reader.
 *
 * Every one of these says what is missing rather than "coming soon". The
 * missing thing is usually the kernel: a hosted process cannot suspend a
 * machine, partition a disk, or reinstall itself.
 */
struct pending_item {
    const char *label;
    const char *detail;   /* What it is for. */
    const char *blocked;  /* What has to exist first. */
};

static const struct pending_item ABOUT_ITEMS[] = {
    { "Device Manager", "The hardware, and the drivers for it.",
      "Drivers are .rts modules and none exist. Enumerating hardware is a "
      "kernel's job; ReconOS would be reading the host's list." },
    { "Product key and activation", "Whether this copy is licensed.",
      "There is nothing to license against. It arrives with the server the "
      "additional-features system needs." },
    { "Disk encryption", "Keep what is on the disk unreadable without a key.",
      "Needs the volume layer to sit on something it can encrypt, which "
      "means partitions, which means the kernel." },
};

static const struct pending_item POWER_ITEMS[] = {
    { "Sleep", "Stop everything and hold it in memory.",
      "Suspending needs a kernel underneath. ReconOS is a process on one." },
    { "Hibernate", "Write memory to disk and switch off.",
      "Needs somewhere on disk to write an image to, and a boot path that "
      "reads it back." },
    { "Power mode", "Trade speed against battery.",
      "Nothing to trade yet: no governor, and no battery to read." },
};

/*
 * What Storage still cannot do. "Clean up" left this list in v0.2.15: the
 * page now measures what is here and can empty the bin, which is what that
 * item was asking for. The three that remain are all the same missing thing
 * said three ways -- there is one filesystem, hosted in a folder, and no
 * volume layer under it.
 */
static const struct pending_item STORAGE_ITEMS[] = {
    { "Disk Cleanup", "Choose what to free, by kind.",
      "The parts are measured; what is missing is the list of what is safe "
      "to remove, and the kept old versions a revert would go back to." },
    { "Move a space to its own disk", "Put System, Programs or User elsewhere.",
      "The three spaces are separate here and share one filesystem "
      "underneath. Separating them for real needs partitions." },
    { "Format and partition", "Prepare a disk for use.",
      "Writing partition tables is a kernel's job, and would be dangerous "
      "from here." },
};

/*
 * What Update still cannot do. "What changed" left this list in v0.2.15 --
 * the change log is in the system now, and this page is where it is read
 * from. The other two are the same missing thing said twice: there is
 * nowhere to fetch an update from and no way to trust one if there were.
 */
static const struct pending_item UPDATE_ITEMS[] = {
    { "Check for updates", "Ask whether a newer ReconOS exists.",
      "Nothing to ask. There is no update server and no signed package "
      "format to trust an answer from." },
    { "Install automatically", "Apply updates without being asked.",
      "Would need updates first, and a way to restart into them." },
};

static const struct pending_item TROUBLESHOOT_ITEMS[] = {
    { "A program stopped answering", "Find it and close it safely.",
      "Partly real already: the Task Manager finds these and can end them. "
      "This page would be the guided version." },
    { "Something looks wrong on screen", "Redraw everything from scratch.",
      "Needs a way to force a full repaint and to reload the theme and "
      "icons together." },
    { "Report a problem", "Collect what happened into one file.",
      "Needs a log the system keeps rather than one the terminal prints." },
};

static const struct pending_item RECOVERY_ITEMS[] = {
    { "Restore points", "Save the system's state, and go back to it.",
      "Needs a copy of /System and the registry taken on a schedule, and "
      "somewhere to keep it." },
    { "Back up", "Copy what matters somewhere else.",
      "Needs a second volume to copy to. There is one filesystem." },
    { "Advanced startup", "Start into a screen for repairing the system.",
      "Needs a boot path of our own. ReconOS is started by whatever is "
      "underneath it." },
    { "Reset this system", "Put it back the way it was installed.",
      "Needs a copy of the original to put back, kept somewhere a reset "
      "cannot reach." },
    { "Reinstall", "Lay the system down again, keeping accounts and files.",
      "Needs an installer that runs from inside a running system." },
};

/* One table, so a page and its items cannot be wired up separately and drift.
 * A page with no items is one that does something real. */
static const struct {
    const char *lede;
    const struct pending_item *items;
    int count;
} PENDING[PAGE_COUNT] = {
    [PAGE_ABOUT] = { "Related, and not built yet.", ABOUT_ITEMS,
        (int)(sizeof(ABOUT_ITEMS) / sizeof(ABOUT_ITEMS[0])) },
    /* Nothing not-built left on this page: resolution was the last of the
     * three and it is built. The summary stays; the list is empty. */
    [PAGE_DISPLAY] = { "How text is sized and spaced, and how big the screen "
        "is.", NULL, 0 },
    [PAGE_POWER] = { "What the machine does when it is left alone.",
        POWER_ITEMS, (int)(sizeof(POWER_ITEMS) / sizeof(POWER_ITEMS[0])) },
    [PAGE_TROUBLESHOOT] = { "When something is not working.",
        TROUBLESHOOT_ITEMS,
        (int)(sizeof(TROUBLESHOOT_ITEMS) / sizeof(TROUBLESHOOT_ITEMS[0])) },
    [PAGE_RECOVERY] = { "Getting back to a system that worked.",
        RECOVERY_ITEMS,
        (int)(sizeof(RECOVERY_ITEMS) / sizeof(RECOVERY_ITEMS[0])) },
};

/* What a pending question is about, since the answer arrives later. */
/*
 * --- Programs, in two lists ---
 *
 * What somebody installed and what ships with ReconOS are two different
 * things, and the buttons that make sense for them are different too. An
 * installed program can be removed; a system application cannot, and asking
 * "why is Remove greyed out" of a list where half the rows behave one way and
 * half the other is a question the page should never have made anybody ask.
 */
enum programs_tab {
    PROGRAMS_INSTALLED,
    PROGRAMS_SYSTEM,
    PROGRAMS_TABS,
};

static const char *const PROGRAMS_TAB_NAMES[PROGRAMS_TABS] = {
    "Installed", "System Apps",
};

/*
 * --- Appearance, in three parts ---
 *
 * It used to be one page with the skins at the top, the buttons under them
 * and the wallpapers under those. With ten skins installed there was room for
 * two wallpapers out of five, and the other three were drawn off the bottom
 * edge where nothing could see them.
 *
 * Splitting it fixes that by construction rather than by arithmetic: each
 * section has the whole window, so neither list can be squeezed by the other
 * growing.
 */
enum appearance_section {
    APPEARANCE_THEMES,
    APPEARANCE_COLOURS,
    APPEARANCE_WALLPAPERS,
    APPEARANCE_SECTIONS,
};

static const char *const APPEARANCE_SECTION_NAMES[APPEARANCE_SECTIONS] = {
    "Themes", "Colours", "Wallpapers",
};

/*
 * Network, split the same way and for the same reason.
 *
 * It was one page holding the machine's name, every interface, the gateway,
 * every resolver, the last test and two buttons -- which fitted only because
 * this machine has two interfaces. Four sections each get the whole window,
 * so none of them is squeezed by another growing.
 */
enum network_section {
    NETWORK_STATUS,
    NETWORK_ADAPTERS,
    NETWORK_USAGE,
    NETWORK_APPS,
    NETWORK_SECTIONS,
};

static const char *const NETWORK_SECTION_NAMES[NETWORK_SECTIONS] = {
    "Status", "Adapters", "Data Used", "Applications",
};

/*
 * --- Firewall presets ---
 *
 * The rules people actually want, written out so nobody has to know that
 * "secure shell" means tcp 22.
 *
 * Nothing here duplicates a default rule: the shipped set already covers web,
 * name lookups, time, and the four incoming services worth naming. These are
 * the next ones people reach for, and the last two are the blunt instruments
 * -- there is no way to say "nothing in" from a page of individual rules
 * unless somebody writes the rule that says it.
 */
struct fw_preset {
    const char *name;
    const char *detail;
    enum recon_fw_direction direction;
    enum recon_fw_protocol protocol;
    int port_from;
    int port_to;
    enum recon_fw_action action;
};

static const struct fw_preset FW_PRESETS[] = {
    { "Web server", "Somebody else's browser reaching a site on this machine",
      RECON_FW_IN, RECON_FW_TCP, 80, 80, RECON_FW_ALLOW },
    { "Web server (secure)", "The same, over TLS",
      RECON_FW_IN, RECON_FW_TCP, 443, 443, RECON_FW_ALLOW },
    { "Mail, sending", "Handing a message to a mail server",
      RECON_FW_OUT, RECON_FW_TCP, 587, 587, RECON_FW_ALLOW },
    { "Mail, collecting", "Reading a mailbox on another machine",
      RECON_FW_OUT, RECON_FW_TCP, 993, 993, RECON_FW_ALLOW },
    { "File transfer", "Old-style FTP, out",
      RECON_FW_OUT, RECON_FW_TCP, 20, 21, RECON_FW_ALLOW },
    { "Database", "PostgreSQL, in",
      RECON_FW_IN, RECON_FW_TCP, 5432, 5432, RECON_FW_ALLOW },
    { "Game server", "A run of the ports games usually take",
      RECON_FW_IN, RECON_FW_UDP, 27015, 27030, RECON_FW_ALLOW },
    { "Local network only", "Anything arriving from anywhere: refused",
      RECON_FW_IN, RECON_FW_ANY_PROTOCOL, 0, 0, RECON_FW_BLOCK },
    { "Nothing out", "Every outgoing connection: refused",
      RECON_FW_OUT, RECON_FW_ANY_PROTOCOL, 0, 0, RECON_FW_BLOCK },
};

#define FW_PRESET_COUNT ((int)(sizeof(FW_PRESETS) / sizeof(FW_PRESETS[0])))

/*
 * --- What Disk Cleanup knows how to free ---
 *
 * Each category is a place and a rule about what in it is safe to remove.
 * Nothing here is a guess: a category exists because there is a definite
 * answer to "what is this, and what happens if it goes", and a cleanup tool
 * that removes things it cannot describe is a tool nobody should run.
 *
 * `pattern` empty means everything in the folder. Otherwise it is a suffix,
 * so screen captures can be swept without taking the folder's other contents
 * with them.
 */
struct clean_category {
    const char *label;
    const char *detail;
    /* What is lost. Shown beside the row, because a checkbox with a size next
     * to it does not say what ticking it costs. */
    const char *cost;
    const char *path;
    const char *pattern;
    /*
     * A suffix this category does *not* take, so two categories over the same
     * folder do not both count the same file.
     *
     * Screen captures and temporary files are both /Temp, and without this
     * one .png was counted in each: ticking both promised twice the room
     * there was to free. A cleanup tool whose total is wrong is a cleanup
     * tool nobody believes the second time.
     */
    const char *except;
    /* The recycle bin is not a folder to sweep -- it has an emptying of its
     * own that also clears the origin notes beside the files. */
    bool is_bin;
};

static const struct clean_category CLEAN_SYSTEM[] = {
    { "Recycle bin", "Files deleted from the system.",
      "They stop being recoverable.", NULL, NULL, NULL, true },
    { "Screen captures", "Pictures of the screen, taken with Print Screen.",
      "The pictures go. Nothing else uses them.",
      RECON_DIR_TEMP, ".png", NULL, false },
    { "Other temporary files", "Scratch space ReconOS writes and never clears.",
      "Nothing running keeps anything here that it needs.",
      RECON_DIR_TEMP, NULL, ".png", false },
    { "Logs", "What has happened on this machine, including error codes.",
      "The record of past faults goes with them.",
      RECON_DIR_LOGS, NULL, NULL, false },
};

static const struct clean_category CLEAN_PROGRAMS[] = {
    { "Recycle bin", "Programs and packages deleted from here.",
      "They stop being recoverable.", NULL, NULL, NULL, true },
    { "Installer packages", "The .rpk files programs arrived in.",
      "Reinstalling one needs the package again.",
      RECON_DIR_APPS, ".rpk", NULL, false },
};

static const struct clean_category CLEAN_USER[] = {
    { "Recycle bin", "What you have deleted.",
      "They stop being recoverable.", NULL, NULL, NULL, true },
};

static const struct {
    const struct clean_category *items;
    int count;
} CLEAN[RECON_VOLUME_COUNT] = {
    [RECON_VOLUME_SYSTEM] = { CLEAN_SYSTEM,
        (int)(sizeof(CLEAN_SYSTEM) / sizeof(CLEAN_SYSTEM[0])) },
    [RECON_VOLUME_PROGRAMS] = { CLEAN_PROGRAMS,
        (int)(sizeof(CLEAN_PROGRAMS) / sizeof(CLEAN_PROGRAMS[0])) },
    [RECON_VOLUME_USER] = { CLEAN_USER,
        (int)(sizeof(CLEAN_USER) / sizeof(CLEAN_USER[0])) },
};

#define CLEAN_MAX 8

enum question {
    QUESTION_NONE,
    QUESTION_REMOVE_USER,
    QUESTION_REMOVE_PROGRAM,
    QUESTION_REMOVE_KEY,
    QUESTION_EMPTY_BIN,
    QUESTION_CUSTOMIZE_SKIN,
    QUESTION_NEW_SKIN,
    QUESTION_CLEAN_UP,
};

struct control_panel {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_appwin *win;

    /*
     * True for the front page, which shows the icons and no page at all.
     *
     * A field rather than another value in `enum page`, because a dozen
     * tables here are indexed by page and a value that must never index them
     * would be a trap laid for whoever writes the next one.
     */
    bool home;

    enum page page;
    int selected;

    /*
     * Adding an account, or changing a password. Both need a name and a
     * secret, so they share the fields and differ in what is done with them.
     */
    bool editing;
    bool editing_password_only;
    struct recon_edit name;
    struct recon_edit password;
    bool password_focused;

    enum question question;
    /*
     * Long enough for the longest thing it carries, which is a registry key.
     * It used to be an account name's length and silently truncated module
     * names, which the compiler had been pointing out for a while.
     */
    char question_target[RECON_REGISTRY_KEY_MAX];

    /*
     * The registry page is locked until an administrator says who they are.
     *
     * Not because the values are secret -- they are a text file anyone with
     * the disk can read -- but because this is the one page where a careless
     * change breaks the system quietly, and asking for the password makes it
     * a deliberate act rather than a click made while looking for something
     * else. The unlock does not outlive the window.
     */
    /* Choosing a picture: the set is laid out over the account list until one
     * is picked. A grid you look at, rather than a button you click until the
     * right one comes round. */
    bool picking_avatar;

    /* Typing the path of something to install. */
    bool installing;

    /*
     * --- Writing a skin ---
     *
     * A skin could be installed and removed since v0.2.4 and still could not
     * be written here: authoring one meant editing a text file somewhere else
     * and bringing it in. Two states, because they are two acts -- naming a
     * copy, and then changing what the copy says.
     */
    /* Which of Themes, Colours, Wallpapers is showing. */
    enum appearance_section section;

    /* Which of the two Programs lists is showing, and where it is scrolled. */
    enum programs_tab programs;
    int programs_scroll;

    /* Which of System, Programs, User the Storage and Cleanup pages show. */
    enum recon_volume volume;

    /* Where the firewall's rule list is scrolled to, and which rule the list
     * was last dragged to keep in view. */
    int fw_scroll;
    int fw_followed;

    /*
     * Which tile the pointer is over on the front page, so it can be lit.
     *
     * What it is for is said by the tooltip, which the shell draws -- so this
     * no longer needs to know where the pointer is, only which tile it is in.
     */
    int hover_tile;

    /*
     * What Disk Cleanup last measured, and what is ticked.
     *
     * Measured on the way in and after a clean, like Storage: walking the
     * tree sixty times a second would make the page feel broken, and a size
     * that is a minute old is a size somebody acts on and finds wrong.
     */
    struct {
        unsigned long long bytes;
        int files;
        bool ticked;
    } clean[CLEAN_MAX];
    bool clean_measured;

    /*
     * --- Adding a firewall rule ---
     *
     * Two steps, because they are two decisions. First which kind of rule,
     * from a list of the ones people want; then, only for somebody who wants
     * something not on that list, the five fields a rule actually has.
     *
     * The list first rather than the fields first: most people adding a rule
     * want one of nine things, and asking them for a protocol and a port
     * range before finding that out is asking them to know the answer before
     * they have been offered it.
     */
    bool fw_adding;
    bool fw_custom;
    struct recon_edit fw_name;
    struct recon_edit fw_port;
    bool fw_port_focused;
    enum recon_fw_direction fw_direction;
    enum recon_fw_protocol fw_protocol;
    enum recon_fw_action fw_action;

    /*
     * Where each list in Appearance has been scrolled to.
     *
     * One per list rather than one shared: they are three different lists of
     * three different lengths, and carrying a position from a list of
     * forty-eight roles into a list of five wallpapers would land past the
     * end of it.
     */
    int theme_scroll;
    int paper_scroll;

    /* How far the zone list is scrolled. */
    int zone_scroll;

    /* Which screen size is picked, before it is applied. */
    int mode_selected;

    /* Which installed font is picked, and how far the list is scrolled. */
    int font_selected;
    int font_scroll;

    /* Which part of Network is showing, and which row in it is picked. */
    enum network_section net_section;
    int net_selected;
    int net_scroll;

    bool naming_skin;

    /*
     * Which built-in a new skin starts from, or empty when the naming form
     * was reached by customizing whatever was selected. A skin has forty-eight
     * roles and every one of them has to be a colour; there is no blank to
     * start from, only a choice of which finished thing to start from.
     */
    char new_skin_from[64];
    struct recon_edit skin_new_name;
    /*
     * And a line saying what it is for. The built-in skins each have one and
     * it is the only thing distinguishing them in a list of names; a skin of
     * your own with a blank one is the odd entry out.
     */
    struct recon_edit skin_new_desc;
    bool skin_desc_focused;

    bool skin_editing;
    char skin_name[48];
    int skin_row;            /* Which role is selected. */
    int skin_scroll;
    /* True while typing a colour into the field, which is the only time the
     * field exists -- a row is chosen first, then changed. */
    bool skin_value_editing;
    struct recon_edit skin_value;

    bool registry_unlocked;
    struct recon_edit unlock;
    int registry_hive;    /* 0 system, 1 user */
    int registry_scroll;

    /*
     * Changing a setting, or adding one.
     *
     * The key being changed is copied out rather than read from the list on
     * the way past: adding or removing anything renumbers the hive, so an
     * index captured when the field opened would not still mean the same key
     * when Save is pressed.
     */
    bool registry_editing;
    bool registry_adding;
    char registry_key[RECON_REGISTRY_KEY_MAX];
    struct recon_edit reg_key;
    struct recon_edit reg_value;
    bool reg_key_focused;

    char status[192];
    bool status_is_warning;

    /* Where the list was drawn, so a right click can tell a row from the
     * space under the last one. */
    int list_x, list_y, list_w, list_h;

    /*
     * What the Storage page last measured.
     *
     * Held rather than recomputed on every draw: measuring walks the whole
     * tree, and a page that walks the disk sixty times a second is a page
     * that makes the desktop feel broken. It is measured when the page is
     * opened and when somebody asks again.
     */
    struct {
        char label[48];
        char detail[96];
        unsigned long long bytes;
        int files;
        /* False for the Recycle Bin, whose bytes are already counted inside
         * the account folder above it. Two rows, one lot of bytes. */
        bool in_total;
    } storage[STORAGE_ROWS_MAX];
    int storage_count;
    unsigned long long storage_total;
    bool storage_measured;
};

/*
 * --- One window per item ---
 *
 * The front page opens each item in a window of its own. They are still the
 * Control Panel -- one application, one entry in the menus -- and they are
 * separate windows so that a wallpaper and a set of colours can be looked at
 * side by side, which is the whole reason for asking.
 *
 * One window per page and no more. Two windows both editing the registry
 * would be two views of one file, each unaware the other had written to it,
 * and the second one to save would quietly undo the first. So a second click
 * on a tile brings the existing window forward.
 *
 * The windows are not destroyed when closed, only hidden -- which is what
 * closing means for every other window in ReconOS. They are built the first
 * time they are asked for and last as long as the session.
 */
static struct control_panel *g_pages[PAGE_COUNT];

/* How many have been built, so each new one is stepped clear of the last. */
static int g_page_windows;

/*
 * The skin editor.
 *
 * Its own window rather than a state the Appearance window goes into, because
 * changing a colour is something you do while looking at the result: the
 * editor covering the very thing it is changing was the worst possible place
 * to put it. Now the desktop, the Appearance window and the editor are all on
 * screen at once, and a colour changes under all three.
 *
 * One editor, not one per skin. Two would be two writers to one file.
 */
static struct control_panel *g_skin_editor;

static void open_skin_editor(struct control_panel *cp, const char *skin);

/* Defined after the window description it builds windows from. */
/*
 * Nothing picked yet, in every list this window has.
 *
 * calloc leaves each of these at zero, which is a real row: a window opened
 * with its first row already highlighted and the buttons that act on a
 * selection already armed, before anybody had pointed at anything. One click
 * highlights is the rule; this is what "no clicks yet" means.
 *
 * Called from both constructors, because there are two -- the front page and
 * a window per item -- and a rule written in only one of them is a rule half
 * the windows do not follow.
 */
static void nothing_chosen(struct control_panel *cp) {
    cp->selected = -1;
    cp->net_selected = -1;
    cp->font_selected = -1;
    cp->mode_selected = -1;

    /* And nothing under the pointer, which is the same kind of zero: the
     * front page opened with its first icon lit as though the pointer were
     * resting on it, before the pointer had been anywhere near the window. */
    cp->hover_tile = -1;
}

static void open_page_window(struct control_panel *cp, enum page page);

static bool page_window_open(enum page page) {
    if (page < 0 || page >= PAGE_COUNT) {
        return false;
    }
    return g_pages[page] != NULL && g_pages[page]->win != NULL &&
        recon_appwin_is_open(g_pages[page]->win);
}

static void set_status(struct control_panel *cp, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct control_panel *cp, bool warning, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(cp->status, sizeof(cp->status), fmt, args);
    va_end(args);
    cp->status_is_warning = warning;
}

/*
 * Nothing to say.
 *
 * Its own function rather than set_status(cp, false, "") because set_status
 * carries a printf format attribute, and an empty format is a warning at
 * every call site -- six of them, which is enough noise to hide a real one.
 */
static void clear_status(struct control_panel *cp) {
    cp->status[0] = '\0';
    cp->status_is_warning = false;
}

/* --- Small drawing helpers --- */

/*
 * A bar down the right of a list, showing how much of it is on screen.
 *
 * Drawn only when there is more than fits. A bar that is always there and
 * always full is furniture; one that appears when there is somewhere to go is
 * the only thing on screen that says the list continues.
 *
 * Returns how much width it took, so the rows can keep clear of it.
 */
#define SCROLLBAR_WIDTH 7

static int draw_scrollbar(struct recon_panel *p, int x, int y, int h,
        int scroll, int visible, int total) {
    if (total <= visible || visible <= 0 || h <= 0) {
        return 0;
    }

    recon_fill_rect(p, x, y, SCROLLBAR_WIDTH, h, COLOR_BG);

    int thumb = h * visible / total;
    if (thumb < 12) {
        thumb = 12;
    }
    if (thumb > h) {
        thumb = h;
    }

    int span = total - visible;
    int travel = h - thumb;
    int at = span > 0 ? travel * scroll / span : 0;
    if (at < 0) {
        at = 0;
    }
    if (at > travel) {
        at = travel;
    }

    recon_fill_rect(p, x + 1, y + at, SCROLLBAR_WIDTH - 2, thumb,
        COLOR_SELECTED);
    return SCROLLBAR_WIDTH;
}

/*
 * Keep a scroll position inside the list it belongs to.
 *
 * Clamped where the list is drawn rather than where the wheel is turned,
 * because this is the only place that knows both how many rows there are and
 * how many fit. The wheel handler only ever adds and subtracts.
 */
static int clamp_scroll(int *scroll, int visible, int total) {
    int most = total - visible;
    if (most < 0) {
        most = 0;
    }
    if (*scroll > most) {
        *scroll = most;
    }
    if (*scroll < 0) {
        *scroll = 0;
    }
    return *scroll;
}

static int draw_button(struct control_panel *cp, struct recon_panel *p,
        int x, int y, const char *label, uint32_t id, bool enabled) {
    int ascent = recon_font_ascent(cp->font);
    int width = recon_text_width(cp->font, label) + 22;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT,
        enabled ? COLOR_BUTTON : COLOR_BG);
    recon_draw_button_edge(p, x, y, width, BUTTON_HEIGHT, false,
        COLOR_BG);
    recon_draw_text(p, cp->font, x + 11, y + (BUTTON_HEIGHT + ascent) / 2 - 2,
        width - 16, label, enabled ? COLOR_TEXT : COLOR_DIM);

    /* A disabled button is drawn but not registered, so it cannot be pressed
     * and cannot report a failure the user could not have avoided. */
    if (enabled) {
        recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);
    }
    return x + width + 6;
}

static int draw_heading(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, const char *title, const char *detail) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w, title, COLOR_TEXT);
    y += line;

    if (detail != NULL) {
        recon_draw_text(p, cp->font, x, y + ascent, w, detail, COLOR_DIM);
        y += line;
    }
    return y + 6;
}

/*
 * A row of a list, with an optional second column, and the text starting
 * `indent` from the left edge so something can be drawn in front of it.
 */
static void draw_row_at(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int indent, int index, const char *label,
        const char *detail, bool selected) {
    int ascent = recon_font_ascent(cp->font);

    if (selected) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_SELECTED);
    } else if (index % 2 == 1) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_ROW_ALT);
    }

    recon_color ink = selected ? COLOR_SELECTED_TEXT : COLOR_TEXT;
    recon_color faint = selected ? COLOR_SELECTED_TEXT : COLOR_DIM;

    recon_draw_text(p, cp->font, x + indent, y + (ROW_HEIGHT + ascent) / 2 - 2,
        w / 2, label, ink);
    if (detail != NULL) {
        recon_draw_text(p, cp->font, x + w / 2, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, detail, faint);
    }

    recon_hit_add(p, x, y, w, ROW_HEIGHT, HIT_ROW_BASE + index);
}

static void draw_row(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int index, const char *label, const char *detail,
        bool selected) {
    draw_row_at(cp, p, x, y, w, 10, index, label, detail, selected);
}

/* --- The pages --- */

/*
 * The pictures, laid out as a grid to choose from.
 *
 * The first tile is "no picture", which puts the account back to a coloured
 * disc with its initial. That has to be reachable or a choice made once could
 * never be undone.
 */
#define AVATAR_TILE 56
#define AVATAR_FACE 40

static void draw_avatar_picker(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);

    /* Sized for what goes in it. question_target now holds a registry key,
     * which is longer than an account name, and this heading was the one
     * place that silently shortened it. */
    char title[RECON_REGISTRY_KEY_MAX + 32];
    snprintf(title, sizeof(title), "A picture for %s", cp->question_target);
    y = draw_heading(cp, p, x, y, w, title,
        "Or drop a file called avatar-something into /System/Icons.");

    int columns = w / AVATAR_TILE;
    if (columns < 1) {
        columns = 1;
    }

    int total = recon_avatar_count() + 1;   /* the first is "none" */
    for (int i = 0; i < total; i++) {
        int tx = x + (i % columns) * AVATAR_TILE;
        int ty = y + (i / columns) * AVATAR_TILE;
        if (ty + AVATAR_TILE > y + h) {
            break;
        }

        char name[64] = "";
        if (i > 0 && !recon_avatar_at(i - 1, name, sizeof(name))) {
            continue;
        }

        const char *current = recon_avatar_of(cp->question_target);
        bool chosen = strcmp(current, name) == 0;
        if (chosen) {
            recon_fill_rect(p, tx, ty, AVATAR_TILE - 4, AVATAR_TILE - 4,
                COLOR_SELECTED);
        }

        int fx = tx + (AVATAR_TILE - 4 - AVATAR_FACE) / 2;
        int fy = ty + 4;

        if (i == 0) {
            /* The account's own initial, which is what "none" means. */
            recon_avatar_draw(p, cp->font, cp->question_target, fx, fy,
                AVATAR_FACE);
            recon_draw_text(p, cp->font, tx + 8, fy + AVATAR_FACE + ascent + 2,
                AVATAR_TILE - 12, "None",
                chosen ? COLOR_SELECTED_TEXT : COLOR_DIM);
        } else if (!recon_icon_draw(p, name, fx, fy, AVATAR_FACE)) {
            continue;
        }

        recon_hit_add(p, tx, ty, AVATAR_TILE - 4, AVATAR_TILE - 4,
            HIT_AVATAR_BASE + i);
    }

    int rows = (total + columns - 1) / columns;
    draw_button(cp, p, x, y + rows * AVATAR_TILE + PADDING, "Done",
        HIT_ACTION_BASE + ACTION_CHOOSE_AVATAR, true);
}

static void draw_accounts(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    bool admin = recon_users_may_administer();

    if (cp->picking_avatar) {
        draw_avatar_picker(cp, p, x, y, w, h);
        return;
    }

    y = draw_heading(cp, p, x, y, w, "Accounts",
        admin ? "Who can use this system."
              : "Only an administrator can change these.");

    int count = recon_users_count();
    int rows = (h - (y - 0) - BUTTON_HEIGHT - PADDING * 2) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    for (int i = 0; i < count && i < rows; i++) {
        struct recon_user user;
        if (!recon_users_at(i, &user)) {
            break;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "%s%s%s",
            user.role == RECON_ROLE_ADMINISTRATOR ? "Administrator" : "Limited",
            user.has_password ? "" : "   no password",
            (recon_users_current() != NULL &&
             strcmp(recon_users_current(), user.name) == 0)
                ? "   signed in" : "");

        int ry = y + i * ROW_HEIGHT;

        /* The row first, then the picture over it, so the highlight does not
         * paint across the face of the account you have selected. */
        draw_row_at(cp, p, x, ry, w, ROW_HEIGHT + 6, i, user.name, detail,
            i == cp->selected);
        recon_avatar_draw(p, cp->font, user.name, x + 4, ry + 2,
            ROW_HEIGHT - 4);
    }

    y += cp->list_h + PADDING;

    /* Adding an account, or changing a password, happens here rather than in
     * a separate window: it is two fields and a button. */
    if (cp->editing) {
        int ascent = recon_font_ascent(cp->font);
        int field_w = (w - 12) / 2;

        if (!cp->editing_password_only) {
            recon_edit_draw(p, cp->font, x, y, field_w, FIELD_HEIGHT, &cp->name);
            if (!cp->password_focused) {
                recon_stroke_rect(p, x - 1, y - 1, field_w + 2, FIELD_HEIGHT + 2,
                    THEME(ACCENT));
            }
            recon_hit_add(p, x, y, field_w, FIELD_HEIGHT, HIT_FIELD_BASE);
        } else {
            recon_draw_text(p, cp->font, x, y + (FIELD_HEIGHT + ascent) / 2 - 2,
                field_w, cp->question_target, COLOR_TEXT);
        }

        recon_edit_draw(p, cp->font, x + field_w + 12, y, field_w,
            FIELD_HEIGHT, &cp->password);
        if (cp->password_focused) {
            recon_stroke_rect(p, x + field_w + 11, y - 1, field_w + 2,
                FIELD_HEIGHT + 2, THEME(ACCENT));
        }
        recon_hit_add(p, x + field_w + 12, y, field_w, FIELD_HEIGHT,
            HIT_FIELD_BASE + 1);

        y += FIELD_HEIGHT + 8;

        int bx = x;
        bx = draw_button(cp, p, bx, y,
            cp->editing_password_only ? "Set Password" : "Create",
            HIT_ACTION_BASE + (cp->editing_password_only
                ? ACTION_SET_PASSWORD : ACTION_ADD_USER), true);
        draw_button(cp, p, bx, y, "Cancel", HIT_ACTION_BASE + ACTION_NONE, true);
        return;
    }

    struct recon_user chosen;
    bool have = recon_users_at(cp->selected, &chosen);

    int bx = x;
    bx = draw_button(cp, p, bx, y, "Add Account",
        HIT_ACTION_BASE + ACTION_ADD_USER, admin);
    bx = draw_button(cp, p, bx, y, "Password",
        HIT_ACTION_BASE + ACTION_SET_PASSWORD, admin && have);
    /*
     * Changing a picture is not administration: it is somebody deciding what
     * they look like. So an account may change its own without being an
     * administrator, which is the one thing on this page that is true of.
     */
    bool own = have && recon_users_current() != NULL &&
        strcmp(recon_users_current(), chosen.name) == 0;
    bx = draw_button(cp, p, bx, y, "Picture",
        HIT_ACTION_BASE + ACTION_NEXT_AVATAR, have && (admin || own));
    bx = draw_button(cp, p, bx, y,
        (have && chosen.role == RECON_ROLE_ADMINISTRATOR)
            ? "Make Limited" : "Make Administrator",
        HIT_ACTION_BASE + ACTION_TOGGLE_ROLE, admin && have);
    draw_button(cp, p, bx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_USER, admin && have);
}

/* --- Writing a skin --- */

/* A colour as a person types it: six digits, or eight when the alpha matters. */
static void colour_text(recon_color c, char *out, size_t size) {
    if ((c >> 24) == 0xFF) {
        snprintf(out, size, "%06X", c & 0xFFFFFFu);
    } else {
        snprintf(out, size, "%08X", c);
    }
}

/*
 * Read a colour somebody typed.
 *
 * Six digits are opaque and eight carry an alpha, the same as a skin file --
 * so what is typed here and what is in the file are the same notation, and
 * somebody who has read one can write the other.
 */
static bool colour_parse(const char *text, recon_color *out) {
    while (*text == ' ' || *text == '#') {
        text++;
    }

    unsigned long value = 0;
    int digits = 0;
    for (; *text != '\0'; text++) {
        if (*text == ' ') {
            break;
        }
        int digit;
        if (*text >= '0' && *text <= '9') {
            digit = *text - '0';
        } else if (*text >= 'a' && *text <= 'f') {
            digit = *text - 'a' + 10;
        } else if (*text >= 'A' && *text <= 'F') {
            digit = *text - 'A' + 10;
        } else {
            return false;
        }
        value = value * 16 + (unsigned long)digit;
        digits++;
        if (digits > 8) {
            return false;
        }
    }

    if (digits == 6) {
        *out = (recon_color)(0xFF000000u | value);
        return true;
    }
    if (digits == 8) {
        *out = (recon_color)value;
        return true;
    }
    return false;
}

/*
 * Every colour the chosen skin answers, and what it answers.
 *
 * The whole list rather than a chosen few. Which roles matter depends
 * entirely on what somebody is trying to change, and a shortened list is a
 * guess about that made by whoever wrote the page.
 */
static void draw_skin_editor(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    char title[80];
    snprintf(title, sizeof(title), "Editing %s", cp->skin_name);

    y = draw_heading(cp, p, x, y, w, title,
        "Colours are RRGGBB, or AARRGGBB where transparency matters. "
        "Saving is immediate.");

    /* Room for the field and the buttons under the list, always, so choosing
     * a row near the bottom does not push the Save button off the page. */
    int footer = FIELD_HEIGHT + BUTTON_HEIGHT + line + PADDING * 3;
    int rows = (h - y - footer) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > RECON_THEME_ROLE_COUNT) {
        rows = RECON_THEME_ROLE_COUNT;
    }

    if (cp->skin_row < 0) {
        cp->skin_row = 0;
    }
    if (cp->skin_row >= RECON_THEME_ROLE_COUNT) {
        cp->skin_row = RECON_THEME_ROLE_COUNT - 1;
    }
    /* Keep the chosen row in sight, so the arrow keys and a click agree about
     * where the list is. */
    if (cp->skin_row < cp->skin_scroll) {
        cp->skin_scroll = cp->skin_row;
    } else if (cp->skin_row >= cp->skin_scroll + rows) {
        cp->skin_scroll = cp->skin_row - rows + 1;
    }
    if (cp->skin_scroll > RECON_THEME_ROLE_COUNT - rows) {
        cp->skin_scroll = RECON_THEME_ROLE_COUNT - rows;
    }
    if (cp->skin_scroll < 0) {
        cp->skin_scroll = 0;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int swatch = ROW_HEIGHT - 8;

    for (int i = 0; i < rows; i++) {
        int role = cp->skin_scroll + i;
        if (role >= RECON_THEME_ROLE_COUNT) {
            break;
        }

        int ry = y + i * ROW_HEIGHT;
        bool chosen = (role == cp->skin_row);

        if (chosen) {
            recon_fill_role(p, x, ry, w, ROW_HEIGHT, RECON_THEME_SELECTION);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color c = recon_theme_color((enum recon_theme_role)role);

        /*
         * The colour itself, beside its name. A list of hex numbers is not a
         * list of colours to anybody, and the whole point of the page is
         * choosing how something looks.
         */
        recon_fill_rect(p, x + 6, ry + 4, swatch, swatch, c);
        recon_stroke_rect(p, x + 6, ry + 4, swatch, swatch, COLOR_SEPARATOR);

        recon_color from, to;
        bool ramped = recon_theme_gradient((enum recon_theme_role)role,
            &from, &to);
        if (ramped) {
            /* The far end of the ramp, in the same square, so a role that is
             * two colours does not look like one. */
            recon_fill_rect(p, x + 6 + swatch / 2, ry + 4, swatch - swatch / 2,
                swatch, to);
            recon_stroke_rect(p, x + 6, ry + 4, swatch, swatch,
                COLOR_SEPARATOR);
        }

        char value[16];
        colour_text(c, value, sizeof(value));

        recon_color ink = chosen ? THEME(SELECTION_TEXT) : COLOR_TEXT;
        recon_color faint = chosen ? THEME(SELECTION_TEXT) : COLOR_DIM;

        int text_x = x + 6 + swatch + 10;
        recon_draw_text(p, cp->font, text_x,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w - (text_x - x) - 210,
            recon_theme_role_name((enum recon_theme_role)role), ink);

        recon_draw_text(p, cp->font, x + w - 200,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, 80, value, faint);

        if (ramped) {
            char ramp[16];
            colour_text(to, ramp, sizeof(ramp));
            char both[24];
            snprintf(both, sizeof(both), "to %s", ramp);
            recon_draw_text(p, cp->font, x + w - 110,
                ry + (ROW_HEIGHT + ascent) / 2 - 2, 100, both, faint);
        }

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }

    y += cp->list_h + PADDING;

    if (cp->skin_value_editing) {
        char label[96];
        snprintf(label, sizeof(label), "%s. Empty leaves it alone.",
            recon_theme_role_name((enum recon_theme_role)cp->skin_row));
        recon_draw_text(p, cp->font, x, y + ascent, w, label, COLOR_DIM);
        y += line;

        recon_edit_draw(p, cp->font, x, y, 200, FIELD_HEIGHT, &cp->skin_value);
        recon_hit_add(p, x, y, 200, FIELD_HEIGHT, HIT_FIELD_BASE + 6);
        y += FIELD_HEIGHT + 8;

        int bx = draw_button(cp, p, x, y, "Set",
            HIT_ACTION_BASE + ACTION_SKIN_SET, true);
        draw_button(cp, p, bx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_SKIN_CANCEL, true);
        return;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Pick a colour to change it. A skin is a file; this writes to it.",
        COLOR_DIM);
    y += line + PADDING;

    int bx = draw_button(cp, p, x, y, "Change Colour",
        HIT_ACTION_BASE + ACTION_EDIT_SKIN, true);

    /* Flattening is its own button rather than a value you can type, because
     * "no gradient" is not a colour and there is nothing to type for it. */
    recon_color from, to;
    bool ramped = recon_theme_gradient((enum recon_theme_role)cp->skin_row,
        &from, &to);
    bx = draw_button(cp, p, bx, y, "Remove Ramp",
        HIT_ACTION_BASE + ACTION_SKIN_FLAT, ramped);

    draw_button(cp, p, bx, y, "Done",
        HIT_ACTION_BASE + ACTION_SKIN_DONE, true);
}

/*
 * Which page of the help answers a question asked from this page.
 *
 * Not every page has one, and the ones that do not say so with NULL rather
 * than with a page that is nearly right -- being sent somewhere unrelated is
 * worse than being sent to the front of the help.
 */
static const char *help_topic_for(enum page page) {
    switch (page) {
    case PAGE_ACCOUNTS:   return "Accounts";
    case PAGE_APPEARANCE: return "How it looks";
    case PAGE_CLEANUP:    return "Storage";
    case PAGE_CLOCK:      return "How it looks";
    case PAGE_DISPLAY:    return "How it looks";
    case PAGE_PROGRAMS:   return "Programs";
    case PAGE_MODULES:    return "Programs";
    case PAGE_STORAGE:    return "Files";
    case PAGE_REGISTRY:   return "The Terminal";
    default:              return "What is not built yet";
    }
}

/*
 * The bar of sections across the top. Drawn like the tabs in Watchtower
 * because they do the same job, and two things that behave alike should not
 * look like two different inventions.
 */
/*
 * A row of tabs, and the y to carry on drawing from.
 *
 * Written for Appearance and then wanted by Network, so it takes the names,
 * which one is on, and where its clicks land rather than knowing any of them.
 * Two copies of a tab bar is two places for a tab bar to be wrong.
 */
static int draw_tabs(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, const char *const *names, int count,
        int current, uint32_t base) {
    int ascent = recon_font_ascent(cp->font);
    int bx = x;

    for (int i = 0; i < count; i++) {
        const char *label = names[i];
        int width = recon_text_width(cp->font, label) + 28;
        if (bx + width > x + w) {
            break;
        }

        bool on = current == i;
        recon_fill_rect(p, bx, y, width, SECTION_HEIGHT,
            on ? COLOR_PANEL : COLOR_BG);
        recon_stroke_rect(p, bx, y, width, SECTION_HEIGHT, COLOR_SEPARATOR);

        int tw = recon_text_width(cp->font, label);
        recon_draw_text(p, cp->font, bx + (width - tw) / 2,
            y + (SECTION_HEIGHT + ascent) / 2 - 2, width - 8, label,
            on ? COLOR_TEXT : COLOR_DIM);

        recon_hit_add(p, bx, y, width, SECTION_HEIGHT, base + (uint32_t)i);
        bx += width;
    }

    /* A rule under the whole bar, so the chosen tab reads as joined to what
     * is under it and the others as sitting behind. */
    recon_fill_rect(p, x, y + SECTION_HEIGHT - 1, w, 1, COLOR_SEPARATOR);
    return y + SECTION_HEIGHT + PADDING;
}

static int draw_sections(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w) {
    return draw_tabs(cp, p, x, y, w, APPEARANCE_SECTION_NAMES,
        APPEARANCE_SECTIONS, (int)cp->section, HIT_SECTION_BASE);
}

/* --- Themes --- */

/*
 * Which skin a new one is being built from.
 *
 * Either the one named when New Skin was pressed, or -- when the naming form
 * was reached by customizing -- whichever row is selected. Both paths end at
 * the same form, and the form should not have to know which way it was
 * reached.
 */
static bool source_skin(struct control_panel *cp,
        struct recon_theme_info *out) {
    if (cp->new_skin_from[0] != '\0') {
        int count = recon_theme_count();
        for (int i = 0; i < count; i++) {
            if (recon_theme_at(i, out) &&
                    strcmp(out->name, cp->new_skin_from) == 0) {
                return true;
            }
        }
        return false;
    }
    return recon_theme_at(cp->selected, out);
}


static void draw_appearance_themes(struct control_panel *cp,
        struct recon_panel *p, int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;

    /*
     * The buttons are laid out from the bottom up, and the list takes what is
     * left. The other way round -- list first, buttons after -- is what left
     * the wallpapers with two rows out of five: whatever grows squeezes
     * whatever is drawn after it, and a list grows.
     */
    int controls = BUTTON_HEIGHT + PADDING;
    if (cp->naming_skin) {
        controls = line + 2 + (FIELD_HEIGHT + 6) * 2 + BUTTON_HEIGHT + PADDING;
    }

    int list_h = bottom - y - controls;
    if (list_h < ROW_HEIGHT) {
        list_h = ROW_HEIGHT;
    }

    int count = recon_theme_count();
    int rows = list_h / ROW_HEIGHT;
    if (rows > count) {
        rows = count;
    }

    clamp_scroll(&cp->theme_scroll, rows, count);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->theme_scroll, rows, count);
    w -= bar;

    const char *current = recon_theme_current();

    for (int row = 0; row < rows; row++) {
        int i = cp->theme_scroll + row;
        if (i >= count) {
            break;
        }

        struct recon_theme_info info;
        if (!recon_theme_at(i, &info)) {
            break;
        }

        int ry = y + row * ROW_HEIGHT;
        bool showing = strcmp(info.name, current) == 0;
        bool picked = cp->selected == i;

        /*
         * Each row is drawn in the colours of the skin it offers, not in the
         * ones currently on screen. A list of skins painted entirely in the
         * present skin shows nothing about any of the others.
         */
        recon_color row_bg = recon_theme_color_of(i, RECON_THEME_SELECTION);
        recon_color row_ink = recon_theme_color_of(i, RECON_THEME_SELECTION_TEXT);

        if (picked) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, row_bg);
        } else {
            if (i % 2 == 1) {
                recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
            }
            /* A swatch of the skin's own selection colour down the left edge:
             * the whole list in ten different backgrounds would be a mess to
             * read. */
            recon_fill_rect(p, x, ry + 3, 5, ROW_HEIGHT - 6, row_bg);
        }

        recon_color ink = picked ? row_ink : COLOR_TEXT;
        recon_color faint = picked ? row_ink : COLOR_DIM;

        /*
         * Which one is on, said in words. The row used to be highlighted for
         * the skin in use and for the skin you had clicked, which are two
         * different facts wearing one appearance -- so choosing a skin to
         * copy looked like changing the skin.
         */
        char name[80];
        if (showing) {
            snprintf(name, sizeof(name), "%s  (in use)", info.name);
        } else {
            snprintf(name, sizeof(name), "%s", info.name);
        }

        recon_draw_text(p, cp->font, x + 14,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w / 2, name, ink);
        recon_draw_text(p, cp->font, x + w / 2,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w / 2 - 10, info.description,
            faint);

        /* The row's number on screen, not its number in the list: the
         * click handler adds the scroll back on. */
        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + row);
    }

    w += bar;
    y += cp->list_h + PADDING;

    struct recon_theme_info chosen;
    bool have_chosen = recon_theme_at(cp->selected, &chosen);

    /*
     * --- Making one of your own ---
     *
     * "Customize", not "Copy". Copying is what happens underneath, and it is
     * not what anybody came here to do: they came to change how the system
     * looks and are told, correctly, that changing a built-in is not
     * possible. Naming the button after the thing they want rather than after
     * the mechanism they have to use is the difference between a system that
     * helps and one that explains itself.
     */
    if (cp->naming_skin) {
        char asking[140];
        struct recon_theme_info from;
        snprintf(asking, sizeof(asking), "A name for your version of %s:",
            source_skin(cp, &from) ? from.name : recon_theme_current());
        recon_draw_text(p, cp->font, x, y + ascent, w, asking, COLOR_DIM);
        y += line + 2;

        recon_edit_draw(p, cp->font, x, y, 240, FIELD_HEIGHT,
            &cp->skin_new_name);
        recon_hit_add(p, x, y, 240, FIELD_HEIGHT, HIT_FIELD_BASE + 5);
        y += FIELD_HEIGHT + 6;

        recon_edit_draw(p, cp->font, x, y, w - 20, FIELD_HEIGHT,
            &cp->skin_new_desc);
        recon_hit_add(p, x, y, w - 20, FIELD_HEIGHT, HIT_FIELD_BASE + 7);
        y += FIELD_HEIGHT + 6;

        int nbx = draw_button(cp, p, x, y, "Create",
            HIT_ACTION_BASE + ACTION_CONFIRM_COPY_SKIN, true);
        draw_button(cp, p, nbx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_SKIN_CANCEL, true);
        return;
    }

    int sbx = draw_button(cp, p, x, y, "Use This Skin",
        HIT_ACTION_BASE + ACTION_USE_SKIN, have_chosen);
    sbx = draw_button(cp, p, sbx, y, "Customize Skin",
        HIT_ACTION_BASE + ACTION_COPY_SKIN, have_chosen);
    sbx = draw_button(cp, p, sbx, y, "Edit Colours",
        HIT_ACTION_BASE + ACTION_EDIT_SKIN,
        have_chosen && !chosen.built_in);

    /*
     * Always enabled: this is the one button here that does not act on the
     * selection, which is the whole reason for it. "Customize Skin" makes a
     * copy of the row you are pointing at, and somebody who wants to make
     * their own has to work out that pointing at somebody else's is how.
     */
    draw_button(cp, p, sbx, y, "New Skin",
        HIT_ACTION_BASE + ACTION_NEW_SKIN, true);
}

/* --- Colours --- */

/*
 * The colours of whichever skin is on, and a way to change one.
 *
 * Changing a colour on a built-in skin cannot write to it -- a built-in is
 * compiled in, and a file cannot shadow one -- so rather than refusing, this
 * offers to make the change on a copy under a name of your own. That is what
 * somebody wanted in the first place; being told "you cannot edit this" and
 * left to work out that copying it first is the way round is the system
 * making its own limitation into the reader's problem.
 */
static void draw_appearance_colours(struct control_panel *cp,
        struct recon_panel *p, int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    const char *current = recon_theme_current();

    /* Whether the skin on screen is one that can be written to. */
    bool editable = false;
    for (int i = 0; i < recon_theme_count(); i++) {
        struct recon_theme_info info;
        if (recon_theme_at(i, &info) && strcmp(info.name, current) == 0) {
            editable = !info.built_in;
            break;
        }
    }

    char heading[160];
    if (editable) {
        snprintf(heading, sizeof(heading),
            "The colours of %s. Changing one writes to it.", current);
    } else {
        snprintf(heading, sizeof(heading),
            "The colours of %s, which is built in. Changing one makes a copy "
            "under your own name.", current);
    }
    recon_draw_text(p, cp->font, x, y + ascent, w, heading, COLOR_DIM);
    y += line + PADDING;

    int bottom = y + h - (line + PADDING);
    int controls = BUTTON_HEIGHT + PADDING;
    int list_h = bottom - y - controls;
    if (list_h < ROW_HEIGHT) {
        list_h = ROW_HEIGHT;
    }

    int roles = RECON_THEME_ROLE_COUNT;
    int rows = list_h / ROW_HEIGHT;
    if (rows > roles) {
        rows = roles;
    }
    if (rows < 0) {
        rows = 0;
    }

    clamp_scroll(&cp->skin_scroll, rows, roles);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->skin_scroll, rows, roles);
    w -= bar;

    int swatch = ROW_HEIGHT - 8;

    for (int i = 0; i < rows; i++) {
        int role = cp->skin_scroll + i;
        int ry = y + i * ROW_HEIGHT;
        bool picked = cp->skin_row == role;

        if (picked) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_SELECTED);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color c = recon_theme_color((enum recon_theme_role)role);
        recon_fill_rect(p, x + 6, ry + 4, swatch, swatch, c);
        recon_stroke_rect(p, x + 6, ry + 4, swatch, swatch, COLOR_SEPARATOR);

        recon_color from, to;
        if (recon_theme_gradient((enum recon_theme_role)role, &from, &to)) {
            /* The far end of the ramp, in the same square, so a role that is
             * two colours does not look like one. */
            recon_fill_rect(p, x + 6 + swatch / 2, ry + 4,
                swatch - swatch / 2, swatch, to);
            recon_stroke_rect(p, x + 6, ry + 4, swatch, swatch,
                COLOR_SEPARATOR);
        }

        char value[16];
        colour_text(c, value, sizeof(value));

        recon_color ink = picked ? COLOR_SELECTED_TEXT : COLOR_TEXT;
        recon_color faint = picked ? COLOR_SELECTED_TEXT : COLOR_DIM;

        int text_x = x + 6 + swatch + 10;
        recon_draw_text(p, cp->font, text_x,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w - (text_x - x) - 110,
            recon_theme_role_name((enum recon_theme_role)role), ink);
        recon_draw_text(p, cp->font, x + w - 100,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, 90, value, faint);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }

    w += bar;
    y += cp->list_h + PADDING;

    if (roles > rows) {
        char more[64];
        snprintf(more, sizeof(more), "%d-%d of %d. The wheel shows the rest.",
            cp->skin_scroll + 1, cp->skin_scroll + rows, roles);
        recon_draw_text(p, cp->font, x + w - 240, y + ascent, 230, more,
            COLOR_DIM);
    }

    draw_button(cp, p, x, y, editable ? "Change Colour" : "Customize Skin",
        HIT_ACTION_BASE + (editable ? ACTION_EDIT_SKIN : ACTION_COPY_SKIN),
        true);
}

/* --- Wallpapers --- */

static void draw_appearance_wallpapers(struct control_panel *cp,
        struct recon_panel *p, int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Choosing a skin puts its own wallpaper on. One chosen here stays "
        "until the skin changes again.", COLOR_DIM);
    y += line + PADDING;

    int papers = recon_wallpaper_count();
    int list_h = h - (line + PADDING) - BUTTON_HEIGHT - PADDING;
    int rows = list_h / ROW_HEIGHT;
    if (rows > papers) {
        rows = papers;
    }
    if (rows < 0) {
        rows = 0;
    }

    clamp_scroll(&cp->paper_scroll, rows, papers);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->paper_scroll, rows, papers);
    w -= bar;

    const char *showing = recon_wallpaper_current();

    for (int row = 0; row < rows; row++) {
        int i = cp->paper_scroll + row;
        if (i >= papers) {
            break;
        }

        char name[96];
        if (!recon_wallpaper_at(i, name, sizeof(name))) {
            break;
        }

        int ry = y + row * ROW_HEIGHT;

        /*
         * Two different facts, and they were sharing one highlight.
         *
         * `on` is the row somebody clicked -- what Remove would act on, and
         * the only thing a click has to show for itself. `showing` is the
         * picture actually on the desktop, which the dot says instead. When
         * the highlight tracked `showing`, clicking a row changed nothing
         * anybody could see and Remove appeared to fire at random.
         */
        bool on = row == cp->selected;
        bool in_use = strcmp(name, showing) == 0;

        if (on) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_SELECTED);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        if (in_use) {
            recon_fill_rect(p, x + 8, ry + ROW_HEIGHT / 2 - 3, 6, 6,
                on ? COLOR_SELECTED_TEXT : COLOR_SELECTED);
        }

        recon_draw_text(p, cp->font, x + 20,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w / 2 - 26, name,
            on ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        /*
         * Where it came from, on the right half.
         *
         * Two folders can both hold a Sunset.png, and a list of names alone
         * cannot tell somebody which of theirs each row is. One that ships
         * says so rather than leaving the space blank, because blank reads as
         * missing rather than as "there is no answer to that here".
         */
        char origin[RECON_PATH_MAX];
        const char *where = "ships with ReconOS";
        if (recon_wallpaper_origin(name, origin, sizeof(origin))) {
            /*
             * Shortened from the left when it is long: the end of a path says
             * which folder, and the beginning says things everybody already
             * knows.
             */
            where = origin;
            if (recon_text_width(cp->font, origin) > w / 2 - 30) {
                const char *cut = strrchr(origin, '/');
                if (cut != NULL && cut != origin) {
                    /* Keep the last two parts: the folder, and what is above
                     * it, which is usually enough to place it. */
                    const char *above = cut - 1;
                    while (above > origin && *above != '/') {
                        above--;
                    }
                    static char shortened[RECON_PATH_MAX];
                    snprintf(shortened, sizeof(shortened), "*%s", above);
                    where = shortened;
                }
            }
        }

        recon_draw_text(p, cp->font, x + w / 2,
            ry + (ROW_HEIGHT + ascent) / 2 - 2, w / 2 - 16, where,
            on ? COLOR_SELECTED_TEXT : COLOR_DIM);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_WALLPAPER_BASE + row);
    }

    w += bar;
    y += cp->list_h + PADDING;

    /*
     * Remove only what somebody added. A picture that ships is a preset, and
     * the button says so by being greyed rather than by refusing after the
     * press.
     */
    char chosen[96];
    char chosen_origin[RECON_PATH_MAX];
    bool own = cp->selected >= 0 &&
        cp->paper_scroll + cp->selected < papers &&
        recon_wallpaper_at(cp->paper_scroll + cp->selected, chosen,
            sizeof(chosen)) &&
        recon_wallpaper_origin(chosen, chosen_origin, sizeof(chosen_origin));

    int wx = draw_button(cp, p, x, y, "Add a Picture",
        HIT_ACTION_BASE + ACTION_ADD_WALLPAPER, true);
    draw_button(cp, p, wx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_WALLPAPER, own);

    if (papers > rows) {
        char more[64];
        snprintf(more, sizeof(more), "%d-%d of %d. The wheel shows the rest.",
            cp->paper_scroll + 1, cp->paper_scroll + rows, papers);
        recon_draw_text(p, cp->font, x + w - 240,
            y + (BUTTON_HEIGHT + ascent) / 2 - 2, 230, more, COLOR_DIM);
    }
}

static void draw_appearance(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    if (cp->skin_editing) {
        draw_skin_editor(cp, p, x, y, w, h);
        return;
    }

    int bottom = y + h;
    y = draw_heading(cp, p, x, y, w, "Appearance",
        "Yours alone; other accounts keep theirs.");
    y = draw_sections(cp, p, x, y, w);

    int room = bottom - y;
    if (room < ROW_HEIGHT) {
        room = ROW_HEIGHT;
    }

    switch (cp->section) {
    case APPEARANCE_COLOURS:
        draw_appearance_colours(cp, p, x, y, w, room);
        break;
    case APPEARANCE_WALLPAPERS:
        draw_appearance_wallpapers(cp, p, x, y, w, room);
        break;
    case APPEARANCE_THEMES:
    default:
        draw_appearance_themes(cp, p, x, y, w, room);
        break;
    }
}

/*
 * Defined below. Two pages draw it after their own settings: the ones that
 * have something working and something that needs a kernel.
 */
static void draw_pending_list(struct control_panel *cp, struct recon_panel *p,
    int x, int y, int w, int h, enum page page, bool heading);

/*
 * --- Display Settings ---
 *
 * It was called Reading, and it was about the spacing and the size of text.
 * That is a fair description of what it does and a poor one of what it is
 * for: somebody looking for how big the letters are does not go to a page
 * called Reading, and somebody looking for the resolution has nowhere to go
 * at all.
 *
 * Still missing here, and worth naming rather than leaving as a gap: the
 * screen resolution, a list of fonts to choose from, and a way to add one.
 * ReconOS has no fonts of its own and no installer for them -- it draws with
 * whichever the host has -- so all three wait on the same thing.
 */
static void draw_display(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int bottom = y + h;
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Display Settings",
        "How text is spaced and sized. Not a colour; see Appearance for that.");

    struct {
        const char *label;
        int value;
        enum action less;
        enum action more;
        const char *detail;
    } rows[] = {
        { "Space between letters", recon_text_letter_spacing(),
          ACTION_SPACING_LESS, ACTION_SPACING_MORE,
          "The best-supported adjustment for a dyslexic reader." },
        { "Space between lines", recon_text_line_spacing(),
          ACTION_LINES_LESS, ACTION_LINES_MORE, NULL },
        { "Text size",
          recon_registry_get_int(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY,
              RECON_ACCESS_FONT_SIZE_DEFAULT),
          ACTION_SIZE_LESS, ACTION_SIZE_MORE,
          "The chrome is laid out in fixed pixels, so a large size crowds it." },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        recon_draw_text(p, cp->font, x, y + ascent, w - 140, rows[i].label,
            COLOR_TEXT);

        char value[16];
        snprintf(value, sizeof(value), "%d", rows[i].value);

        int bx = x + w - 120;
        bx = draw_button(cp, p, bx, y - 4, "-",
            HIT_ACTION_BASE + rows[i].less, true);
        recon_draw_text(p, cp->font, bx + 6, y + ascent, 30, value, COLOR_TEXT);
        draw_button(cp, p, bx + 34, y - 4, "+",
            HIT_ACTION_BASE + rows[i].more, true);

        y += line + 6;
        if (rows[i].detail != NULL) {
            recon_draw_text(p, cp->font, x, y + ascent, w, rows[i].detail,
                COLOR_DIM);
            y += line;
        }
        y += 10;
    }

    draw_button(cp, p, x, y, "Back to the defaults",
        HIT_ACTION_BASE + ACTION_RESET_READING, true);
    y += BUTTON_HEIGHT + PADDING * 2;

    /* --- The font --- */

    const char *chosen = recon_registry_get(RECON_REG_USER,
        RECON_ACCESS_FONT_KEY, "");

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Font", COLOR_TEXT);
    y += line + 4;

    int installed = recon_fonts_count();

    /*
     * Room kept for the buttons under it, so the list never grows into them.
     * Four rows at most: this is one section of a page, not the page.
     */
    int shown = (bottom - y - BUTTON_HEIGHT - PADDING * 4) / ROW_HEIGHT;
    if (shown > 4) {
        shown = 4;
    }
    if (shown > installed) {
        shown = installed;
    }
    if (shown < 1) {
        shown = 1;
    }

    clamp_scroll(&cp->font_scroll, shown, installed);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = shown * ROW_HEIGHT;

    int fbar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->font_scroll, shown, installed);
    int flw = w - fbar;

    recon_fill_rect(p, x, y, flw, cp->list_h, COLOR_PANEL);

    for (int row = 0; row < shown; row++) {
        char name[96];
        if (!recon_fonts_at(cp->font_scroll + row, name, sizeof(name))) {
            break;
        }

        char path[RECON_PATH_MAX];
        bool in_use = recon_fonts_path(name, path, sizeof(path)) &&
            strcmp(path, chosen) == 0;

        char origin[RECON_PATH_MAX];
        draw_row(cp, p, x, y + row * ROW_HEIGHT, flw, row, name,
            recon_fonts_origin(name, origin, sizeof(origin))
                ? origin : "ships with ReconOS",
            row == cp->font_selected);

        /* The one being drawn with, marked the way the wallpaper list marks
         * the picture on the desktop: chosen and in use are two facts. */
        if (in_use) {
            recon_fill_rect(p, x + 5, y + row * ROW_HEIGHT + ROW_HEIGHT / 2 - 3,
                6, 6, row == cp->font_selected
                    ? COLOR_SELECTED_TEXT : COLOR_SELECTED);
        }

        recon_hit_add(p, x, y + row * ROW_HEIGHT, flw, ROW_HEIGHT,
            HIT_FONT_BASE + (uint32_t)row);
    }

    if (installed == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "None installed. The system's own is being used.",
            COLOR_DIM);
    }

    y += cp->list_h + 6;

    char summary[256];
    snprintf(summary, sizeof(summary), "Drawing with: %s",
        *chosen != '\0' ? chosen : "the system's own");
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_DIM);
    y += line + 6;

    char picked[96];
    bool have = cp->font_selected >= 0 &&
        recon_fonts_at(cp->font_scroll + cp->font_selected, picked,
            sizeof(picked));

    char picked_origin[RECON_PATH_MAX];
    bool own = have &&
        recon_fonts_origin(picked, picked_origin, sizeof(picked_origin));

    int fbx = draw_button(cp, p, x, y, "Use This Font",
        HIT_ACTION_BASE + ACTION_USE_FONT, have);
    fbx = draw_button(cp, p, fbx, y, "Add a Font",
        HIT_ACTION_BASE + ACTION_ADD_FONT, true);
    fbx = draw_button(cp, p, fbx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_FONT, own);
    draw_button(cp, p, fbx, y, "Use the System's",
        HIT_ACTION_BASE + ACTION_DEFAULT_FONT, *chosen != '\0');
    y += BUTTON_HEIGHT + PADDING * 2;

    /* --- The screen --- */

    recon_draw_text(p, cp->font, x, y + ascent, w, "Screen", COLOR_TEXT);
    y += line + 4;

    struct recon_display screen;
    if (!recon_display_at(0, &screen)) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "No display is attached.", COLOR_DIM);
        return;
    }

    char now[128];
    snprintf(now, sizeof(now), "%s is %d by %d", screen.name, screen.width,
        screen.height);
    recon_draw_text(p, cp->font, x, y + ascent, w, now, COLOR_DIM);
    y += line + 6;

    /*
     * A display that cannot be changed says so instead of offering a list.
     *
     * This is the usual case while ReconOS runs inside another compositor:
     * it is whatever size that window is, and a resolution picker there would
     * be a control that could only ever fail. Saying what is actually true is
     * more use than a disabled list.
     */
    if (!screen.can_change) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "It takes its size from what ReconOS is running inside, so there "
            "is nothing here to change.", COLOR_DIM);
        y += line;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "On a machine ReconOS has to itself, the sizes it offers appear "
            "here.", COLOR_DIM);
        return;
    }

    int sizes = screen.mode_count;
    if (sizes > 6) {
        sizes = 6;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = sizes * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    for (int row = 0; row < sizes; row++) {
        struct recon_display_mode mode;
        if (!recon_display_mode_at(screen.id, row, &mode)) {
            break;
        }

        char label[64];
        snprintf(label, sizeof(label), "%d by %d", mode.width, mode.height);

        /* Refresh in millihertz, divided only to show it. 59.94 is a real
         * refresh rate and rounding it to 59 would be inventing a number. */
        char detail[96];
        snprintf(detail, sizeof(detail), "%.2f Hz%s%s",
            (double)mode.refresh / 1000.0,
            mode.preferred ? "   this display's own" : "",
            mode.current ? "   in use" : "");

        draw_row(cp, p, x, y + row * ROW_HEIGHT, w, row, label, detail,
            row == cp->mode_selected);
        recon_hit_add(p, x, y + row * ROW_HEIGHT, w, ROW_HEIGHT,
            HIT_MODE_BASE + (uint32_t)row);
    }

    y += cp->list_h + PADDING;

    draw_button(cp, p, x, y, "Use This Size",
        HIT_ACTION_BASE + ACTION_SET_RESOLUTION, cp->mode_selected >= 0);
}

/*
 * A page of settings that do not work yet.
 *
 * Each one says what it is for, and clicking it says what has to exist first.
 * The tag on the right is deliberately plain: this is a note about the state
 * of the system, not a feature being advertised.
 */
/*
 * --- Power ---
 *
 * One thing here is real. Sleep, hibernate and a power mode all need control
 * of a machine that ReconOS is a process on; covering the screen with black
 * after a while does not.
 *
 * And it is described as what it is. "The display turns off" would be a claim
 * about hardware ReconOS cannot make -- what this saves is the picture, not
 * the watt.
 */
static const int BLANK_STEPS[] = { 0, 1, 2, 5, 10, 15, 30, 60 };

#define BLANK_STEP_COUNT ((int)(sizeof(BLANK_STEPS) / sizeof(BLANK_STEPS[0])))

static void blank_label(int minutes, char *out, size_t size) {
    if (minutes <= 0) {
        snprintf(out, size, "Never");
    } else if (minutes == 1) {
        snprintf(out, size, "After 1 minute");
    } else if (minutes < 60) {
        snprintf(out, size, "After %d minutes", minutes);
    } else {
        snprintf(out, size, "After 1 hour");
    }
}

static void draw_power(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;
    y = draw_heading(cp, p, x, y, w, "Power",
        "What the machine does when it is left alone.");

    int minutes = recon_registry_get_int(RECON_REG_USER,
        RECON_BLANK_AFTER_KEY, 0);
    bool lock = recon_registry_get_bool(RECON_REG_USER,
        RECON_BLANK_LOCK_KEY, false);

    /*
     * Two lines rather than one that runs off the edge, and the second one is
     * the honest caveat: this covers the screen, it does not switch the panel
     * off. Saying "the display turns off" would be a claim about hardware
     * ReconOS cannot make.
     */
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "Cover the screen when nothing has happened for a while.", COLOR_DIM);
    y += line;
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "It covers the screen rather than switching the display off, which "
        "would need a kernel.", COLOR_DIM);
    y += line + PADDING;

    char label[48];
    blank_label(minutes, label, sizeof(label));

    recon_draw_text(p, cp->font, x + 8, y + ascent + 6, 150, "Blank the screen",
        COLOR_TEXT);

    int bx = draw_button(cp, p, x + 170, y, "Sooner",
        HIT_ACTION_BASE + ACTION_BLANK_SHORTER, minutes > 0);
    bx = draw_button(cp, p, bx, y, "Later",
        HIT_ACTION_BASE + ACTION_BLANK_LONGER,
        minutes < BLANK_STEPS[BLANK_STEP_COUNT - 1]);
    recon_draw_text(p, cp->font, bx + 8, y + ascent + 6, w - (bx - x) - 16,
        label, minutes > 0 ? COLOR_TEXT : COLOR_DIM);

    y += BUTTON_HEIGHT + PADDING;

    draw_button(cp, p, x + 170, y,
        lock ? "Ask for the password: yes" : "Ask for the password: no",
        HIT_ACTION_BASE + ACTION_BLANK_LOCK, minutes > 0);
    recon_draw_text(p, cp->font, x + 8, y + ascent + 6, 160, "When it wakes",
        minutes > 0 ? COLOR_TEXT : COLOR_DIM);

    y += BUTTON_HEIGHT + PADDING;

    if (minutes <= 0) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "The screen never blanks, so there is nothing to wake from.",
            COLOR_DIM);
        y += line;
    }
    y += PADDING;

    /* And the three that genuinely need the kernel, in the same shape the
     * other unbuilt pages use -- without a second heading. */
    draw_pending_list(cp, p, x, y, w, bottom - y, PAGE_POWER, false);
}

/*
 * `heading` is false for a page that has already drawn its own, which is the
 * Power page: it has one setting that works and three that need a kernel, and
 * a second "Power -- what the machine does when it is left alone" halfway
 * down reads as the page having started again.
 */
static void draw_pending_list(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h, enum page page, bool heading) {
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    if (heading) {
        y = draw_heading(cp, p, x, y, w, PAGES[page].label,
            PENDING[page].lede);
    }

    const char *tag = "Not built yet";
    int tag_w = recon_text_width(cp->font, tag);

    for (int i = 0; i < PENDING[page].count; i++) {
        const struct pending_item *item = &PENDING[page].items[i];
        int row_h = line * 2 + 8;

        if (i % 2 == 1) {
            recon_fill_rect(p, x, y, w, row_h, COLOR_ROW_ALT);
        }

        recon_draw_text(p, cp->font, x + 8, y + 4 + ascent,
            w - tag_w - 30, item->label, COLOR_TEXT);
        recon_draw_text(p, cp->font, x + 8, y + 4 + line + ascent,
            w - 20, item->detail, COLOR_DIM);

        recon_draw_text(p, cp->font, x + w - tag_w - 10, y + 4 + ascent,
            tag_w + 4, tag, COLOR_DIM);

        recon_hit_add(p, x, y, w, row_h, HIT_PENDING_BASE + i);
        y += row_h + 2;
    }
}

/* --- Date and Time --- */

static void draw_clock_page(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;
    y = draw_heading(cp, p, x, y, w, "Date and Time",
        "What time this machine thinks it is, and where it thinks it is.");

    /* What it currently says, in full, because that is the thing being
     * changed and it should be visible while changing it. */
    char now[32];
    char date[96];
    char zone[RECON_CLOCK_ZONE_NAME_MAX];
    recon_clock_short(now, sizeof(now));
    recon_clock_date(date, sizeof(date));
    recon_clock_zone_label(zone, sizeof(zone));

    recon_draw_text(p, cp->font, x, y + ascent, w, date, COLOR_TEXT);
    y += line + 2;

    char summary[160];
    snprintf(summary, sizeof(summary), "%s   %s", now, zone);
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_DIM);
    y += line + PADDING;

    int bx = draw_button(cp, p, x, y, recon_registry_get_bool(RECON_REG_USER,
            RECON_CLOCK_24H_KEY, true) ? "Show am and pm" : "Show a 24-hour clock",
        HIT_ACTION_BASE + ACTION_TOGGLE_24H, true);
    draw_button(cp, p, bx, y, "Check Against a Time Server",
        HIT_ACTION_BASE + ACTION_CHECK_TIME, recon_clock_can_check());
    y += BUTTON_HEIGHT + 6;

    /*
     * What the last check found. Shown only once there has been one: a
     * machine that has never been asked to check is not in a state worth
     * reporting, and "never checked" beside a button offering to check reads
     * as a fault rather than as the default.
     */
    char detail[192];
    recon_clock_sync_detail(detail, sizeof(detail));
    if (detail[0] != '\0') {
        recon_color ink = recon_clock_sync_state() == RECON_CLOCK_DRIFTED ||
            recon_clock_sync_state() == RECON_CLOCK_UNREACHABLE
            ? COLOR_WARNING : COLOR_DIM;
        recon_draw_text(p, cp->font, x, y + ascent, w, detail, ink);
        y += line + 2;
    } else if (!recon_clock_can_check()) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Checking is off. Turn on 'clock/ask-the-network' in the "
            "Registry to allow it.", COLOR_DIM);
        y += line + 2;
    }
    y += PADDING;

    recon_draw_text(p, cp->font, x, y + ascent, w, "Time zone", COLOR_TEXT);
    y += line + 4;

    int count = recon_clock_zone_count();
    int current = recon_clock_zone_current();

    int rows = (bottom - y - line * 3 - PADDING * 2) / ROW_HEIGHT;
    if (rows > count) {
        rows = count;
    }
    if (rows < 1) {
        rows = 1;
    }

    clamp_scroll(&cp->zone_scroll, rows, count);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->zone_scroll, rows, count);
    int lw = w - bar;

    recon_fill_rect(p, x, y, lw, cp->list_h, COLOR_PANEL);

    for (int row = 0; row < rows; row++) {
        struct recon_clock_zone z;
        if (!recon_clock_zone_at(cp->zone_scroll + row, &z)) {
            break;
        }

        /*
         * What the clock would read there, on every row.
         *
         * A list of offsets asks somebody to do arithmetic to find their own
         * zone. A list of times lets them look for the one that matches the
         * watch on their wrist, which is the actual question.
         */
        int minutes = z.minutes;
        int here = recon_registry_get_int(RECON_REG_USER,
            RECON_CLOCK_ZONE_KEY, 0);
        recon_registry_set_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY, minutes);
        char there[32];
        recon_clock_short(there, sizeof(there));
        recon_registry_set_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY, here);

        draw_row(cp, p, x, y + row * ROW_HEIGHT, lw, row, z.name, there,
            cp->zone_scroll + row == current);
        recon_hit_add(p, x, y + row * ROW_HEIGHT, lw, ROW_HEIGHT,
            HIT_ZONE_BASE + (uint32_t)row);
    }

    y += cp->list_h + 6;

    if (y + line <= bottom) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Nothing here follows daylight saving. The offset is what is "
            "kept and what is applied.", COLOR_DIM);
        y += line;
    }
    if (y + line <= bottom) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "ReconOS reads the host's clock and does not set it. Its own "
            "comes with its own kernel.", COLOR_DIM);
    }
}

/* --- Storage --- */

/*
 * A figure at the right-hand edge of a column.
 *
 * Sizes are read against each other -- is Icons bigger than Skins -- and that
 * comparison is made on the digits, which only line up if the numbers end in
 * the same place. Drawn from the left they do not: "0 bytes" and "262.3 KB"
 * start together and end nowhere near each other.
 */
static void draw_figure(struct control_panel *cp, struct recon_panel *p,
        int right, int baseline, const char *text, recon_color colour) {
    int width = recon_text_width(cp->font, text);
    recon_draw_text(p, cp->font, right - width, baseline, width + 2, text,
        colour);
}

/*
 * A share of the whole, as a bar.
 *
 * The track is the window's own background with a line round it, so it reads
 * as a groove something sits in. Filled white, it read as an empty text field
 * -- a place to type rather than a measurement of nothing.
 *
 * A category holding nothing draws no track at all. An empty groove is a
 * measurement that came back zero, and a row already says "0 bytes" beside
 * it; drawing the same fact twice, once as an outline, makes the page look
 * like it is waiting for something.
 */
static void draw_share(struct recon_panel *p, int x, int y, int w, int h,
        unsigned long long part, unsigned long long whole) {
    if (part == 0 || whole == 0) {
        return;
    }

    recon_fill_rect(p, x, y, w, h, COLOR_BG);
    recon_stroke_rect(p, x, y, w, h, COLOR_SEPARATOR);

    int filled = (int)((part * (unsigned long long)w) / whole);
    if (filled > w) {
        filled = w;      /* The bin, which is counted twice. */
    }
    if (filled < 2) {
        filled = 2;      /* Present, however little. */
    }
    recon_fill_rect(p, x + 1, y + 1, filled - 1, h - 2, THEME(ACCENT));
}

/* A size a person can read, rather than a number of bytes. */
static void storage_size(unsigned long long bytes, char *out, size_t size) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, size, "%.1f GB",
            (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, size, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, size, "%llu bytes", bytes);
    }
}

/* Add one measured folder to the page's list. */
static void storage_add(struct control_panel *cp, const char *label,
        const char *path, const char *detail) {
    if (cp->storage_count >= STORAGE_ROWS_MAX) {
        return;
    }

    unsigned long long bytes = 0;
    int files = 0;
    if (!recon_fs_usage(NULL, path, &bytes, &files)) {
        /*
         * A folder that is not there is left out rather than shown as empty.
         * Zero and absent look the same on a bar chart and are not the same
         * fact, and only one of them is worth a row.
         */
        return;
    }

    int i = cp->storage_count++;
    snprintf(cp->storage[i].label, sizeof(cp->storage[i].label), "%s", label);
    snprintf(cp->storage[i].detail, sizeof(cp->storage[i].detail), "%s",
        detail != NULL ? detail : path);
    cp->storage[i].bytes = bytes;
    cp->storage[i].files = files;
    cp->storage[i].in_total = true;
    cp->storage_total += bytes;
}

/*
 * Walk what ReconOS owns.
 *
 * The accounts are listed one by one rather than as a single /Users row,
 * because "the accounts take most of the room" is not an answer anybody can
 * act on and "this account takes most of the room" is.
 */
/*
 * What is in the space that is showing.
 *
 * One space at a time rather than everything at once, because "how much room
 * have my files taken" and "how much room has the system taken" are different
 * questions with different things to do about them, and a single list mixing
 * them answers neither.
 */
static void measure_storage(struct control_panel *cp) {
    cp->storage_count = 0;
    cp->storage_total = 0;

    if (cp->volume == RECON_VOLUME_SYSTEM) {
        storage_add(cp, "Settings", RECON_DIR_SYSTEM_CONFIG,
            "The registry, the firewall's rules, and what is installed.");
        storage_add(cp, "Skins", RECON_DIR_SYSTEM_THEMES,
            "Every skin, built in or added.");
        storage_add(cp, "Icons", RECON_DIR_SYSTEM_ICONS,
            "Drawn once at first start, and replaceable.");
        storage_add(cp, "Modules", RECON_DIR_SYSTEM_MODULES,
            "Code ReconOS loads: applications and subsystems.");
        storage_add(cp, "Help", RECON_DIR_SYSTEM_APPS,
            "The pages the Help application reads.");
        storage_add(cp, "Logs", RECON_DIR_LOGS,
            "What has happened on this machine, including error codes.");
        storage_add(cp, "Temporary", RECON_DIR_TEMP,
            "Scratch space. Screen captures land here when no path is given.");
    } else if (cp->volume == RECON_VOLUME_PROGRAMS) {
        storage_add(cp, "Installed", RECON_DIR_APPS,
            "Applications, and the packages they arrived in.");
    } else {
        int count = recon_users_count();
        for (int i = 0; i < count; i++) {
            struct recon_user user;
            if (!recon_users_at(i, &user)) {
                continue;
            }

            char path[RECON_PATH_MAX];
            if (!recon_fs_join(path, sizeof(path), RECON_DIR_USERS,
                    user.name)) {
                continue;
            }

            char label[48];
            snprintf(label, sizeof(label), "%s", user.name);
            storage_add(cp, label, path, "This account's own files.");
        }
    }

    /*
     * This space's bin, last, and not counted in the total -- it lives inside
     * the space, so its bytes are already in one of the rows above. It is
     * listed anyway because it is the one row somebody can act on, and a
     * storage page that does not mention the bin is not much of one.
     */
    unsigned long long bytes = 0;
    int files = 0;
    if (recon_fs_trash_usage_in(cp->volume, &bytes, &files) &&
            cp->storage_count < STORAGE_ROWS_MAX) {
        int i = cp->storage_count++;
        snprintf(cp->storage[i].label, sizeof(cp->storage[i].label),
            "Recycle Bin");
        snprintf(cp->storage[i].detail, sizeof(cp->storage[i].detail),
            "Deleted from %s. Counted in the rows above, not again here.",
            recon_volume_name(cp->volume));
        cp->storage[i].bytes = bytes;
        cp->storage[i].files = files;
        cp->storage[i].in_total = false;
    }

    cp->storage_measured = true;
}

static void draw_storage(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Storage",
        "Three spaces, measured separately. How much is left is the host's "
        "to answer.");

    if (!cp->storage_measured) {
        measure_storage(cp);
    }

    /*
     * --- The three spaces ---
     *
     * A selector rather than one list of everything. System, Programs and
     * User are separate spaces with separate bins, and a page that ran them
     * together would be a page that cannot answer either of the two questions
     * anybody brings to it.
     *
     * Each button carries that space's size, so choosing between them does
     * not mean visiting all three to find out which one is the problem.
     */
    /*
     * The same tab bar Appearance and Network use. These were buttons, which
     * is not what they do: a button acts, and choosing which of three spaces
     * you are looking at is choosing a view. Four pages now pick a section
     * one way.
     */
    char labels[RECON_VOLUME_COUNT][64];
    const char *names[RECON_VOLUME_COUNT];
    for (int i = 0; i < RECON_VOLUME_COUNT; i++) {
        unsigned long long used = 0;
        recon_volume_usage((enum recon_volume)i, &used, NULL);

        char size[32];
        storage_size(used, size, sizeof(size));
        snprintf(labels[i], sizeof(labels[i]), "%s  %s",
            recon_volume_name((enum recon_volume)i), size);
        names[i] = labels[i];
    }

    y = draw_tabs(cp, p, x, y, w, names, RECON_VOLUME_COUNT, (int)cp->volume,
        HIT_VOLUME_BASE);

    /*
     * The space's own total, not the sum of the rows.
     *
     * They are not the same number and it would be dishonest to print the
     * smaller one beside a button showing the larger: the rows are the parts
     * worth naming, and a space holds things that are not worth a row. The
     * button and this line now say the same thing because they ask the same
     * question.
     */
    unsigned long long used = 0;
    recon_volume_usage(cp->volume, &used, NULL);

    char total[32];
    storage_size(used, total, sizeof(total));

    char summary[160];
    snprintf(summary, sizeof(summary), "%s: %s. %s",
        recon_volume_name(cp->volume), total,
        recon_volume_detail(cp->volume));
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, summary,
        COLOR_TEXT);
    y += line + 8;

    /* And say so where the rows visibly do not add up to it. */
    if (cp->storage_total < used) {
        char rest[64];
        storage_size(used - cp->storage_total, rest, sizeof(rest));
        char note[128];
        snprintf(note, sizeof(note),
            "The parts below account for all but %s of it.", rest);
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, note,
            COLOR_DIM);
        y += line + 4;
    }

    /*
     * The rows.
     *
     * The figures are drawn from their right-hand edge now, so the column
     * they sit in only has to be as wide as the widest of them rather than as
     * wide as the widest one plus wherever it started. What that gave back
     * went to the descriptions, one of which was being cut off.
     */
    int bar_w = w / 5;
    int size_w = 76;
    int row_h = line * 2 + 8;

    for (int i = 0; i < cp->storage_count; i++) {
        if (y + row_h > h - BUTTON_HEIGHT - PADDING * 2) {
            break;
        }

        if (i % 2 == 1) {
            recon_fill_rect(p, x, y, w, row_h, COLOR_ROW_ALT);
        }

        char size[32];
        storage_size(cp->storage[i].bytes, size, sizeof(size));

        char count[48];
        snprintf(count, sizeof(count), "%d file%s", cp->storage[i].files,
            cp->storage[i].files == 1 ? "" : "s");

        int label_w = w - bar_w - size_w - 32;
        recon_draw_text(p, cp->font, x + 8, y + 4 + ascent,
            label_w, cp->storage[i].label, COLOR_TEXT);
        recon_draw_text(p, cp->font, x + 8, y + 4 + line + ascent,
            label_w, cp->storage[i].detail, COLOR_DIM);

        /*
         * A share of the whole rather than of a disk. Without a volume layer
         * there is no capacity to be a fraction of, and drawing one anyway
         * would be inventing the number the page opens by saying it does not
         * have.
         */
        int bar_x = x + w - bar_w - size_w - 16;
        draw_share(p, bar_x, y + 6, bar_w, line, cp->storage[i].bytes,
            cp->storage_total);

        draw_figure(cp, p, x + w - 10, y + 4 + ascent, size, COLOR_TEXT);
        draw_figure(cp, p, x + w - 10, y + 4 + line + ascent, count,
            COLOR_DIM);

        y += row_h + 2;
    }

    y += PADDING;

    int bx = draw_button(cp, p, x, y, "Measure Again",
        HIT_ACTION_BASE + ACTION_MEASURE_STORAGE, true);
    char empty_label[64];
    snprintf(empty_label, sizeof(empty_label), "Empty %s Bin",
        recon_volume_name(cp->volume));
    draw_button(cp, p, bx, y, empty_label,
        HIT_ACTION_BASE + ACTION_EMPTY_BIN,
        recon_fs_trash_count_in(cp->volume) > 0);
    y += BUTTON_HEIGHT + PADDING * 2;

    /*
     * What this page still cannot do, kept in sight.
     *
     * The page used to be nothing but this list. Now that most of it works,
     * the temptation is to drop the remainder -- and a gap nobody can see is
     * a gap nobody remembers. Drawn smaller than the rows above, because it
     * is a note about the system rather than a control.
     */
    if (y + line * 2 > h) {
        return;
    }

    recon_fill_rect(p, x, y, w, 1, COLOR_SEPARATOR);
    y += PADDING;

    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "Not built yet:", COLOR_DIM);
    y += line + 4;

    int count = (int)(sizeof(STORAGE_ITEMS) / sizeof(STORAGE_ITEMS[0]));
    for (int i = 0; i < count && y + line <= h; i++) {
        char text[192];
        snprintf(text, sizeof(text), "%s -- %s", STORAGE_ITEMS[i].label,
            STORAGE_ITEMS[i].blocked);
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, text,
            COLOR_DIM);
        y += line + 2;
    }
}

/*
 * What version this is, and what it brought.
 *
 * The page was three notes about things that do not work. One of them --
 * "the history exists in the repository; nothing brings it into the system"
 * -- stopped being true when the change log arrived, and a page that keeps
 * saying so after the fact is worse than one that never said it.
 */
/* --- Disk Cleanup --- */

/*
 * How much one category is holding.
 *
 * The bin is asked its own way, because a bin is not a folder to be swept:
 * emptying it also clears the origin notes beside the files, and measuring
 * the files folder alone would report a size that emptying does not free all
 * of.
 */
static bool category_covers(const struct clean_category *item,
        const struct recon_dirent *entry) {
    if (entry->kind != RECON_FILE_REGULAR) {
        return false;
    }

    size_t name_len = strlen(entry->name);

    if (item->pattern != NULL) {
        size_t len = strlen(item->pattern);
        if (name_len < len ||
                strcasecmp(entry->name + name_len - len, item->pattern) != 0) {
            return false;
        }
    }

    if (item->except != NULL) {
        size_t len = strlen(item->except);
        if (name_len >= len &&
                strcasecmp(entry->name + name_len - len, item->except) == 0) {
            return false;
        }
    }

    return true;
}

static void measure_category(struct control_panel *cp,
        const struct clean_category *item, unsigned long long *bytes_out,
        int *files_out) {
    *bytes_out = 0;
    *files_out = 0;

    if (item->is_bin) {
        recon_fs_trash_usage_in(cp->volume, bytes_out, files_out);
        return;
    }

    struct recon_dirent entries[256];
    int count = recon_fs_list("/", item->path, entries, 256);
    if (count < 0) {
        return;
    }
    if (count > 256) {
        count = 256;
    }

    for (int i = 0; i < count; i++) {
        if (!category_covers(item, &entries[i])) {
            continue;
        }
        *bytes_out += entries[i].size;
        (*files_out)++;
    }
}

static void measure_cleanup(struct control_panel *cp) {
    int count = CLEAN[cp->volume].count;
    for (int i = 0; i < count && i < CLEAN_MAX; i++) {
        measure_category(cp, &CLEAN[cp->volume].items[i],
            &cp->clean[i].bytes, &cp->clean[i].files);
    }
    cp->clean_measured = true;
}

static void draw_cleanup(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;
    y = draw_heading(cp, p, x, y, w, "Disk Cleanup",
        "What can be freed, and what it costs to free it.");

    if (!cp->clean_measured) {
        measure_cleanup(cp);
    }

    /* The same three spaces as Storage, chosen the same way -- and now
     * drawn the same way too. */
    const char *names[RECON_VOLUME_COUNT];
    for (int i = 0; i < RECON_VOLUME_COUNT; i++) {
        names[i] = recon_volume_name((enum recon_volume)i);
    }

    y = draw_tabs(cp, p, x, y, w, names, RECON_VOLUME_COUNT, (int)cp->volume,
        HIT_VOLUME_BASE);

    int count = CLEAN[cp->volume].count;
    unsigned long long chosen_bytes = 0;
    int chosen_rows = 0;
    for (int i = 0; i < count && i < CLEAN_MAX; i++) {
        if (cp->clean[i].ticked) {
            chosen_bytes += cp->clean[i].bytes;
            chosen_rows++;
        }
    }

    char picked[96];
    if (chosen_rows == 0) {
        snprintf(picked, sizeof(picked), "Nothing ticked.");
    } else {
        char size[32];
        storage_size(chosen_bytes, size, sizeof(size));
        snprintf(picked, sizeof(picked), "%s would be freed.", size);
    }
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, picked,
        chosen_rows > 0 ? COLOR_TEXT : COLOR_DIM);
    y += line + 6;

    /* The rows: a box, what it is, what it costs, and how much. */
    int box = 13;
    int row_h = line * 2 + 10;

    for (int i = 0; i < count && i < CLEAN_MAX; i++) {
        if (y + row_h > bottom - BUTTON_HEIGHT - PADDING * 2) {
            break;
        }

        const struct clean_category *item = &CLEAN[cp->volume].items[i];
        bool selected = cp->selected == i;

        if (selected) {
            recon_fill_rect(p, x, y, w, row_h, COLOR_SELECTED);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, y, w, row_h, COLOR_ROW_ALT);
        }

        recon_color ink = selected ? COLOR_SELECTED_TEXT : COLOR_TEXT;
        recon_color faint = selected ? COLOR_SELECTED_TEXT : COLOR_DIM;

        /* The box. Drawn rather than a glyph, because a tick has to read at
         * this size and the font's is not reliable across skins. */
        int by = y + (row_h - box) / 2;
        recon_fill_rect(p, x + 8, by, box, box, COLOR_PANEL);
        recon_stroke_rect(p, x + 8, by, box, box, COLOR_SEPARATOR);
        if (cp->clean[i].ticked) {
            recon_fill_rect(p, x + 11, by + 3, box - 6, box - 6, COLOR_TEXT);
        }

        recon_draw_text(p, cp->font, x + 30, y + 5 + ascent, 260, item->label,
            ink);
        recon_draw_text(p, cp->font, x + 30, y + 5 + ascent + line,
            w - 160, item->detail, faint);

        char size[32];
        storage_size(cp->clean[i].bytes, size, sizeof(size));

        char files[32];
        snprintf(files, sizeof(files), "%d file%s", cp->clean[i].files,
            cp->clean[i].files == 1 ? "" : "s");

        /* Ended in the same place, so what one row costs can be compared
         * against another by looking rather than by reading. */
        draw_figure(cp, p, x + w - 10, y + 5 + ascent, size, ink);
        draw_figure(cp, p, x + w - 10, y + 5 + ascent + line, files, faint);

        recon_hit_add(p, x, y, w, row_h, HIT_CLEAN_BASE + i);
        y += row_h + 2;
    }

    y += PADDING;

    /* What ticking the chosen row costs, in words, above the button that
     * would do it. A size is not a consequence. */
    if (cp->selected >= 0 && cp->selected < count) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            CLEAN[cp->volume].items[cp->selected].cost, COLOR_DIM);
        y += line + 4;
    }

    int bx = draw_button(cp, p, x, y, "Clean Up",
        HIT_ACTION_BASE + ACTION_CLEAN_NOW, chosen_rows > 0);
    bx = draw_button(cp, p, bx, y, "View Files",
        HIT_ACTION_BASE + ACTION_CLEAN_VIEW,
        cp->selected >= 0 && cp->selected < count &&
        !CLEAN[cp->volume].items[cp->selected].is_bin);
    draw_button(cp, p, bx, y, "Clean Up System Files",
        HIT_ACTION_BASE + ACTION_CLEAN_SYSTEM_FILES, true);
    y += BUTTON_HEIGHT + PADDING;

    if (y + line <= bottom) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "Ticking a row does nothing until Clean Up is pressed, and Clean "
            "Up asks first.", COLOR_DIM);
    }
}

static void draw_update(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Update",
        "What is running, and what it brought.");

    char version[64];
    snprintf(version, sizeof(version), "ReconOS v%s", RECONOS_VERSION);
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, version,
        THEME(ACCENT));
    y += line + 4;

    /*
     * The version this system last *started* as. The same when nothing has
     * changed, and different only in the moment between an update landing and
     * the first start after it -- which is exactly when somebody would want
     * to know.
     */
    const char *installed =
        recon_registry_get(RECON_REG_SYSTEM, "system/installed-version", "");

    char note[192];
    if (installed[0] == '\0') {
        snprintf(note, sizeof(note),
            "This system has not recorded a version yet.");
    } else if (strcmp(installed, RECONOS_VERSION) == 0) {
        snprintf(note, sizeof(note),
            "Installed and running the same version. Nothing is pending.");
    } else {
        snprintf(note, sizeof(note),
            "Last started as v%s. The next start will finish the update.",
            installed);
    }
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, note, COLOR_DIM);
    y += line + PADDING;

    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "What changed in this version:", COLOR_TEXT);
    y += line + 4;

    const char *changes = recon_help_current_changes();
    if (changes == NULL || changes[0] == '\0') {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "The change log says nothing about this version.", COLOR_DIM);
        y += line;
    } else {
        /*
         * The first few lines only, with a button to the rest. This is a page
         * about the state of the system, not a place to read a document in --
         * Help is that, and sending somebody there is better than growing a
         * second reader here that would then have to be kept in step.
         */
        int reserved = BUTTON_HEIGHT + PADDING * 4 + line * 2 +
            (int)(sizeof(UPDATE_ITEMS) / sizeof(UPDATE_ITEMS[0])) * (line + 2);
        int room = (h - y - reserved) / line;

        /*
         * Eight lines at most, whatever the window's height. This is a
         * summary with a way to the whole thing, and a summary that grows
         * until it fills the page is not a summary -- it is the document,
         * shown in the wrong reader, pushing what is under it off the end.
         */
        if (room > 8) {
            room = 8;
        }
        if (room < 1) {
            room = 1;
        }

        /*
         * The source lines as written, not re-wrapped. Help is the reader;
         * this is a look at the first few lines of what it holds, and
         * building a second wrapper here would be a second thing to keep in
         * step with the first.
         */
        char buffer[2048];
        recon_text_copy(buffer, sizeof(buffer), changes);

        int shown = 0;
        char *save = NULL;
        for (char *at = strtok_r(buffer, "\n", &save);
                at != NULL && shown < room;
                at = strtok_r(NULL, "\n", &save)) {
            if (*at == '\0') {
                continue;
            }
            recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, at,
                COLOR_DIM);
            y += line;
            shown++;
        }
    }

    y += PADDING;
    draw_button(cp, p, x, y, "All Changes",
        HIT_ACTION_BASE + ACTION_SHOW_CHANGES, true);
    y += BUTTON_HEIGHT + PADDING * 2;

    if (y + line * 2 > h) {
        return;
    }

    recon_fill_rect(p, x, y, w, 1, COLOR_SEPARATOR);
    y += PADDING;

    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, "Not built yet:",
        COLOR_DIM);
    y += line + 4;

    int count = (int)(sizeof(UPDATE_ITEMS) / sizeof(UPDATE_ITEMS[0]));
    for (int i = 0; i < count && y + line <= h; i++) {
        char text[192];
        snprintf(text, sizeof(text), "%s -- %s", UPDATE_ITEMS[i].label,
            UPDATE_ITEMS[i].blocked);
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, text,
            COLOR_DIM);
        y += line + 2;
    }
}

/* --- The firewall --- */

/*
 * What may open and what may be opened.
 *
 * The shape most systems have, because that is the shape people already know:
 * a switch, a default per direction, and a numbered list where the first
 * match decides. The numbers are shown because the order is part of the rule
 * and a list whose order matters has to say what the order is.
 */
/*
 * --- Adding a rule ---
 *
 * Presets first. Most people adding a firewall rule want one of nine things,
 * and asking for a protocol and a port range before finding that out is
 * asking somebody to know the answer before they have been offered it.
 *
 * "Something else" is under them, for the rest.
 */
static void draw_firewall_add(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;

    if (cp->fw_custom) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "A rule of your own. The first rule that matches decides, and a "
            "new one goes at the end.", COLOR_DIM);
        y += line + PADDING;

        recon_draw_text(p, cp->font, x + 8, y + ascent, 120, "Name",
            COLOR_TEXT);
        recon_edit_draw(p, cp->font, x + 120, y, 240, FIELD_HEIGHT,
            &cp->fw_name);
        recon_hit_add(p, x + 120, y, 240, FIELD_HEIGHT, HIT_FIELD_BASE + 8);
        y += FIELD_HEIGHT + 8;

        recon_draw_text(p, cp->font, x + 8, y + ascent, 120,
            "Port, or a range", COLOR_TEXT);
        recon_edit_draw(p, cp->font, x + 120, y, 120, FIELD_HEIGHT,
            &cp->fw_port);
        recon_hit_add(p, x + 120, y, 120, FIELD_HEIGHT, HIT_FIELD_BASE + 9);
        recon_draw_text(p, cp->font, x + 250, y + ascent, w - 258,
            "80, or 27015-27030. Empty means every port.", COLOR_DIM);
        y += FIELD_HEIGHT + PADDING;

        /*
         * Three buttons that cycle rather than three sets of radio buttons.
         * Each has two or three values and the label says which one it is on,
         * so the control and its readout are the same object.
         */
        char label[64];
        snprintf(label, sizeof(label), "Direction: %s",
            recon_fw_direction_name(cp->fw_direction));
        int bx = draw_button(cp, p, x + 8, y, label,
            HIT_ACTION_BASE + ACTION_FIREWALL_CUSTOM_DIRECTION, true);

        snprintf(label, sizeof(label), "Protocol: %s",
            recon_fw_protocol_name(cp->fw_protocol));
        bx = draw_button(cp, p, bx, y, label,
            HIT_ACTION_BASE + ACTION_FIREWALL_CUSTOM_PROTOCOL, true);

        snprintf(label, sizeof(label), "Then: %s",
            recon_fw_action_name(cp->fw_action));
        draw_button(cp, p, bx, y, label,
            HIT_ACTION_BASE + ACTION_FIREWALL_CUSTOM_ACTION, true);
        y += BUTTON_HEIGHT + PADDING;

        bx = draw_button(cp, p, x + 8, y, "Add Rule",
            HIT_ACTION_BASE + ACTION_FIREWALL_CUSTOM_CONFIRM,
            cp->fw_name.text[0] != '\0');
        draw_button(cp, p, bx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_FIREWALL_ADD_CANCEL, true);
        return;
    }

    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "Pick one, or make your own. A new rule goes at the end of the list "
        "and starts switched on.", COLOR_DIM);
    y += line + PADDING;

    int rows = (bottom - y - BUTTON_HEIGHT - PADDING) / ROW_HEIGHT;
    if (rows > FW_PRESET_COUNT) {
        rows = FW_PRESET_COUNT;
    }
    if (rows < 0) {
        rows = 0;
    }

    recon_fill_rect(p, x, y, w, rows * ROW_HEIGHT, COLOR_PANEL);

    for (int i = 0; i < rows; i++) {
        const struct fw_preset *preset = &FW_PRESETS[i];
        int ry = y + i * ROW_HEIGHT;

        if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        /* What it does, in the same shorthand the rule list uses, so the row
         * somebody picks here is recognisable in the list afterwards. */
        char shape[64];
        if (preset->port_from == 0 && preset->port_to == 0) {
            snprintf(shape, sizeof(shape), "%s  %s  any port",
                recon_fw_action_name(preset->action),
                recon_fw_direction_name(preset->direction));
        } else if (preset->port_from == preset->port_to) {
            snprintf(shape, sizeof(shape), "%s  %s  %s %d",
                recon_fw_action_name(preset->action),
                recon_fw_direction_name(preset->direction),
                recon_fw_protocol_name(preset->protocol), preset->port_from);
        } else {
            snprintf(shape, sizeof(shape), "%s  %s  %s %d-%d",
                recon_fw_action_name(preset->action),
                recon_fw_direction_name(preset->direction),
                recon_fw_protocol_name(preset->protocol), preset->port_from,
                preset->port_to);
        }

        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
        recon_draw_text(p, cp->font, x + 10, baseline, 170, preset->name,
            COLOR_TEXT);
        recon_draw_text(p, cp->font, x + 185, baseline, 190, shape,
            preset->action == RECON_FW_BLOCK ? COLOR_WARNING : COLOR_DIM);
        recon_draw_text(p, cp->font, x + 380, baseline, w - 388,
            preset->detail, COLOR_DIM);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_PRESET_BASE + i);
    }

    y += rows * ROW_HEIGHT + PADDING;

    int bx = draw_button(cp, p, x + 8, y, "Something Else",
        HIT_ACTION_BASE + ACTION_FIREWALL_ADD_CUSTOM, true);
    draw_button(cp, p, bx, y, "Cancel",
        HIT_ACTION_BASE + ACTION_FIREWALL_ADD_CANCEL, true);
}

static void draw_firewall(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;

    y = draw_heading(cp, p, x, y, w, "Firewall",
        cp->fw_adding
            ? "A new rule. It goes at the end, and the first rule that "
              "matches decides."
            : "What ReconOS may open, and what may be opened to it. Not the "
              "host's firewall.");

    if (cp->fw_adding) {
        draw_firewall_add(cp, p, x, y, w, bottom - y);
        return;
    }

    bool on = recon_firewall_is_on();

    /* The switch, and what happens when nothing matches. */
    char state[96];
    snprintf(state, sizeof(state), "The firewall is %s.", on ? "on" : "off");
    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16, state,
        on ? COLOR_TEXT : COLOR_WARNING);
    y += line + 4;

    if (!on) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "Nothing below is being enforced while it is off.", COLOR_DIM);
        y += line;
    }
    y += PADDING;

    int bx = draw_button(cp, p, x, y, on ? "Turn Off" : "Turn On",
        HIT_ACTION_BASE + ACTION_FIREWALL_TOGGLE, true);

    char in_label[64];
    char out_label[64];
    snprintf(in_label, sizeof(in_label), "Incoming: %s",
        recon_fw_action_name(recon_firewall_default(RECON_FW_IN)));
    snprintf(out_label, sizeof(out_label), "Outgoing: %s",
        recon_fw_action_name(recon_firewall_default(RECON_FW_OUT)));

    bx = draw_button(cp, p, bx, y, in_label,
        HIT_ACTION_BASE + ACTION_FIREWALL_DEFAULT_IN, on);
    draw_button(cp, p, bx, y, out_label,
        HIT_ACTION_BASE + ACTION_FIREWALL_DEFAULT_OUT, on);

    y += BUTTON_HEIGHT + PADDING;

    recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
        "Rules, in the order they are consulted. The first one that matches "
        "decides.", COLOR_DIM);
    y += line + 4;

    /* Room for the buttons under the list, always, so choosing the last rule
     * does not push the controls off the page. */
    int footer = BUTTON_HEIGHT + PADDING * 2 + line;
    int count = recon_firewall_count();
    int rows = (h - y - footer) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > count) {
        rows = count;
    }

    if (count > 0 && cp->selected >= count) {
        cp->selected = count - 1;
    }

    /*
     * Keep the chosen rule in view, but only when the choice has just moved.
     *
     * Following it on every draw meant the wheel could not go anywhere: the
     * selection sat at the top, the list was dragged straight back to it, and
     * scrolling did nothing at all. Following it only when it changes lets
     * Move Up and Move Down carry the list with them -- which is what that
     * rule was for -- without pinning it the rest of the time.
     */
    clamp_scroll(&cp->fw_scroll, rows, count);
    if (cp->selected != cp->fw_followed) {
        if (cp->selected < cp->fw_scroll) {
            cp->fw_scroll = cp->selected;
        }
        if (cp->selected >= cp->fw_scroll + rows) {
            cp->fw_scroll = cp->selected - rows + 1;
        }
        cp->fw_followed = cp->selected;
        clamp_scroll(&cp->fw_scroll, rows, count);
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->fw_scroll, rows, count);
    w -= bar;

    for (int row = 0; row < rows; row++) {
        int i = cp->fw_scroll + row;
        if (i >= count) {
            break;
        }

        struct recon_fw_rule rule;
        if (!recon_firewall_at(i, &rule)) {
            break;
        }

        /* Where it sits on screen, not where it sits in the list. */
        int ry = y + row * ROW_HEIGHT;
        bool chosen = (i == cp->selected);

        if (chosen) {
            recon_fill_role(p, x, ry, w, ROW_HEIGHT, RECON_THEME_SELECTION);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color ink = chosen ? THEME(SELECTION_TEXT) : COLOR_TEXT;
        recon_color faint = chosen ? THEME(SELECTION_TEXT) : COLOR_DIM;

        /*
         * A rule that is off is drawn dim whether or not it is selected: its
         * being written down and not in force is the single most important
         * thing about it, and a row that looks the same either way is a row
         * that gets misread.
         */
        if (!rule.enabled) {
            ink = faint;
        }

        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;

        /* Wide enough for any rule number the list can hold, so the compiler
         * can see it will fit as well as the reader. */
        char number[16];
        snprintf(number, sizeof(number), "%d", i + 1);
        recon_draw_text(p, cp->font, x + 8, baseline, 24, number, faint);

        recon_draw_text(p, cp->font, x + 34, baseline, 34,
            rule.enabled ? "on" : "off", faint);

        recon_draw_text(p, cp->font, x + 70, baseline, 44,
            recon_fw_action_name(rule.action),
            rule.action == RECON_FW_BLOCK && rule.enabled
                ? COLOR_WARNING : ink);

        recon_draw_text(p, cp->font, x + 118, baseline, 30,
            recon_fw_direction_name(rule.direction), faint);

        char ports[48];
        if (rule.port_from == 0 && rule.port_to == 0) {
            recon_text_copy(ports, sizeof(ports), "any");
        } else if (rule.port_from == rule.port_to) {
            snprintf(ports, sizeof(ports), "%d", rule.port_from);
        } else {
            snprintf(ports, sizeof(ports), "%d-%d", rule.port_from,
                rule.port_to);
        }
        recon_draw_text(p, cp->font, x + 152, baseline, 70, ports, faint);

        recon_draw_text(p, cp->font, x + 226, baseline, 36,
            recon_fw_protocol_name(rule.protocol), faint);

        recon_draw_text(p, cp->font, x + 268, baseline, w - 276, rule.name,
            ink);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + row);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10,
            y + (ROW_HEIGHT + ascent) / 2 - 2, w - 20,
            "No rules. The defaults above decide everything.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    struct recon_fw_rule chosen;
    bool have = recon_firewall_at(cp->selected, &chosen);

    bx = draw_button(cp, p, x, y,
        have && chosen.enabled ? "Turn Rule Off" : "Turn Rule On",
        HIT_ACTION_BASE + ACTION_FIREWALL_RULE_TOGGLE, have && on);
    bx = draw_button(cp, p, bx, y,
        have && chosen.action == RECON_FW_ALLOW ? "Make It Block"
                                                : "Make It Allow",
        HIT_ACTION_BASE + ACTION_FIREWALL_RULE_ACTION, have && on);
    bx = draw_button(cp, p, bx, y, "Move Up",
        HIT_ACTION_BASE + ACTION_FIREWALL_RULE_UP,
        have && cp->selected > 0);
    draw_button(cp, p, bx, y, "Move Down",
        HIT_ACTION_BASE + ACTION_FIREWALL_RULE_DOWN,
        have && cp->selected < count - 1);

    y += BUTTON_HEIGHT + PADDING;

    bx = draw_button(cp, p, x, y, "Add Rule",
        HIT_ACTION_BASE + ACTION_FIREWALL_ADD, on);
    /* Greyed for a rule that ships: it can be turned off but not removed,
     * and a button that refuses when pressed is worse than one that shows it
     * will. */
    draw_button(cp, p, bx, y, "Remove Rule",
        HIT_ACTION_BASE + ACTION_FIREWALL_REMOVE,
        have && on && !recon_firewall_is_built_in(&chosen));

    y += BUTTON_HEIGHT + PADDING;

    if (y + line <= bottom) {
        recon_draw_text(p, cp->font, x + 8, y + ascent, w - 16,
            "The rules are a text file in /System/Config, and 'firewall' in "
            "the Terminal is the same list.", COLOR_DIM);
    }
}

/* What is installed, and what could be done about it. */
/*
 * Whether an application is one that ships.
 *
 * A system application has no module behind it -- it is compiled into
 * ReconOS. One that arrived as a `.rex` names the module it came from, and
 * that name is also how it is removed.
 */
static bool app_is_system(const struct recon_installed_app *app) {
    return app->module[0] == '\0';
}

/* How many are in the list showing. */
static int programs_in_tab(struct control_panel *cp,
        struct recon_installed_app *out, int max) {
    int total = recon_installed_app_count();
    int found = 0;

    for (int i = 0; i < total && found < max; i++) {
        struct recon_installed_app app;
        if (!recon_installed_app_at(i, &app)) {
            continue;
        }
        bool system = app_is_system(&app);
        if ((cp->programs == PROGRAMS_SYSTEM) != system) {
            continue;
        }
        out[found++] = app;
    }
    return found;
}

/* Whichever row is chosen, from the list that is showing. */
static bool program_selected(struct control_panel *cp,
        struct recon_installed_app *out) {
    struct recon_installed_app apps[64];
    int count = programs_in_tab(cp, apps, 64);
    if (cp->selected < 0 || cp->selected >= count) {
        return false;
    }
    *out = apps[cp->selected];
    return true;
}

static void draw_programs(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;

    y = draw_heading(cp, p, x, y, w,
        cp->programs == PROGRAMS_SYSTEM ? "System Apps" : "Programs",
        cp->programs == PROGRAMS_SYSTEM
            ? "What ships with ReconOS. These cannot be removed."
            : "What has been installed here. Applications come from modules "
              "in /Apps.");

    /* The two lists. */
    int bx = x;
    for (int i = 0; i < PROGRAMS_TABS; i++) {
        const char *label = PROGRAMS_TAB_NAMES[i];
        int width = recon_text_width(cp->font, label) + 28;
        bool on = cp->programs == (enum programs_tab)i;

        recon_fill_rect(p, bx, y, width, SECTION_HEIGHT,
            on ? COLOR_PANEL : COLOR_BG);
        recon_stroke_rect(p, bx, y, width, SECTION_HEIGHT, COLOR_SEPARATOR);

        int tw = recon_text_width(cp->font, label);
        recon_draw_text(p, cp->font, bx + (width - tw) / 2,
            y + (SECTION_HEIGHT + ascent) / 2 - 2, width - 8, label,
            on ? COLOR_TEXT : COLOR_DIM);

        recon_hit_add(p, bx, y, width, SECTION_HEIGHT,
            HIT_PROGRAMS_TAB_BASE + i);
        bx += width;
    }
    recon_fill_rect(p, x, y + SECTION_HEIGHT - 1, w, 1, COLOR_SEPARATOR);
    y += SECTION_HEIGHT + PADDING;

    struct recon_installed_app apps[64];
    int count = programs_in_tab(cp, apps, 64);

    int footer = BUTTON_HEIGHT + PADDING * 2 + line;
    int rows = (bottom - y - footer) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > count) {
        rows = count;
    }

    clamp_scroll(&cp->programs_scroll, rows, count);

    if (count > 0 && cp->selected >= count) {
        cp->selected = count - 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->programs_scroll, rows, count);

    for (int row = 0; row < rows; row++) {
        int i = cp->programs_scroll + row;
        if (i >= count) {
            break;
        }
        /* Turned off said on the row, because this is the only list that
         * still shows one and so the only place the state can be seen. */
        char detail[128];
        snprintf(detail, sizeof(detail), "%s%s",
            app_is_system(&apps[i]) ? "built into ReconOS" : apps[i].module,
            apps[i].disabled ? "   turned off" : "");

        draw_row(cp, p, x, y + row * ROW_HEIGHT, w - bar, row, apps[i].name,
            detail, i == cp->selected);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, cp->programs == PROGRAMS_SYSTEM
                ? "Nothing built in is listed here."
                : "Nothing has been installed yet.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    bool admin = recon_users_may_administer();

    /*
     * Installing is typing a path. A module runs inside ReconOS with
     * everything ReconOS can do, so it is an administrator's decision --
     * closer to installing a driver than to saving a file.
     */
    if (cp->installing) {
        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->name);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE);
        y += FIELD_HEIGHT + 8;

        int ix = draw_button(cp, p, x, y, "Install",
            HIT_ACTION_BASE + ACTION_CONFIRM_INSTALL, true);
        draw_button(cp, p, ix, y, "Cancel",
            HIT_ACTION_BASE + ACTION_NONE, true);
        return;
    }

    bool have = cp->selected >= 0 && cp->selected < count;

    if (cp->programs == PROGRAMS_SYSTEM) {
        /*
         * Repair and Disable, which are the two things worth doing to
         * something that cannot be removed.
         *
         * The button says which way it goes rather than sitting there
         * meaning both. "Disable" over something already off is a button
         * whose label is a lie about what pressing it does.
         */
        struct recon_installed_app chosen;
        bool off = have && program_selected(cp, &chosen) && chosen.disabled;

        int sx = draw_button(cp, p, x, y, "Repair",
            HIT_ACTION_BASE + ACTION_REPAIR_PROGRAM, admin && have && !off);
        draw_button(cp, p, sx, y, off ? "Turn On" : "Disable",
            HIT_ACTION_BASE + ACTION_DISABLE_PROGRAM, admin && have);
        y += BUTTON_HEIGHT + PADDING;

        if (y + line <= bottom) {
            recon_draw_text(p, cp->font, x, y + ascent, w,
                "A system application cannot be removed. Disable stops it "
                "being offered anywhere, and survives a restart.", COLOR_DIM);
            y += line;
        }
        if (y + line <= bottom) {
            recon_draw_text(p, cp->font, x, y + ascent, w,
                "Repair checks that a built-in is registered as it should "
                "be. It is compiled in, so there are no files to put back.",
                COLOR_DIM);
        }
        return;
    }

    int ix = draw_button(cp, p, x, y, "Install a Program",
        HIT_ACTION_BASE + ACTION_INSTALL_PROGRAM, admin);
    ix = draw_button(cp, p, ix, y, "Repair",
        HIT_ACTION_BASE + ACTION_REPAIR_PROGRAM, admin && have);
    draw_button(cp, p, ix, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_PROGRAM, admin && have);
    y += BUTTON_HEIGHT + PADDING;

    if (y + line <= bottom) {
        recon_draw_text(p, cp->font, x, y + ascent, w, admin
            ? "Installing takes the path of a .rex module."
            : "Only an administrator can install or remove a program.",
            COLOR_DIM);
    }
}

static void draw_modules(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Modules",
        "Code loaded into the running system: .rts for the system, .rex for "
        "an application.");

    int count = recon_modules_count();
    int rows = (h - y - BUTTON_HEIGHT - PADDING * 3) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > count) {
        rows = count;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    if (count == 0) {
        int ascent = recon_font_ascent(cp->font);
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "Nothing is loaded.", COLOR_DIM);
    }

    for (int i = 0; i < rows; i++) {
        struct recon_module_state module;
        if (!recon_modules_at(i, &module)) {
            break;
        }

        /* A module that refused to load says why, in the place where a
         * working one says what it is. A silent failure is the thing a
         * module list exists to prevent. */
        /* Long enough for the longer of the two things it shows: the reason
         * a module refused to load, which is a sentence rather than a
         * version number. */
        char detail[RECON_MODULES_PROBLEM_MAX + 32];
        snprintf(detail, sizeof(detail), "%s   %s",
            module.is_app ? "application" : "system",
            module.loaded ? module.version : module.problem);

        draw_row(cp, p, x, y + i * ROW_HEIGHT, w, i, module.name, detail,
            i == cp->selected);
    }

    y += cp->list_h + PADDING;

    struct recon_module_state chosen;
    bool have = recon_modules_at(cp->selected, &chosen);
    bool admin = recon_users_may_administer();

    int bx = draw_button(cp, p, x, y, "Load from /Apps",
        HIT_ACTION_BASE + ACTION_LOAD_MODULE, admin);
    draw_button(cp, p, bx, y, "Unload",
        HIT_ACTION_BASE + ACTION_UNLOAD_MODULE,
        admin && have && chosen.loaded);
}

/*
 * The network, as ReconOS can see it.
 *
 * Everything here is read from the host, and the page says so at the bottom
 * rather than letting a list of addresses imply that ReconOS is doing the
 * networking. It is not; see include/recon_net.h.
 */
/* Written for System Information, below; Network asks the same question of
 * a chosen adapter and gets to ask it the same way. */
static void info_row(struct control_panel *cp, struct recon_panel *p,
    int x, int *y, int w, const char *label, const char *value);
static int info_group(struct control_panel *cp, struct recon_panel *p,
    int x, int y, int w, const char *title);

/*
 * How many rows fit between here and the buttons at the foot, and where the
 * list starts. Shared by the sections that show one, so the three of them
 * cannot drift apart about how tall a list is.
 */
static int net_rows(struct control_panel *cp, int y, int h, int reserve) {
    (void)cp;
    int rows = (h - y - reserve) / ROW_HEIGHT;
    return rows < 1 ? 1 : rows;
}

/* Every interface, real ones first: loopback is real and is never the answer
 * to "am I connected". */
static bool net_interface_in_order(int index, struct recon_net_interface *out) {
    int count = recon_net_interface_count();
    int seen = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            struct recon_net_interface interface;
            if (!recon_net_interface_at(i, &interface)) {
                continue;
            }
            if (interface.loopback != (pass == 1)) {
                continue;
            }
            if (seen++ == index) {
                *out = interface;
                return true;
            }
        }
    }
    return false;
}

/* --- Network: Status --- */

static void draw_net_status(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    char summary[192];
    snprintf(summary, sizeof(summary), "Called          %s",
        recon_net_machine_name());
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
    y += line + 2;

    const char *gateway = recon_net_gateway();
    snprintf(summary, sizeof(summary), "Gateway         %s",
        gateway[0] != '\0' ? gateway : "none");
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
    y += line + 2;

    int servers = recon_net_nameserver_count();
    for (int i = 0; i < servers; i++) {
        snprintf(summary, sizeof(summary), "%-15s %s",
            i == 0 ? "Resolver" : "", recon_net_nameserver_at(i));
        recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
        y += line + 2;
    }
    if (servers == 0) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Resolver        none configured", COLOR_TEXT);
        y += line + 2;
    }

    char host[128];
    enum recon_net_result result;
    int elapsed = 0;
    if (recon_net_last_probe(host, sizeof(host), &result, &elapsed)) {
        if (result == RECON_NET_OK) {
            snprintf(summary, sizeof(summary),
                "Last test       %s: reached in %d ms", host, elapsed);
        } else {
            snprintf(summary, sizeof(summary), "Last test       %s: %s",
                host, recon_net_result_name(result));
        }
        recon_draw_text(p, cp->font, x, y + ascent, w, summary,
            result == RECON_NET_OK ? COLOR_TEXT : COLOR_WARNING);
        y += line + 2;
    }

    if (recon_net_probe_count() > 0) {
        recon_draw_text(p, cp->font, x, y + ascent, w, "Testing...",
            COLOR_DIM);
        y += line + 2;
    }

    /*
     * The fingerprint of this machine's own certificate, when there is one.
     *
     * Here rather than only in the terminal because this is the page somebody
     * looks at when they are about to let a machine in, and the number is the
     * only thing standing between "encrypted to somebody" and "encrypted to
     * this machine". It is not shown when remote access has never been turned
     * on, because there is no certificate until then and inventing a row that
     * says "none" would be answering a question nobody asked.
     */
    char print[RECON_TLS_FINGERPRINT_MAX];
    if (recon_tls_fingerprint(print, sizeof(print))) {
        y += line / 2;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "This machine's fingerprint, for remote access:", COLOR_TEXT);
        y += line + 2;
        recon_draw_text(p, cp->font, x, y + ascent, w, print, COLOR_DIM);
        y += line + 2;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Check it the first time you connect from somewhere else. "
            "Nothing else vouches for this machine.", COLOR_DIM);
        y += line + 2;
    }

    y += PADDING;
    int bx = draw_button(cp, p, x, y, "Test the connection",
        HIT_ACTION_BASE + ACTION_TEST_NETWORK, true);
    bx = draw_button(cp, p, bx, y, "Read again",
        HIT_ACTION_BASE + ACTION_REFRESH_NETWORK, true);

    /*
     * The firewall is a page of its own and belongs to the network as much as
     * anything here does. Reaching it meant going back to the front page and
     * finding a second icon, which is two steps to arrive somewhere the
     * reader was already looking at.
     */
    draw_button(cp, p, bx, y, "Firewall",
        HIT_ACTION_BASE + ACTION_OPEN_FIREWALL, true);
    y += BUTTON_HEIGHT + PADDING;

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "ReconOS has no network stack of its own. This is the host's,",
        COLOR_DIM);
    y += line;
    recon_draw_text(p, cp->font, x, y + ascent, w,
        "reported through ReconOS. Its own comes with its own kernel.",
        COLOR_DIM);
}

/* --- Network: Adapters --- */

/*
 * Every interface, and everything the host will say about the one picked.
 *
 * This is the "hardware properties" question: not "am I online" but "what is
 * this thing and what is it doing". A machine with a wired port, a wireless
 * card and a virtual bridge has three answers to that and they are different.
 */
static void draw_net_adapters(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int count = recon_net_interface_count();

    /* Room kept below for the picked one's details, so choosing a row never
     * pushes the list about. */
    int detail_h = line * 5 + PADDING * 2;
    int rows = net_rows(cp, y, h, detail_h);
    if (rows > count) {
        rows = count;
    }
    /*
     * One row's worth even when there is nothing in it. An empty list with no
     * height is a list box that is not there, and the line saying why it is
     * empty then lands on top of whatever was drawn under it.
     */
    if (rows < 1) {
        rows = 1;
    }

    clamp_scroll(&cp->net_scroll, rows, count);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->net_scroll, rows, count);
    int lw = w - bar;

    if (cp->list_h > 0) {
        recon_fill_rect(p, x, y, lw, cp->list_h, COLOR_PANEL);
    }

    for (int row = 0; row < rows; row++) {
        struct recon_net_interface interface;
        if (!net_interface_in_order(cp->net_scroll + row, &interface)) {
            break;
        }

        char detail[128];
        snprintf(detail, sizeof(detail), "%s%s",
            interface.address[0] != '\0' ? interface.address : "no address",
            interface.up ? "" : "   down");

        draw_row(cp, p, x, y + row * ROW_HEIGHT, lw, row, interface.name,
            detail, row == cp->net_selected);
        recon_hit_add(p, x, y + row * ROW_HEIGHT, lw, ROW_HEIGHT,
            HIT_NET_ROW_BASE + (uint32_t)row);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "No interfaces at all.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    struct recon_net_interface chosen;
    if (cp->net_selected < 0 ||
            !net_interface_in_order(cp->net_scroll + cp->net_selected,
                &chosen)) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Choose an adapter to see what the host says about it.",
            COLOR_DIM);
        return;
    }

    y = info_group(cp, p, x, y, w, chosen.name);
    info_row(cp, p, x, &y, w, "Address",
        chosen.address[0] != '\0' ? chosen.address : "none");
    info_row(cp, p, x, &y, w, "Netmask",
        chosen.netmask[0] != '\0' ? chosen.netmask : "none");
    info_row(cp, p, x, &y, w, "State", chosen.up ? "up" : "down");
    info_row(cp, p, x, &y, w, "Kind",
        chosen.loopback ? "loopback -- this machine only"
            : (chosen.wireless ? "wireless" : "wired, or the host is not "
                "saying"));
}

/* --- Network: Data Used --- */

/*
 * What each interface has carried. The host counts it, which means the count
 * starts when the interface came up and not when anybody started watching --
 * so the page says that rather than implying a figure it does not have.
 */
static void draw_net_usage(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Since each adapter came up, as the host counts it.", COLOR_DIM);
    y += line + PADDING;

    int count = recon_net_interface_count();
    int rows = net_rows(cp, y, h, PADDING * 2 + line * 2);
    if (rows > count) {
        rows = count;
    }
    /*
     * One row's worth even when there is nothing in it. An empty list with no
     * height is a list box that is not there, and the line saying why it is
     * empty then lands on top of whatever was drawn under it.
     */
    if (rows < 1) {
        rows = 1;
    }

    clamp_scroll(&cp->net_scroll, rows, count);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->net_scroll, rows, count);
    int lw = w - bar;

    if (cp->list_h > 0) {
        recon_fill_rect(p, x, y, lw, cp->list_h, COLOR_PANEL);
    }

    unsigned long long rx_total = 0;
    unsigned long long tx_total = 0;

    for (int row = 0; row < rows; row++) {
        struct recon_net_interface interface;
        if (!net_interface_in_order(cp->net_scroll + row, &interface)) {
            break;
        }

        char received[32];
        char sent[32];
        storage_size(interface.rx_bytes, received, sizeof(received));
        storage_size(interface.tx_bytes, sent, sizeof(sent));

        char detail[96];
        snprintf(detail, sizeof(detail), "%s in, %s out", received, sent);

        draw_row(cp, p, x, y + row * ROW_HEIGHT, lw, row, interface.name,
            detail, false);
    }

    /* The total counts every interface, not only the ones on screen -- a
     * total that changed when you scrolled would not be a total. */
    for (int i = 0; i < count; i++) {
        struct recon_net_interface interface;
        if (net_interface_in_order(i, &interface) && !interface.loopback) {
            rx_total += interface.rx_bytes;
            tx_total += interface.tx_bytes;
        }
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "No interfaces at all.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    char received[32];
    char sent[32];
    storage_size(rx_total, received, sizeof(received));
    storage_size(tx_total, sent, sizeof(sent));

    char total[128];
    snprintf(total, sizeof(total), "In and out of this machine:  %s in, "
        "%s out", received, sent);
    recon_draw_text(p, cp->font, x, y + ascent, w, total, COLOR_TEXT);
    y += line + 2;
    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Loopback left out: it never left the machine.", COLOR_DIM);
}

/* --- Network: Applications --- */

/*
 * Which programs may use the network.
 *
 * This is the sharing question turned round. ReconOS has no stack to share,
 * and it does have a list of applications that are allowed to open a
 * connection -- which is the decision somebody actually gets to make.
 */
static void draw_net_apps(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Programs that have asked to use the network, and whether they may.",
        COLOR_DIM);
    y += line + PADDING;

    int count = recon_net_allowed_count();
    int rows = net_rows(cp, y, h, BUTTON_HEIGHT + PADDING * 3);
    if (rows > count) {
        rows = count;
    }
    /*
     * One row's worth even when there is nothing in it. An empty list with no
     * height is a list box that is not there, and the line saying why it is
     * empty then lands on top of whatever was drawn under it.
     */
    if (rows < 1) {
        rows = 1;
    }

    clamp_scroll(&cp->net_scroll, rows, count);

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    int bar = draw_scrollbar(p, x + w - SCROLLBAR_WIDTH, y, cp->list_h,
        cp->net_scroll, rows, count);
    int lw = w - bar;

    if (cp->list_h > 0) {
        recon_fill_rect(p, x, y, lw, cp->list_h, COLOR_PANEL);
    }

    for (int row = 0; row < rows; row++) {
        char name[96];
        bool allowed = false;
        if (!recon_net_allowed_at(cp->net_scroll + row, name, sizeof(name),
                &allowed)) {
            break;
        }

        draw_row(cp, p, x, y + row * ROW_HEIGHT, lw, row, name,
            allowed ? "may use the network" : "blocked",
            row == cp->net_selected);
        recon_hit_add(p, x, y + row * ROW_HEIGHT, lw, ROW_HEIGHT,
            HIT_NET_ROW_BASE + (uint32_t)row);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "Nothing has asked yet.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    char name[96];
    bool allowed = false;
    bool have = cp->net_selected >= 0 &&
        recon_net_allowed_at(cp->net_scroll + cp->net_selected, name,
            sizeof(name), &allowed);

    draw_button(cp, p, x, y, have && allowed ? "Block" : "Allow",
        HIT_ACTION_BASE + ACTION_TOGGLE_NET_APP, have);
}

/* --- Network --- */

static void draw_network(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    /* Read again every time the page is drawn: an address that was true when
     * the window opened is not a fact, it is a memory. */
    recon_net_refresh();

    bool online = recon_net_online();
    y = draw_heading(cp, p, x, y, w, "Network",
        online ? "This machine has a way out."
               : "This machine has no way out at the moment.");

    y = draw_tabs(cp, p, x, y, w, NETWORK_SECTION_NAMES, NETWORK_SECTIONS,
        (int)cp->net_section, HIT_NET_TAB_BASE);

    switch (cp->net_section) {
    case NETWORK_STATUS:   draw_net_status(cp, p, x, y, w, h); break;
    case NETWORK_ADAPTERS: draw_net_adapters(cp, p, x, y, w, h); break;
    case NETWORK_USAGE:    draw_net_usage(cp, p, x, y, w, h); break;
    case NETWORK_APPS:     draw_net_apps(cp, p, x, y, w, h); break;
    case NETWORK_SECTIONS: break;
    }
}

/*
 * The registry, behind a password.
 *
 * Read-only for now: seeing what the system remembers is most of the value,
 * and an editor here would be the fastest way yet built to make a system
 * unbootable. The terminal's 'reg' command still changes things for anyone
 * who means to.
 */
static void draw_registry(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Registry",
        "Everything the system remembers between runs.");

    if (!cp->registry_unlocked) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "An administrator's password is needed to look at this.",
            COLOR_TEXT);
        y += line + 4;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Not because it is secret -- it is a text file on disk -- but "
            "because a", COLOR_DIM);
        y += line;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "careless change here breaks the system quietly.", COLOR_DIM);
        y += line + PADDING;

        const char *who = recon_users_current();
        char label[128];
        snprintf(label, sizeof(label), "Password for %s",
            who != NULL ? who : "the administrator");
        recon_draw_text(p, cp->font, x, y + ascent, w, label, COLOR_DIM);
        y += line + 2;

        recon_edit_draw(p, cp->font, x, y, 240, FIELD_HEIGHT, &cp->unlock);
        recon_hit_add(p, x, y, 240, FIELD_HEIGHT, HIT_FIELD_BASE + 1);
        y += FIELD_HEIGHT + 8;

        draw_button(cp, p, x, y, "Unlock",
            HIT_ACTION_BASE + ACTION_UNLOCK_REGISTRY,
            recon_users_may_administer());
        return;
    }

    /* Which hive, and how many keys are in it. */
    int hive = cp->registry_hive;
    enum recon_registry_scope scope =
        hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

    int bx = draw_button(cp, p, x, y,
        hive == 0 ? "Showing: the machine's" : "Showing: this account's",
        HIT_ACTION_BASE + ACTION_REGISTRY_HIVE, true);
    draw_button(cp, p, bx, y, "Lock",
        HIT_ACTION_BASE + ACTION_LOCK_REGISTRY, true);
    y += BUTTON_HEIGHT + PADDING;

    int count = recon_registry_count(scope, "");

    /*
     * Room kept below the list for the buttons, and for the fields when one
     * of them is open. Taken off before the rows are counted rather than
     * after, so the list never draws over its own controls -- which it would
     * do on a short window, and the controls are the half that changes
     * things.
     */
    int reserved = BUTTON_HEIGHT + PADDING * 2;
    if (cp->registry_editing || cp->registry_adding) {
        reserved += FIELD_HEIGHT + 8;
    }
    if (cp->registry_adding) {
        reserved += FIELD_HEIGHT + 8;
    }

    int rows = (h - y - reserved) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    /*
     * Back inside a list that has shrunk. Not forced into existence, though:
     * -1 means nobody has clicked yet, and a row lit before anybody pointed
     * at it is the page answering a question it was not asked.
     */
    if (cp->selected >= count) {
        cp->selected = count - 1;
    }

    /* Follow the selection, so a key chosen and then scrolled past does not
     * leave the buttons acting on something nobody can see. */
    if (cp->selected < cp->registry_scroll) {
        cp->registry_scroll = cp->selected;
    }
    if (cp->selected >= cp->registry_scroll + rows) {
        cp->registry_scroll = cp->selected - rows + 1;
    }
    if (cp->registry_scroll > count - rows) {
        cp->registry_scroll = count - rows;
    }
    if (cp->registry_scroll < 0) {
        cp->registry_scroll = 0;
    }

    for (int i = 0; i < rows && cp->registry_scroll + i < count; i++) {
        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->registry_scroll + i,
                &key, &value)) {
            break;
        }

        int ry = y + i * ROW_HEIGHT;
        bool chosen = (cp->registry_scroll + i == cp->selected);

        if (chosen) {
            recon_fill_role(p, x, ry, w, ROW_HEIGHT, RECON_THEME_SELECTION);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_draw_text(p, cp->font, x + 10, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2, key != NULL ? key : "",
            chosen ? THEME(SELECTION_TEXT) : COLOR_TEXT);
        recon_draw_text(p, cp->font, x + w / 2, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, value != NULL ? value : "",
            chosen ? THEME(SELECTION_TEXT) : COLOR_DIM);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "This hive is empty.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    /*
     * Adding takes two fields, changing takes one.
     *
     * The key is not editable when changing an existing setting: a key that
     * can be typed over is a rename, and a rename here is a delete and an add
     * that look like one act -- which is how somebody ends up with the old
     * key still in the file and no idea it is there.
     */
    if (cp->registry_adding) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Key, then value. Keys are paths and cannot contain spaces.",
            COLOR_DIM);
        y += line;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_key);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 2);
        y += FIELD_HEIGHT + 8;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_value);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 3);
        y += FIELD_HEIGHT + 8;

        int abx = draw_button(cp, p, x, y, "Add",
            HIT_ACTION_BASE + ACTION_REGISTRY_SAVE, true);
        draw_button(cp, p, abx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_REGISTRY_CANCEL, true);
        return;
    }

    if (cp->registry_editing) {
        char label[RECON_REGISTRY_KEY_MAX + 32];
        snprintf(label, sizeof(label), "New value for %s", cp->registry_key);
        recon_draw_text(p, cp->font, x, y + ascent, w, label, COLOR_DIM);
        y += line;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_value);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 3);
        y += FIELD_HEIGHT + 8;

        int ebx = draw_button(cp, p, x, y, "Save",
            HIT_ACTION_BASE + ACTION_REGISTRY_SAVE, true);
        draw_button(cp, p, ebx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_REGISTRY_CANCEL, true);
        return;
    }

    bool have = (count > 0 && cp->selected >= 0 && cp->selected < count);

    int cbx = draw_button(cp, p, x, y, "Change",
        HIT_ACTION_BASE + ACTION_REGISTRY_EDIT, have);
    cbx = draw_button(cp, p, cbx, y, "Add",
        HIT_ACTION_BASE + ACTION_REGISTRY_ADD, true);
    draw_button(cp, p, cbx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REGISTRY_REMOVE, have);
}

/*
 * --- System Information ---
 *
 * It was eight lines and a paragraph, called About. Joshua's read of it:
 *
 *   It should say system information and just have a bunch of system
 *   information.
 *
 * So it does, in three groups, and the groups are the point. What the machine
 * is, what ReconOS is, and what is underneath -- because the third is the one
 * that explains why the first is readable at all, and running them together
 * would let somebody believe ReconOS had found the processor itself.
 */
static void info_row(struct control_panel *cp, struct recon_panel *p,
        int x, int *y, int w, const char *label, const char *value) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, *y + ascent, 150, label, COLOR_DIM);
    recon_draw_text(p, cp->font, x + 160, *y + ascent, w - 168, value,
        COLOR_TEXT);
    *y += line + 2;
}

static int info_group(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, const char *title) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y += 6;
    recon_draw_text(p, cp->font, x, y + ascent, w, title, THEME(ACCENT));
    y += line;
    recon_fill_rect(p, x, y, w, 1, COLOR_SEPARATOR);
    return y + 6;
}

static void draw_system(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    int bottom = y + h;

    y = draw_heading(cp, p, x, y, w, "System Information",
        "What this machine is, and what is running on it.");

    char value[192];

    /* --- The machine --- */
    y = info_group(cp, p, x, y, w, "This machine");

    char cpu[160];
    if (recon_proc_cpu_name(cpu, sizeof(cpu))) {
        int cores = recon_proc_cpu_cores();
        if (cores > 0) {
            snprintf(value, sizeof(value), "%s  (%d core%s)", cpu, cores,
                cores == 1 ? "" : "s");
        } else {
            snprintf(value, sizeof(value), "%s", cpu);
        }
    } else {
        snprintf(value, sizeof(value), "not readable");
    }
    info_row(cp, p, x, &y, w, "Processor", value);

    struct recon_proc_snapshot *snapshot = recon_proc_snapshot_create();
    size_t total_mb = 0;
    size_t used_mb = 0;
    if (snapshot != NULL && recon_proc_snapshot_refresh(snapshot)) {
        total_mb = recon_proc_total_memory_kb(snapshot) / 1024;
        used_mb = recon_proc_used_memory_kb(snapshot) / 1024;
    }
    recon_proc_snapshot_destroy(snapshot);

    snprintf(value, sizeof(value), "%zu MB, %zu MB in use", total_mb,
        used_mb);
    info_row(cp, p, x, &y, w, "Memory", value);

    snprintf(value, sizeof(value), "%d by %d", cp->server->screen_width,
        cp->server->screen_height);
    info_row(cp, p, x, &y, w, "Display", value);

    /* Every space, so the one number here is the one the Storage page adds
     * up to rather than a different total nobody can reconcile. */
    unsigned long long across = 0;
    for (int i = 0; i < RECON_VOLUME_COUNT; i++) {
        unsigned long long used = 0;
        recon_volume_usage((enum recon_volume)i, &used, NULL);
        across += used;
    }
    char size[32];
    storage_size(across, size, sizeof(size));
    snprintf(value, sizeof(value), "%s used across three spaces", size);
    info_row(cp, p, x, &y, w, "Storage", value);

    /* --- ReconOS --- */
    y = info_group(cp, p, x, y, w, RECONOS_FULL_NAME);

    info_row(cp, p, x, &y, w, "Version", RECONOS_VERSION);
    info_row(cp, p, x, &y, w, "Built", __DATE__);
    info_row(cp, p, x, &y, w, "Filesystem", recon_fs_host_root());

    snprintf(value, sizeof(value), "%s (%s)",
        recon_users_current() != NULL ? recon_users_current() : "nobody",
        recon_users_current_is_admin() ? "administrator" : "limited");
    info_row(cp, p, x, &y, w, "Signed in as", value);

    snprintf(value, sizeof(value), "%d", recon_users_count());
    info_row(cp, p, x, &y, w, "Accounts", value);

    info_row(cp, p, x, &y, w, "Skin", recon_theme_current());

    snprintf(value, sizeof(value), "%d installed, %d loaded as modules",
        recon_installed_app_count(), recon_modules_count());
    info_row(cp, p, x, &y, w, "Applications", value);

    snprintf(value, sizeof(value), "%d, %d switched on",
        recon_firewall_count(), recon_firewall_is_on() ? 1 : 0);
    info_row(cp, p, x, &y, w, "Firewall", recon_firewall_is_on()
        ? "on" : "off");

    /* --- The host --- */
    y = info_group(cp, p, x, y, w, "Underneath");

    info_row(cp, p, x, &y, w, "Runs on", recon_net_machine_name());
    info_row(cp, p, x, &y, w, "Kernel", "the host's, until Phase 2");

    y += 6;
    if (y + line * 2 <= bottom) {
        recon_draw_text(p, cp->font, x, y + recon_font_ascent(cp->font), w,
            "Everything above the last group is read through the host. "
            "ReconOS has no kernel of", COLOR_DIM);
        y += line;
        recon_draw_text(p, cp->font, x, y + recon_font_ascent(cp->font), w,
            "its own yet, so accounts are enforced by ReconOS inside ReconOS "
            "-- a program running", COLOR_DIM);
        y += line;
        recon_draw_text(p, cp->font, x, y + recon_font_ascent(cp->font), w,
            "on the host underneath is not subject to them.", COLOR_DIM);
        y += line;
    }

    y += PADDING;
    draw_pending_list(cp, p, x, y, w, bottom - y, PAGE_ABOUT, false);
}

/*
 * How many tiles fit across the front page, and where one of them sits.
 * Worked out from the width the window happens to be rather than fixed, so
 * widening it puts more on a row instead of leaving a margin down the side.
 */
static int tiles_across(int w) {
    int across = (w + TILE_GAP) / (TILE_W + TILE_GAP);
    return across < 1 ? 1 : across;
}

static void tile_rect(int index, int x, int y, int w, int *tx, int *ty) {
    int across = tiles_across(w);

    /* Centred as a block. A grid pinned left with a wide gap down the right
     * reads as a mistake rather than as a layout. */
    int used = across * TILE_W + (across - 1) * TILE_GAP;
    int left = x + (w - used) / 2;

    *tx = left + (index % across) * (TILE_W + TILE_GAP);
    *ty = y + (index / across) * (TILE_H + TILE_GAP);
}

static void draw_home(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);

    int bottom = y + h;

    /*
     * The window's name, centred and larger, and nothing else.
     *
     * It used to carry a line explaining that each item opens in its own
     * window. That is true and it is not what a heading is for: somebody
     * reading the top of a window wants to know which window it is, and a
     * sentence there is read once and then in the way forever.
     */
    struct recon_font *big = recon_font_system(recon_font_line_height(cp->font)
        + 8);
    if (big == NULL) {
        big = cp->font;
    }

    int title_w = recon_text_width(big, "Control Panel");
    recon_draw_text(p, big, x + (w - title_w) / 2,
        y + recon_font_ascent(big), w, "Control Panel", COLOR_TEXT);
    y += recon_font_line_height(big) + PADDING;

    for (int i = 0; i < PAGE_COUNT; i++) {
        int tx, ty;
        tile_rect(i, x, y, w, &tx, &ty);
        if (ty + TILE_H > bottom) {
            break;
        }

        /*
         * Three states, and they mean different things. Open is a fact about
         * the system; chosen is a fact about this window; and hovered is
         * neither, it is the pointer. Drawn strongest to weakest, so the one
         * that matters most is the one that reads first.
         */
        bool open = page_window_open((enum page)i);
        bool chosen = cp->selected == i;

        if (chosen) {
            recon_fill_rect(p, tx, ty, TILE_W, TILE_H, COLOR_SELECTED);
        } else if (open) {
            recon_fill_rect(p, tx, ty, TILE_W, TILE_H, COLOR_ROW_ALT);
        } else if (cp->hover_tile == i) {
            recon_fill_rect(p, tx, ty, TILE_W, TILE_H, COLOR_PANEL);
        }

        int icon_x = tx + (TILE_W - TILE_ICON) / 2;
        int icon_y = ty + (TILE_H - TILE_ICON - recon_font_line_height(cp->font)) / 2;
        if (!recon_icon_draw(p, PAGES[i].icon, icon_x, icon_y, TILE_ICON)) {
            recon_fill_rect(p, icon_x, icon_y, TILE_ICON, TILE_ICON,
                COLOR_SELECTED);
        }

        int label_w = recon_text_width(cp->font, PAGES[i].label);
        int label_x = tx + (TILE_W - label_w) / 2;
        if (label_x < tx + 4) {
            label_x = tx + 4;
        }
        recon_draw_text(p, cp->font, label_x,
            icon_y + TILE_ICON + 8 + ascent, TILE_W - 8, PAGES[i].label,
            chosen ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        recon_hit_add(p, tx, ty, TILE_W, TILE_H, HIT_TILE_BASE + i);

        /*
         * What it is for, said when somebody stops on it.
         *
         * This used to be a line of text under every name, which had to fit
         * under an icon and so was cut off mid-word -- "Code the system
         * lo..." -- and was drawn fourteen times whether anybody wanted it or
         * not. Then it was a tooltip this file drew itself, which could not
         * hang past the edge of the window. Now it is the shell's, like every
         * other tooltip.
         */
        if (PAGES[i].summary != NULL) {
            recon_hit_tip(p, PAGES[i].summary);
        }
    }

}

/* Whichever page this window is about. Shared by both kinds of window, so
 * there is one switch rather than two that can fall out of step. */
static void draw_page(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    switch (cp->page) {
    case PAGE_ACCOUNTS:   draw_accounts(cp, p, x, y, w, h); break;
    case PAGE_POWER:      draw_power(cp, p, x, y, w, h); break;
    case PAGE_APPEARANCE: draw_appearance(cp, p, x, y, w, h); break;
    case PAGE_CLOCK:      draw_clock_page(cp, p, x, y, w, h); break;
    case PAGE_DISPLAY:    draw_display(cp, p, x, y, w, h); break;
    case PAGE_PROGRAMS:   draw_programs(cp, p, x, y, w, h); break;
    case PAGE_MODULES:    draw_modules(cp, p, x, y, w, h); break;
    case PAGE_NETWORK:    draw_network(cp, p, x, y, w, h); break;
    case PAGE_FIREWALL:   draw_firewall(cp, p, x, y, w, h); break;
    case PAGE_STORAGE:    draw_storage(cp, p, x, y, w, h); break;
    case PAGE_CLEANUP:    draw_cleanup(cp, p, x, y, w, h); break;
    case PAGE_UPDATE:     draw_update(cp, p, x, y, w, h); break;
    case PAGE_REGISTRY:   draw_registry(cp, p, x, y, w, h); break;
    case PAGE_ABOUT:      draw_system(cp, p, x, y, w, h); break;
    default:
        draw_pending_list(cp, p, x, y, w, h, cp->page, true);
        break;
    }
}

static void panel_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct control_panel *cp = user;
    int ascent = recon_font_ascent(cp->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    int cx = x + PADDING;
    int cy = y + PADDING;
    int cw = w - PADDING * 2;
    int ch = h - PADDING * 2 - STATUS_HEIGHT;

    /*
     * A page window has no sidebar. It is a window about one thing, opened
     * from the front page, and a list of the other thirteen down its left
     * edge would be thirteen ways to turn it into a window you did not ask
     * for.
     */
    if (cp->home) {
        draw_home(cp, p, cx, cy, cw, ch);
    } else {
        draw_page(cp, p, cx, cy, cw, ch);
    }

    /* Status, along the bottom. */
    int sy = y + h - STATUS_HEIGHT;
    recon_fill_rect(p, x, sy, w, STATUS_HEIGHT, COLOR_BG);
    recon_draw_text(p, cp->font, x + PADDING,
        sy + (STATUS_HEIGHT + ascent) / 2 - 2, w - PADDING * 2, cp->status,
        cp->status_is_warning ? COLOR_WARNING : COLOR_DIM);
}

/* --- Doing things --- */

static void begin_editing(struct control_panel *cp, bool password_only) {
    cp->editing = true;
    cp->editing_password_only = password_only;
    cp->password_focused = password_only;

    recon_edit_begin(&cp->name, "", false);
    recon_edit_begin(&cp->password, "", false);
    cp->password.masked = true;

    if (password_only) {
        struct recon_user user;
        if (recon_users_at(cp->selected, &user)) {
            snprintf(cp->question_target, sizeof(cp->question_target),
                "%s", user.name);
        }
        set_status(cp, false, "A new password for '%s'. Empty removes it.",
            cp->question_target);
    } else {
        set_status(cp, false, "A name, and a password if you want one.");
    }
}

static void stop_editing(struct control_panel *cp) {
    cp->editing = false;
    /* The typed password goes out of memory rather than sitting in the
     * window's state for the rest of the session. */
    recon_edit_end(&cp->name);
    recon_edit_end(&cp->password);
}

/* The answer to "remove this account?". */
/* Defined below; the answer to a question is usually an action. */
static void do_action(struct control_panel *cp, enum action action);

static void answered(void *user, int choice) {
    struct control_panel *cp = user;

    char name[RECON_REGISTRY_KEY_MAX];
    snprintf(name, sizeof(name), "%s", cp->question_target);

    enum question asked = cp->question;
    cp->question = QUESTION_NONE;

    /*
     * The last button is always Cancel, and dismissing gives -1. Removing an
     * account offers two ways of saying yes, so "anything but the first" is
     * no longer the same as "no".
     */
    int button_count = 2;
    if (asked == QUESTION_REMOVE_USER) {
        button_count = 3;
    } else if (asked == QUESTION_NEW_SKIN) {
        button_count = 4;
    }
    if (choice < 0 || choice >= button_count - 1) {
        set_status(cp, false, "Nothing was changed.");
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_CLEAN_UP) {
        int removed = 0;
        int failed = 0;

        for (int i = 0; i < CLEAN[cp->volume].count && i < CLEAN_MAX; i++) {
            if (!cp->clean[i].ticked) {
                continue;
            }

            const struct clean_category *item = &CLEAN[cp->volume].items[i];

            if (item->is_bin) {
                if (recon_fs_trash_empty_in(cp->volume)) {
                    removed += cp->clean[i].files;
                } else {
                    failed++;
                }
                continue;
            }

            struct recon_dirent entries[256];
            int count = recon_fs_list("/", item->path, entries, 256);
            if (count < 0) {
                continue;
            }
            if (count > 256) {
                count = 256;
            }

            for (int e = 0; e < count; e++) {
                if (!category_covers(item, &entries[e])) {
                    continue;
                }

                char victim[RECON_PATH_MAX];
                if (!recon_fs_join(victim, sizeof(victim), item->path,
                        entries[e].name)) {
                    failed++;
                    continue;
                }

                /*
                 * Removed rather than put in the bin. Everything on this page
                 * is already either in the bin or scratch, and moving scratch
                 * into the bin would free nothing -- which is the one thing
                 * somebody pressing Clean Up asked for.
                 */
                if (recon_fs_remove("/", victim)) {
                    removed++;
                } else {
                    failed++;
                }
            }
        }

        /* The sizes on screen are now wrong by what was just freed, which is
         * the whole reason somebody pressed it. */
        for (int i = 0; i < CLEAN_MAX; i++) {
            cp->clean[i].ticked = false;
        }
        cp->clean_measured = false;
        measure_cleanup(cp);
        cp->storage_measured = false;

        if (failed > 0) {
            set_status(cp, true, "Removed %d; %d could not be removed.",
                removed, failed);
        } else {
            set_status(cp, false, "Removed %d file%s.", removed,
                removed == 1 ? "" : "s");
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_NEW_SKIN) {
        /*
         * Named for what they look like rather than for the skin behind each
         * one. Somebody starting a skin knows they want a dark one; whether
         * the dark one they are handed is called Midnight is the system's
         * business, not theirs.
         */
        static const char *const FROM[] = { "Aqua", "Midnight", "Contrast" };
        snprintf(cp->new_skin_from, sizeof(cp->new_skin_from), "%s",
            FROM[choice]);
        do_action(cp, ACTION_BEGIN_NAMING_SKIN);
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_CUSTOMIZE_SKIN) {
        do_action(cp, ACTION_BEGIN_NAMING_SKIN);
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_EMPTY_BIN) {
        if (!recon_fs_trash_empty_in(cp->volume)) {
            set_status(cp, true, "%s", recon_fs_last_error());
        } else {
            /* The numbers on the page are now wrong by exactly what was just
             * thrown away, which is the whole reason somebody pressed it. */
            measure_storage(cp);
            set_status(cp, false, "The %s bin is empty.",
                recon_volume_name(cp->volume));
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_REMOVE_KEY) {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        if (!recon_registry_remove(scope, name)) {
            set_status(cp, true, "%s", recon_registry_last_error());
        } else {
            set_status(cp, false, "Removed '%s'.", name);
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_REMOVE_PROGRAM) {
        if (!recon_modules_uninstall(name)) {
            set_status(cp, true, "%s", recon_modules_last_error());
        } else {
            /* Nothing chosen: the row number would survive the removal
             * and point at whichever program slid up into its place. */
            cp->selected = -1;
            /* The Start menu listed it; it has to stop. */
            recon_shell_restyle(cp->server->shell);
            set_status(cp, false, "Removed '%s'.", name);
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    /* Button 1 was "Delete Files"; button 0 was "Keep Files". */
    bool delete_files = (choice == 1);

    if (!recon_users_remove(name, delete_files)) {
        set_status(cp, true, "%s", recon_users_last_error());
    } else {
        cp->selected = -1;
        set_status(cp, false, delete_files
            ? "Removed '%s' and its files."
            : "Removed '%s'. Its files are still there.", name);
    }
    recon_appwin_refresh(cp->win);
}

/*
 * A preset, added as a rule.
 *
 * Switched on. The shipped incoming rules are written down and off because
 * they are there to be found rather than to be in force; one somebody has
 * just picked out of a list is the opposite -- they asked for it, and adding
 * it switched off would be adding nothing.
 */
static void add_preset_rule(struct control_panel *cp,
        const struct fw_preset *preset) {
    struct recon_fw_rule rule;
    memset(&rule, 0, sizeof(rule));

    snprintf(rule.name, sizeof(rule.name), "%s", preset->name);
    rule.direction = preset->direction;
    rule.protocol = preset->protocol;
    rule.port_from = preset->port_from;
    rule.port_to = preset->port_to;
    rule.action = preset->action;
    rule.enabled = true;

    if (!recon_firewall_add(&rule)) {
        set_status(cp, true, "%s", recon_firewall_last_error());
        return;
    }

    cp->fw_adding = false;
    cp->selected = recon_firewall_count() - 1;
    set_status(cp, false, "Added '%s'. It is at the end of the list.",
        rule.name);
}

static void do_action(struct control_panel *cp, enum action action) {
    struct recon_user chosen;
    bool have = recon_users_at(cp->selected, &chosen);

    switch (action) {
    case ACTION_NONE:
        stop_editing(cp);
        cp->installing = false;
        clear_status(cp);
        break;

    case ACTION_ADD_USER:
        if (!cp->editing) {
            begin_editing(cp, false);
            break;
        }
        if (!recon_users_create(cp->name.text, cp->password.text,
                RECON_ROLE_LIMITED)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "Created '%s' as a limited account.",
            cp->name.text);
        stop_editing(cp);
        break;

    case ACTION_SET_PASSWORD:
        if (!cp->editing) {
            if (have) {
                begin_editing(cp, true);
            }
            break;
        }
        if (!recon_users_set_password(cp->question_target,
                cp->password.length > 0 ? cp->password.text : NULL)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "Password %s for '%s'.",
            cp->password.length > 0 ? "changed" : "removed",
            cp->question_target);
        stop_editing(cp);
        break;

    case ACTION_TOGGLE_ROLE: {
        if (!have) {
            break;
        }
        enum recon_user_role role =
            chosen.role == RECON_ROLE_ADMINISTRATOR
                ? RECON_ROLE_LIMITED : RECON_ROLE_ADMINISTRATOR;

        if (!recon_users_set_role(chosen.name, role)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "'%s' is now %s.", chosen.name,
            role == RECON_ROLE_ADMINISTRATOR ? "an administrator" : "limited");
        break;
    }

    case ACTION_REMOVE_USER: {
        if (!have) {
            break;
        }
        cp->question = QUESTION_REMOVE_USER;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s",
            chosen.name);

        char message[256];
        snprintf(message, sizeof(message),
            "Remove the account '%s'?\n"
            "Its folder can be kept or deleted. Deleting cannot be undone.",
            chosen.name);

        /*
         * Three answers, because there are three. Closing an account and
         * destroying somebody's documents are separate decisions, and a
         * dialog offering only "Remove" has already made the second one on
         * their behalf.
         *
         * Cancel last, as everywhere: it is what Enter and Escape choose.
         */
        const char *buttons[3] = { "Keep Files", "Delete Files", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Account", message, buttons, 3,
            answered);
        break;
    }

    case ACTION_NEXT_AVATAR:
        /*
         * Show the pictures rather than cycling through them.
         *
         * A button that steps to the next one asks somebody to click it
         * repeatedly and watch a thumbnail change, which is a way of choosing
         * that shows you one option at a time and never the set. Choosing
         * from a picture is the whole point of having pictures.
         */
        if (have) {
            cp->picking_avatar = true;
            snprintf(cp->question_target, sizeof(cp->question_target), "%s",
                chosen.name);
            set_status(cp, false, "Choose a picture for %s.", chosen.name);
        }
        break;

    case ACTION_CHOOSE_AVATAR:
        cp->picking_avatar = false;
        clear_status(cp);
        break;

    case ACTION_REPAIR_PROGRAM: {
        struct recon_installed_app chosen;
        if (!program_selected(cp, &chosen)) {
            set_status(cp, true, "Choose a program first.");
            break;
        }

        /*
         * A built-in has no files of its own -- it is compiled in -- so the
         * only thing that can be wrong with one is its registration, and the
         * only honest repair is to say whether that is intact.
         */
        if (cp->programs == PROGRAMS_SYSTEM) {
            set_status(cp, false, "'%s' is registered and its icon is '%s'. "
                "A built-in has no files to put back; it is compiled in.",
                chosen.name,
                chosen.icon[0] != '\0' ? chosen.icon : "none");
            break;
        }

        /*
         * An application arrives one of two ways, and repair has to answer
         * for both.
         *
         * A package leaves a receipt naming every file it placed, and those
         * can be checked. A bare `.rex` module dropped into /Apps leaves no
         * receipt -- there is only the one file, and whether it is still
         * there and still loading is the whole question.
         */
        if (!recon_package_installed(chosen.name)) {
            char path[RECON_PATH_MAX];
            if (!recon_modules_path_of(chosen.module, path, sizeof(path))) {
                set_status(cp, true, "'%s' came from a module ReconOS can no "
                    "longer find.", chosen.name);
                break;
            }

            if (!recon_fs_exists("/", path)) {
                set_status(cp, true, "'%s' is registered but its module '%s' "
                    "is gone. Install it again from its .rex file.",
                    chosen.name, path);
                break;
            }

            set_status(cp, false, "'%s' is intact: it came as a single module "
                "and '%s' is still there and loaded.", chosen.name, path);
            break;
        }

        /*
         * Installed from a package, so the receipt says which files. Checking
         * them is real; putting one back is not, because putting a file back
         * needs the package it came from and nothing keeps one.
         */
        int placed = 0;
        int missing = 0;
        char gone[RECON_PATH_MAX];
        if (!recon_package_verify(chosen.name, &placed, &missing, gone,
                sizeof(gone))) {
            set_status(cp, true, "%s", recon_package_last_error());
            break;
        }

        if (missing == 0) {
            set_status(cp, false, "'%s' is intact: all %d file%s the install "
                "placed are still there.", chosen.name, placed,
                placed == 1 ? "" : "s");
            break;
        }

        set_status(cp, true, "'%s' is missing %d of its %d file%s, starting "
            "with '%s'. Install it again from its package to put them back.",
            chosen.name, missing, placed, placed == 1 ? "" : "s", gone);
        break;
    }

    case ACTION_DISABLE_PROGRAM: {
        struct recon_installed_app chosen;
        if (!program_selected(cp, &chosen)) {
            set_status(cp, true, "Choose a program first.");
            break;
        }

        bool turning_off = !chosen.disabled;

        /*
         * The Control Panel is one of them, and turning it off from inside
         * itself would close the door on the way back. Refused rather than
         * allowed and regretted.
         */
        if (turning_off && strcmp(chosen.name, "Control Panel") == 0) {
            set_status(cp, true, "The Control Panel cannot be turned off from "
                "inside itself -- there would be no way to turn it back on.");
            break;
        }

        if (!recon_installed_app_set_disabled(chosen.name, turning_off)) {
            set_status(cp, true, "Could not record that.");
            break;
        }

        /*
         * Its window goes, and the menus are rebuilt. An application turned
         * off with a window still on the screen is turned off everywhere
         * except where somebody is looking.
         */
        if (turning_off) {
            struct recon_appwin *open =
                recon_installed_app_existing(chosen.name);
            if (open != NULL) {
                recon_appwin_hide(open);
            }
        }

        /* The taskbar and the menu are drawn from the application list, so
         * they have to be told the list now answers differently. */
        recon_shell_restyle(cp->server->shell);

        set_status(cp, false, turning_off
            ? "'%s' is turned off. It is offered nowhere until it is turned "
              "back on."
            : "'%s' is on again.", chosen.name);
        break;
    }

    case ACTION_INSTALL_PROGRAM: {
        /*
         * The File Explorer, at Downloads, which is where a program somebody
         * fetched is.
         *
         * It was a path to type. Typing a path is the thing a person is worst
         * at and a file list is best at, and the same right-click that adds a
         * picture or a font now installs a program -- three ways of saying
         * "take this file into the system", said one way.
         */
        recon_shell_open_named(cp->server->shell, "File Explorer");

        struct recon_appwin *win =
            recon_installed_app_existing("File Explorer");
        if (win == NULL) {
            /* Turned off, most likely -- it is in the list above and can be.
             * The field still works, so offer it rather than stopping. */
            cp->installing = true;
            recon_edit_begin(&cp->name, recon_fs_user_dir("Downloads"), false);
            set_status(cp, true, "The File Explorer would not open, so type "
                "the path to a %s, %s or %s instead.",
                RECON_APP_EXT, RECON_MODULE_EXT, RECON_PACKAGE_EXT);
            break;
        }

        recon_explorer_open_at(win, recon_fs_user_dir("Downloads"));
        set_status(cp, false, "Right-click a %s, %s or %s and choose Install "
            "Program.", RECON_APP_EXT, RECON_MODULE_EXT, RECON_PACKAGE_EXT);
        break;
    }

    case ACTION_CONFIRM_INSTALL: {
        /*
         * A folder is a package and a file is a bare module, the same rule
         * the terminal uses. Typing a path and being told the wrong thing
         * about it because this page only understood one of the two would be
         * a difference with no reason behind it.
         */
        struct recon_dirent entry;
        bool is_package = recon_fs_stat("/", cp->name.text, &entry) &&
            entry.kind == RECON_FILE_DIRECTORY;

        if (is_package) {
            struct recon_package_info info;
            bool named = recon_package_read(cp->name.text, &info);

            if (!recon_package_install(cp->name.text)) {
                set_status(cp, true, "%s", recon_package_last_error());
                break;
            }
            cp->installing = false;
            recon_edit_end(&cp->name);
            recon_shell_restyle(cp->server->shell);
            set_status(cp, false, named
                ? "Installed %s %s." : "Installed.",
                info.name, info.version);
            break;
        }

        if (!recon_modules_install(cp->name.text)) {
            set_status(cp, true, "%s", recon_modules_last_error());
            break;
        }
        cp->installing = false;
        recon_edit_end(&cp->name);
        /* The Start menu lists applications, so it has to hear about a new
         * one arriving. */
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Installed, and loaded.");
        break;
    }

    case ACTION_REMOVE_PROGRAM: {
        /*
         * Only something that came from a module can be removed. The rest are
         * compiled into ReconOS, and "remove the File Explorer" is a request
         * to delete part of the system rather than a program.
         */
        /*
         * Resolved through the list that is showing, not the whole
         * registry. The page has two lists now and the selection is an
         * index into one of them; reading it as an index into everything
         * would remove whichever program happened to sit at that
         * position overall.
         */
        struct recon_installed_app app;
        if (!program_selected(cp, &app)) {
            set_status(cp, true, "Choose a program first.");
            break;
        }
        if (app.module[0] == '\0') {
            set_status(cp, true,
                "'%s' is part of ReconOS, not something installed.", app.name);
            break;
        }

        cp->question = QUESTION_REMOVE_PROGRAM;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s",
            app.module);

        char message[256];
        snprintf(message, sizeof(message),
            "Remove '%s'? Its file is deleted, and it will not come back on "
            "the next start.", app.name);

        const char *buttons[2] = { "Remove", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Program", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_LOAD_MODULE: {
        /* Real. Everything in /Apps that is not loaded already. */
        int before = recon_modules_count();
        int loaded = recon_modules_load_all();
        int now = recon_modules_count();

        if (loaded > 0 || now != before) {
            set_status(cp, false, "%d module%s loaded; %d known.",
                loaded, loaded == 1 ? "" : "s", now);
        } else {
            set_status(cp, false, "Nothing new in /Apps.");
        }
        break;
    }

    case ACTION_UNLOAD_MODULE: {
        struct recon_module_state module;
        if (!recon_modules_at(cp->selected, &module)) {
            set_status(cp, true, "Choose a module first.");
            break;
        }
        if (!module.loaded) {
            set_status(cp, true, "'%s' is not loaded.", module.name);
            break;
        }
        if (!recon_modules_unload(module.name)) {
            set_status(cp, true, "%s", recon_modules_last_error());
            break;
        }
        set_status(cp, false, "Unloaded '%s'.", module.name);
        break;
    }

    case ACTION_UNLOCK_REGISTRY: {
        const char *who = recon_users_current();
        if (who == NULL || !recon_users_may_administer()) {
            set_status(cp, true, "Only an administrator can open this.");
            break;
        }
        if (!recon_users_check(who, cp->unlock.text)) {
            /* The password goes out of memory whether it worked or not. */
            recon_edit_begin(&cp->unlock, "", false);
            cp->unlock.masked = true;
            set_status(cp, true, "That is not the password.");
            break;
        }
        recon_edit_end(&cp->unlock);
        cp->registry_unlocked = true;
        cp->registry_scroll = 0;
        set_status(cp, false, "Unlocked. This does not outlive the window.");
        break;
    }

    case ACTION_LOCK_REGISTRY:
        cp->registry_unlocked = false;
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        recon_edit_begin(&cp->unlock, "", false);
        cp->unlock.masked = true;
        set_status(cp, false, "Locked again.");
        break;

    case ACTION_REGISTRY_EDIT: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->selected, &key, &value)) {
            set_status(cp, true, "Choose a setting first.");
            break;
        }

        /* Copied out, because adding or removing anything renumbers the hive
         * and the index would then point at a different key. */
        snprintf(cp->registry_key, sizeof(cp->registry_key), "%s", key);
        cp->registry_editing = true;
        cp->registry_adding = false;
        recon_edit_begin(&cp->reg_value, value != NULL ? value : "", false);
        set_status(cp, false, "Changing '%s'.", cp->registry_key);
        break;
    }

    case ACTION_REGISTRY_ADD:
        cp->registry_adding = true;
        cp->registry_editing = false;
        cp->reg_key_focused = true;
        recon_edit_begin(&cp->reg_key, "", false);
        recon_edit_begin(&cp->reg_value, "", false);
        set_status(cp, false, "A key, then what it should say.");
        break;

    case ACTION_REGISTRY_CANCEL:
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        set_status(cp, false, "Nothing was changed.");
        break;

    case ACTION_REGISTRY_SAVE: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        /*
         * Copied out, not pointed at. The edit fields are closed further
         * down before the status line is written, and recon_edit_end
         * clears the text -- so a pointer into the field left the status
         * saying Saved '' by the time anybody read it.
         */
        const char *typed = cp->registry_adding
            ? cp->reg_key.text : cp->registry_key;

        if (cp->registry_adding && typed[0] == '\0') {
            set_status(cp, true, "A setting needs a key.");
            break;
        }

        /*
         * Refused rather than shortened. The edit field holds more than a
         * key can, and quietly storing the first 127 characters would put
         * a setting in the registry under a name nobody typed -- which
         * then looks like the one that was typed simply did not save.
         */
        size_t typed_len = strlen(typed);
        if (typed_len >= RECON_REGISTRY_KEY_MAX) {
            set_status(cp, true,
                "That key is too long: %d characters at most.",
                RECON_REGISTRY_KEY_MAX - 1);
            break;
        }

        /* Copied by the length just checked, so the bound is the one
         * the refusal above enforces rather than a second guess at
         * it. */
        char key[RECON_REGISTRY_KEY_MAX];
        memcpy(key, typed, typed_len);
        key[typed_len] = '\0';

        if (!recon_registry_set(scope, key, cp->reg_value.text)) {
            /* The registry says why -- a space in the key, a value too long,
             * a full hive -- and its sentence is better than a general one. */
            set_status(cp, true, "%s", recon_registry_last_error());
            break;
        }

        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);

        /*
         * Everything drawn, because a setting here is a setting something is
         * already using: the skin, the spacing, where a window opens. Writing
         * one and leaving the screen as it was would make the registry look
         * like it does not do anything.
         */
        recon_theme_init();
        recon_access_apply(recon_shell_font(cp->server->shell));
        recon_shell_restyle(cp->server->shell);

        set_status(cp, false, "Saved '%s'.", key);
        break;
    }

    case ACTION_REGISTRY_REMOVE: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->selected, &key, &value)) {
            set_status(cp, true, "Choose a setting first.");
            break;
        }

        cp->question = QUESTION_REMOVE_KEY;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s", key);

        char message[RECON_REGISTRY_KEY_MAX + 160];
        snprintf(message, sizeof(message),
            "Remove '%s'? Whatever reads it goes back to its default, which "
            "may not be what is on screen now.", key);

        const char *buttons[2] = { "Remove", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Setting", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_REGISTRY_HIVE:
        cp->registry_hive = cp->registry_hive == 0 ? 1 : 0;
        cp->registry_scroll = 0;
        cp->selected = -1;
        /* A field open on the other hive's key would save into this one. */
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        break;

    case ACTION_OPEN_FIREWALL:
        open_page_window(cp, PAGE_FIREWALL);
        break;

    case ACTION_TOGGLE_NET_APP: {
        char name[96];
        bool allowed = false;
        if (cp->net_selected < 0 ||
                !recon_net_allowed_at(cp->net_scroll + cp->net_selected, name,
                    sizeof(name), &allowed)) {
            set_status(cp, true, "Choose a program first.");
            break;
        }

        if (!recon_net_set_allowed(name, !allowed)) {
            set_status(cp, true, "%s", recon_net_last_error());
            break;
        }

        set_status(cp, false, allowed
            ? "'%s' can no longer use the network."
            : "'%s' may use the network.", name);
        break;
    }

    case ACTION_TEST_NETWORK: {
        /*
         * Asks whether something out there answers, and does not wait for the
         * answer. A dead network takes the whole timeout to say so, and a
         * Control Panel frozen for three seconds because somebody pressed a
         * button is worse than the answer is useful. The result lands where
         * the page reads it, and the page redraws when it next does.
         */
        if (!recon_net_online()) {
            set_status(cp, true,
                "Nothing is configured to test with -- no gateway.");
            break;
        }
        if (!recon_net_probe("example.com", 80, 3000, NULL, NULL)) {
            set_status(cp, true, "%s", recon_net_last_error());
            break;
        }
        set_status(cp, false,
            "Asking example.com. The answer appears above.");
        break;
    }

    case ACTION_REFRESH_NETWORK:
        recon_net_refresh();
        set_status(cp, false, "Read the network again.");
        break;

    case ACTION_SPACING_LESS:
    case ACTION_SPACING_MORE: {
        int value = recon_text_letter_spacing() +
            (action == ACTION_SPACING_MORE ? 1 : -1);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LETTER_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_LINES_LESS:
    case ACTION_LINES_MORE: {
        int value = recon_text_line_spacing() +
            (action == ACTION_LINES_MORE ? 2 : -2);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LINE_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_SIZE_LESS:
    case ACTION_SIZE_MORE: {
        int value = recon_registry_get_int(RECON_REG_USER,
            RECON_ACCESS_FONT_SIZE_KEY, RECON_ACCESS_FONT_SIZE_DEFAULT) +
            (action == ACTION_SIZE_MORE ? 1 : -1);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_USE_FONT: {
        char name[96];
        char path[RECON_PATH_MAX];
        if (cp->font_selected < 0 ||
                !recon_fonts_at(cp->font_scroll + cp->font_selected, name,
                    sizeof(name)) ||
                !recon_fonts_path(name, path, sizeof(path))) {
            set_status(cp, true, "Choose a font first.");
            break;
        }

        recon_registry_set(RECON_REG_USER, RECON_ACCESS_FONT_KEY, path);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);

        /*
         * Reported honestly if it did not take. A font file the drawing code
         * cannot read leaves the old one in place, and a page that says
         * "changed" over text that has not changed is the page lying about
         * the only thing the reader can check for themselves.
         */
        /*
         * Checked through the same resolution the loader will use, so the
         * answer is the loader's answer rather than a second opinion about a
         * path the loader never sees.
         */
        char host[RECON_PATH_MAX];
        char canonical[RECON_PATH_MAX];
        const char *readable = recon_fs_resolve("/", path, host, sizeof(host),
            canonical, sizeof(canonical)) ? host : path;

        struct recon_font *check = recon_font_load(readable,
            RECON_ACCESS_FONT_SIZE_DEFAULT);
        if (check == NULL) {
            set_status(cp, true, "'%s' could not be read as a font, so the "
                "old one is still on.", name);
            break;
        }
        /* Loaded only to find out whether it loads. The desktop has its own
         * copy by now; this one has nothing left to do. */
        recon_font_destroy(check);

        set_status(cp, false, "Drawing with '%s'.", name);
        break;
    }

    case ACTION_DEFAULT_FONT:
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_KEY);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Back to the system's own font.");
        break;

    case ACTION_TOGGLE_24H: {
        bool was = recon_registry_get_bool(RECON_REG_USER,
            RECON_CLOCK_24H_KEY, true);
        recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, !was);
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, was ? "Showing am and pm."
            : "Showing a 24-hour clock.");
        break;
    }

    case ACTION_CHECK_TIME: {
        char why[192];
        if (!recon_clock_check(why, sizeof(why))) {
            set_status(cp, true, "%s", why);
            break;
        }
        /* The answer arrives on the event loop, so this says what is
         * happening rather than what happened. */
        set_status(cp, false, "Asking the time server...");
        break;
    }

    case ACTION_SET_RESOLUTION: {
        if (cp->mode_selected < 0) {
            set_status(cp, true, "Choose a size first.");
            break;
        }

        /*
         * Asked for again by id rather than remembered from the draw. The
         * screen this acts on is the one that is there now, and between
         * drawing the list and pressing the button a display can go away --
         * on hardware that can be unplugged, which is where this is headed.
         */
        struct recon_display screen;
        if (!recon_display_at(0, &screen)) {
            set_status(cp, true, "There is no display to change.");
            break;
        }

        struct recon_display_mode mode;
        if (!recon_display_mode_at(screen.id, cp->mode_selected, &mode)) {
            set_status(cp, true, "That size is no longer offered.");
            break;
        }

        char why[192];
        if (!recon_display_set_mode(screen.id, cp->mode_selected, why,
                sizeof(why))) {
            set_status(cp, true, "%s", why);
            break;
        }

        /*
         * Everything is laid out against the screen's size, so the shell has
         * to be told the screen changed. Without this the taskbar stays where
         * the old bottom edge was, which on a larger screen is a bar floating
         * across the middle of the desktop.
         */
        recon_shell_restyle(cp->server->shell);

        set_status(cp, false, "The screen is now %d by %d.", mode.width,
            mode.height);
        break;
    }

    case ACTION_ADD_FONT: {
        /*
         * The File Explorer, the same way a wallpaper is added.
         *
         * A file dialog that hands a path back would be tidier and does not
         * exist; what does exist is a right-click on a file. This opens
         * somewhere fonts plausibly are and says what to do there.
         */
        recon_shell_open_named(cp->server->shell, "File Explorer");

        struct recon_appwin *win =
            recon_installed_app_existing("File Explorer");
        if (win == NULL) {
            set_status(cp, true, "Could not open the File Explorer.");
            break;
        }

        recon_explorer_open_at(win, recon_fs_user_dir("Documents"));
        set_status(cp, false, "Right-click a .ttf or .otf file and choose "
            "Install Font. It joins this list.");
        break;
    }

    case ACTION_REMOVE_FONT: {
        char name[96];
        if (cp->font_selected < 0 ||
                !recon_fonts_at(cp->font_scroll + cp->font_selected, name,
                    sizeof(name))) {
            set_status(cp, true, "Choose a font first.");
            break;
        }

        if (!recon_fonts_remove(name)) {
            set_status(cp, true, "%s", recon_fonts_last_error());
            break;
        }

        /* Nothing chosen afterwards: the row number would survive and point
         * at whichever font slid up into its place. */
        cp->font_selected = -1;
        set_status(cp, false, "Removed '%s'.", name);
        break;
    }

    case ACTION_RESET_READING:
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LETTER_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LINE_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Back to the defaults.");
        break;

    case ACTION_CLEAN_VIEW: {
        int count = CLEAN[cp->volume].count;
        if (cp->selected < 0 || cp->selected >= count) {
            set_status(cp, true, "Choose something first.");
            break;
        }

        const struct clean_category *item =
            &CLEAN[cp->volume].items[cp->selected];
        if (item->path == NULL) {
            set_status(cp, true, "The bin is opened from the desktop.");
            break;
        }

        /*
         * Opened in the File Explorer rather than listed here. A cleanup page
         * that grew its own file list would be a second explorer, worse than
         * the one that exists, and somebody who wants to see what is about to
         * go wants to see it in the thing they already know how to use.
         */
        recon_shell_open_named(cp->server->shell, "File Explorer");

        struct recon_appwin *win =
            recon_installed_app_existing("File Explorer");
        if (win == NULL) {
            set_status(cp, true, "Could not open the File Explorer.");
            break;
        }
        recon_explorer_open_at(win, item->path);
        set_status(cp, false, "%s, in the File Explorer.", item->path);
        break;
    }

    case ACTION_CLEAN_SYSTEM_FILES:
        /*
         * The old versions a revert would go back to. There are none: nothing
         * keeps a copy of the previous version yet, so there is nothing here
         * to offer and nothing to warn about losing.
         *
         * The button is here rather than absent because this is the one that
         * has to exist before Recovery can, and a gap nobody can see is a gap
         * nobody remembers to fill.
         */
        set_status(cp, true, "Nothing keeps previous versions yet, so there "
            "are none to remove. This is what Recovery will revert to.");
        break;

    case ACTION_CLEAN_NOW: {
        unsigned long long bytes = 0;
        int rows = 0;
        int files = 0;
        for (int i = 0; i < CLEAN[cp->volume].count && i < CLEAN_MAX; i++) {
            if (cp->clean[i].ticked) {
                bytes += cp->clean[i].bytes;
                files += cp->clean[i].files;
                rows++;
            }
        }

        if (rows == 0) {
            set_status(cp, true, "Nothing is ticked.");
            break;
        }

        char size[32];
        storage_size(bytes, size, sizeof(size));

        char message[240];
        snprintf(message, sizeof(message),
            "Permanently delete %d file%s, freeing %s?\n"
            "This cannot be undone -- these do not go to the recycle bin.",
            files, files == 1 ? "" : "s", size);

        static const char *const buttons[] = { "Clean Up", "Cancel" };
        cp->question = QUESTION_CLEAN_UP;
        recon_appwin_ask(cp->win, "Disk Cleanup", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_MEASURE_STORAGE:
        measure_storage(cp);
        set_status(cp, false, "Measured %d folders.", cp->storage_count);
        break;

    case ACTION_EMPTY_BIN: {
        /*
         * The bin of the space that is showing. Each of the three has its
         * own, and one button that emptied all of them would be a button
         * nobody could press safely -- deleting a document and deleting a
         * system file are not the same act.
         */
        int count = recon_fs_trash_count_in(cp->volume);
        if (count == 0) {
            set_status(cp, false, "The %s bin is already empty.",
                recon_volume_name(cp->volume));
            break;
        }

        /*
         * Asked about, because this is the one button on the page that
         * destroys anything. Emptying the bin is what the bin is for and is
         * still not undoable.
         */
        char message[192];
        snprintf(message, sizeof(message),
            "Permanently delete %d item%s in the %s recycle bin?\n"
            "This cannot be undone.", count, count == 1 ? "" : "s",
            recon_volume_name(cp->volume));

        static const char *const buttons[] = { "Empty", "Cancel" };
        cp->question = QUESTION_EMPTY_BIN;
        recon_appwin_ask(cp->win, "Empty Recycle Bin", message, buttons, 2,
            answered);
        break;
    }

    /* --- Skins --- */

    case ACTION_REMOVE_WALLPAPER: {
        char name[96];
        if (!recon_wallpaper_at(cp->paper_scroll + cp->selected, name,
                sizeof(name))) {
            set_status(cp, true, "Choose a picture first.");
            break;
        }
        if (!recon_wallpaper_remove(name)) {
            set_status(cp, true, "%s", recon_wallpaper_last_error());
            break;
        }
        /*
         * Nothing chosen afterwards. The row number would survive the removal
         * and point at whichever picture slid up into its place, arming
         * Remove against something nobody picked.
         */
        cp->selected = -1;
        set_status(cp, false, "Removed '%s'.", name);
        break;
    }

    case ACTION_ADD_WALLPAPER: {
        /*
         * The File Explorer, at Pictures, where the picture is.
         *
         * A file dialog that hands a path back would be tidier and does not
         * exist; what does exist is a right-click on any picture anywhere
         * that makes it the background. So this opens the place they are and
         * says what to do there, which is the same number of clicks and one
         * fewer thing to build.
         */
        recon_shell_open_named(cp->server->shell, "File Explorer");

        struct recon_appwin *win =
            recon_installed_app_existing("File Explorer");
        if (win == NULL) {
            set_status(cp, true, "Could not open the File Explorer.");
            break;
        }

        recon_explorer_open_at(win, recon_fs_user_dir("Pictures"));
        set_status(cp, false, "Right-click a picture and choose Set as "
            "Desktop Background. It joins this list.");
        break;
    }

    case ACTION_USE_SKIN: {
        struct recon_theme_info info;
        if (!recon_theme_at(cp->selected, &info)) {
            set_status(cp, true, "Choose a skin first.");
            break;
        }
        if (!recon_theme_set(info.name)) {
            set_status(cp, true, "%s", recon_theme_last_error());
            break;
        }
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Skin is now '%s'.", info.name);
        break;
    }

    /*
     * --- Customize Skin ---
     *
     * Asked about before anything happens. Making a skin leaves a file behind
     * with a name in it, and a button that quietly creates something on a
     * single click is a button people learn to be careful of. One question,
     * then a name and a line describing it, then the editor in its own
     * window.
     */
    case ACTION_NEW_SKIN:
        /*
         * Three starting points rather than a blank one. Forty-eight roles
         * all have to hold a colour for anything to draw, so "empty" is not
         * an available state -- the only real question is which finished set
         * to start from, and light, dark and high-contrast are the three
         * answers that lead somewhere different.
         */
        cp->question = QUESTION_NEW_SKIN;
        recon_appwin_ask(cp->win, "New Skin",
            "A skin needs all forty-eight of its colours set, so a new one "
            "starts from an existing set rather than from nothing.\n\n"
            "Which one should it start from?",
            (const char *const[]){ "Light", "Dark", "High Contrast",
                "Cancel" }, 4, answered);
        break;

    case ACTION_COPY_SKIN: {
        struct recon_theme_info info;
        if (!recon_theme_at(cp->selected, &info)) {
            set_status(cp, true, "Choose a skin first.");
            break;
        }

        char message[240];
        snprintf(message, sizeof(message),
            "This makes your own copy of %s that you can change. %s stays as "
            "it is.\n\nYou will be asked for a name for it.", info.name,
            info.name);

        static const char *const buttons[] = { "Customize", "Cancel" };
        cp->question = QUESTION_CUSTOMIZE_SKIN;
        recon_appwin_ask(cp->win, "Customize Skin", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_BEGIN_NAMING_SKIN: {
        struct recon_theme_info info;
        if (!source_skin(cp, &info)) {
            set_status(cp, true, "Choose a skin first.");
            break;
        }

        /* Suggested rather than empty: a name is wanted, and "My Beacon" is a
         * better thing to correct than a blank field is to fill. */
        char suggestion[sizeof(info.name) + 8];
        snprintf(suggestion, sizeof(suggestion), "My %s", info.name);

        char about[96];
        snprintf(about, sizeof(about), "%s, with my own colours", info.name);

        cp->naming_skin = true;
        cp->skin_desc_focused = false;
        cp->section = APPEARANCE_THEMES;
        recon_edit_begin(&cp->skin_new_name, suggestion, false);
        recon_edit_begin(&cp->skin_new_desc, about, false);
        set_status(cp, false, "A name, and a line saying what it is.");
        break;
    }

    case ACTION_CONFIRM_COPY_SKIN: {
        struct recon_theme_info info;
        if (!source_skin(cp, &info)) {
            set_status(cp, true, "Choose a skin first.");
            break;
        }

        /*
         * Refused rather than shortened. A skin quietly saved under half the
         * name somebody typed is a file they will not find again, and the
         * name is also the file name.
         */
        char name[sizeof(info.name)];
        size_t length = strlen(cp->skin_new_name.text);
        if (length >= sizeof(name)) {
            set_status(cp, true, "That name is too long -- %zu characters at "
                "most.", sizeof(name) - 1);
            break;
        }
        /* Copied by the length just checked rather than by a formatter, so
         * the bound is one the compiler can see too. */
        memcpy(name, cp->skin_new_name.text, length + 1);

        /* The line describing it, if one was typed. Empty is allowed and
         * means the copy carries the original's own description. */
        const char *about = cp->skin_new_desc.text[0] != '\0'
            ? cp->skin_new_desc.text : NULL;

        if (!recon_theme_copy(info.name, name, about)) {
            set_status(cp, true, "%s", recon_theme_last_error());
            break;
        }

        cp->naming_skin = false;
        cp->new_skin_from[0] = '\0';
        recon_edit_end(&cp->skin_new_name);
        recon_edit_end(&cp->skin_new_desc);

        /*
         * Put the copy on and open it for editing. Somebody who has just
         * copied a skin is about to change it, and looking at the thing you
         * are changing is the point of doing it here rather than in a text
         * file.
         */
        recon_theme_set(name);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);

        open_skin_editor(cp, name);
        set_status(cp, false, "'%s' is yours. Pick a colour to change it.",
            name);
        break;
    }

    case ACTION_EDIT_SKIN:
        /*
         * From the Appearance window this opens the editor; inside the editor
         * the same action changes the chosen colour. One name for one idea --
         * "edit this colour" -- reached from two places that mean it at two
         * different scales.
         */
        if (!cp->skin_editing) {
            struct recon_theme_info info;
            if (!recon_theme_at(cp->selected, &info)) {
                set_status(cp, true, "Choose a skin first.");
                break;
            }
            if (info.built_in) {
                set_status(cp, true, "%s is built in. Customize it first.",
                    info.name);
                break;
            }

            /* The colours shown are the ones on screen, so the skin being
             * edited has to be the skin in use. */
            if (strcmp(info.name, recon_theme_current()) != 0) {
                recon_theme_set(info.name);
                recon_access_apply(cp->font);
                recon_shell_restyle(cp->server->shell);
            }

            open_skin_editor(cp, info.name);
            break;
        }

        if (cp->skin_editing) {
            /* Inside the editor this button changes the chosen colour. */
            cp->skin_value_editing = true;

            char current[16];
            colour_text(recon_theme_color((enum recon_theme_role)cp->skin_row),
                current, sizeof(current));
            recon_edit_begin(&cp->skin_value, current, false);
            set_status(cp, false, "%s, as RRGGBB or AARRGGBB.",
                recon_theme_role_name((enum recon_theme_role)cp->skin_row));
            break;
        }

        {
            struct recon_theme_info info;
            if (!recon_theme_at(cp->selected, &info)) {
                set_status(cp, true, "Choose a skin first.");
                break;
            }
            if (info.built_in) {
                set_status(cp, true,
                    "'%s' is built in. Copy it first, then edit the copy.",
                    info.name);
                break;
            }

            /* Editing a skin means looking at it. Changing one you cannot see
             * is guessing at hex numbers. */
            recon_theme_set(info.name);
            recon_access_apply(cp->font);
            recon_shell_restyle(cp->server->shell);

            snprintf(cp->skin_name, sizeof(cp->skin_name), "%s", info.name);
            cp->skin_editing = true;
            cp->skin_row = 0;
            cp->skin_scroll = 0;
            cp->skin_value_editing = false;
            clear_status(cp);
        }
        break;

    case ACTION_SKIN_SET: {
        recon_color colour;
        const char *typed = cp->skin_value.text;

        if (!colour_parse(typed, &colour)) {
            set_status(cp, true,
                "'%s' is not a colour. Six digits, or eight with an alpha.",
                typed);
            break;
        }

        if (!recon_theme_set_role(cp->skin_name,
                (enum recon_theme_role)cp->skin_row, colour)) {
            set_status(cp, true, "%s", recon_theme_last_error());
            break;
        }

        /* Said before the field is closed, because closing it clears the text
         * this sentence is quoting -- which is how it came out as "bar is
         * now ." the first time. */
        set_status(cp, false, "%s is now %s.",
            recon_theme_role_name((enum recon_theme_role)cp->skin_row), typed);

        cp->skin_value_editing = false;
        recon_edit_end(&cp->skin_value);

        /* Everything on screen is drawn from this palette, so the change is
         * visible before the sentence saying it happened. */
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_SKIN_FLAT:
        if (!recon_theme_set_gradient(cp->skin_name,
                (enum recon_theme_role)cp->skin_row, false, 0)) {
            set_status(cp, true, "%s", recon_theme_last_error());
            break;
        }
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "%s is flat now.",
            recon_theme_role_name((enum recon_theme_role)cp->skin_row));
        break;

    case ACTION_SKIN_CANCEL:
        /* One button for both, because it means the same thing in both: put
         * away whatever is half-typed and leave what is saved alone. */
        if (cp->skin_value_editing) {
            cp->skin_value_editing = false;
            recon_edit_end(&cp->skin_value);
        }
        if (cp->naming_skin) {
            cp->naming_skin = false;
            cp->new_skin_from[0] = '\0';
            recon_edit_end(&cp->skin_new_name);
        }
        clear_status(cp);
        break;

    /* --- Firewall --- */

    case ACTION_FIREWALL_TOGGLE: {
        bool on = !recon_firewall_is_on();
        if (!recon_firewall_set_on(on)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }
        /*
         * Said plainly, both ways. Turning a firewall off is not a neutral
         * act and the sentence should not read like one.
         */
        if (on) {
            set_status(cp, false, "The firewall is on. The rules apply.");
        } else {
            set_status(cp, true,
                "The firewall is off. Nothing is being enforced.");
        }
        break;
    }

    case ACTION_FIREWALL_DEFAULT_IN:
    case ACTION_FIREWALL_DEFAULT_OUT: {
        enum recon_fw_direction direction =
            (action == ACTION_FIREWALL_DEFAULT_IN) ? RECON_FW_IN
                                                   : RECON_FW_OUT;
        enum recon_fw_action was = recon_firewall_default(direction);
        enum recon_fw_action now =
            (was == RECON_FW_ALLOW) ? RECON_FW_BLOCK : RECON_FW_ALLOW;

        if (!recon_firewall_set_default(direction, now)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }

        set_status(cp, now == RECON_FW_BLOCK && direction == RECON_FW_OUT,
            "%s traffic that no rule matches is %s now.",
            direction == RECON_FW_IN ? "Incoming" : "Outgoing",
            recon_fw_action_name(now));
        break;
    }

    case ACTION_FIREWALL_RULE_TOGGLE: {
        struct recon_fw_rule rule;
        if (!recon_firewall_at(cp->selected, &rule)) {
            set_status(cp, true, "Choose a rule first.");
            break;
        }
        if (!recon_firewall_set_rule_on(cp->selected, !rule.enabled)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }
        set_status(cp, false, "'%s' is %s.", rule.name,
            rule.enabled ? "off" : "on");
        break;
    }

    case ACTION_FIREWALL_RULE_ACTION: {
        struct recon_fw_rule rule;
        if (!recon_firewall_at(cp->selected, &rule)) {
            set_status(cp, true, "Choose a rule first.");
            break;
        }

        int index = cp->selected;
        rule.action = (rule.action == RECON_FW_ALLOW) ? RECON_FW_BLOCK
                                                     : RECON_FW_ALLOW;

        /*
         * Removed and re-added, then moved back to where it was. The rule
         * store has no "change this one" and does not need one -- but the
         * order is part of the rule, so an edit that quietly moved it to the
         * end of the list would change what it does.
         */
        if (!recon_firewall_remove(index) || !recon_firewall_add(&rule)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }
        int last = recon_firewall_count() - 1;
        if (last != index) {
            recon_firewall_move(last, index - last);
        }

        set_status(cp, false, "'%s' now says %s.", rule.name,
            recon_fw_action_name(rule.action));
        break;
    }

    /*
     * Stepped through a list rather than typed. The useful values are a short
     * list -- one, two, five, ten, fifteen, thirty, an hour -- and a field
     * that accepts 7 is a field somebody has to be told 7 is allowed in.
     */
    case ACTION_BLANK_LONGER:
    case ACTION_BLANK_SHORTER: {
        int minutes = recon_registry_get_int(RECON_REG_USER,
            RECON_BLANK_AFTER_KEY, 0);

        int at = 0;
        for (int i = 0; i < BLANK_STEP_COUNT; i++) {
            if (BLANK_STEPS[i] == minutes) {
                at = i;
                break;
            }
            /* A value from a hand-edited registry that is not on the list
             * lands on the nearest step below it, rather than jumping to
             * never. */
            if (BLANK_STEPS[i] < minutes) {
                at = i;
            }
        }

        at += (action == ACTION_BLANK_LONGER) ? 1 : -1;
        if (at < 0) {
            at = 0;
        }
        if (at >= BLANK_STEP_COUNT) {
            at = BLANK_STEP_COUNT - 1;
        }

        recon_registry_set_int(RECON_REG_USER, RECON_BLANK_AFTER_KEY,
            BLANK_STEPS[at]);
        recon_shell_blank_reload(cp->server->shell);

        char label[48];
        blank_label(BLANK_STEPS[at], label, sizeof(label));
        set_status(cp, false, "The screen blanks: %s.", label);
        break;
    }

    case ACTION_BLANK_LOCK: {
        bool lock = !recon_registry_get_bool(RECON_REG_USER,
            RECON_BLANK_LOCK_KEY, false);
        recon_registry_set_bool(RECON_REG_USER, RECON_BLANK_LOCK_KEY, lock);
        recon_shell_blank_reload(cp->server->shell);
        set_status(cp, false, lock
            ? "Waking the screen will ask for your password."
            : "Waking the screen goes straight back to your desktop.");
        break;
    }

    case ACTION_FIREWALL_ADD:
        cp->fw_adding = true;
        cp->fw_custom = false;
        clear_status(cp);
        break;

    case ACTION_FIREWALL_ADD_CUSTOM:
        cp->fw_custom = true;
        cp->fw_port_focused = false;
        cp->fw_direction = RECON_FW_IN;
        cp->fw_protocol = RECON_FW_TCP;
        cp->fw_action = RECON_FW_ALLOW;
        recon_edit_begin(&cp->fw_name, "", false);
        recon_edit_begin(&cp->fw_port, "", false);
        set_status(cp, false, "A name, a port, and what to do about it.");
        break;

    case ACTION_FIREWALL_ADD_CANCEL:
        if (cp->fw_custom) {
            /* Back to the presets rather than out of adding altogether:
             * Cancel on the second step undoes the second step. */
            cp->fw_custom = false;
            recon_edit_end(&cp->fw_name);
            recon_edit_end(&cp->fw_port);
        } else {
            cp->fw_adding = false;
        }
        clear_status(cp);
        break;

    /*
     * Three values cycling rather than three sets of radio buttons. Each
     * button says which value it is on, so the control and its readout are
     * the same object and there is nothing to keep in step.
     */
    case ACTION_FIREWALL_CUSTOM_DIRECTION:
        cp->fw_direction = cp->fw_direction == RECON_FW_IN
            ? RECON_FW_OUT : RECON_FW_IN;
        break;

    case ACTION_FIREWALL_CUSTOM_PROTOCOL:
        cp->fw_protocol = cp->fw_protocol == RECON_FW_TCP ? RECON_FW_UDP
            : cp->fw_protocol == RECON_FW_UDP ? RECON_FW_ANY_PROTOCOL
            : RECON_FW_TCP;
        break;

    case ACTION_FIREWALL_CUSTOM_ACTION:
        cp->fw_action = cp->fw_action == RECON_FW_ALLOW
            ? RECON_FW_BLOCK : RECON_FW_ALLOW;
        break;

    case ACTION_FIREWALL_CUSTOM_CONFIRM: {
        struct recon_fw_rule rule;
        memset(&rule, 0, sizeof(rule));

        /*
         * Refused rather than shortened. The name is what the log says when
         * the rule fires, and half a name in a log is a rule nobody can find
         * again.
         */
        size_t length = strlen(cp->fw_name.text);
        if (length >= sizeof(rule.name)) {
            set_status(cp, true, "That name is too long -- %zu characters at "
                "most.", sizeof(rule.name) - 1);
            break;
        }
        memcpy(rule.name, cp->fw_name.text, length + 1);

        rule.direction = cp->fw_direction;
        rule.protocol = cp->fw_protocol;
        rule.action = cp->fw_action;
        rule.enabled = true;

        /*
         * "80", or "27015-27030", or nothing for every port. Read here rather
         * than in the firewall, because this is the only place a person types
         * one: the file has two numbers in it and always did.
         */
        const char *text = cp->fw_port.text;
        if (text[0] != '\0') {
            int from = 0;
            int to = 0;
            int read = sscanf(text, "%d-%d", &from, &to);
            if (read < 1 || from < 0 || from > 65535) {
                set_status(cp, true,
                    "A port is a number from 0 to 65535, or two with a dash "
                    "between them.");
                break;
            }
            if (read == 1) {
                to = from;
            }
            if (to < from || to > 65535) {
                set_status(cp, true, "The end of the range comes after the "
                    "start of it.");
                break;
            }
            rule.port_from = from;
            rule.port_to = to;
        }

        if (!recon_firewall_add(&rule)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }

        cp->fw_adding = false;
        cp->fw_custom = false;
        recon_edit_end(&cp->fw_name);
        recon_edit_end(&cp->fw_port);
        cp->selected = recon_firewall_count() - 1;
        set_status(cp, false, "Added '%s'. It is at the end of the list.",
            rule.name);
        break;
    }

    case ACTION_FIREWALL_REMOVE: {
        struct recon_fw_rule going;
        if (!recon_firewall_at(cp->selected, &going)) {
            set_status(cp, true, "Choose a rule first.");
            break;
        }
        if (!recon_firewall_remove(cp->selected)) {
            set_status(cp, true, "%s", recon_firewall_last_error());
            break;
        }
        /* Nothing chosen: the row number would survive the removal and
         * point at whichever rule slid up into its place, arming Remove
         * against something nobody picked. */
        cp->selected = -1;
        set_status(cp, false, "Removed '%s'.", going.name);
        break;
    }

    case ACTION_FIREWALL_RULE_UP:
    case ACTION_FIREWALL_RULE_DOWN: {
        int by = (action == ACTION_FIREWALL_RULE_UP) ? -1 : 1;
        if (!recon_firewall_move(cp->selected, by)) {
            break;
        }
        cp->selected += by;

        struct recon_fw_rule rule;
        if (recon_firewall_at(cp->selected, &rule)) {
            set_status(cp, false, "'%s' is rule %d now. The first match "
                "decides, so this changes what it does.", rule.name,
                cp->selected + 1);
        }
        break;
    }

    case ACTION_SHOW_CHANGES: {
        /* Help, at this version's entry. The whole log is under it, back to
         * the first version, which is what "all changes" means. */
        char version[48];
        snprintf(version, sizeof(version), "v%s", RECONOS_VERSION);

        recon_shell_open_named(cp->server->shell, "Help");
        recon_help_show_topic(recon_installed_app_existing("Help"), version);
        clear_status(cp);
        break;
    }

    case ACTION_SKIN_DONE:
        cp->skin_editing = false;
        cp->skin_value_editing = false;
        recon_edit_end(&cp->skin_value);
        set_status(cp, false, "'%s' is saved. It is a file in %s.",
            cp->skin_name, RECON_DIR_THEMES);
        break;
    }
}

static void panel_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct control_panel *cp = user;
    (void)cx;
    (void)cy;

    if (!cp->home) {
        return;
    }

    int was = cp->hover_tile;
    cp->hover_tile = (hit_id >= HIT_TILE_BASE &&
        hit_id < HIT_TILE_BASE + PAGE_COUNT)
        ? (int)(hit_id - HIT_TILE_BASE) : -1;
    /*
     * Redrawn only when the tile under the pointer changes, not on every
     * pixel of movement. Repainting fourteen icons sixty times a second to
     * light the same one it already lit is a desktop that feels heavy for no
     * reason.
     */
    if (was != cp->hover_tile) {
        recon_appwin_refresh(cp->win);
    }
}

static bool panel_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct control_panel *cp = user;
    (void)cx;
    (void)cy;

    if (!pressed) {
        return false;
    }

    /* A wallpaper chosen from the Appearance page. */
    /*
     * Highest base first, all the way down. Every test in this ladder is an
     * unbounded >=, so an id that belongs to a base above the one being
     * tested is answered by the wrong branch and vanishes without a trace.
     */
    if (hit_id >= HIT_ZONE_BASE) {
        int row = (int)(hit_id - HIT_ZONE_BASE);
        if (recon_clock_set_zone(cp->zone_scroll + row)) {
            char zone[RECON_CLOCK_ZONE_NAME_MAX];
            recon_clock_zone_label(zone, sizeof(zone));

            /* The taskbar shows the time too, and it is the reason somebody
             * came here. It has to change while they are looking at it. */
            recon_shell_restyle(cp->server->shell);
            set_status(cp, false, "The clock is now on %s.", zone);
        }
        return true;
    }

    if (hit_id >= HIT_MODE_BASE) {
        int row = (int)(hit_id - HIT_MODE_BASE);
        cp->mode_selected = (cp->mode_selected == row) ? -1 : row;
        clear_status(cp);
        return true;
    }

    if (hit_id >= HIT_FONT_BASE) {
        int row = (int)(hit_id - HIT_FONT_BASE);
        cp->font_selected = (cp->font_selected == row) ? -1 : row;
        clear_status(cp);
        return true;
    }

    if (hit_id >= HIT_NET_ROW_BASE) {
        int row = (int)(hit_id - HIT_NET_ROW_BASE);
        cp->net_selected = (cp->net_selected == row) ? -1 : row;
        clear_status(cp);
        return true;
    }

    if (hit_id >= HIT_NET_TAB_BASE) {
        int tab = (int)(hit_id - HIT_NET_TAB_BASE);
        if (tab >= 0 && tab < NETWORK_SECTIONS) {
            cp->net_section = (enum network_section)tab;
            /* A different list: neither the selection nor the scroll from the
             * one before means anything in it. */
            cp->net_selected = -1;
            cp->net_scroll = 0;
            clear_status(cp);
        }
        return true;
    }

    if (hit_id >= HIT_PROGRAMS_TAB_BASE) {
        int tab = (int)(hit_id - HIT_PROGRAMS_TAB_BASE);
        if (tab >= 0 && tab < PROGRAMS_TABS) {
            cp->programs = (enum programs_tab)tab;
            /* A different list: the selection and the scroll belong to the
             * one that was showing. */
            cp->selected = -1;
            cp->programs_scroll = 0;
            cp->installing = false;
            clear_status(cp);
        }
        return true;
    }

    if (hit_id >= HIT_CLEAN_BASE) {
        int index = (int)(hit_id - HIT_CLEAN_BASE);
        if (index >= 0 && index < CLEAN[cp->volume].count &&
                index < CLEAN_MAX) {
            /*
             * Clicking a row both chooses it and ticks it. Two targets in a
             * row -- a box that ticks and a body that selects -- is a row
             * where half of it does something different from the other half
             * for no reason a reader can see.
             */
            cp->selected = index;
            cp->clean[index].ticked = !cp->clean[index].ticked;
            clear_status(cp);
        }
        return true;
    }

    if (hit_id >= HIT_VOLUME_BASE) {
        int volume = (int)(hit_id - HIT_VOLUME_BASE);
        if (volume >= 0 && volume < RECON_VOLUME_COUNT) {
            cp->volume = (enum recon_volume)volume;
            /* Measured again: this is a different space, and the rows on
             * screen are the last one's. */
            cp->storage_measured = false;
            cp->clean_measured = false;

            /*
             * And nothing is ticked. The ticks belong to the rows they were
             * put on, and the rows have just changed -- carrying them across
             * would mean pressing Clean Up deleted something nobody looked
             * at.
             */
            for (int i = 0; i < CLEAN_MAX; i++) {
                cp->clean[i].ticked = false;
            }

            cp->selected = -1;
            clear_status(cp);
        }
        return true;
    }

    if (hit_id >= HIT_PRESET_BASE) {
        int index = (int)(hit_id - HIT_PRESET_BASE);
        if (index >= 0 && index < FW_PRESET_COUNT) {
            add_preset_rule(cp, &FW_PRESETS[index]);
        }
        return true;
    }

    if (hit_id >= HIT_SECTION_BASE) {
        int section = (int)(hit_id - HIT_SECTION_BASE);
        if (section >= 0 && section < APPEARANCE_SECTIONS) {
            cp->section = (enum appearance_section)section;
            /* Leaving a section leaves whatever was half-done in it. Coming
             * back to a half-typed colour three sections later is a
             * surprise, not a convenience. */
            cp->naming_skin = false;
            cp->skin_value_editing = false;
            /*
             * Nothing chosen yet, rather than row zero chosen. Row zero would
             * arm Remove against a picture nobody pointed at.
             */
            cp->selected = -1;
            clear_status(cp);
        }
        return true;
    }

    /*
     * The tiles next, because this is a ladder of unbounded `>=` tests and
     * the highest base has to be asked about before every lower one. Tile 1
     * is 1701, which is also "row 601" and "wallpaper 1101" to any test that
     * runs before this one -- the click went to the row branch and vanished.
     */
    if (hit_id >= HIT_TILE_BASE) {
        int tile = (int)(hit_id - HIT_TILE_BASE);
        if (tile < 0 || tile >= PAGE_COUNT) {
            return true;
        }

        /*
         * One click chooses, two opens.
         *
         * A single click that opened a window meant there was no way to point
         * at something and find out what it was without also going into it.
         * Now the first click picks it out and the tooltip says what it does;
         * the second opens it.
         */
        cp->selected = tile;
        if (recon_click_is_double(hit_id)) {
            open_page_window(cp, (enum page)tile);
        } else {
            clear_status(cp);
        }
        return true;
    }

    if (hit_id >= HIT_WALLPAPER_BASE) {
        /* Screen row plus where the list is scrolled to. */
        int index = cp->paper_scroll + (int)(hit_id - HIT_WALLPAPER_BASE);

        /*
         * One click chooses the row, two puts the picture on. The same rule
         * as everywhere else, and here it is the only rule that works:
         * applying on a single click would make the chosen row and the
         * showing row the same row always, and Remove refuses the one
         * showing -- so the button could never be reached.
         */
        cp->selected = (int)(hit_id - HIT_WALLPAPER_BASE);

        char name[96];
        if (!recon_wallpaper_at(index, name, sizeof(name))) {
            return true;
        }

        if (!recon_click_is_double(hit_id)) {
            set_status(cp, false, "'%s' chosen. Double-click to put it on.",
                name);
            return true;
        }

        if (recon_wallpaper_set(name)) {
            recon_background_reload(cp->server);
            set_status(cp, false, "Wallpaper is now '%s'.", name);
        }
        return true;
    }

    /* A picture chosen from the grid. */
    if (hit_id >= HIT_AVATAR_BASE) {
        int index = (int)(hit_id - HIT_AVATAR_BASE);

        char wanted[64] = "";
        if (index > 0 && !recon_avatar_at(index - 1, wanted, sizeof(wanted))) {
            return true;
        }

        if (!recon_avatar_set(cp->question_target, wanted)) {
            set_status(cp, true, "Could not change the picture.");
            return true;
        }
        cp->picking_avatar = false;
        /* The Start menu and the login screen show it too. */
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "%s's picture is set.", cp->question_target);
        return true;
    }

    /* A row on a page of things that are not built: say what is missing. */
    if (hit_id >= HIT_PENDING_BASE) {
        int index = (int)(hit_id - HIT_PENDING_BASE);
        if (index >= 0 && index < PENDING[cp->page].count) {
            set_status(cp, false, "%s",
                PENDING[cp->page].items[index].blocked);
        }
        return true;
    }

    if (hit_id >= HIT_FIELD_BASE) {
        /* The registry's two fields are numbered above the account page's,
         * so a click lands on the one that was drawn there. */
        if (hit_id == HIT_FIELD_BASE + 2) {
            cp->reg_key_focused = true;
            return true;
        }
        if (hit_id == HIT_FIELD_BASE + 3) {
            cp->reg_key_focused = false;
            return true;
        }
        /* A custom firewall rule: its name, then its port. */
        if (hit_id == HIT_FIELD_BASE + 8) {
            cp->fw_port_focused = false;
            return true;
        }
        if (hit_id == HIT_FIELD_BASE + 9) {
            cp->fw_port_focused = true;
            return true;
        }

        /* Naming a skin: the name, then the line describing it. */
        if (hit_id == HIT_FIELD_BASE + 5) {
            cp->skin_desc_focused = false;
            return true;
        }
        if (hit_id == HIT_FIELD_BASE + 7) {
            cp->skin_desc_focused = true;
            return true;
        }
        cp->password_focused = (hit_id > HIT_FIELD_BASE);
        return true;
    }
    if (hit_id >= HIT_ACTION_BASE) {
        do_action(cp, (enum action)(hit_id - HIT_ACTION_BASE));
        return true;
    }
    if (hit_id >= HIT_ROW_BASE) {
        int index = (int)(hit_id - HIT_ROW_BASE);

        if (cp->page == PAGE_FIREWALL) {
            /* Screen row plus where the list is scrolled to. */
            cp->selected = cp->fw_scroll + index;
            return true;
        }

        if (cp->page == PAGE_PROGRAMS) {
            cp->selected = cp->programs_scroll + index;
            return true;
        }

        if (cp->page == PAGE_APPEARANCE && cp->skin_editing) {
            /* The rows are numbered from the first one showing, so a click
             * on the third row means the third *visible* role. */
            cp->skin_row = cp->skin_scroll + index;
            cp->skin_value_editing = false;
            return true;
        }

        if (cp->page == PAGE_APPEARANCE &&
                cp->section == APPEARANCE_COLOURS) {
            /* The rows are numbered from the first one showing, so a click on
             * the third row means the third *visible* role. */
            cp->skin_row = cp->skin_scroll + index;
            cp->skin_value_editing = false;
            return true;
        }

        if (cp->page == PAGE_APPEARANCE) {
            index += cp->theme_scroll;
        }

        if (cp->page == PAGE_APPEARANCE) {
            /*
             * Choosing a skin picks it out; Use This Skin puts it on.
             *
             * It used to apply on the click, which made choosing one to copy
             * indistinguishable from changing the whole desktop -- somebody
             * clicking down a list to read the descriptions restyled the
             * system nine times on the way.
             */
            struct recon_theme_info info;
            if (recon_theme_at(index, &info)) {
                cp->selected = index;
                set_status(cp, false, "%s. %s", info.name, info.description);
            }
            return true;
        }

        if (cp->page == PAGE_REGISTRY) {
            /* The rows are numbered from the first one showing, and the
             * selection is an index into the whole hive. */
            cp->selected = cp->registry_scroll + index;
            return true;
        }

        cp->selected = index;
        return true;
    }
    return true;
}

static bool panel_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct control_panel *cp = user;

    /* Typing the path of something to install. */
    if (cp->installing) {
        switch (recon_edit_key(&cp->name, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_CONFIRM_INSTALL);
            return true;
        case RECON_EDIT_CANCEL:
            cp->installing = false;
            recon_edit_end(&cp->name);
            clear_status(cp);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /* Naming a copy of a skin. */
    if (cp->naming_skin) {
        /* Tab moves between the name and the line describing it, which is
         * what Tab does in every other pair of fields here. */
        if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
            cp->skin_desc_focused = !cp->skin_desc_focused;
            return true;
        }

        struct recon_edit *field = cp->skin_desc_focused
            ? &cp->skin_new_desc : &cp->skin_new_name;

        switch (recon_edit_key(field, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_CONFIRM_COPY_SKIN);
            return true;
        case RECON_EDIT_CANCEL:
            do_action(cp, ACTION_SKIN_CANCEL);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /* A custom firewall rule: its name and its port. */
    if (cp->fw_adding && cp->fw_custom) {
        if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
            cp->fw_port_focused = !cp->fw_port_focused;
            return true;
        }

        struct recon_edit *field = cp->fw_port_focused
            ? &cp->fw_port : &cp->fw_name;

        switch (recon_edit_key(field, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_FIREWALL_CUSTOM_CONFIRM);
            return true;
        case RECON_EDIT_CANCEL:
            do_action(cp, ACTION_FIREWALL_ADD_CANCEL);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /* Typing a colour into the skin editor. */
    if (cp->skin_value_editing) {
        switch (recon_edit_key(&cp->skin_value, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_SKIN_SET);
            return true;
        case RECON_EDIT_CANCEL:
            do_action(cp, ACTION_SKIN_CANCEL);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /*
     * Walking the roles. Forty-eight of them is more than anybody wants to
     * click through, and Enter opening the field is what makes changing
     * several in a row bearable.
     */
    if (cp->skin_editing) {
        switch (sym) {
        case XKB_KEY_Up:
            if (cp->skin_row > 0) {
                cp->skin_row--;
            }
            return true;
        case XKB_KEY_Down:
            if (cp->skin_row < RECON_THEME_ROLE_COUNT - 1) {
                cp->skin_row++;
            }
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            do_action(cp, ACTION_EDIT_SKIN);
            return true;
        case XKB_KEY_Escape:
            do_action(cp, ACTION_SKIN_DONE);
            return true;
        default:
            return false;
        }
    }

    /*
     * Changing or adding a setting. Tab moves between the two fields while
     * adding; there is only one to type in when changing, because the key of
     * an existing setting is not editable.
     */
    if (cp->page == PAGE_REGISTRY &&
            (cp->registry_editing || cp->registry_adding)) {
        if (cp->registry_adding && sym == XKB_KEY_Tab) {
            cp->reg_key_focused = !cp->reg_key_focused;
            return true;
        }

        struct recon_edit *edit = (cp->registry_adding && cp->reg_key_focused)
            ? &cp->reg_key : &cp->reg_value;

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_REGISTRY_SAVE);
            return true;
        case RECON_EDIT_CANCEL:
            do_action(cp, ACTION_REGISTRY_CANCEL);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /* The registry's password field has the keyboard while it is showing. */
    if (cp->page == PAGE_REGISTRY && !cp->registry_unlocked) {
        switch (recon_edit_key(&cp->unlock, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_UNLOCK_REGISTRY);
            return true;
        case RECON_EDIT_CANCEL:
            recon_edit_begin(&cp->unlock, "", false);
            cp->unlock.masked = true;
            clear_status(cp);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    if (cp->page == PAGE_REGISTRY && cp->registry_unlocked) {
        if (sym == XKB_KEY_Up) {
            cp->registry_scroll--;
            return true;
        }
        if (sym == XKB_KEY_Down) {
            cp->registry_scroll++;
            return true;
        }
    }

    if (cp->editing) {
        if (sym == XKB_KEY_Tab && !cp->editing_password_only) {
            cp->password_focused = !cp->password_focused;
            return true;
        }

        struct recon_edit *edit = cp->password_focused
            ? &cp->password : &cp->name;

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, cp->editing_password_only
                ? ACTION_SET_PASSWORD : ACTION_ADD_USER);
            return true;
        case RECON_EDIT_CANCEL:
            stop_editing(cp);
            clear_status(cp);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    switch (sym) {
    case XKB_KEY_Up:
        if (cp->selected > 0) {
            cp->selected--;
        }
        return true;
    case XKB_KEY_Down:
        cp->selected++;
        return true;
    default:
        return false;
    }
}

static void panel_scroll(void *user, double delta) {
    struct control_panel *cp = user;

    /*
     * Every list in Appearance takes the wheel.
     *
     * Only the skin editor did, which meant the Colours section said "scroll
     * for the rest" under a list that could not be scrolled -- a page telling
     * somebody to do something it would not let them do. Themes and
     * Wallpapers are here for the same reason in advance: neither overflows
     * today, and both will the first time somebody adds a tenth skin or a
     * sixth picture.
     *
     * Only floored here. The upper bound belongs where the list is drawn,
     * which is the only place that knows how many rows there are and how many
     * fit.
     */
    int step = (delta > 0) ? 3 : -3;

    if (cp->page == PAGE_APPEARANCE) {
        int *at = cp->skin_editing ? &cp->skin_scroll
            : cp->section == APPEARANCE_COLOURS ? &cp->skin_scroll
            : cp->section == APPEARANCE_WALLPAPERS ? &cp->paper_scroll
            : &cp->theme_scroll;

        *at += step;
        if (*at < 0) {
            *at = 0;
        }
        return;
    }

    if (cp->page == PAGE_DISPLAY) {
        cp->font_scroll += step;
        if (cp->font_scroll < 0) {
            cp->font_scroll = 0;
        }
        return;
    }

    if (cp->page == PAGE_NETWORK) {
        /* Three of the four sections show a list; Status does not, and
         * scrolling a page with no list is not an error worth a message. */
        cp->net_scroll += step;
        if (cp->net_scroll < 0) {
            cp->net_scroll = 0;
        }
        return;
    }

    if (cp->page == PAGE_FIREWALL) {
        cp->fw_scroll += step;
        if (cp->fw_scroll < 0) {
            cp->fw_scroll = 0;
        }
        return;
    }

    if (cp->page == PAGE_PROGRAMS) {
        cp->programs_scroll += step;
        if (cp->programs_scroll < 0) {
            cp->programs_scroll = 0;
        }
        return;
    }

    if (cp->page == PAGE_REGISTRY && cp->registry_unlocked) {
        cp->registry_scroll += (delta > 0) ? 3 : -3;
        if (cp->registry_scroll < 0) {
            cp->registry_scroll = 0;
        }
    }
}

static void panel_describe(void *user, char *out, size_t size) {
    struct control_panel *cp = user;

    snprintf(out, size,
        "  page: %s\n"
        "  pages: %d\n"
        "  selected: %d\n"
        "  editing: %s\n"
        "  registry: %s, hive %s, scroll %d\n"
        "  status: %s\n",
        PAGES[cp->page].label, PAGE_COUNT, cp->selected,
        cp->editing ? (cp->editing_password_only ? "password" : "new account")
                    : "no",
        cp->registry_unlocked ? "unlocked" : "locked",
        cp->registry_hive == 0 ? "system" : "user", cp->registry_scroll,
        cp->status);
}

static void panel_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl CONTROL_PANEL_IMPL = {
    .title = "Control Panel",
    .help = "Accounts",
    .icon = RECON_ICON_CONTROL_PANEL,
    /* Tall enough for the whole page list without scrolling it: a settings
     * window whose own list of settings does not fit is a poor advertisement
     * for the settings. */
    .default_width = 720,
    .default_height = 560,
    .min_width = 560,
    .min_height = 420,
    .draw = panel_draw,
    .click = panel_click,
    .motion = panel_motion,
    .key = panel_key,
    .scroll = panel_scroll,
    .describe = panel_describe,
    .destroy = panel_destroy,
};

/*
 * Open the window for one item, building it the first time.
 *
 * Handed to the shell rather than merely shown: a window the shell has not
 * been told about is drawn and reachable by nothing -- no taskbar button, no
 * clicks, no Alt+Tab. It looks like a window and behaves like a picture.
 */
static void open_page_window(struct control_panel *cp, enum page page) {
    if (page < 0 || page >= PAGE_COUNT) {
        return;
    }

    bool built = false;

    if (g_pages[page] == NULL) {
        struct control_panel *sub = calloc(1, sizeof(*sub));
        if (sub == NULL) {
            set_status(cp, true, "Out of memory opening %s.",
                PAGES[page].label);
            return;
        }

        sub->server = cp->server;
        sub->font = cp->font;
        sub->home = false;
        sub->page = page;
        nothing_chosen(sub);

        recon_edit_begin(&sub->unlock, "", false);
        sub->unlock.masked = true;

        sub->win = recon_appwin_create(cp->server, cp->font,
            &CONTROL_PANEL_IMPL, sub);
        if (sub->win == NULL) {
            free(sub);
            set_status(cp, true, "Could not open %s.", PAGES[page].label);
            return;
        }

        /*
         * Named for the item rather than "Control Panel". Fourteen taskbar
         * buttons all saying Control Panel would be fourteen buttons nobody
         * can tell apart, which is the same as no taskbar.
         */
        recon_appwin_set_title(sub->win, PAGES[page].label);
        recon_appwin_set_icon(sub->win, PAGES[page].icon);
        recon_appwin_set_help_topic(sub->win, help_topic_for(page));

        g_pages[page] = sub;
        built = true;
    }

    struct control_panel *sub = g_pages[page];

    /* Measured on the way in, so the page never shows what the disk looked
     * like some minutes ago. */
    sub->storage_measured = false;

    if (!recon_shell_adopt_window(cp->server->shell, sub->win)) {
        set_status(cp, true, "No room for another window.");
        return;
    }

    recon_appwin_show(sub->win);

    /*
     * Focused, not merely raised. Raising puts it in front; focusing is what
     * decides where typing goes and which title bar is drawn as the active
     * one. Only raising it handed somebody a window on top that their
     * keyboard was not talking to.
     */
    recon_shell_focus_window(cp->server->shell, sub->win);

    /*
     * Placed after it is shown, not before.
     *
     * A window does not know how big the screen is until it has been handed
     * to the shell, and showing it for the first time restores whatever
     * position it had last time -- so a position set before either of those
     * is a position that gets clamped to nothing and then overwritten. Every
     * item opened exactly on top of the one before it, which is the opposite
     * of the point of opening them separately.
     *
     * Only on the first open. After that the window remembers where it was
     * put, and moving it back every time would undo that.
     */
    if (built) {
        int px, py, pw, ph;
        recon_appwin_geometry(cp->win, &px, &py, &pw, &ph);
        (void)pw;
        (void)ph;

        /* Wrapping after six: a seventh step would be off the bottom of a
         * small screen and would be clamped back onto the sixth anyway. */
        int step = g_page_windows % 6;
        recon_appwin_set_origin(sub->win, px + 30 + step * 28,
            py + 30 + step * 26);
        g_page_windows++;
    }

    /*
     * Nothing said. The window is on screen and on the taskbar; a line at the
     * bottom of a different window saying so is the system narrating what
     * somebody just watched happen.
     */
    clear_status(cp);
}

/*
 * Open the skin editor, on the skin named.
 *
 * Built once and reused. Reopening it on a different skin points it at the
 * new one rather than making a second editor: two windows writing to two
 * skin files from one list of forty-eight roles is a way to save a colour
 * into the wrong file.
 */
static void open_skin_editor(struct control_panel *cp, const char *skin) {
    if (cp == NULL || skin == NULL) {
        return;
    }

    if (g_skin_editor == NULL) {
        struct control_panel *ed = calloc(1, sizeof(*ed));
        if (ed == NULL) {
            set_status(cp, true, "Out of memory opening the editor.");
            return;
        }

        ed->server = cp->server;
        ed->font = cp->font;
        ed->home = false;
        ed->page = PAGE_APPEARANCE;
        ed->skin_editing = true;

        recon_edit_begin(&ed->unlock, "", false);
        ed->unlock.masked = true;

        ed->win = recon_appwin_create(cp->server, cp->font,
            &CONTROL_PANEL_IMPL, ed);
        if (ed->win == NULL) {
            free(ed);
            set_status(cp, true, "Could not open the editor.");
            return;
        }

        recon_appwin_set_title(ed->win, "Skin Editor");
        recon_appwin_set_icon(ed->win, RECON_ICON_APPEARANCE);
        recon_appwin_set_help_topic(ed->win, "How it looks");
        g_skin_editor = ed;
    }

    struct control_panel *ed = g_skin_editor;

    /* Pointed at this skin, and back to the top of the list: the row that was
     * chosen in another skin means a different colour in this one. */
    snprintf(ed->skin_name, sizeof(ed->skin_name), "%s", skin);
    ed->skin_editing = true;
    ed->skin_row = 0;
    ed->skin_scroll = 0;
    ed->skin_value_editing = false;
    clear_status(ed);

    if (!recon_shell_adopt_window(cp->server->shell, ed->win)) {
        set_status(cp, true, "No room for another window.");
        return;
    }

    recon_appwin_show(ed->win);

    /*
     * Beside the window that opened it rather than on top of it. The whole
     * reason the editor is its own window is so the skin list and the colours
     * can be seen together.
     */
    int px, py, pw, ph;
    recon_appwin_geometry(cp->win, &px, &py, &pw, &ph);
    (void)ph;
    recon_appwin_set_origin(ed->win, px + pw / 3, py + 40);

    recon_shell_focus_window(cp->server->shell, ed->win);
}

struct recon_appwin *recon_control_panel_create(struct recon_server *server,
        struct recon_font *font) {
    struct control_panel *cp = calloc(1, sizeof(*cp));
    if (cp == NULL) {
        return NULL;
    }

    cp->server = server;
    cp->font = font;

    /* The window the menus open is the front page: the icons, and no page.
     * Every other one is built by open_page_window when a tile is clicked. */
    cp->home = true;
    nothing_chosen(cp);

    recon_edit_begin(&cp->unlock, "", false);
    cp->unlock.masked = true;

    cp->win = recon_appwin_create(server, font, &CONTROL_PANEL_IMPL, cp);
    if (cp->win == NULL) {
        free(cp);
        return NULL;
    }
    return cp->win;
}
