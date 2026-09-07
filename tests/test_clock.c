/*
 * Tests for the clock.
 *
 * Calendar arithmetic is the classic thing that is wrong in a way nobody
 * finds out about for decades. 2000 was a leap year and 2100 is not, because
 * the rule is not "every four years" -- and code that gets that wrong is
 * correct for every date anybody tests it against and wrong seventy years
 * later.
 *
 * The other failure worth catching is the twelve-hour clock. Midnight is
 * 12 am and noon is 12 pm, and `hour % 12` gives zero for both.
 *
 * Run with: cmake --build build && ./build/recon_clock_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_clock.h"
#include "recon_fs.h"
#include "recon_registry.h"

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
 * The clock reads the host's time, which is not a thing a test can choose.
 * What a test *can* choose is the zone offset, and the offset is applied to
 * the same instant -- so shifting the zone by a known amount and checking the
 * result moved by that amount exercises the arithmetic without needing to
 * control the clock.
 */
static void set_offset(int minutes) {
    recon_registry_set_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY, minutes);
}

/* Minutes since midnight, from the broken-up time. */
static int minutes_of(const struct recon_clock_time *t) {
    return t->hour * 60 + t->minute;
}

static void test_zones_shift_the_clock(void) {
    printf("a zone offset moves the clock by exactly that much\n");

    set_offset(0);
    struct recon_clock_time utc;
    recon_clock_now(&utc);

    set_offset(60);
    struct recon_clock_time plus_one;
    recon_clock_now(&plus_one);

    int moved = minutes_of(&plus_one) - minutes_of(&utc);
    if (moved < 0) {
        moved += 24 * 60;   /* Crossed midnight. */
    }
    check(moved == 60, "an hour east is an hour later");

    /* The awkward ones. Nepal is +5:45 and a design that assumed whole hours
     * would be wrong for everyone there. */
    set_offset(345);
    struct recon_clock_time nepal;
    recon_clock_now(&nepal);

    moved = minutes_of(&nepal) - minutes_of(&utc);
    if (moved < 0) {
        moved += 24 * 60;
    }
    check(moved == 345, "five hours forty-five is exactly that");

    set_offset(-360);
    struct recon_clock_time central;
    recon_clock_now(&central);

    moved = minutes_of(&utc) - minutes_of(&central);
    if (moved < 0) {
        moved += 24 * 60;
    }
    check(moved == 360, "six hours west is six hours earlier");

    set_offset(0);
}

static void test_the_date_is_sane(void) {
    printf("the date it produces is a real date\n");

    set_offset(0);
    struct recon_clock_time t;
    recon_clock_now(&t);

    check(t.year >= 2024 && t.year < 2200, "the year is plausible");
    check(t.month >= 1 && t.month <= 12, "the month is 1 to 12");
    check(t.day >= 1 && t.day <= 31, "the day is 1 to 31");
    check(t.hour >= 0 && t.hour <= 23, "the hour is 0 to 23");
    check(t.minute >= 0 && t.minute <= 59, "the minute is 0 to 59");
    check(t.second >= 0 && t.second <= 60, "the second is 0 to 60");
    check(t.weekday >= 0 && t.weekday <= 6, "the weekday is 0 to 6");
}

/*
 * Known dates, checked against the arithmetic directly.
 *
 * These are the cases the clock cannot reach by being asked what time it is
 * now. 2100 is the one that matters: it is divisible by four and is *not* a
 * leap year, because centuries are only leap years when divisible by 400.
 * Code that gets that wrong is correct for every date a person would test.
 */
