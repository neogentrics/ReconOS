/*
 * The clock. See include/recon_clock.h.
 *
 * Two things are borrowed from the host and both are marked: the moment
 * itself, which comes from `time()`, and the UDP socket the time check goes
 * out on. Everything else -- the zone, the arithmetic, the formatting, the
 * names of the days -- is here, because a machine that owns itself will have
 * a chip and no libc to ask.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include "ReconOS.h"
#include "recon_clock.h"
#include "recon_net.h"
#include "recon_registry.h"

/*
 * The zones offered, by the name a person would use rather than by the
 * Olson identifier.
 *
 * "Central" rather than "America/Chicago" because this list is read by
 * somebody setting their clock, not by a program. Nothing here handles
 * daylight saving: the offset is what is stored and what is applied, and a
 * rule engine for the world's summer-time legislation is a database with
 * politics in it, not a clock. The page says so rather than getting it wrong
 * twice a year.
 */
static const struct recon_clock_zone ZONES[] = {
    { "Baker Island (UTC-12)",          -720 },
    { "Hawaii (UTC-10)",                -600 },
    { "Alaska (UTC-9)",                 -540 },
    { "Pacific (UTC-8)",                -480 },
    { "Mountain (UTC-7)",               -420 },
    { "Central (UTC-6)",                -360 },
    { "Eastern (UTC-5)",                -300 },
    { "Atlantic (UTC-4)",               -240 },
    { "Newfoundland (UTC-3:30)",        -210 },
    { "Brazil (UTC-3)",                 -180 },
    { "Cape Verde (UTC-1)",              -60 },
    { "Coordinated Universal Time",        0 },
    { "Central European (UTC+1)",         60 },
    { "Eastern European (UTC+2)",        120 },
    { "Moscow (UTC+3)",                  180 },
    { "Gulf (UTC+4)",                    240 },
    { "Pakistan (UTC+5)",                300 },
    { "India (UTC+5:30)",                330 },
    { "Nepal (UTC+5:45)",                345 },
    { "Bangladesh (UTC+6)",              360 },
    { "Indochina (UTC+7)",               420 },
    { "China (UTC+8)",                   480 },
    { "Japan (UTC+9)",                   540 },
    { "Central Australia (UTC+9:30)",    570 },
    { "Eastern Australia (UTC+10)",      600 },
    { "New Zealand (UTC+12)",            720 },
};

#define ZONE_COUNT ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

static const char *const DAYS[] = { "Sunday", "Monday", "Tuesday",
    "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MONTHS[] = { "January", "February", "March", "April",
    "May", "June", "July", "August", "September", "October", "November",
    "December" };

#define NTP_DEFAULT "pool.ntp.org"
#define NTP_PORT 123

/*
 * The seconds between 1900 and 1970.
 *
 * NTP counts from 1900 and everything else here counts from 1970, and the
 * difference is a constant with seventeen leap days in it. Written out rather
 * than computed so it can be checked against the specification by eye.
 */
#define NTP_EPOCH_OFFSET 2208988800ULL

static struct {
    enum recon_clock_sync state;
    char detail[192];
    int64_t checked_at;      /* when, in our own seconds */

    struct wl_event_source *source;
    int fd;
} g_sync = { .fd = -1 };

/* --- The moment --- */

/*
 * Seconds since 1970, from the host.
 *
 * The one place ReconOS reads somebody else's clock. When there is a
 * real-time chip to read, this is the function that changes.
 */
static int64_t now_utc(void) {
    return (int64_t)time(NULL);
}

static int zone_minutes(void) {
    return recon_registry_get_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY, 0);
}

/*
 * Break a count of seconds into a date, in the chosen zone.
 *
 * Written out rather than handed to `localtime_r` because localtime reads the
 * *host's* idea of a zone from the environment and a file, and ReconOS's zone
 * is its own. Using it would mean the setting worked on a machine whose host
 * agreed with it and silently did nothing on one that did not.
 *
 * The civil-date arithmetic is Howard Hinnant's days-from-civil, run
 * backwards. It is a published algorithm with a proof, which is the only kind
 * of calendar arithmetic worth writing: the ones people derive themselves are
 * wrong in 2100, which is not a leap year, and nobody finds out for decades.
 */
