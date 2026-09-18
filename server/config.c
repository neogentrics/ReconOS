/*
 * Reading the configuration file.
 *
 * See `config.h` for what this is for and what it deliberately does not do.
 * Everything here is pure: bytes in, a `struct config` or a refusal out. No
 * allocation, no syscalls, no clock -- `server_init.c` reads the file, because
 * it is the only place that can, and hands the bytes here.
 *
 * --- What was tried and rejected ---
 *
 * **Applying what parsed and skipping what did not.** It is the friendlier
 * shape and it produces a machine running a configuration nobody wrote: half
 * of one file and half of the compiled-in defaults, with no single place that
 * describes it. The whole file is taken or none of it is.
 *
 * **Quoting values.** A quoted value needs an escape, an escape needs a rule
 * for a backslash before a quote, and every one of those rules is somewhere
 * two readers can disagree. A value here is *the rest of the line, trimmed*,
 * which has exactly one reading. What it costs: a value cannot end in
 * whitespace, and nothing needs one to.
 *
 * **Being generous about `[site name]` spacing.** Tempting, and it means
 * `[ site  name ]` and `[site name]` are the same line -- which is fine until
 * somebody writes `[site my name]` and the parser has to decide whether the
 * name has a space in it. One space, one name, and a refusal that says so.
 *
 * --- One thing that is accepted, and why it is not leniency ---
 *
 * A line may end `\r\n`. Not because CRLF is tidy, but because a carriage
 * return at the end of a line has exactly one possible meaning and this
 * repository has already been bitten once by pretending otherwise: the suite
 * runner parsed CRLF target names out of `CMakeLists.txt` for its whole life
 * and carried a trailing carriage return into every comparison. A file edited
 * on the machine somebody actually uses should not be a fault, and accepting
 * it introduces no second reading. A carriage return anywhere *else* on a line
 * is a control byte and is refused like any other.
 */

#include "config.h"

/* --- small things, because there is no C library here ---------------------- */

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int is_name_byte(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/* Printable ASCII, which is what every value here may contain. A byte outside
 * this is refused rather than stripped: a path with a control byte in it is a
 * different path, and one that is silently repaired is a path nobody typed. */
static int is_printable(char c)
{
	unsigned char u = (unsigned char)c;

	return u >= 0x20 && u < 0x7F;
}

static int same(const char *a, const char *b, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* A NUL-terminated literal against a counted run of bytes. */
static int is_word(const char *text, size_t len, const char *word)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (word[i] == '\0' || text[i] != word[i])
			return 0;
	}
	return word[len] == '\0';
}