static void test_known_dates(void) {
    printf("known dates come out right, including the ones far away\n");

    set_offset(0);

    struct {
        int64_t epoch;
        int year, month, day, weekday;
        const char *what;
    } cases[] = {
        { 0,          1970,  1,  1, 4, "the epoch itself, a Thursday" },
        { 946598400,  1999, 12, 31, 5, "the last day of 1999" },
        { 951782400,  2000,  2, 29, 2, "29 February 2000, which was a leap "
                                       "year because 2000 divides by 400" },
        { 951868800,  2000,  3,  1, 3, "and the day after it" },
        { 1709164800, 2024,  2, 29, 4, "29 February 2024" },
        { 4107456000, 2100,  2, 28, 0, "28 February 2100" },
        { 4107542400, 2100,  3,  1, 1, "and 1 March 2100 -- the day after, "
                                       "because 2100 is NOT a leap year" },
        { 1788566400, 2026,  9,  5, 6, "a Saturday in 2026" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct recon_clock_time t;
        recon_clock_break_up(cases[i].epoch, &t);

        bool right = t.year == cases[i].year && t.month == cases[i].month &&
            t.day == cases[i].day && t.weekday == cases[i].weekday;

        g_checks++;
        if (!right) {
            g_failures++;
            printf("  FAIL: %s -- got %04d-%02d-%02d weekday %d\n",
                cases[i].what, t.year, t.month, t.day, t.weekday);
        }
    }

    /* Midnight is midnight in all of them. */
    struct recon_clock_time t;
    recon_clock_break_up(0, &t);
    check(t.hour == 0 && t.minute == 0 && t.second == 0,
        "the epoch is midnight exactly");

    /* And the offset moves a known instant by a known amount, across a day
     * boundary in the direction that is easy to get backwards. */
    set_offset(-360);
    recon_clock_break_up(0, &t);
    check(t.year == 1969 && t.month == 12 && t.day == 31 && t.hour == 18,
        "six hours west of the epoch is the evening before");

    set_offset(0);
}

/*
 * Every day from 1970 to 2100, turned into a number and back again.
 *
 * The eight dates above are the ones somebody thinks to check, and they are
 * well chosen -- 2000 is a leap year, 2100 is not, and both are there. What
 * they cannot cover is the arithmetic *between* them: a month length wrong by
 * a day in one month of one year is invisible to a list of examples and
 * obvious to a walk.
 *
 * Forty-seven thousand days, which takes no measurable time, and the property
 * is exactly what a date routine is for: a day that goes in comes back as
 * itself.
 *
 * The second half is the harder one. It is easy to write a pair of functions
 * that agree with each other and are both wrong -- so this also counts the
 * days as it goes and checks that every month has the length it should, and
 * that the weekday advances by exactly one each day and never skips.
 */
static void test_every_day_goes_there_and_back(void) {
    printf("every day from 1970 to 2100, there and back\n");

    set_offset(0);

    static const int LENGTHS[] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    int wrong_round_trip = 0;
    int wrong_length = 0;
    int wrong_weekday = 0;
    int days = 0;

    char first_bad[128] = "";
    int last_weekday = -1;

    for (int year = 1970; year <= 2100; year++) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

        for (int month = 1; month <= 12; month++) {
            int length = LENGTHS[month - 1] + ((month == 2 && leap) ? 1 : 0);

            for (int day = 1; day <= length; day++) {
                int64_t epoch = recon_clock_epoch_of(year, month, day);

                struct recon_clock_time t;
                recon_clock_break_up(epoch, &t);

                if (t.year != year || t.month != month || t.day != day) {
                    if (wrong_round_trip == 0) {
                        snprintf(first_bad, sizeof(first_bad),
                            "%04d-%02d-%02d came back as %04d-%02d-%02d",
                            year, month, day, t.year, t.month, t.day);
                    }
                    wrong_round_trip++;
                }

                /* Midnight, since only a date went in. */
                if (t.hour != 0 || t.minute != 0 || t.second != 0) {
                    wrong_round_trip++;
                }

                /*
                 * One day later than the last, every time. This is what
                 * catches a month with the wrong number of days even when the
                 * round trip agrees with itself: a February of thirty days
                 * would round-trip perfectly and put the weekday out by two
                 * for the rest of time.
                 */
                if (last_weekday >= 0 &&
                        t.weekday != (last_weekday + 1) % 7) {
                    if (wrong_weekday == 0) {
                        snprintf(first_bad, sizeof(first_bad),
                            "%04d-%02d-%02d is weekday %d after %d",
                            year, month, day, t.weekday, last_weekday);
                    }
                    wrong_weekday++;
                }
                last_weekday = t.weekday;
                days++;
            }

            /*
             * And the day after the last day of a month is the first of the
             * next, which is the same fact from the other side.
             */
            struct recon_clock_time over;
            recon_clock_break_up(
                recon_clock_epoch_of(year, month, length) + 86400, &over);
            int next_month = (month == 12) ? 1 : month + 1;
            int next_year = (month == 12) ? year + 1 : year;
            if (over.day != 1 || over.month != next_month ||
                    over.year != next_year) {
                wrong_length++;
            }
        }
    }

    printf("        %d days\n", days);
    if (first_bad[0] != '\0') {
        printf("        first: %s\n", first_bad);
    }

    /*
     * 131 years of 365 days plus 32 leap days. 2100 is divisible by four and
     * is not one of the 32, which is the whole reason a count is written down
     * here rather than trusted: getting this number by running the loop and
     * copying what it said would make the check agree with whatever the code
     * does.
     */
    check(days == 131 * 365 + 32,
        "the walk covers every day from 1970 to the end of 2100");
    check(wrong_round_trip == 0,
        "EVERY DAY COMES BACK AS ITSELF, at midnight");
    check(wrong_length == 0,
        "and the day after the end of every month is the first of the next");
    check(wrong_weekday == 0,
        "AND THE WEEKDAY ADVANCES BY ONE AND NEVER SKIPS -- which catches a "
        "month of the wrong length that the round trip agrees with");
}