static void break_up(int64_t seconds, struct recon_clock_time *out) {
    int64_t local = seconds + (int64_t)zone_minutes() * 60;

    int64_t days = local / 86400;
    int64_t rest = local % 86400;
    if (rest < 0) {
        rest += 86400;
        days -= 1;
    }

    out->hour = (int)(rest / 3600);
    out->minute = (int)((rest % 3600) / 60);
    out->second = (int)(rest % 60);

    /* 1970-01-01 was a Thursday, which is day 4 counting Sunday as 0. */
    out->weekday = (int)(((days % 7) + 11) % 7);

    /* Shift the era so March is month 0 and the leap day lands at the end of
     * a year, which is what makes the rest of this arithmetic branchless. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);

    out->year = (int)(y + (m <= 2 ? 1 : 0));
    out->month = (int)m;
    out->day = (int)d;
}

void recon_clock_init(void) {
    g_sync.state = RECON_CLOCK_NEVER;
    g_sync.detail[0] = '\0';
}

void recon_clock_now(struct recon_clock_time *out) {
    if (out != NULL) {
        break_up(now_utc(), out);
    }
}

/*
 * Days from a civil date, which is `break_up`'s arithmetic run forwards.
 *
 * Hinnant's days_from_civil. Paired with the one above deliberately: the two
 * are inverses and a bug in either shows up as a date that will not survive a
 * round trip, which the tests check.
 */
int64_t recon_clock_epoch_of(int year, int month, int day) {
    int64_t y = year;
    int64_t m = month;
    int64_t d = day;

    /* March is the start of the year, so the leap day is at the end of one. */
    y -= (m <= 2);

    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;

    return days * 86400;
}

void recon_clock_break_up(int64_t seconds, struct recon_clock_time *out) {
    if (out != NULL) {
        break_up(seconds, out);
    }
}

/* --- How it reads --- */

void recon_clock_short(char *out, size_t size) {
    if (out == NULL || size == 0) {
        return;
    }

    struct recon_clock_time t;
    break_up(now_utc(), &t);

    if (recon_registry_get_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, true)) {
        snprintf(out, size, "%02d:%02d", t.hour, t.minute);
        return;
    }

    /* Midnight is 12 am and noon is 12 pm, which is the one part of the
     * twelve-hour clock that a modulo gets wrong. */
    int hour = t.hour % 12;
    if (hour == 0) {
        hour = 12;
    }
    snprintf(out, size, "%d:%02d %s", hour, t.minute, t.hour < 12 ? "am" : "pm");
}

void recon_clock_date(char *out, size_t size) {
    if (out == NULL || size == 0) {
        return;
    }

    struct recon_clock_time t;
    break_up(now_utc(), &t);
    snprintf(out, size, "%s, %d %s %d", DAYS[t.weekday % 7], t.day,
        MONTHS[(t.month - 1) % 12], t.year);
}

void recon_clock_zone_label(char *out, size_t size) {
    if (out == NULL || size == 0) {
        return;
    }

    const char *named = recon_registry_get(RECON_REG_USER,
        RECON_CLOCK_ZONE_NAME_KEY, "");
    if (named[0] != '\0') {
        snprintf(out, size, "%s", named);
        return;
    }

    /*
     * No name recorded, so one is built from the offset. This is the state a
     * machine is in before anybody has chosen a zone, and "UTC+00:00" is a
     * truthful thing to show rather than a guess at where it is.
     */
    int minutes = zone_minutes();
    int sign = minutes < 0 ? -1 : 1;
    int absolute = minutes * sign;
    snprintf(out, size, "UTC%c%02d:%02d", sign < 0 ? '-' : '+',
        absolute / 60, absolute % 60);
}

/* --- Zones --- */

int recon_clock_zone_count(void) {
    return ZONE_COUNT;
}

bool recon_clock_zone_at(int index, struct recon_clock_zone *out) {
    if (index < 0 || index >= ZONE_COUNT || out == NULL) {
        return false;
    }
    *out = ZONES[index];
    return true;
}

int recon_clock_zone_current(void) {
    int minutes = zone_minutes();
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (ZONES[i].minutes == minutes) {
            return i;
        }
    }
    return -1;
}

bool recon_clock_set_zone(int index) {
    if (index < 0 || index >= ZONE_COUNT) {
        return false;
    }
    recon_registry_set_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY,
        ZONES[index].minutes);
    return recon_registry_set(RECON_REG_USER, RECON_CLOCK_ZONE_NAME_KEY,
        ZONES[index].name);
}

/* --- Asking the network --- */

enum recon_clock_sync recon_clock_sync_state(void) {
    return g_sync.state;
}

void recon_clock_sync_detail(char *out, size_t size) {
    if (out != NULL && size > 0) {
        snprintf(out, size, "%s", g_sync.detail);
    }
}

bool recon_clock_can_check(void) {
    return recon_net_online() &&
        recon_registry_get_bool(RECON_REG_USER, RECON_CLOCK_NTP_ON_KEY, false);
}

static void sync_finish(void) {
    if (g_sync.source != NULL) {
        wl_event_source_remove(g_sync.source);
        g_sync.source = NULL;
    }
    if (g_sync.fd >= 0) {
        close(g_sync.fd);
        g_sync.fd = -1;
    }
}

/*
 * The reply, if one came.
 *
 * SNTP, which is NTP with the statistics left out: one packet each way, and
 * the answer is the transmit timestamp. The full protocol's value is in
 * filtering many samples over hours to discipline a local oscillator, and
 * ReconOS has no oscillator to discipline -- it is asking what the time is,
 * once, because somebody pressed a button.
 */
