/*
 * Reading a configuration file, and every file that must not be believed.
 *
 * `config.c` is pure -- bytes in, a `struct config` or a refusal out -- so
 * every case here is a string literal and the whole suite runs in
 * milliseconds, with no filesystem and no machine. That is the same
 * arrangement `test_http.c` has and for the same reason: the part that faces
 * somebody's typing is the part worth testing exhaustively.
 *
 * --- What this is really checking ---
 *
 * Not that a correct file parses. That is one check and it is the easy one.
 *
 * What matters is that **a file with a fault in it is refused rather than
 * half-applied**, and that the refusal says where. A configuration parser that
 * shrugs at a line it does not understand produces a machine running something
 * nobody wrote, and the person who wrote the file has no way to find out --
 * they read it, it looks right, and the server disagrees silently.
 *
 * So the majority of the checks below are files that must not be accepted, and
 * every one of them also asserts the line number, because a refusal that does
 * not say where has moved the work back to the person.
 */

#include "../config.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* Parse a NUL-terminated literal. The length is taken from the literal rather
 * than passed, because every case here is written as source. */
static int parse(const char *text, struct config *into)
{
	return config_parse(text, strlen(text), into);
}

/* A refusal, its code and its line, in one check each. */
static void refuses(const char *text, int verdict, unsigned line,
                    const char *what)
{
	struct config c;
	int got = parse(text, &c);

	checks++;
	if (got != verdict) {
		failures++;
		printf("  FAIL  %s -- verdict %d, wanted %d\n", what, got,
		       verdict);
		return;
	}
	checks++;
	if (c.line != line) {
		failures++;
		printf("  FAIL  %s -- line %u, wanted %u\n", what, c.line,
		       line);
	}
}

