/*
 * The firewall, which had no tests at all.
 *
 * Found by scripts/coverage.sh on the day it was written: 747 lines, none of
 * them run by anything. It is compiled into the network suite as a dependency
 * and never called, which is the worst shape a gap can have -- the file
 * appears in a test target's source list, so it looks tested from every angle
 * except the one that counts.
 *
 * --- What is worth checking here ---
 *
 * Not that a rule can be added and read back. That is bookkeeping, and a
 * firewall that only did bookkeeping correctly would still let everything
 * through.
 *
 * What matters is the *answer* -- `recon_firewall_allows` -- and the small
 * number of rules that decide it:
 *
 *   * the first matching rule wins, so order is a decision and not a detail
 *   * a rule that is off is not consulted, which is different from not being
 *     there
 *   * a rule for one program does not answer for another
 *   * the default applies when nothing matched, per direction
 *   * turning the firewall off allows everything AND SAYS SO, rather than
 *     quietly continuing to enforce
 *   * a rule ReconOS ships cannot be deleted -- the system-wide preset rule,
 *     which is worth a test in the one place where losing a preset means
 *     losing a protection
 *
 * And the refusals: a rule with a port outside the range, a backwards range,
 * a name that will not fit. A firewall that silently accepts a rule it cannot
 * represent is a firewall with a line in it that does not do what it says.
 *
 * Run with: cmake --build build && ./build/recon_firewall_tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_firewall.h"
#include "recon_fs.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* A rule, filled in the way the Control Panel's form fills one. */
static struct recon_fw_rule rule_for(const char *name,
        enum recon_fw_direction direction, int from, int to,
        const char *program, enum recon_fw_action action) {
    struct recon_fw_rule rule;
    memset(&rule, 0, sizeof(rule));
    snprintf(rule.name, sizeof(rule.name), "%s", name);
    rule.direction = direction;
    rule.protocol = RECON_FW_TCP;
    rule.port_from = from;
    rule.port_to = to;
    if (program != NULL) {
        snprintf(rule.program, sizeof(rule.program), "%s", program);
    }
    rule.action = action;
    rule.enabled = true;
    return rule;
}

/* Remove everything that can be removed, so each test starts from the rules
 * ReconOS ships and not from the last test's. */
static void clear_added(void) {
    for (int i = recon_firewall_count() - 1; i >= 0; i--) {
        struct recon_fw_rule rule;
        if (recon_firewall_at(i, &rule) &&
                !recon_firewall_is_built_in(&rule)) {
            recon_firewall_remove(i);
        }
    }
}

static void test_it_comes_up(void) {
    printf("Starting\n");

    check(recon_firewall_init(), "the firewall starts");
    check(recon_firewall_is_on(), "and is on");
    check(recon_firewall_count() > 0,
        "with rules, because a firewall with none is a switch that does "
        "nothing");
}

static void test_what_will_not_be_accepted(void) {
    printf("Rules that make no sense\n");

    clear_added();
    int before = recon_firewall_count();

    struct recon_fw_rule bad = rule_for("Too high", RECON_FW_IN, 70000, 70000,
        NULL, RECON_FW_ALLOW);
    check(!recon_firewall_add(&bad), "a port above 65535 is refused");

    bad = rule_for("Negative", RECON_FW_IN, -1, 100, NULL, RECON_FW_ALLOW);
    check(!recon_firewall_add(&bad), "and a negative one");

    bad = rule_for("Backwards", RECON_FW_IN, 900, 100, NULL, RECON_FW_ALLOW);
    check(!recon_firewall_add(&bad),
        "a range that ends before it starts is refused");

    bad = rule_for("", RECON_FW_IN, 80, 80, NULL, RECON_FW_ALLOW);
    check(!recon_firewall_add(&bad),
        "and a rule with no name -- the name is what the log says when it "
        "fires");

    check(recon_firewall_count() == before,
        "AND NONE OF THEM WAS ADDED, which is the point of refusing");
}

