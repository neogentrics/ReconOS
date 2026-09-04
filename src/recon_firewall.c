/*
 * The firewall. See include/recon_firewall.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_firewall.h"
#include "recon_fs.h"

static struct recon_fw_rule g_rules[RECON_FIREWALL_RULES_MAX];
static int g_count;
static bool g_on = true;
static enum recon_fw_action g_defaults[RECON_FW_DIRECTION_COUNT];
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_firewall_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Names --- */

const char *recon_fw_direction_name(enum recon_fw_direction direction) {
    switch (direction) {
    case RECON_FW_IN:  return "in";
    case RECON_FW_OUT: return "out";
    default:           return "?";
    }
}

const char *recon_fw_protocol_name(enum recon_fw_protocol protocol) {
    switch (protocol) {
    case RECON_FW_TCP: return "tcp";
    case RECON_FW_UDP: return "udp";
    default:           return "any";
    }
}

const char *recon_fw_action_name(enum recon_fw_action action) {
    return action == RECON_FW_ALLOW ? "allow" : "block";
}

/*
 * The ports people write rules about.
 *
 * Not the assigned list, which is thousands of entries and has never helped
 * anybody read a firewall. These are the ones that appear in a rule somebody
 * wrote on purpose.
 */
const char *recon_fw_port_name(int port) {
    static const struct { int port; const char *name; } KNOWN[] = {
        { 20,  "FTP data" },
        { 21,  "FTP" },
        { 22,  "Secure shell" },
        { 23,  "Telnet" },
        { 25,  "Mail (SMTP)" },
        { 53,  "Name lookups (DNS)" },
        { 67,  "Address assignment (DHCP)" },
        { 68,  "Address assignment (DHCP)" },
        { 80,  "Web (HTTP)" },
        { 110, "Mail (POP3)" },
        { 123, "Time (NTP)" },
        { 143, "Mail (IMAP)" },
        { 389, "Directory (LDAP)" },
        { 443, "Web (HTTPS)" },
        { 445, "Windows file sharing" },
        { 465, "Mail (SMTPS)" },
        { 587, "Mail (submission)" },
        { 631, "Printing (IPP)" },
        { 993, "Mail (IMAPS)" },
        { 995, "Mail (POP3S)" },
        { 3389, "Remote desktop (RDP)" },
        { 5353, "Local discovery (mDNS)" },
        { 5900, "Remote screen (VNC)" },
        { RECON_FW_RECON_PORT, "ReconOS remote access" },
    };

    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (KNOWN[i].port == port) {
            return KNOWN[i].name;
        }
    }
    return NULL;
}

/* --- The file --- */

/*
 * One rule a line, in the order they are consulted.
 *
 *   on = yes
 *   default in = block
 *   default out = allow
 *   rule = yes | in | tcp | 7420-7420 | * | allow | ReconOS remote access
 *
 * Pipe-separated because a rule's name has spaces in it and its own field,
 * and because a format nobody can read is a format nobody can check.
 */

static enum recon_fw_action action_from(const char *word,
        enum recon_fw_action fallback) {
    if (strcasecmp(word, "allow") == 0) {
        return RECON_FW_ALLOW;
    }
    if (strcasecmp(word, "block") == 0) {
        return RECON_FW_BLOCK;
    }
    return fallback;
}

static enum recon_fw_protocol protocol_from(const char *word) {
    if (strcasecmp(word, "tcp") == 0) {
        return RECON_FW_TCP;
    }
    if (strcasecmp(word, "udp") == 0) {
        return RECON_FW_UDP;
    }
    return RECON_FW_ANY_PROTOCOL;
}