static void test_the_short_form(void) {
    printf("the short form reads the way a clock reads\n");

    set_offset(0);

    recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, true);
    char twenty_four[32];
    recon_clock_short(twenty_four, sizeof(twenty_four));

    check(strlen(twenty_four) == 5, "24-hour is five characters");
    check(twenty_four[2] == ':', "with a colon in the middle");

    recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, false);
    char twelve[32];
    recon_clock_short(twelve, sizeof(twelve));

    check(strstr(twelve, "am") != NULL || strstr(twelve, "pm") != NULL,
        "12-hour says which half of the day it is");
    /*
     * The first character, not a substring.
     *
     * This was strstr(twelve, "0:"), which means "the hour is never zero" and
     * also matches the '0' in "10:" -- so it failed at ten in the morning and
     * ten at night, and passed at the other twenty hours of the day. It went
     * years without being run in either of them.
     *
     * The hour is the start of the string, so this asks the question directly:
     * a twelve-hour clock reads 12, never 0.
     */
    check(twelve[0] != '0',
        "and never shows a zero hour, because no clock face has one");

    recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, true);
}

static void test_midnight_and_noon(void) {
    printf("midnight is 12 am and noon is 12 pm\n");

    /*
     * The one case `hour % 12` gets wrong, and it gets it wrong twice a day.
     *
     * Driven by moving the zone until the local hour is the one wanted, which
     * works whatever the host's clock says: some offset in the list puts the
     * local time in hour zero, and some other one puts it in hour twelve.
     */
    recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, false);

    bool saw_midnight = false;
    bool saw_noon = false;

    for (int offset = -720; offset <= 720 && !(saw_midnight && saw_noon);
            offset += 60) {
        set_offset(offset);

        struct recon_clock_time t;
        recon_clock_now(&t);

        char shown[32];
        recon_clock_short(shown, sizeof(shown));

        if (t.hour == 0 && !saw_midnight) {
            saw_midnight = true;
            check(strncmp(shown, "12:", 3) == 0,
                "hour zero shows as 12, not 0");
            check(strstr(shown, "am") != NULL, "and as am");
        }
        if (t.hour == 12 && !saw_noon) {
            saw_noon = true;
            check(strncmp(shown, "12:", 3) == 0, "hour twelve shows as 12");
            check(strstr(shown, "pm") != NULL, "and as pm");
        }
    }

    check(saw_midnight, "some zone put the clock in hour zero");
    check(saw_noon, "some zone put the clock in hour twelve");

    recon_registry_set_bool(RECON_REG_USER, RECON_CLOCK_24H_KEY, true);
    set_offset(0);
}