static size_t length(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* Copy a counted run into a bounded buffer. Returns 0 if it does not fit --
 * never a truncation, for the reason `config.h` gives. */
static int take(char *out, size_t room, const char *from, size_t len)
{
	size_t i;

	if (len + 1 > room)
		return 0;
	for (i = 0; i < len; i++)
		out[i] = from[i];
	out[len] = '\0';
	return 1;
}

/*
 * Record what was refused, for the console line.
 *
 * Bounded, and **anything unprintable becomes a `?`** rather than being copied
 * through. This text is about to be printed on a console and written into an
 * access log by the machine that could not read it, and a file it refused is
 * exactly the file whose bytes should not be trusted to be text.
 */
static void note(struct config *into, const char *from, size_t len)
{
	size_t i;
	size_t room = sizeof(into->found) - 1;

	if (len > room)
		len = room;
	for (i = 0; i < len; i++)
		into->found[i] = is_printable(from[i]) ? from[i] : '?';
	into->found[len] = '\0';
}

/*
 * Refuse, and leave nothing behind but the reason.
 *
 * **The clear is the point.** Everything that parsed before the fault parsed
 * perfectly, and a caller that can see it is a caller that can use it -- which
 * is a machine running the first half of a file it rejected, in a state
 * nobody described and no file documents. `config.h` promises that the whole
 * file is taken or none of it is, and a promise kept by asking every caller to
 * be careful is not kept.
 */
static int fail(struct config *into, int verdict, unsigned line,
                const char *from, size_t len)
{
	char kept[CONFIG_VALUE_MAX];
	char *raw = (char *)into;
	size_t i;

	/* Copied out first: some callers point at text that lives inside
	 * `into` -- a site's own name is the one that bites -- and the clear
	 * below would take the message with it. */
	if (len > sizeof(kept) - 1)
		len = sizeof(kept) - 1;
	for (i = 0; i < len; i++)
		kept[i] = from[i];

	for (i = 0; i < sizeof(*into); i++)
		raw[i] = 0;

	into->line = line;
	note(into, kept, len);
	return verdict;
}

/* --- the two rules with suites of their own -------------------------------- */

int config_root_ok(const char *path)
{
	size_t n, i;

	if (!path)
		return 0;
	n = length(path);
	if (n < 2 || n + 1 > CONFIG_VALUE_MAX)
		return 0;		/* a bare "/" is not a document root */
	if (path[0] != '/')
		return 0;
	if (path[n - 1] == '/')
		return 0;		/* one spelling, so two roots cannot
					 * be the same directory */

	for (i = 0; i < n; i++) {
		if (!is_printable(path[i]) || path[i] == '\\')
			return 0;
		if (path[i] == '/' && i && path[i - 1] == '/')
			return 0;	/* an empty component */
	}

	/*
	 * No `..` component, anywhere.
	 *
	 * The load-bearing one. A document root is the boundary every served
	 * path is resolved against: `files.c` refuses a *request* that climbs,
	 * and a root that climbs moves the boundary itself, so nothing after
	 * it is refused because nothing after it is wrong. `/System/Web/../..`
	 * is a correctly-formed path to the whole volume.
	 */
	for (i = 0; i + 1 < n; i++) {
		if (path[i] != '.' || path[i + 1] != '.')
			continue;
		if (i && path[i - 1] != '/')
			continue;	/* part of a longer name, like `a..b` */
		if (i + 2 == n || path[i + 2] == '/')
			return 0;
	}
	return 1;
}

int config_host_ok(const char *host)
{
	size_t n, i;

	if (!host)
		return 0;
	n = length(host);
	if (n == 0 || n + 1 > CONFIG_VALUE_MAX)
		return 0;
	if (host[0] == '.' || host[n - 1] == '.')
		return 0;

	for (i = 0; i < n; i++) {
		if (!is_name_byte(host[i]))
			return 0;	/* which also refuses a `:port` -- see
					 * the header, it could never match */
		if (host[i] == '.' && i && host[i - 1] == '.')
			return 0;	/* an empty label */
	}
	return 1;
}

/* --- values ---------------------------------------------------------------- */

static int port_of(const char *text, size_t len, unsigned *out)
{
	unsigned value = 0;
	size_t i;

	if (len == 0 || len > 5)
		return 0;
	for (i = 0; i < len; i++) {
		if (!is_digit(text[i]))
			return 0;
		value = value * 10 + (unsigned)(text[i] - '0');
	}
	if (value == 0 || value > 65535)
		return 0;
	*out = value;
	return 1;
}

/* A dotted quad, and only that. A name here would need a resolver to read the
 * configuration that says where the resolver is. */
static int address_ok(const char *text)
{
	size_t n = length(text);
	size_t i = 0;
	int parts = 0;

	if (n == 0 || n > 15)
		return 0;

	while (i < n) {
		unsigned value = 0;
		size_t digits = 0;

		while (i < n && is_digit(text[i])) {
			value = value * 10 + (unsigned)(text[i] - '0');
			digits++;
			i++;
		}
		if (digits == 0 || digits > 3 || value > 255)
			return 0;
		parts++;
		if (i == n)
			break;
		if (text[i] != '.')
			return 0;
		i++;
		if (i == n)
			return 0;	/* a trailing dot */
	}
	return parts == 4;
}

/* An index is a file name, not a path: `index root/../../etc/passwd` would
 * otherwise be a root that climbs written in the other field. */
static int index_ok(const char *name)
{
	size_t n = length(name);
	size_t i;

	if (n == 0 || n + 1 > CONFIG_VALUE_MAX)
		return 0;
	for (i = 0; i < n; i++) {
		if (!is_printable(name[i]) || name[i] == '/' ||
		    name[i] == '\\')
			return 0;
	}
	if (is_word(name, n, ".") || is_word(name, n, ".."))
		return 0;
	return 1;
}

/* --- the file -------------------------------------------------------------- */

/*
 * Which keys exist, and where each one belongs.
 *
 * One table, read by the parser, so that *this key is not known* and *this key
 * is in the wrong place* are answered from the same list -- see `http.h` on
 * what happens when one fact is enumerated in two places.
 */
#define CONFIG_KEYS(X)          \
	X("name",     0)        \
	X("port",     0)        \
	X("resolver", 0)        \
	X("clock",    0)        \
	X("root",     1)        \
	X("index",    1)

/* Returns 1 if the key exists, and sets `*in_site` to where it belongs. */
static int key_known(const char *key, size_t len, int *in_site)
{
#define X(word, site)                           \
	if (is_word(key, len, word)) {          \
		*in_site = (site);              \
		return 1;                       \
	}
	CONFIG_KEYS(X)
#undef X
	return 0;
}

struct seen {
	int name, port, resolver, clock;	/* at the top level */
	int root, index;			/* in the current site */
};

static int apply_top(struct config *into, struct seen *seen, const char *key,
                     size_t key_len, const char *value, size_t value_len,
                     unsigned line)
{
	if (is_word(key, key_len, "name")) {
		if (seen->name)
			return fail(into, CONFIG_EREPEATED, line, key, key_len);
		seen->name = 1;
		if (!take(into->name, sizeof(into->name), value, value_len))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		if (!config_host_ok(into->name))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		into->has_name = 1;
		return CONFIG_OK;
	}
	if (is_word(key, key_len, "port")) {
		if (seen->port)
			return fail(into, CONFIG_EREPEATED, line, key, key_len);
		seen->port = 1;
		if (!port_of(value, value_len, &into->port))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		into->has_port = 1;
		return CONFIG_OK;
	}
	if (is_word(key, key_len, "resolver")) {
		if (seen->resolver)
			return fail(into, CONFIG_EREPEATED, line, key, key_len);
		seen->resolver = 1;
		if (!take(into->resolver, sizeof(into->resolver), value,
		          value_len))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		if (!address_ok(into->resolver))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		into->has_resolver = 1;
		return CONFIG_OK;
	}
	/* clock */
	if (seen->clock)
		return fail(into, CONFIG_EREPEATED, line, key, key_len);
	seen->clock = 1;
	if (!take(into->clock, sizeof(into->clock), value, value_len))
		return fail(into, CONFIG_EVALUE, line, value, value_len);
	if (!config_host_ok(into->clock))
		return fail(into, CONFIG_EVALUE, line, value, value_len);
	into->has_clock = 1;
	return CONFIG_OK;
}

static int apply_site(struct config *into, struct seen *seen, const char *key,
                      size_t key_len, const char *value, size_t value_len,
                      unsigned line)
{
	struct config_site *site = &into->sites[into->site_count - 1];

	if (is_word(key, key_len, "root")) {
		if (seen->root)
			return fail(into, CONFIG_EREPEATED, line, key, key_len);
		seen->root = 1;
		if (!take(site->root, sizeof(site->root), value, value_len))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		if (!config_root_ok(site->root))
			return fail(into, CONFIG_EVALUE, line, value, value_len);
		return CONFIG_OK;
	}
	/* index */
	if (seen->index)
		return fail(into, CONFIG_EREPEATED, line, key, key_len);
	seen->index = 1;
	if (!take(site->index, sizeof(site->index), value, value_len))
		return fail(into, CONFIG_EVALUE, line, value, value_len);
	if (!index_ok(site->index))
		return fail(into, CONFIG_EVALUE, line, value, value_len);
	return CONFIG_OK;
}

/*
 * `[site <name>]`, and nothing else in brackets.
 *
 * Opens a section. The previous one must be complete before this one starts --
 * checked here as well as at the end of the file, so the reported line is the
 * section that is wrong rather than the last line of the file.
 */
static int open_section(struct config *into, struct seen *seen,
                        const char *line_text, size_t len, unsigned line)
{
	static const char WORD[] = "site ";
	struct config_site *site;
	size_t i;
	size_t name_len;

	/* The previous site, if there is one, must have had a root. */
	if (into->site_count && !seen->root) {
		struct config_site *last = &into->sites[into->site_count - 1];

		return fail(into, CONFIG_EINCOMPLETE, last->line, last->host,
		            length(last->host));
	}

	if (len < sizeof(WORD) + 1 || line_text[len - 1] != ']')
		return fail(into, CONFIG_ESECTION, line, line_text, len);
	if (!same(line_text + 1, WORD, sizeof(WORD) - 1))
		return fail(into, CONFIG_ESECTION, line, line_text, len);

	name_len = len - 1 - (1 + sizeof(WORD) - 1);
	if (into->site_count == CONFIG_SITES_MAX)
		return fail(into, CONFIG_ETOOMANY, line,
		            line_text + 1 + sizeof(WORD) - 1, name_len);

	site = &into->sites[into->site_count];
	if (!take(site->host, sizeof(site->host),
	          line_text + 1 + sizeof(WORD) - 1, name_len))
		return fail(into, CONFIG_ESECTION, line, line_text, len);
	if (!config_host_ok(site->host))
		return fail(into, CONFIG_ESECTION, line,
		            line_text + 1 + sizeof(WORD) - 1, name_len);

	/*
	 * Two sites with one name.
	 *
	 * Refused rather than letting the first win. `serve.c` walks the chain
	 * in order and the second would be unreachable -- a configured site
	 * that never answers, with nothing anywhere saying why.
	 */
	for (i = 0; i < into->site_count; i++) {
		if (is_word(into->sites[i].host, length(into->sites[i].host),
		            site->host))
			return fail(into, CONFIG_EDUPLICATE, line, site->host,
			            length(site->host));
	}

	site->line = line;
	site->root[0] = '\0';
	/* The default, applied here so a site that does not say has one and
	 * `files.c` is never handed an empty name. */
	take(site->index, sizeof(site->index), "index.html", 10);
	into->site_count++;

	seen->root = 0;
	seen->index = 0;
	return CONFIG_OK;
}

int config_parse(const char *text, size_t len, struct config *into)
{
	struct seen seen;
	size_t at = 0;
	unsigned line = 0;
	size_t i;

	if (!text || !into)
		return CONFIG_EVALUE;

	/* Zeroed here rather than trusted from the caller: a refusal must
	 * leave nothing usable behind, and a caller that reads `site_count`
	 * after one is reading whatever it set up beforehand. */
	{
		char *raw = (char *)into;

		for (i = 0; i < sizeof(*into); i++)
			raw[i] = 0;
	}
	seen.name = seen.port = seen.resolver = seen.clock = 0;
	seen.root = seen.index = 0;

	if (len > CONFIG_FILE_MAX)
		return fail(into, CONFIG_EFILE_LONG, 0, "", 0);

	while (at < len) {
		const char *start = text + at;
		size_t line_len = 0;
		size_t key_len, value_len;
		const char *key, *value;
		int in_site = 0;
		int verdict;

		line++;

		while (at + line_len < len && start[line_len] != '\n')
			line_len++;
		at += line_len;
		if (at < len)
			at++;			/* step over the newline */

		/* One carriage return, at the end, and only there. See the
		 * header: it has exactly one meaning, and this repository has
		 * already paid once for treating CRLF as somebody else's
		 * problem. */
		if (line_len && start[line_len - 1] == '\r')
			line_len--;

		if (line_len > CONFIG_LINE_MAX)
			return fail(into, CONFIG_ELINE_LONG, line, start,
			            CONFIG_VALUE_MAX - 1);

		/* Leading whitespace, so a file can be indented. */
		while (line_len && is_space(start[0])) {
			start++;
			line_len--;
		}
		/* And trailing, so a value never carries one. */
		while (line_len && is_space(start[line_len - 1]))
			line_len--;

		if (line_len == 0 || start[0] == '#')
			continue;

		for (i = 0; i < line_len; i++) {
			if (!is_printable(start[i]))
				return fail(into, CONFIG_EVALUE, line, start, i);
		}

		if (start[0] == '[') {
			verdict = open_section(into, &seen, start, line_len,
			                       line);
			if (verdict != CONFIG_OK)
				return verdict;
			continue;
		}

		key = start;
		key_len = 0;
		while (key_len < line_len && !is_space(key[key_len]))
			key_len++;

		value = key + key_len;
		value_len = line_len - key_len;
		while (value_len && is_space(value[0])) {
			value++;
			value_len--;
		}

		if (!key_known(key, key_len, &in_site))
			return fail(into, CONFIG_EUNKNOWN, line, key, key_len);
		if (value_len == 0)
			return fail(into, CONFIG_EVALUE, line, key, key_len);

		if (in_site && into->site_count == 0)
			return fail(into, CONFIG_EMISPLACED, line, key, key_len);
		if (!in_site && into->site_count != 0)
			return fail(into, CONFIG_EMISPLACED, line, key, key_len);

		verdict = in_site
		        ? apply_site(into, &seen, key, key_len, value,
		                     value_len, line)
		        : apply_top(into, &seen, key, key_len, value,
		                    value_len, line);
		if (verdict != CONFIG_OK)
			return verdict;
	}

	/* The last site, which nothing else will check. */
	if (into->site_count && !seen.root) {
		struct config_site *last = &into->sites[into->site_count - 1];

		return fail(into, CONFIG_EINCOMPLETE, last->line, last->host,
		            length(last->host));
	}

	into->line = 0;
	into->found[0] = '\0';
	return CONFIG_OK;
}

/*
 * The template, as text.
 *
 * Written as one string rather than assembled, so that what the suite parses
 * is byte-for-byte what a volume receives. Every line is a comment: a template
 * that configured something would be a machine configured by a file nobody
 * wrote.
 */
static const char TEMPLATE[] =
	"# ReconOS server configuration.\n"
	"#\n"
	"# Written by the server because this volume did not have one. Every\n"
	"# line here is a comment, so as it stands it changes nothing.\n"
	"#\n"
	"# One setting a line, `key value`. A value runs to the end of the\n"
	"# line and is not quoted. A line that is not understood is not\n"
	"# skipped: the whole file is refused and the console says which line\n"
	"# and why, because a setting that is silently ignored is a machine\n"
	"# disagreeing with its own documentation.\n"
	"#\n"
	"# --- the machine ---\n"
	"#\n"
	"# name       what this machine is called; the same rule POST /api/name\n"
	"#            applies, so a name this refuses is a name that endpoint\n"
	"#            would refuse too\n"
	"# port       what the web server listens on (default 80)\n"
	"# resolver   the DNS server, as an address -- a name here would need\n"
	"#            the resolver this line is setting\n"
	"# clock      the time server, as a name; it is resolved on the first\n"
	"#            check\n"
	"#\n"
	"# name m16\n"
	"# port 80\n"
	"# resolver 10.0.2.3\n"
	"# clock time.cloudflare.com\n"
	"#\n"
	"# --- sites ---\n"
	"#\n"
	"# A site answers to a name and serves files from a directory. The\n"
	"# first whose name matches a request's Host answers, in the order\n"
	"# they appear here, so the order is yours rather than alphabetical.\n"
	"#\n"
	"# The console is not listed and cannot be: it is built in, it has no\n"
	"# name, and it stays last -- so it answers to every name no site\n"
	"# above it claimed. A name that nothing claims gets 421.\n"
	"#\n"
	"# Every setting above must come before the first site. After a site\n"
	"# opens, every line belongs to it.\n"
	"#\n"
	"# [site shop.example]\n"
	"# root /System/Web/shop\n"
	"# index index.html\n"
	"#\n"
	"# [site docs.example]\n"
	"# root /System/Web/docs\n";

const char *config_template(size_t *len)
{
	if (len)
		*len = sizeof(TEMPLATE) - 1;
	return TEMPLATE;
}

/*
 * The words a console line is built from.
 *
 * Short, lower case, and no full stop: the caller writes
 * `line 12: two sites have the same name (shop.example)`, so these have to read
 * as a fragment rather than a sentence.
 */
const char *config_reason(int verdict)
{
	switch (verdict) {
	case CONFIG_OK:          return "no fault";
	case CONFIG_ELINE_LONG:  return "a line is too long";
	case CONFIG_EUNKNOWN:    return "no such setting";
	case CONFIG_EREPEATED:   return "the same setting twice";
	case CONFIG_EMISPLACED:  return "that setting does not belong there";
	case CONFIG_EVALUE:      return "a value this cannot use";
	case CONFIG_ESECTION:    return "a section header this cannot read";
	case CONFIG_ETOOMANY:    return "more sites than this server holds";
	case CONFIG_EDUPLICATE:  return "two sites with the same name";
	case CONFIG_EINCOMPLETE: return "a site with no root";
	case CONFIG_EFILE_LONG:  return "the file is too large";
	default:                 return "unknown";
	}
}

/* --- generations ----------------------------------------------------------
 *
 * See `config.h` for why a configuration is superseded rather than edited. This
 * is the naming, and it is deliberately the same shape as `logfile.c`'s: the
 * two solve one problem and a reader who has understood one has understood the
 * other.
 */

long config_generation_name(unsigned long number, char *out, size_t room)
{
	static const char SUFFIX[] = ".conf";
	size_t i;

	if (!out || room < CONFIG_NAME_MAX)
		return -1;
	if (number > CONFIG_MAX_NUMBER)
		return -1;

	/* Written backwards from the last digit, which is what makes the
	 * zero-padding fall out rather than needing a second pass. */
	for (i = CONFIG_DIGITS; i > 0; i--) {
		out[i - 1] = (char)('0' + (int)(number % 10));
		number /= 10;
	}
	for (i = 0; i < sizeof(SUFFIX) - 1; i++)
		out[CONFIG_DIGITS + i] = SUFFIX[i];
	out[CONFIG_DIGITS + sizeof(SUFFIX) - 1] = 0;

	return (long)(CONFIG_DIGITS + sizeof(SUFFIX) - 1);
}

int config_is_generation(const char *name, size_t len, unsigned long *number)
{
	static const char SUFFIX[] = ".conf";
	unsigned long n = 0;
	size_t i;

	if (!name || len != CONFIG_DIGITS + sizeof(SUFFIX) - 1)
		return 0;

	for (i = 0; i < CONFIG_DIGITS; i++) {
		if (!is_digit(name[i]))
			return 0;
		n = n * 10UL + (unsigned long)(name[i] - '0');
	}
	for (i = 0; i < sizeof(SUFFIX) - 1; i++) {
		if (name[CONFIG_DIGITS + i] != SUFFIX[i])
			return 0;
	}

	if (number)
		*number = n;
	return 1;
}

unsigned long config_generation_next(const char *names, size_t len,
                                     unsigned long *highest)
{
	unsigned long best = 0;
	size_t at = 0;

	while (at < len) {
		size_t end = at;
		unsigned long n = 0;

		while (end < len && names[end])
			end++;

		if (config_is_generation(names + at, end - at, &n) && n > best)
			best = n;

		at = end + 1;		/* step over the terminator */
	}

	if (highest)
		*highest = best;
	return best + 1;
}