/* Trim spaces from both ends, in place. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
    return text;
}

static bool write_rules(void) {
    char text[RECON_FIREWALL_RULES_MAX * 160 + 512];
    size_t used = 0;

    int n = snprintf(text, sizeof(text),
        "# The ReconOS firewall.\n"
        "#\n"
        "# This decides what ReconOS itself opens and accepts. It is not the\n"
        "# host's firewall and does not touch it.\n"
        "#\n"
        "# The first rule that matches decides. Order is what settles it, so\n"
        "# the order of these lines is part of the rule.\n"
        "#\n"
        "#   rule = on | direction | protocol | ports | program | action | name\n"
        "#\n"
        "# ports is from-to, or 0-0 for any. program is * for any.\n"
        "\n"
        "on = %s\n"
        "default in = %s\n"
        "default out = %s\n"
        "\n",
        g_on ? "yes" : "no",
        recon_fw_action_name(g_defaults[RECON_FW_IN]),
        recon_fw_action_name(g_defaults[RECON_FW_OUT]));

    if (n < 0) {
        return false;
    }
    used = (size_t)n;

    for (int i = 0; i < g_count && used < sizeof(text); i++) {
        const struct recon_fw_rule *rule = &g_rules[i];

        n = snprintf(text + used, sizeof(text) - used,
            "rule = %s | %s | %s | %d-%d | %s | %s | %s\n",
            rule->enabled ? "on" : "off",
            recon_fw_direction_name(rule->direction),
            recon_fw_protocol_name(rule->protocol),
            rule->port_from, rule->port_to,
            rule->program[0] != '\0' ? rule->program : "*",
            recon_fw_action_name(rule->action),
            rule->name);

        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            break;
        }
        used += (size_t)n;
    }

    if (!recon_fs_write("/", RECON_FIREWALL_FILE, text, used)) {
        set_error("cannot write '%s'", RECON_FIREWALL_FILE);
        return false;
    }
    return true;
}

static bool parse_rule(char *value, struct recon_fw_rule *out) {
    /* Seven fields. Fewer is a line somebody half-wrote; more is a name with
     * a pipe in it, which is a name this format cannot carry. */
    char *fields[7];
    int count = 0;

    char *at = value;
    while (count < 7) {
        fields[count++] = at;
        char *bar = strchr(at, '|');
        if (bar == NULL) {
            break;
        }
        *bar = '\0';
        at = bar + 1;
    }

    if (count < 7) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->enabled = (strcasecmp(trim(fields[0]), "on") == 0);
    out->direction = (strcasecmp(trim(fields[1]), "in") == 0)
        ? RECON_FW_IN : RECON_FW_OUT;
    out->protocol = protocol_from(trim(fields[2]));

    char *ports = trim(fields[3]);
    char *dash = strchr(ports, '-');
    if (dash != NULL) {
        *dash = '\0';
        out->port_from = atoi(ports);
        out->port_to = atoi(dash + 1);
    } else {
        out->port_from = atoi(ports);
        out->port_to = out->port_from;
    }

    char *program = trim(fields[4]);
    if (strcmp(program, "*") != 0) {
        recon_text_copy(out->program, sizeof(out->program), program);
    }

    out->action = action_from(trim(fields[5]), RECON_FW_BLOCK);
    recon_text_copy(out->name, sizeof(out->name), trim(fields[6]));

    return true;
}

/*
 * What ships.
 *
 * Outgoing allowed and incoming blocked, which is right almost always and
 * wrong loudly when it is wrong. The named rules are the ports people
 * actually open, written down and **off**: the useful state for a rule about
 * remote access is there, correct, and not in force until somebody wants
 * remote access. Turning one on is a switch rather than an exercise in
 * remembering a port number.
 */