static void test_the_zone_list(void) {
    printf("the zone list is usable and round-trips\n");

    int count = recon_clock_zone_count();
    check(count > 10, "there are zones to choose from");

    bool has_half = false;
    bool has_quarter = false;
    for (int i = 0; i < count; i++) {
        struct recon_clock_zone z;
        if (!recon_clock_zone_at(i, &z)) {
            continue;
        }
        check(z.name[0] != '\0', "every zone has a name");
        check(z.minutes >= -720 && z.minutes <= 840,
            "and an offset inside the range the world uses");

        if (z.minutes % 60 == 30 || z.minutes % 60 == -30) {
            has_half = true;
        }
        if (z.minutes % 30 != 0) {
            has_quarter = true;
        }
    }

    /* If these ever fail it means somebody "tidied" the list into whole
     * hours, which is wrong for about a twelfth of the world. */
    check(has_half, "half-hour zones are in the list");
    check(has_quarter, "and at least one quarter-hour zone");

    check(recon_clock_set_zone(5), "a zone can be chosen");
    check(recon_clock_zone_current() == 5, "and it is the one that is set");

    check(!recon_clock_set_zone(-1), "a zone off the start is refused");
    check(!recon_clock_set_zone(count), "and one off the end");
    check(recon_clock_zone_current() == 5, "neither disturbed the choice");

    set_offset(0);
}

static void test_a_check_needs_the_network(void) {
    printf("checking against a time server is honest about being off\n");

    /* Ships off. A machine that reached out to a time server without being
     * asked would be a machine talking to the internet on first boot. */
    check(!recon_clock_can_check(),
        "checking is off until somebody turns it on");
    check(recon_clock_sync_state() == RECON_CLOCK_NEVER,
        "and nothing has been checked");
}

/* --- Summer time --- */

static void test_daylight_saving(void) {
    printf("Summer time\n");

    recon_registry_set_int(RECON_REG_USER, RECON_CLOCK_ZONE_KEY, -360);
    recon_clock_set_daylight_saving(false);

    struct recon_clock_time standard;
    recon_clock_now(&standard);

    recon_clock_set_daylight_saving(true);
    struct recon_clock_time summer;
    recon_clock_now(&summer);

    /* Compared as minutes since midnight so the check holds across the hour
     * that rolls the date over -- which it would otherwise fail on for one
     * hour of every day. */
    int a = standard.hour * 60 + standard.minute;
    int b = summer.hour * 60 + summer.minute;
    int forward = (b - a + 1440) % 1440;

    check(forward == 60, "summer time puts the clock exactly an hour forward");
    check(recon_clock_daylight_saving(), "and the setting reads back on");

    recon_clock_set_daylight_saving(false);
    check(!recon_clock_daylight_saving(), "and off again");

    /*
     * It reaches the short form too, which is what the taskbar draws. An hour
     * applied to the clock and not to the thing that shows the clock is worse
     * than an hour applied to neither.
     */
    char before[32], after[32];
    recon_clock_short(before, sizeof(before));
    recon_clock_set_daylight_saving(true);
    recon_clock_short(after, sizeof(after));
    check(strcmp(before, after) != 0,
        "and the corner of the screen shows the difference");
    recon_clock_set_daylight_saving(false);
}

/*
 * What the host says, taken apart into an offset and a switch.
 *
 * Written the standard way rather than with tm_gmtoff, so the arithmetic is
 * ours and worth checking: it subtracts two broken-down times and has to carry
 * across a date boundary, which is true for about half the world at any moment.
 */
static void test_the_host_zone(void) {
    printf("What the host machine thinks\n");

    int standard = 9999;
    bool summer = true;
    bool answered = recon_clock_host_zone(&standard, &summer);

    check(answered, "the host can say what its zone is");
    if (!answered) {
        return;
    }

    check(standard > -13 * 60 && standard < 15 * 60,
        "and the offset is one a place on Earth could have");
    check(standard % 15 == 0,
        "and lands on a quarter hour, as every real zone does");

    /*
     * Checked against the C library rather than against a number written here.
     * A fixture would pin this to whatever zone the machine that wrote it was
     * in, and then fail for everybody else.
     */
    time_t now = time(NULL);
    struct tm local;
    check(localtime_r(&now, &local) != NULL,
        "and localtime agrees there is one");
    check(summer == (local.tm_isdst > 0),
        "and summer time matches what the library says");
}

int main(void) {
    char root[] = "/tmp/reconos-clock-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS clock tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }
    recon_registry_init();
    recon_clock_init();

    test_zones_shift_the_clock();
    test_the_date_is_sane();
    test_known_dates();
    test_every_day_goes_there_and_back();
    test_the_short_form();
    test_midnight_and_noon();
    test_the_zone_list();
    test_a_check_needs_the_network();
    test_daylight_saving();
    test_the_host_zone();

    recon_registry_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