int main(void)
{
	struct config c;

	printf("reading a configuration, and the files that must not be believed\n");

	/* --- nothing at all ---------------------------------------------------
	 *
	 * An empty file is a valid configuration: it says nothing, so every
	 * default stands. This is the case a parser that refuses empty input
	 * gets wrong, and it is the most likely file in the world -- somebody
	 * created it and has not typed anything yet. */
	ok(parse("", &c) == CONFIG_OK, "an empty file is accepted");
	ok(c.site_count == 0, "and configures no sites");
	ok(c.has_name == 0 && c.has_port == 0,
	   "and sets nothing, so every default stands");

	ok(parse("\n\n\n", &c) == CONFIG_OK, "blank lines are accepted");
	ok(parse("# just a comment\n", &c) == CONFIG_OK,
	   "so is a file of comments");
	ok(parse("   \t  \n", &c) == CONFIG_OK, "so is a line of whitespace");

	/* --- the ordinary file ------------------------------------------------ */
	ok(parse("name m16\nport 80\n", &c) == CONFIG_OK,
	   "a name and a port parse");
	ok(c.has_name && strcmp(c.name, "m16") == 0, "the name is kept");
	ok(c.has_port && c.port == 80, "the port is kept");

	ok(parse("  name m16  \n", &c) == CONFIG_OK,
	   "a line may be indented");
	ok(strcmp(c.name, "m16") == 0,
	   "and the value carries no surrounding whitespace");

	ok(parse("name m16\r\nport 80\r\n", &c) == CONFIG_OK,
	   "a file written on a machine that ends lines with CRLF is read");
	ok(strcmp(c.name, "m16") == 0,
	   "and the carriage return is not part of the value");

	ok(parse("name m16", &c) == CONFIG_OK,
	   "a last line with no newline is still a line");
	ok(strcmp(c.name, "m16") == 0, "and its value arrives whole");

	/* A comment is only a comment at the start of a line. `#` in a value is
	 * a `#`: a path may contain one, and a parser that cut the line there
	 * would serve a different directory than the one written. */
	ok(parse("[site a.example]\nroot /System/Web/a#b\n", &c) == CONFIG_OK,
	   "a # inside a value is part of the value");
	ok(strcmp(c.sites[0].root, "/System/Web/a#b") == 0,
	   "and the value is not cut at it");

	/* --- sites ------------------------------------------------------------ */
	ok(parse("[site shop.example]\nroot /System/Web/shop\n", &c)
	   == CONFIG_OK, "a site parses");
	ok(c.site_count == 1, "and there is one of it");
	ok(strcmp(c.sites[0].host, "shop.example") == 0, "with its name");
	ok(strcmp(c.sites[0].root, "/System/Web/shop") == 0, "and its root");
	ok(strcmp(c.sites[0].index, "index.html") == 0,
	   "and an index, defaulted rather than left empty");

	ok(parse("[site a.example]\nroot /System/Web/a\nindex start.html\n", &c)
	   == CONFIG_OK, "an index can be given");
	ok(strcmp(c.sites[0].index, "start.html") == 0, "and replaces the default");

	ok(parse("name m16\n"
	         "[site a.example]\nroot /System/Web/a\n"
	         "[site b.example]\nroot /System/Web/b\n", &c) == CONFIG_OK,
	   "two sites after a top-level setting");
	ok(c.site_count == 2, "and both are kept");
	ok(strcmp(c.sites[0].host, "a.example") == 0 &&
	   strcmp(c.sites[1].host, "b.example") == 0,
	   "in the order they were written -- which is the order they answer in");

	/* --- what must be refused --------------------------------------------- */

	/*
	 * A key nobody has heard of.
	 *
	 * The single most valuable refusal in the file. A typo -- `prot 80`,
	 * `Root /System/Web` -- is the ordinary way a configuration goes wrong,
	 * and a parser that ignores what it does not recognise leaves a machine
	 * running a default while the file on disk says otherwise. Every reader
	 * of that file afterwards is reading a lie.
	 */
	refuses("name m16\nprot 80\n", CONFIG_EUNKNOWN, 2, "a misspelled key");
	refuses("Name m16\n", CONFIG_EUNKNOWN, 1,
	        "a key in the wrong case -- these are not case-insensitive");
	refuses("name m16\nport 80\nextra yes\n", CONFIG_EUNKNOWN, 3,
	        "an unknown key after two good ones");

	/* A real key in the wrong place, which is a different fault from a key
	 * that does not exist, and says so. */
	refuses("root /System/Web\n", CONFIG_EMISPLACED, 1,
	        "a site setting before any site");
	refuses("[site a.example]\nroot /System/Web/a\nport 80\n",
	        CONFIG_EMISPLACED, 3,
	        "a top-level setting after a site has opened");

	/* Twice. The same rule `request.c` holds for `Content-Length` and
	 * `Host`: agreement is not the property that makes a message safe. */
	refuses("name m16\nname m17\n", CONFIG_EREPEATED, 2,
	        "a setting given twice");
	refuses("name m16\nname m16\n", CONFIG_EREPEATED, 2,
	        "and twice with the same value, which is still two answers");
	refuses("[site a.example]\nroot /a/b\nroot /a/c\n", CONFIG_EREPEATED, 3,
	        "a site setting given twice");

	/* A key with nothing after it. `port` alone is not "the default port",
	 * it is a line somebody did not finish. */
	refuses("port\n", CONFIG_EVALUE, 1, "a key with no value");
	refuses("port    \n", CONFIG_EVALUE, 1,
	        "a key with nothing but whitespace after it");

	/* --- values that do not mean what they say ----------------------------- */
	refuses("port 0\n", CONFIG_EVALUE, 1, "port 0");
	refuses("port 65536\n", CONFIG_EVALUE, 1, "a port past the end");
	refuses("port 80x\n", CONFIG_EVALUE, 1, "a port with a letter in it");
	refuses("port 0x50\n", CONFIG_EVALUE, 1, "a port in hexadecimal");
	refuses("port -1\n", CONFIG_EVALUE, 1, "a negative port");
	refuses("port 8 0\n", CONFIG_EVALUE, 1, "a port with a space in it");
	ok(parse("port 65535\n", &c) == CONFIG_OK && c.port == 65535,
	   "the last real port is accepted");

	refuses("resolver 10.0.2\n", CONFIG_EVALUE, 1, "three parts of an address");
	refuses("resolver 10.0.2.256\n", CONFIG_EVALUE, 1, "a part over 255");
	refuses("resolver 10.0.2.3.4\n", CONFIG_EVALUE, 1, "five parts");
	refuses("resolver 10.0.2.\n", CONFIG_EVALUE, 1, "a trailing dot");
	refuses("resolver dns.example\n", CONFIG_EVALUE, 1,
	        "a name -- resolving it would need the resolver this line sets");
	ok(parse("resolver 10.0.2.3\n", &c) == CONFIG_OK &&
	   strcmp(c.resolver, "10.0.2.3") == 0, "an address is accepted");

	/* --- the section header ------------------------------------------------ */
	refuses("[site]\n", CONFIG_ESECTION, 1, "a site with no name");
	refuses("[site ]\n", CONFIG_ESECTION, 1, "a site with an empty name");
	refuses("[site a.example\n", CONFIG_ESECTION, 1, "an unclosed bracket");
	refuses("[host a.example]\n", CONFIG_ESECTION, 1,
	        "a section that is not a site");
	refuses("[site my name]\n", CONFIG_ESECTION, 1,
	        "a name with a space in it -- one space, one name");
	refuses("[site .example]\n", CONFIG_ESECTION, 1, "a leading dot");
	refuses("[site a..example]\n", CONFIG_ESECTION, 1, "an empty label");
	refuses("[site a.example:8080]\n", CONFIG_ESECTION, 1,
	        "a port on a site name, which could never match a request");
	refuses("[site a.example]\nroot /a\n[site a.example]\nroot /b\n",
	        CONFIG_EDUPLICATE, 3,
	        "two sites with one name -- the second could never answer");

	/* A site that says nothing about where its files are. There is no
	 * sensible default: serving the console's root under somebody else's
	 * name is the one outcome nobody wanted. */
	refuses("[site a.example]\n", CONFIG_EINCOMPLETE, 1,
	        "a site with no root, at the end of the file");
	refuses("[site a.example]\n[site b.example]\nroot /b\n",
	        CONFIG_EINCOMPLETE, 1,
	        "a site with no root, reported at its own line and not the next one");
	refuses("[site a.example]\nindex start.html\n", CONFIG_EINCOMPLETE, 1,
	        "an index does not make a site complete");

	/* --- a site that asks somebody else ----------------------------------
	 *
	 * `proxy` is the other answer to the question `root` answers: where do
	 * this name's replies come from. So the checks here are mostly about
	 * the two not being allowed together, and about a malformed upstream
	 * stopping the configuration from loading rather than one request at a
	 * time.
	 */
	{
		struct config c;

		ok(parse("[site a.example]\nproxy 10.0.2.2:8080\n",
		         &c) == CONFIG_OK,
		   "a site may proxy instead of having a root");
	}

	refuses("[site a.example]\nroot /a\nproxy 10.0.2.2:8080\n",
	        CONFIG_ECONFLICT, 3,
	        "a site may not both serve and proxy -- two answers to one "
	        "question, resolved by whichever the code checked first");
	refuses("[site a.example]\nproxy 10.0.2.2:8080\nroot /a\n",
	        CONFIG_ECONFLICT, 3,
	        "and the refusal does not depend on which was written first");

	refuses("[site a.example]\nproxy 10.0.2.2\n", CONFIG_EVALUE, 2,
	        "an upstream without a port is refused -- there is no sensible "
	        "default to dial");
	refuses("[site a.example]\nproxy 10.0.2.2:0\n", CONFIG_EVALUE, 2,
	        "port zero is refused: it is the wildcard a listener uses and "
	        "nothing to dial");
	refuses("[site a.example]\nproxy 10.0.2.2:70000\n", CONFIG_EVALUE, 2,
	        "and a port past 65535");
	refuses("[site a.example]\nproxy 10.0.2.999:80\n", CONFIG_EVALUE, 2,
	        "and an address that is not four bytes");
	refuses("[site a.example]\nproxy upstream.example:80\n",
	        CONFIG_EVALUE, 2,
	        "and a name, which would need a resolver to read the "
	        "configuration that says where the resolver is");
	refuses("[site a.example]\nproxy :80\n", CONFIG_EVALUE, 2,
	        "and a port with no host");
	refuses("[site a.example]\nproxy 10.0.2.2:\n", CONFIG_EVALUE, 2,
	        "and a host with no port");
	refuses("[site a.example]\nproxy 10.0.2.2:80\nproxy 10.0.2.3:80\n",
	        CONFIG_EREPEATED, 3,
	        "and two upstreams for one site");

	/* The value is kept both ways, and the parsed half is what `dial.h`
	 * takes. Checked because a parser that validated without producing
	 * would push the second reading to the point of use. */
	{
		struct config c;
		const char *text = "[site a.example]\nproxy 10.0.2.2:8080\n";

		if (config_parse(text, strlen(text), &c) == CONFIG_OK
		    && c.site_count == 1) {
			ok(c.sites[0].proxies == 1,
			   "a proxying site says so");
			ok(strcmp(c.sites[0].upstream, "10.0.2.2:8080") == 0,
			   "and keeps what the file said, for the console to "
			   "show back");
			ok(c.sites[0].upstream_addr == 0x0A000202u,
			   "and the address parsed once, here, rather than "
			   "again at the point of use");
			ok(c.sites[0].upstream_port == 8080,
			   "and the port beside it");
		} else {
			ok(0, "a proxying site says so");
			ok(0, "and keeps what the file said");
			ok(0, "and the address parsed once");
			ok(0, "and the port beside it");
		}
	}

	/* --- the root, which is the boundary everything else is checked against - */

	/*
	 * A root that climbs.
	 *
	 * `files.c` refuses a *request* that climbs, and it does that by
	 * resolving against the root. A root containing `..` moves the boundary
	 * itself, so nothing afterwards is refused -- because nothing
	 * afterwards is wrong. `/System/Web/../..` is a correctly formed path
	 * to the whole volume, served under somebody's chosen name.
	 */
	refuses("[site a.example]\nroot /System/Web/../..\n", CONFIG_EVALUE, 2,
	        "a root that climbs out of itself");
	refuses("[site a.example]\nroot /System/../etc\n", CONFIG_EVALUE, 2,
	        "a root that climbs in the middle");
	refuses("[site a.example]\nroot /..\n", CONFIG_EVALUE, 2,
	        "a root that is nothing but a climb");
	ok(parse("[site a.example]\nroot /System/Web/a..b\n", &c) == CONFIG_OK,
	   "two dots inside a name are not a climb");

	refuses("[site a.example]\nroot System/Web\n", CONFIG_EVALUE, 2,
	        "a relative root -- relative to what, on a machine with no cwd");
	refuses("[site a.example]\nroot /\n", CONFIG_EVALUE, 2,
	        "the whole volume as a root");
	refuses("[site a.example]\nroot /System/Web/\n", CONFIG_EVALUE, 2,
	        "a trailing slash, so two roots cannot be one directory twice");
	refuses("[site a.example]\nroot /System//Web\n", CONFIG_EVALUE, 2,
	        "an empty component");
	refuses("[site a.example]\nroot /System\\Web\n", CONFIG_EVALUE, 2,
	        "a backslash, which is not a separator here and never will be");

	refuses("[site a.example]\nroot /a\nindex sub/start.html\n",
	        CONFIG_EVALUE, 3,
	        "an index with a path in it -- that is a root written twice");
	refuses("[site a.example]\nroot /a\nindex ..\n", CONFIG_EVALUE, 3,
	        "an index that is a climb");

	/* --- bytes that are not text -------------------------------------------- */
	{
		/* A NUL in the middle. A parser that stops at it reads a
		 * different file than one that does not, which is the whole of
		 * the smuggling argument in `http.h`, arriving a year later. */
		static const char NUL_INSIDE[] = "name m16\npo\0rt 80\n";
		struct config got;

		ok(config_parse(NUL_INSIDE, sizeof(NUL_INSIDE) - 1, &got)
		   == CONFIG_EVALUE, "a NUL byte inside the file is refused");
		ok(got.line == 2, "and reported at its line");
	}
	refuses("name m\x7f" "16\n", CONFIG_EVALUE, 1,
	        "a control byte in a value");
	refuses("name m16\n\tport\t80\x01\n", CONFIG_EVALUE, 2,
	        "a control byte at the end of a line");

	/* --- bounds ------------------------------------------------------------- */
	{
		static char LONG_LINE[CONFIG_LINE_MAX + 32];
		static char BIG[CONFIG_FILE_MAX + 64];
		struct config got;
		size_t i;

		for (i = 0; i < sizeof(LONG_LINE) - 2; i++)
			LONG_LINE[i] = 'a';
		LONG_LINE[sizeof(LONG_LINE) - 2] = '\n';
		LONG_LINE[sizeof(LONG_LINE) - 1] = '\0';
		ok(parse(LONG_LINE, &got) == CONFIG_ELINE_LONG,
		   "a line past the bound is refused, not truncated");

		for (i = 0; i < sizeof(BIG) - 1; i++)
			BIG[i] = '\n';
		BIG[sizeof(BIG) - 1] = '\0';
		ok(config_parse(BIG, sizeof(BIG) - 1, &got)
		   == CONFIG_EFILE_LONG, "a file past the bound is refused");
	}
	{
		/* One more site than this server holds. Refused, and not
		 * quietly dropped: a site that is in the file and not in the
		 * server is a site somebody will spend an afternoon on. */
		static const char NINE[] =
			"[site a1.example]\nroot /a1\n"
			"[site a2.example]\nroot /a2\n"
			"[site a3.example]\nroot /a3\n"
			"[site a4.example]\nroot /a4\n"
			"[site a5.example]\nroot /a5\n"
			"[site a6.example]\nroot /a6\n"
			"[site a7.example]\nroot /a7\n"
			"[site a8.example]\nroot /a8\n"
			"[site a9.example]\nroot /a9\n";

		refuses(NINE, CONFIG_ETOOMANY, 17, "one site too many");
	}

	/* --- a refusal leaves nothing usable ------------------------------------- */
	{
		struct config got;

		ok(parse("name m16\nport 80\nprot 8080\n", &got)
		   == CONFIG_EUNKNOWN, "a file with a fault late in it is refused");
		/*
		 * The two settings before the fault parsed perfectly, and a
		 * caller must not see them. This is the check that a refusal
		 * means *none of this file* rather than *the rest of this
		 * file*: a machine running the first two lines of a file it
		 * rejected is a machine in a state nobody described.
		 */
		ok(got.has_name == 0 && got.has_port == 0,
		   "and nothing from before the fault is left behind");
		ok(got.site_count == 0, "and no sites either");
	}

	/* --- what the console is going to print ---------------------------------- */
	{
		struct config got;

		ok(parse("name m16\nprot 80\n", &got) == CONFIG_EUNKNOWN,
		   "an unknown key, again, to look at what is reported");
		ok(strcmp(got.found, "prot") == 0,
		   "the offending word is reported, so a person can find it");

		ok(parse("[site a.example]\nroot /..\n", &got) == CONFIG_EVALUE,
		   "a bad root, to look at what is reported");
		ok(strcmp(got.found, "/..") == 0, "the offending value is reported");

		/* Whatever was in the file, what comes back is printable. It
		 * is about to go on a console and into a log, written by the
		 * machine that could not read the file -- so the bytes it
		 * could not read are the last bytes to trust. */
		ok(parse("name m\x01\x02z\n", &got) == CONFIG_EVALUE,
		   "a value with control bytes is refused");
		{
			size_t i;
			int clean = 1;

			for (i = 0; got.found[i]; i++) {
				if ((unsigned char)got.found[i] < 0x20 ||
				    (unsigned char)got.found[i] >= 0x7F)
					clean = 0;
			}
			ok(clean, "and what is reported back is printable");
		}
	}

	/* --- every verdict has words ---------------------------------------------
	 *
	 * The same lesson as `http_reason`: a table and a switch that are
	 * written separately drift, and the first anybody hears of it is a
	 * console line saying `unknown`. */
	{
		static const int CODES[] = {
			CONFIG_OK, CONFIG_ELINE_LONG, CONFIG_EUNKNOWN,
			CONFIG_EREPEATED, CONFIG_EMISPLACED, CONFIG_EVALUE,
			CONFIG_ESECTION, CONFIG_ETOOMANY, CONFIG_EDUPLICATE,
			CONFIG_EINCOMPLETE, CONFIG_EFILE_LONG
		};
		size_t i;
		int all = 1;

		for (i = 0; i < sizeof(CODES) / sizeof(CODES[0]); i++) {
			const char *say = config_reason(CODES[i]);

			if (!say || strcmp(say, "unknown") == 0)
				all = 0;
		}
		ok(all, "every verdict this file can return has words for it");
		ok(strcmp(config_reason(-999), "unknown") == 0,
		   "and one it cannot return says so rather than crashing");
	}

	/* --- the two rules, directly ---------------------------------------------- */
	ok(config_root_ok("/System/Web") == 1, "a plain root is usable");
	ok(config_root_ok("/a") == 1, "a short one is too");
	ok(config_root_ok("/") == 0, "the volume itself is not");
	ok(config_root_ok("") == 0, "nor is nothing");
	ok(config_root_ok(0) == 0, "nor is a null pointer");
	ok(config_root_ok("/a/../b") == 0, "nor is one that climbs");
	ok(config_root_ok("/a/..") == 0, "nor one that ends in a climb");
	ok(config_root_ok("..") == 0, "nor a bare climb");
	ok(config_root_ok("/a/...b") == 1, "three dots in a name are a name");

	ok(config_host_ok("a.example") == 1, "a host name is usable");
	ok(config_host_ok("a") == 1, "a single label is too");
	ok(config_host_ok("A.EXAMPLE") == 1,
	   "and so is a shouted one -- matching is case-insensitive");
	ok(config_host_ok("") == 0, "an empty name is not");
	ok(config_host_ok(0) == 0, "nor is a null pointer");
	ok(config_host_ok("a.example.") == 0,
	   "nor is a trailing dot, which is a form a request may use but a "
	   "configuration should not have two spellings of");
	ok(config_host_ok("a_b.example") == 0, "nor an underscore");
	ok(config_host_ok("a.example:80") == 0, "nor a port");
	ok(config_host_ok("a example") == 0, "nor a space");

	/* --- the file this server writes for somebody to edit ---------------------
	 *
	 * A template the parser refuses is the worst first impression a file
	 * format can make, and it is exactly the thing that breaks quietly: the
	 * day somebody adds a setting and updates the example beside it, nothing
	 * checks that the example is still a file this reads.
	 */
	{
		const char *text;
		size_t len = 0;
		struct config got;

		text = config_template(&len);
		ok(text != 0 && len > 0, "the template has text in it");
		ok(config_parse(text, len, &got) == CONFIG_OK,
		   "and the server's own template parses");
		ok(got.site_count == 0 && got.has_name == 0 && got.has_port == 0,
		   "and configures nothing, so writing it changes no machine");

		/* Every line a comment or blank. A template that quietly set
		 * something would configure a machine from a file nobody wrote. */
		{
			size_t i = 0;
			int all = 1;

			while (i < len) {
				size_t start = i;

				while (i < len && text[i] != '\n')
					i++;
				if (i > start && text[start] != '#')
					all = 0;
				if (i < len)
					i++;
			}
			ok(all, "and every line of it is a comment");
		}
	}

	/* --- generations, because a file here is written once ---------------------
	 *
	 * See `config.h`. A configuration is superseded rather than edited, and
	 * the numbering is what makes that work. It is the same shape as the
	 * log's, and it has the same two ways of going wrong: a name that does
	 * not sort chronologically, and a number taken from memory rather than
	 * from the directory.
	 */
	{
		char name[CONFIG_NAME_MAX];
		unsigned long n = 0;
		unsigned long highest = 0;

		ok(config_generation_name(1, name, sizeof(name)) == 11
		   && strcmp(name, "000001.conf") == 0,
		   "a generation is zero-padded, so a lexical sort is a "
		   "chronological one");
		ok(config_generation_name(999999, name, sizeof(name)) == 11
		   && strcmp(name, "999999.conf") == 0,
		   "and the last one fits");
		ok(config_generation_name(1000000, name, sizeof(name)) < 0,
		   "one past the end is refused rather than wrapping");
		ok(config_generation_name(1, name, 4) < 0,
		   "and so is a buffer too small to hold it");

		ok(config_is_generation("000007.conf", 11, &n) == 1 && n == 7,
		   "a generation's name reads back as its number");
		ok(config_is_generation("7.conf", 6, &n) == 0,
		   "an unpadded name is not one -- it would sort wrongly");
		ok(config_is_generation("000007.txt", 10, &n) == 0,
		   "nor is another suffix");
		ok(config_is_generation("00000a.conf", 11, &n) == 0,
		   "nor a letter among the digits");
		ok(config_is_generation("000007.conf", 10, &n) == 0,
		   "nor the right name with the wrong length");
		ok(config_is_generation("", 0, &n) == 0, "nor nothing at all");

		/*
		 * The next number comes from the listing.
		 *
		 * `logfile.h` records what happens when it comes from memory
		 * instead: a machine starting again at one each boot does not
		 * clobber anything, because `SYS_CREATE` refuses -- it fails
		 * every write from the second boot onwards, and says nothing.
		 */
		{
			static const char LISTING[] =
				"000001.conf\0" "000003.conf\0" "000002.conf\0";

			ok(config_generation_next(LISTING, sizeof(LISTING) - 1,
			                          &highest) == 4
			   && highest == 3,
			   "the next number is one past the highest, whatever "
			   "order the directory gives them in");
		}
		{
			static const char MIXED[] =
				"000002.conf\0" "notes.txt\0" "\0"
				"000010.conf\0" "9.conf\0";

			ok(config_generation_next(MIXED, sizeof(MIXED) - 1,
			                          &highest) == 11
			   && highest == 10,
			   "and names that are not generations are stepped "
			   "over, so a stray file cannot stop a machine "
			   "configuring itself");
		}
		{
			static const char EMPTY[] = "";

			ok(config_generation_next(EMPTY, 0, &highest) == 1
			   && highest == 0,
			   "an empty directory starts at one, and says there "
			   "is nothing in force");
		}
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