static void write_defaults(void) {
    g_on = true;
    g_defaults[RECON_FW_IN] = RECON_FW_BLOCK;
    g_defaults[RECON_FW_OUT] = RECON_FW_ALLOW;
    g_count = 0;

    static const struct recon_fw_rule DEFAULTS[] = {
        { "ReconOS remote access", RECON_FW_IN, RECON_FW_TCP,
          RECON_FW_RECON_PORT, RECON_FW_RECON_PORT, "", RECON_FW_ALLOW,
          false },
        { "Secure shell", RECON_FW_IN, RECON_FW_TCP, 22, 22, "",
          RECON_FW_ALLOW, false },
        { "Remote screen (VNC)", RECON_FW_IN, RECON_FW_TCP, 5900, 5900, "",
          RECON_FW_ALLOW, false },
        { "Windows file sharing", RECON_FW_IN, RECON_FW_TCP, 445, 445, "",
          RECON_FW_ALLOW, false },
        { "Printing", RECON_FW_IN, RECON_FW_TCP, 631, 631, "",
          RECON_FW_ALLOW, false },

        /*
         * And the outgoing ones, on. These are what the default policy would
         * allow anyway -- they are here so that somebody who changes the
         * outgoing default to block still has a working machine, and can see
         * at a glance what it is that has to keep working.
         */
        { "Web", RECON_FW_OUT, RECON_FW_TCP, 80, 80, "", RECON_FW_ALLOW,
          true },
        { "Web (secure)", RECON_FW_OUT, RECON_FW_TCP, 443, 443, "",
          RECON_FW_ALLOW, true },
        { "Name lookups", RECON_FW_OUT, RECON_FW_UDP, 53, 53, "",
          RECON_FW_ALLOW, true },
        { "Time", RECON_FW_OUT, RECON_FW_UDP, 123, 123, "", RECON_FW_ALLOW,
          true },
    };

    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]) &&
            g_count < RECON_FIREWALL_RULES_MAX; i++) {
        g_rules[g_count++] = DEFAULTS[i];
    }
}

bool recon_firewall_init(void) {
    /*
     * The built-in defaults first, always.
     *
     * A firewall that fails open because its file is missing is worse than no
     * firewall, because it looks like one. Whatever happens below, the rules
     * in memory are a working set from this point on.
     */
    write_defaults();

    size_t size = 0;
    char *text = recon_fs_read("/", RECON_FIREWALL_FILE, &size);
    if (text == NULL) {
        /* First run: write what is in memory so there is something to edit. */
        return write_rules();
    }

    /* Read into a fresh set rather than over the defaults, so a file with no
     * rules in it means no rules rather than the defaults. */
    g_count = 0;

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save);
            line != NULL;
            line = strtok_r(NULL, "\n", &save)) {

        char *at = trim(line);
        if (*at == '\0' || *at == '#') {
            continue;
        }

        char *equals = strchr(at, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';

        char *key = trim(at);
        char *value = trim(equals + 1);

        if (strcasecmp(key, "on") == 0) {
            g_on = (strcasecmp(value, "yes") == 0 ||
                    strcasecmp(value, "on") == 0);
        } else if (strcasecmp(key, "default in") == 0) {
            g_defaults[RECON_FW_IN] = action_from(value, RECON_FW_BLOCK);
        } else if (strcasecmp(key, "default out") == 0) {
            g_defaults[RECON_FW_OUT] = action_from(value, RECON_FW_ALLOW);
        } else if (strcasecmp(key, "rule") == 0) {
            if (g_count >= RECON_FIREWALL_RULES_MAX) {
                continue;
            }
            struct recon_fw_rule rule;
            if (parse_rule(value, &rule)) {
                g_rules[g_count++] = rule;
            }
        }
    }

    free(text);
    return true;
}

/* --- The switch and the defaults --- */

bool recon_firewall_is_on(void) {
    return g_on;
}

bool recon_firewall_set_on(bool on) {
    g_on = on;
    return write_rules();
}

enum recon_fw_action recon_firewall_default(enum recon_fw_direction direction) {
    if (direction < 0 || direction >= RECON_FW_DIRECTION_COUNT) {
        return RECON_FW_BLOCK;
    }
    return g_defaults[direction];
}

bool recon_firewall_set_default(enum recon_fw_direction direction,
        enum recon_fw_action action) {
    if (direction < 0 || direction >= RECON_FW_DIRECTION_COUNT) {
        set_error("there is no such direction");
        return false;
    }
    g_defaults[direction] = action;
    return write_rules();
}

/* --- The rules --- */

