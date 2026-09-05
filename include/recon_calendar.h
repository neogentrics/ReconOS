/*
 * A calendar, and the things somebody wrote down against a day.
 *
 * The month is the view because the month is how people hold a plan: "the
 * week of the 14th", "the end of April". A list of appointments sorted by
 * date is a database report, and it is what every calendar falls back to
 * when it has run out of ideas about layout.
 *
 * --- What an event is here ---
 *
 * A day, an optional time, and a line of text. Nothing else: no duration, no
 * repeat, no invitations, no reminders. Each of those is a real feature and
 * each brings a question this cannot yet answer -- a reminder needs something
 * running when the calendar is closed, and a repeat needs a rule engine that
 * is wrong about the last Friday of the month in ways nobody notices for a
 * year.
 *
 * The file is text, one event per line, for the same reason skins and the
 * firewall's rules are: a plan somebody can rescue with a text editor is a
 * plan they cannot lose to this program having a bad day.
 */

#ifndef RECON_CALENDAR_H
#define RECON_CALENDAR_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;
struct recon_appwin;
struct recon_font;

/* Where a person's own events live. Under their account, because a diary is
 * not the machine's. */
#define RECON_CALENDAR_FILE "Calendar.txt"

#define RECON_CALENDAR_TEXT_MAX 120
#define RECON_CALENDAR_EVENTS_MAX 512

struct recon_calendar_event {
    int year, month, day;

    /*
     * Minutes since midnight, or -1 for something that is simply *on* a day.
     *
     * "Anna's birthday" has no time and giving it one would be inventing a
     * fact. All-day things sort before timed ones because that is the order
     * a day is read in.
     */
    int minute_of_day;

    char text[RECON_CALENDAR_TEXT_MAX];
};

struct recon_appwin *recon_calendar_create(struct recon_server *server,
    struct recon_font *font);

/* --- The events themselves, usable without a window --- */

/* Read the account's events from disk. Safe to call repeatedly. */
void recon_calendar_load(void);

/* How many fall on a day, and the nth of them. Ordered by time, with
 * all-day entries first. */
int recon_calendar_count_on(int year, int month, int day);
bool recon_calendar_on(int year, int month, int day, int index,
    struct recon_calendar_event *out);

/*
 * Write one down. `minute_of_day` may be -1.
 *
 * Saves immediately. A calendar that loses what you typed because the window
 * closed is worse than no calendar, and there is no moment at which somebody
 * would think to press Save on a diary.
 */
bool recon_calendar_add(int year, int month, int day, int minute_of_day,
    const char *text);

/* Take one away, by its position within that day. */
bool recon_calendar_remove(int year, int month, int day, int index);

const char *recon_calendar_last_error(void);

#endif /* RECON_CALENDAR_H */