static void test_the_first_match_wins(void) {
    printf("Order\n");

    clear_added();

    /*
     * Two rules covering the same port with opposite answers. If order did
     * not decide, one of them would be unreachable and there would be no way
     * to say which -- so this is not a test of a convenience, it is a test of
     * the thing that makes a rule list mean anything.
     */
    struct recon_fw_rule allow = rule_for("Allow 8080", RECON_FW_OUT,
        8080, 8080, NULL, RECON_FW_ALLOW);
    struct recon_fw_rule block = rule_for("Block 8080", RECON_FW_OUT,
        8080, 8080, NULL, RECON_FW_BLOCK);

    check(recon_firewall_add(&allow), "a rule allowing a port");
    check(recon_firewall_add(&block), "and one blocking it, after");

    char why[128] = "";
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8080, NULL,
        why, sizeof(why)), "the first one decides");
    check(strstr(why, "Allow 8080") != NULL, "and says which rule it was");

    /* The blocking rule moved above the allowing one changes the answer. */
    int block_at = recon_firewall_count() - 1;
    check(recon_firewall_move(block_at, -1), "the second is moved up");

    why[0] = '\0';
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8080, NULL,
        why, sizeof(why)), "AND THE ANSWER CHANGES");
    check(strstr(why, "Block 8080") != NULL, "naming the rule that decided");
}

static void test_a_rule_that_is_off(void) {
    printf("A rule that is turned off\n");

    clear_added();

    struct recon_fw_rule block = rule_for("Block 8081", RECON_FW_OUT,
        8081, 8081, NULL, RECON_FW_BLOCK);
    check(recon_firewall_add(&block), "a blocking rule");

    int at = recon_firewall_count() - 1;
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8081, NULL,
        NULL, 0), "blocks while it is on");

    check(recon_firewall_set_rule_on(at, false), "it is turned off");

    struct recon_fw_rule read;
    check(recon_firewall_at(at, &read) && !read.enabled,
        "and stays in the list, turned off");
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8081, NULL,
        NULL, 0), "AND IS NOT CONSULTED, which is different from being gone");
}

static void test_a_rule_for_one_program(void) {
    printf("A rule about one program\n");

    clear_added();

    struct recon_fw_rule block = rule_for("Block Mail's port", RECON_FW_OUT,
        8082, 8082, "Mail", RECON_FW_BLOCK);
    check(recon_firewall_add(&block), "a rule naming a program");

    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8082, "Mail",
        NULL, 0), "it applies to that program");
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8082, "Web",
        NULL, 0), "AND NOT TO ANOTHER ONE");
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8082, NULL,
        NULL, 0), "and not to the system itself");
}

static void test_a_range(void) {
    printf("A range of ports\n");

    clear_added();

    struct recon_fw_rule block = rule_for("Block a range", RECON_FW_OUT,
        9000, 9010, NULL, RECON_FW_BLOCK);
    check(recon_firewall_add(&block), "a rule covering eleven ports");

    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 9000, NULL,
        NULL, 0), "the first is covered");
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 9005, NULL,
        NULL, 0), "one in the middle is covered");
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 9010, NULL,
        NULL, 0), "and the last, because the range is inclusive");
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 9011, NULL,
        NULL, 0), "one past the end is not");
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8999, NULL,
        NULL, 0), "and neither is one before the start");
}

static void test_the_default(void) {
    printf("What happens when nothing matches\n");

    clear_added();

    /*
     * Set per direction, because they are different questions. Reaching out
     * is something this machine chose to do; being reached is something
     * somebody else chose, and defaulting both the same way is how a firewall
     * ends up either useless or unusable.
     */
    check(recon_firewall_set_default(RECON_FW_OUT, RECON_FW_ALLOW),
        "outgoing is allowed by default");
    check(recon_firewall_set_default(RECON_FW_IN, RECON_FW_BLOCK),
        "and incoming is blocked");

    char why[128] = "";
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 12345, NULL,
        why, sizeof(why)), "an unmatched outgoing port is allowed");
    check(why[0] != '\0', "and says it was the default that decided");

    why[0] = '\0';
    check(!recon_firewall_allows(RECON_FW_IN, RECON_FW_TCP, 12345, NULL,
        why, sizeof(why)), "an unmatched incoming one is blocked");
    check(why[0] != '\0', "and says so");

    check(recon_firewall_default(RECON_FW_OUT) == RECON_FW_ALLOW &&
        recon_firewall_default(RECON_FW_IN) == RECON_FW_BLOCK,
        "and both are remembered separately");
}