int recon_firewall_count(void) {
    return g_count;
}

bool recon_firewall_at(int index, struct recon_fw_rule *out) {
    if (index < 0 || index >= g_count) {
        return false;
    }
    if (out != NULL) {
        *out = g_rules[index];
    }
    return true;
}

bool recon_firewall_add(const struct recon_fw_rule *rule) {
    if (rule == NULL) {
        return false;
    }
    if (g_count >= RECON_FIREWALL_RULES_MAX) {
        set_error("there is no room for another rule");
        return false;
    }
    if (rule->name[0] == '\0') {
        set_error("a rule needs a name");
        return false;
    }
    if (rule->port_from < 0 || rule->port_to > 65535 ||
            rule->port_from > rule->port_to) {
        set_error("that is not a range of ports");
        return false;
    }

    g_rules[g_count++] = *rule;
    return write_rules();
}

bool recon_firewall_remove(int index) {
    if (index < 0 || index >= g_count) {
        return false;
    }
    memmove(&g_rules[index], &g_rules[index + 1],
        (size_t)(g_count - index - 1) * sizeof(g_rules[0]));
    g_count--;
    return write_rules();
}

bool recon_firewall_set_rule_on(int index, bool on) {
    if (index < 0 || index >= g_count) {
        return false;
    }
    g_rules[index].enabled = on;
    return write_rules();
}

bool recon_firewall_move(int index, int by) {
    int to = index + by;
    if (index < 0 || index >= g_count || to < 0 || to >= g_count) {
        return false;
    }

    struct recon_fw_rule moving = g_rules[index];
    if (to > index) {
        memmove(&g_rules[index], &g_rules[index + 1],
            (size_t)(to - index) * sizeof(g_rules[0]));
    } else {
        memmove(&g_rules[to + 1], &g_rules[to],
            (size_t)(index - to) * sizeof(g_rules[0]));
    }
    g_rules[to] = moving;
    return write_rules();
}

/* --- The question --- */

static bool rule_matches(const struct recon_fw_rule *rule,
        enum recon_fw_direction direction, enum recon_fw_protocol protocol,
        int port, const char *program) {
    if (!rule->enabled || rule->direction != direction) {
        return false;
    }

    if (rule->protocol != RECON_FW_ANY_PROTOCOL &&
            protocol != RECON_FW_ANY_PROTOCOL &&
            rule->protocol != protocol) {
        return false;
    }

    /* Both zero is every port. */
    if (rule->port_from != 0 || rule->port_to != 0) {
        if (port < rule->port_from || port > rule->port_to) {
            return false;
        }
    }

    if (rule->program[0] != '\0') {
        if (program == NULL || strcasecmp(rule->program, program) != 0) {
            return false;
        }
    }

    return true;
}

bool recon_firewall_allows(enum recon_fw_direction direction,
        enum recon_fw_protocol protocol, int port, const char *program,
        char *why_out, size_t why_size) {

    if (!g_on) {
        /* A switch that quietly kept enforcing would be worse than no
         * switch. */
        if (why_out != NULL) {
            recon_text_copy(why_out, why_size, "the firewall is off");
        }
        return true;
    }

    for (int i = 0; i < g_count; i++) {
        if (!rule_matches(&g_rules[i], direction, protocol, port, program)) {
            continue;
        }

        /* First match wins. Not the most specific: reading the list top to
         * bottom is the only way a person can work out what it does. */
        if (why_out != NULL) {
            recon_text_copy(why_out, why_size, g_rules[i].name);
        }
        return g_rules[i].action == RECON_FW_ALLOW;
    }

    if (why_out != NULL) {
        char note[96];
        snprintf(note, sizeof(note), "nothing matched; %s is %s by default",
            direction == RECON_FW_IN ? "incoming" : "outgoing",
            recon_fw_action_name(g_defaults[direction]));
        recon_text_copy(why_out, why_size, note);
    }
    return g_defaults[direction] == RECON_FW_ALLOW;
}
