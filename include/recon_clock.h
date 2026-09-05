/*
 * What time it is, where, and how sure the machine is about it.
 *
 * --- Why this is a subsystem and not a call to strftime ---
 *
 * A clock in a taskbar looks like a formatting problem. It is three:
 *
 *   What time is it here?    Needs a zone, which is a person's choice and
 *                            not the host's.
 *   Is that time right?      Needs somewhere to check against, which means
 *                            the network, which means it can fail and has to
 *                            say so.
 *   Whose time is it?        ReconOS reads the host's clock today. When it
 *                            owns the machine it will read a chip, and the
 *                            difference has to be invisible above here.
 *
 * The third is why this file exists rather than the taskbar calling
 * `localtime_r` directly. It is the same boundary as `recon_display_*` and
 * `recon_volume_*`: the question is ReconOS's, and today's answer is
 * borrowed. When the kernel can read a real-time clock, this file changes and
 * nothing above it does. See docs/KERNEL-WANTS.md.
 *
 * --- The zone is ours, not the host's ---
 *
 * ReconOS keeps its own zone offset in the registry and applies it itself. It
 * does not use TZ or the host's zone database, because a machine that owns
 * itself has neither, and because "the desktop's zone" and "the account the
 * compositor happens to run as" are different things that would only look the
 * same on a developer's laptop.
 *
 * Offsets are minutes, not hours: Kathmandu is +5:45 and Adelaide is +9:30,
 * and a design that assumed whole hours would be wrong for about a twelfth of
 * the world in a way nobody there finds charming.
 */

#ifndef RECON_CLOCK_H
#define RECON_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the choice lives. Minutes east of UTC, as a signed number. */
#define RECON_CLOCK_ZONE_KEY "clock/zone-minutes"
#define RECON_CLOCK_ZONE_NAME_KEY "clock/zone-name"
/* Whether to show a 24-hour clock. Off means the 12-hour one with am/pm. */
#define RECON_CLOCK_24H_KEY "clock/twenty-four-hour"
/* Where to ask the time, and whether to ask at all. */
#define RECON_CLOCK_NTP_KEY "clock/time-server"
#define RECON_CLOCK_NTP_ON_KEY "clock/ask-the-network"

#define RECON_CLOCK_ZONE_NAME_MAX 48

/* A moment, broken up, already in the chosen zone. */
struct recon_clock_time {
    int year;      /* 2026, not 126 */
    int month;     /* 1-12 */
    int day;       /* 1-31 */
    int weekday;   /* 0 = Sunday */
    int hour;      /* 0-23, whatever the display setting says */
    int minute;
    int second;
};

/* One of the zones the system offers. */
struct recon_clock_zone {
    char name[RECON_CLOCK_ZONE_NAME_MAX];
    int minutes;   /* east of UTC */
};

void recon_clock_init(void);

/* Now, in the chosen zone. */
void recon_clock_now(struct recon_clock_time *out);

/*
 * Any moment, in the chosen zone: seconds since 1970 broken into a date.
 *
 * Public so it can be tested. The failure this guards against is a leap-year
 * rule that is right for every date anybody would think to try and wrong in
 * 2100 -- which is not a leap year, because the rule is not "every four
 * years" -- and a test that can only ask what time it is now cannot catch
 * that for another seventy-odd years.
 */
void recon_clock_break_up(int64_t seconds, struct recon_clock_time *out);

/*
 * The clock as the taskbar shows it, and as the longer forms below it.
 *
 * Three separate calls rather than one with a mode, because they are read in
 * three different places and a mode argument is a thing every caller has to
 * look up.
 */
void recon_clock_short(char *out, size_t size);   /* 14:32 */
void recon_clock_date(char *out, size_t size);    /* Friday, 5 September 2026 */
void recon_clock_zone_label(char *out, size_t size); /* Central (UTC-6) */

/* --- Zones --- */

int recon_clock_zone_count(void);
bool recon_clock_zone_at(int index, struct recon_clock_zone *out);

/* Which one is set, as an index into the list above, or -1 for an offset
 * that no listed zone matches -- which is possible, because the offset is
 * what is stored and somebody can set one by hand. */
int recon_clock_zone_current(void);

bool recon_clock_set_zone(int index);

/* --- Asking the network --- */

/*
 * How the last attempt to check the time went.
 *
 * `RECON_CLOCK_NEVER` is not a failure: it is a machine that has not been
 * asked to check, which is the state it ships in.
 */
enum recon_clock_sync {
    RECON_CLOCK_NEVER,
    RECON_CLOCK_ASKING,
    RECON_CLOCK_AGREED,      /* the server and this machine agree closely */
    RECON_CLOCK_DRIFTED,     /* they differ; by how much is in the detail */
    RECON_CLOCK_UNREACHABLE,
};

enum recon_clock_sync recon_clock_sync_state(void);

/*
 * What the last check found, as a sentence. Empty before the first one.
 *
 * A sentence rather than a number because "checked 4 minutes ago, and this
 * machine is 1.2 seconds fast" is the answer, and every part of that is
 * needed to know whether to care.
 */
void recon_clock_sync_detail(char *out, size_t size);

/*
 * Ask the time server what time it is.
 *
 * Does not block: the result arrives later and `recon_clock_sync_state`
 * reports it, the same shape `recon_net_probe` uses. A clock that froze the
 * desktop while a UDP packet went missing would be worse than a clock that is
 * wrong.
 *
 * This *reports*; it does not set anything. Setting the clock on a hosted
 * ReconOS would mean setting the host's clock, which is not ReconOS's to
 * touch. When ReconOS owns the machine, this is where the correction goes.
 */
bool recon_clock_check(char *why_out, size_t why_size);

/* Whether a check is worth offering: needs the network and a server name. */
bool recon_clock_can_check(void);

#endif /* RECON_CLOCK_H */
