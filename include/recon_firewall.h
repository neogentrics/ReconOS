/*
 * The firewall.
 *
 * --- What it does and does not do ---
 *
 * ReconOS has no network stack of its own; it reports the host's and reaches
 * across it. So this cannot filter packets, and saying it could would be the
 * same lie as a network page that pretended to implement one.
 *
 * What it does is decide **what ReconOS itself opens and accepts**. Every
 * outgoing connection ReconOS makes asks first, and every incoming one it
 * would accept asks first. That is a real boundary with real teeth: a module
 * that wants to reach across the network is refused here, and remote access
 * cannot be listening on a port the firewall has not been told to open.
 *
 * The host's own firewall is a separate thing and is not touched. On a machine
 * ReconOS owns there will be one firewall; until then there are two, and this
 * is the one that governs ReconOS.
 *
 * --- The shape of it ---
 *
 * The shape most systems have, because that is the shape people already know:
 * a switch, a default per direction, and a list of rules that override the
 * default. Outgoing is allowed by default and incoming is blocked, which is
 * the arrangement that is right almost always and wrong loudly when it is
 * wrong.
 *
 * The first enabled rule that matches decides. Not the most specific: "first
 * match wins" is what every firewall worth copying does, and it is the only
 * ordering a person can reason about by reading the list top to bottom.
 *
 * Rules live in /System/Config/firewall.txt as text, because a firewall whose
 * rules cannot be read outside the program that wrote them is a firewall
 * nobody can audit.
 *
 * --- What replaces it ---
 *
 * This is deliberately the plain version. A firewall with per-program rules,
 * profiles and its own history belongs in an application, and when that
 * application is installed it takes over from this: the interface below is
 * what it would implement, so nothing above has to change.
 */

#ifndef RECON_FIREWALL_H
#define RECON_FIREWALL_H

#include <stdbool.h>
#include <stddef.h>

#define RECON_FIREWALL_FILE "/System/Config/firewall.txt"

/* How many rules there is room for. A hundred is far past what a plain
 * firewall wants, and the list is drawn on one page. */
#define RECON_FIREWALL_RULES_MAX 100

#define RECON_FW_NAME_MAX 48

/*
 * The port ReconOS listens on for remote access.
 *
 * Not one of the well-known assignments: ReconOS is not pretending to be a
 * service that already exists, and a system that squatted on 22 would be
 * lying to whatever connected to it.
 */
#define RECON_FW_RECON_PORT 7420

enum recon_fw_direction {
    /* Something reaching in to ReconOS. */
    RECON_FW_IN,
    /* ReconOS reaching out. */
    RECON_FW_OUT,
    RECON_FW_DIRECTION_COUNT,
};

enum recon_fw_protocol {
    RECON_FW_TCP,
    RECON_FW_UDP,
    /* Matches either. What a rule means when it does not care. */
    RECON_FW_ANY_PROTOCOL,
};

enum recon_fw_action {
    RECON_FW_ALLOW,
    RECON_FW_BLOCK,
};

struct recon_fw_rule {
    /* What a person calls it. This is what the log says when the rule fires,
     * so "Web" is worth more than "tcp/443". */
    char name[RECON_FW_NAME_MAX];

    enum recon_fw_direction direction;
    enum recon_fw_protocol protocol;

    /*
     * The ports this covers, inclusive. Both zero means every port.
     *
     * A range rather than a list, because a list needs a length and a
     * separator and a parser, and every rule anybody has written so far is
     * either one port or a run of them.
     */
    int port_from;
    int port_to;

    /* Which program this applies to, by the name it is registered under, or
     * empty for all of them. */
    char program[RECON_FW_NAME_MAX];

    enum recon_fw_action action;

    /*
     * Off means the rule is not consulted.
     *
     * Kept rather than deleted, because the useful state for most rules is
     * "written down and not in force": a rule for remote access should be
     * there, correct, and off until somebody wants remote access.
     */
    bool enabled;
};

/*
 * Read the rules, writing the defaults if there are none.
 *
 * Returns false only when the rules could not be read *and* the defaults
 * could not be written -- and even then the firewall works, on the built-in
 * defaults. A firewall that fails open because its file is missing is worse
 * than no firewall, because it looks like one.
 */
bool recon_firewall_init(void);

/* --- The switch and the defaults --- */

bool recon_firewall_is_on(void);
bool recon_firewall_set_on(bool on);

enum recon_fw_action recon_firewall_default(enum recon_fw_direction direction);
bool recon_firewall_set_default(enum recon_fw_direction direction,
    enum recon_fw_action action);

/* --- The rules --- */

int recon_firewall_count(void);
bool recon_firewall_at(int index, struct recon_fw_rule *out);

/* Add a rule at the end of the list. False if there is no room or the rule
 * makes no sense. */
bool recon_firewall_add(const struct recon_fw_rule *rule);

bool recon_firewall_remove(int index);
bool recon_firewall_set_rule_on(int index, bool on);

/* Move a rule up or down. Order is what decides, so being able to change it
 * is not a convenience. */
bool recon_firewall_move(int index, int by);

/* --- The question --- */

/*
 * May this happen?
 *
 * `program` is the name the thing asking is registered under, or NULL for the
 * system itself. `why_out` is filled with the name of the rule that decided,
 * or with the default that did, so a refusal can say which line to look at
 * rather than only that there was one.
 *
 * With the firewall off, everything is allowed and `why_out` says so. That is
 * deliberate: a switch that quietly kept enforcing would be worse than no
 * switch.
 */
bool recon_firewall_allows(enum recon_fw_direction direction,
    enum recon_fw_protocol protocol, int port, const char *program,
    char *why_out, size_t why_size);

/* --- Names, for the page and the terminal --- */

const char *recon_fw_direction_name(enum recon_fw_direction direction);
const char *recon_fw_protocol_name(enum recon_fw_protocol protocol);
const char *recon_fw_action_name(enum recon_fw_action action);

/*
 * The well-known port a number belongs to, or NULL.
 *
 * So a rule for 443 can say "Web (443)" without somebody having to remember
 * which number that was. Only the handful people actually write rules about;
 * the full assigned list is thousands of entries and none of the rest has
 * ever helped anybody read a firewall.
 */
const char *recon_fw_port_name(int port);

const char *recon_firewall_last_error(void);

#endif