static void test_turning_it_off(void) {
    printf("Turning it off\n");

    clear_added();

    struct recon_fw_rule block = rule_for("Block 8083", RECON_FW_OUT,
        8083, 8083, NULL, RECON_FW_BLOCK);
    check(recon_firewall_add(&block), "something is blocked");
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8083, NULL,
        NULL, 0), "and is");

    check(recon_firewall_set_on(false), "the firewall is turned off");
    check(!recon_firewall_is_on(), "and says it is off");

    char why[128] = "";
    check(recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8083, NULL,
        why, sizeof(why)),
        "EVERYTHING IS ALLOWED, including what a rule blocks");
    check(why[0] != '\0',
        "and it says the firewall is off rather than naming a rule -- a "
        "switch that quietly kept enforcing would be worse than no switch");

    check(recon_firewall_set_on(true), "and back on");
    check(!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, 8083, NULL,
        NULL, 0), "the rule is in force again");
}

static void test_what_ships_cannot_be_deleted(void) {
    printf("The rules ReconOS ships\n");

    clear_added();

    int built_in = -1;
    for (int i = 0; i < recon_firewall_count() && built_in < 0; i++) {
        struct recon_fw_rule rule;
        if (recon_firewall_at(i, &rule) &&
                recon_firewall_is_built_in(&rule)) {
            built_in = i;
        }
    }

    check(built_in >= 0, "there is at least one rule that ships");
    if (built_in < 0) {
        return;
    }

    int before = recon_firewall_count();
    check(!recon_firewall_remove(built_in),
        "AND IT CANNOT BE DELETED -- a preset that can be removed is gone "
        "for good, and there is nowhere to get it back from");
    check(recon_firewall_count() == before, "so the count does not change");

    check(recon_firewall_set_rule_on(built_in, false),
        "it can be turned off, which is what somebody actually wants");
    check(recon_firewall_set_rule_on(built_in, true), "and back on");
}

static void test_the_names(void) {
    printf("Saying what a rule is, in words\n");

    check(recon_fw_direction_name(RECON_FW_IN) != NULL &&
        recon_fw_direction_name(RECON_FW_OUT) != NULL,
        "both directions have a name");
    check(recon_fw_action_name(RECON_FW_ALLOW) != NULL &&
        recon_fw_action_name(RECON_FW_BLOCK) != NULL,
        "and both actions");
    check(recon_fw_protocol_name(RECON_FW_TCP) != NULL,
        "and the protocols");

    /* A well-known port reads as a name, so a rule about 443 does not make
     * somebody remember which number that was. */
    check(recon_fw_port_name(443) != NULL, "443 has a name");
    check(recon_fw_port_name(RECON_FW_RECON_PORT) != NULL,
        "and so does the one ReconOS listens on");
    check(recon_fw_port_name(43219) == NULL,
        "and a number nobody has a name for says so rather than inventing "
        "one");
}

int main(void) {
    printf("Firewall\n\n");

    /* Its own filesystem, so the rules written here are not somebody's. */
    char root[] = "/tmp/recon-firewall-test-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("could not make a temporary root\n");
        return 1;
    }

    if (!recon_fs_init(root)) {
        printf("could not start the filesystem: %s\n", recon_fs_last_error());
        return 1;
    }

    test_it_comes_up();
    test_what_will_not_be_accepted();
    test_the_first_match_wins();
    test_a_rule_that_is_off();
    test_a_rule_for_one_program();
    test_a_range();
    test_the_default();
    test_turning_it_off();
    test_what_ships_cannot_be_deleted();
    test_the_names();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