static int sync_readable(int fd, uint32_t mask, void *data) {
    (void)data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        g_sync.state = RECON_CLOCK_UNREACHABLE;
        snprintf(g_sync.detail, sizeof(g_sync.detail),
            "The time server did not answer.");
        sync_finish();
        return 0;
    }

    unsigned char packet[48];
    ssize_t got = recv(fd, packet, sizeof(packet), 0);
    if (got < (ssize_t)sizeof(packet)) {
        g_sync.state = RECON_CLOCK_UNREACHABLE;
        snprintf(g_sync.detail, sizeof(g_sync.detail),
            "The time server sent something this does not understand.");
        sync_finish();
        return 0;
    }

    /* Transmit timestamp: seconds since 1900, big-endian, at byte 40. */
    uint64_t seconds_1900 = ((uint64_t)packet[40] << 24) |
        ((uint64_t)packet[41] << 16) | ((uint64_t)packet[42] << 8) |
        (uint64_t)packet[43];

    if (seconds_1900 <= NTP_EPOCH_OFFSET) {
        g_sync.state = RECON_CLOCK_UNREACHABLE;
        snprintf(g_sync.detail, sizeof(g_sync.detail),
            "The time server gave a time before this system existed.");
        sync_finish();
        return 0;
    }

    int64_t theirs = (int64_t)(seconds_1900 - NTP_EPOCH_OFFSET);
    int64_t ours = now_utc();
    int64_t drift = ours - theirs;
    int64_t off = drift < 0 ? -drift : drift;

    /*
     * Two seconds. Below that the difference is this packet's flight time
     * rather than the clock being wrong, and reporting network latency as
     * drift would have the machine accuse itself of a fault it does not have.
     */
    if (off <= 2) {
        g_sync.state = RECON_CLOCK_AGREED;
        snprintf(g_sync.detail, sizeof(g_sync.detail),
            "Checked just now. This machine agrees with the time server "
            "to within a second or two.");
    } else {
        g_sync.state = RECON_CLOCK_DRIFTED;
        snprintf(g_sync.detail, sizeof(g_sync.detail),
            "Checked just now. This machine is %lld second%s %s. ReconOS "
            "reads the host's clock and does not set it, so the correction "
            "belongs there.",
            (long long)off, off == 1 ? "" : "s",
            drift > 0 ? "fast" : "slow");
    }

    g_sync.checked_at = ours;
    sync_finish();
    return 0;
}

bool recon_clock_check(char *why_out, size_t why_size) {
    if (why_out != NULL && why_size > 0) {
        why_out[0] = '\0';
    }

    if (g_sync.state == RECON_CLOCK_ASKING) {
        return true;   /* One at a time; the answer is already coming. */
    }

    if (!recon_net_online()) {
        recon_text_copy(why_out, why_size,
            "There is no way out to a time server at the moment.");
        return false;
    }

    const char *server = recon_registry_get(RECON_REG_USER,
        RECON_CLOCK_NTP_KEY, NTP_DEFAULT);
    if (server[0] == '\0') {
        server = NTP_DEFAULT;
    }

    /*
     * Resolved through recon_net, so the firewall and the name rules that
     * apply to everything else apply here. A clock that reached the network
     * by a private route would be a hole in the one place that decides what
     * this machine may talk to.
     */
    char address[RECON_NET_ADDR_MAX];
    if (recon_net_resolve(server, address, sizeof(address)) != RECON_NET_OK) {
        recon_text_copy(why_out, why_size,
            "That time server's name could not be looked up.");
        return false;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        recon_text_copy(why_out, why_size, "No socket could be made.");
        return false;
    }

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(NTP_PORT);
    if (inet_pton(AF_INET, address, &to.sin_addr) != 1) {
        close(fd);
        recon_text_copy(why_out, why_size,
            "That time server's address could not be read.");
        return false;
    }

    /*
     * The request: version 4, mode 3 (client), and forty-seven zero bytes.
     * A client packet carries no information the server needs -- it is a
     * knock, and the answer is the whole of the exchange.
     */
    unsigned char packet[48] = {0};
    packet[0] = 0x23;

    if (sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&to,
            sizeof(to)) < 0) {
        close(fd);
        recon_text_copy(why_out, why_size,
            "The question could not be sent.");
        return false;
    }

    struct wl_event_loop *loop = recon_net_event_loop();
    if (loop == NULL) {
        close(fd);
        recon_text_copy(why_out, why_size, "Networking is not up.");
        return false;
    }

    sync_finish();
    g_sync.fd = fd;
    g_sync.source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
        sync_readable, NULL);
    g_sync.state = RECON_CLOCK_ASKING;
    snprintf(g_sync.detail, sizeof(g_sync.detail), "Asking %s...", server);
    return true;
}
