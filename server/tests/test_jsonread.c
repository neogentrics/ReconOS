/*
 * Reading JSON, and every document that means two things.
 *
 * JSON has a specification and a much larger set of things parsers accept
 * anyway: comments, trailing commas, single quotes, `NaN`, `01`, a lone
 * surrogate, the same key twice. Every one of those is a place where the
 * client's parser and this one read one document differently -- which is
 * request smuggling with a different syntax, and it is why most of this file
 * is documents that must be **refused**.
 *
 * --- The two narrowings that are not about taste ---
 *
 * **No floating point**, because this machine has none: the init program is
 * built with `-mno-80387 -mno-sse`. A number is kept as its text and read back
 * as an integer or not at all, and the checks below hold that line -- `1.5` is
 * a valid number that `json_int` refuses, which is a different thing from
 * being invalid.
 *
 * **ASCII only**, to stay symmetric with `json.c`, which cannot emit a byte
 * above 0x7F. A reader that accepted more would accept a value this server
 * cannot report back.
 */

#include "../http/jsonread.h"

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

static int parse(const char *text, struct json *into)
{
	return json_parse(text, strlen(text), into);
}

static void accepts(const char *text, const char *what)
{
	struct json doc;
	int rc = parse(text, &doc);

	checks++;
	if (rc != JSON_OK) {
		failures++;
		printf("  FAIL  %s -- refused with %d (%s)\n", what, rc,
		       json_reason(rc));
	}
}

static void refuses(const char *text, int verdict, const char *what)
{
	struct json doc;
	int rc = parse(text, &doc);

	checks++;
	if (rc != verdict) {
		failures++;
		printf("  FAIL  %s -- verdict %d (%s), wanted %d\n", what, rc,
		       json_reason(rc), verdict);
	}
}

int main(void)
{
	struct json doc;

	printf("reading JSON, and every document that means two things\n");

	/* --- documents that must be read -------------------------------------- */
	accepts("{}", "an empty object");
	accepts("[]", "an empty array");
	accepts("{\"a\":1}", "one member");
	accepts("{\"a\":1,\"b\":2}", "two members");
	accepts("[1,2,3]", "an array of numbers");
	accepts("{\"a\":{\"b\":[1,{\"c\":null}]}}", "objects and arrays nested");
	accepts("  \t\r\n {\"a\" : 1}  \n ", "whitespace around and inside");
	accepts("true", "a bare true is a document");
	accepts("null", "so is null");
	accepts("\"just a string\"", "so is a string");
	accepts("-0", "negative zero, which the grammar allows");
	accepts("0", "zero on its own");
	accepts("1.5", "a fraction is a valid number");
	accepts("1e3", "so is an exponent");
	accepts("-2.5E-3", "and all of it together");
	accepts("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "every simple escape");
	accepts("\"\\u0041\"", "a \\u escape inside ASCII");

	/* --- what is in them -------------------------------------------------- */
	{
		const struct json_node *root, *a, *b;
		const char *s;
		size_t len = 0;
		long n = 0;
		int flag = 0;

		ok(parse("{\"name\":\"M16\",\"port\":80,\"on\":true,"
		         "\"off\":false,\"none\":null,\"list\":[10,20]}", &doc)
		   == JSON_OK, "a document with one of everything");

		root = json_root(&doc);
		ok(root != 0 && root->kind == JSON_OBJECT, "the root is an object");
		ok(json_count(&doc, root) == 6, "with six members");

		a = json_member(&doc, root, "name");
		s = json_string(&doc, a, &len);
		ok(s != 0 && len == 3 && strcmp(s, "M16") == 0,
		   "a string member reads back");

		a = json_member(&doc, root, "port");
		ok(json_int(&doc, a, &n) && n == 80, "a number reads back");

		a = json_member(&doc, root, "on");
		ok(json_bool(&doc, a, &flag) == 1 && flag == 1, "true reads back");
		a = json_member(&doc, root, "off");
		ok(json_bool(&doc, a, &flag) == 1 && flag == 0, "false reads back");

		a = json_member(&doc, root, "none");
		ok(a != 0 && a->kind == JSON_NULL, "null is a value, not an absence");
		ok(json_bool(&doc, a, &flag) == 0, "and null is not a boolean");

		a = json_member(&doc, root, "list");
		ok(a != 0 && a->kind == JSON_ARRAY && json_count(&doc, a) == 2,
		   "an array member, with two in it");
		b = json_at(&doc, a, 1);
		ok(json_int(&doc, b, &n) && n == 20, "and it can be indexed");
		ok(json_at(&doc, a, 2) == 0, "past the end is nothing");

		ok(json_member(&doc, root, "nope") == 0,
		   "a member that is not there is nothing");
		/*
		 * A key that is a prefix of a real one. A comparison that
		 * stopped at the stored length would match `nam` to `name`,
		 * which is a handler reading a field the client did not send.
		 */
		ok(json_member(&doc, root, "nam") == 0,
		   "and a prefix of a key is not that key");
		ok(json_member(&doc, root, "names") == 0,
		   "nor is a longer name");
		/* JSON keys are case-sensitive. A server that matched `Name`
		 * would accept a document a strict client thinks it did not
		 * send. */
		ok(json_member(&doc, root, "Name") == 0,
		   "nor the same key in another case");
	}

	/* --- numbers, and the machine with no floating point -------------------- */
	{
		const struct json_node *root;
		long n = 0;

		ok(parse("1.5", &doc) == JSON_OK, "a fraction parses");
		ok(json_int(&doc, json_root(&doc), &n) == 0,
		   "and json_int refuses it rather than rounding");

		ok(parse("1e3", &doc) == JSON_OK, "an exponent parses");
		ok(json_int(&doc, json_root(&doc), &n) == 0,
		   "and json_int refuses that too -- 1000 would be this "
		   "parser's arithmetic, not the document's");

		ok(parse("-17", &doc) == JSON_OK, "a negative integer parses");
		ok(json_int(&doc, json_root(&doc), &n) && n == -17,
		   "and reads back negative");

		ok(parse("999999999999999999999", &doc) == JSON_OK,
		   "a number past any integer is still a valid number");
		ok(json_int(&doc, json_root(&doc), &n) == 0,
		   "and json_int refuses it rather than wrapping");

		ok(parse("\"80\"", &doc) == JSON_OK, "a quoted number parses");
		root = json_root(&doc);
		ok(json_int(&doc, root, &n) == 0,
		   "and is not a number -- several parsers disagree, which is "
		   "the point");
	}

	/* --- strings, and the NUL that is legal ---------------------------------- */
	{
		const char *s;
		size_t len = 0;

		ok(parse("\"a\\u0000b\"", &doc) == JSON_OK,
		   "\\u0000 is valid JSON and is accepted");
		s = json_string(&doc, json_root(&doc), &len);
		ok(s != 0 && len == 3,
		   "and the length says three, which the terminator alone "
		   "could not");
		ok(s != 0 && s[0] == 'a' && s[1] == '\0' && s[2] == 'b',
		   "with the NUL in the middle where it was sent");

		ok(parse("\"\"", &doc) == JSON_OK, "an empty string");
		s = json_string(&doc, json_root(&doc), &len);
		ok(s != 0 && len == 0 && s[0] == '\0', "reads back as empty");
	}

	/* --- and every document that must be refused ----------------------------- */

	refuses("", JSON_EEMPTY, "nothing at all");
	refuses("   \n  ", JSON_EEMPTY, "nothing but whitespace");

	/*
	 * The same key twice.
	 *
	 * RFC 8259 says the behaviour is undefined and real parsers differ --
	 * some take the first, some the last. `{"role":"reader","role":"admin"}`
	 * is then two documents depending on who reads it, and the one that
	 * checked the permission may not be the one that acted.
	 */
	refuses("{\"role\":\"reader\",\"role\":\"admin\"}", JSON_EDUPLICATE,
	        "the same key twice");
	refuses("{\"a\":1,\"a\":1}", JSON_EDUPLICATE,
	        "and twice with the same value, which is still two answers");
	/* Hidden behind an escape. A duplicate check that compared the raw
	 * bytes would not see this one, which is exactly why it is done on the
	 * decoded key. */
	refuses("{\"a\":1,\"\\u0061\":2}", JSON_EDUPLICATE,
	        "the same key twice, one of them spelled with an escape");
	accepts("{\"a\":1,\"A\":2}", "two keys differing only in case are two keys");

	/* Syntax that several parsers allow and no version of JSON does. */
	refuses("{\"a\":1,}", JSON_EMALFORMED, "a trailing comma in an object");
	refuses("[1,]", JSON_EMALFORMED, "a trailing comma in an array");
	refuses("[,1]", JSON_EMALFORMED, "a leading comma");
	refuses("[1,,2]", JSON_EMALFORMED, "two commas");
	refuses("{'a':1}", JSON_EMALFORMED, "single quotes");
	refuses("{a:1}", JSON_EMALFORMED, "an unquoted key");
	refuses("{\"a\"1}", JSON_EMALFORMED, "a member with no colon");
	refuses("{\"a\":}", JSON_EMALFORMED, "a member with no value");
	refuses("{\"a\"}", JSON_EMALFORMED, "a key with nothing after it");
	refuses("[1 2]", JSON_EMALFORMED, "two values with no comma");
	refuses("// a comment\n{}", JSON_EMALFORMED, "a comment");
	refuses("{} // a comment", JSON_ETRAILING, "a comment after the document");

	/* Unclosed, mismatched, and closed without being open. */
	refuses("{", JSON_EMALFORMED, "an object that never closes");
	refuses("[", JSON_EMALFORMED, "an array that never closes");
	refuses("{\"a\":1", JSON_EMALFORMED, "an object cut off after a value");
	refuses("{\"a\":1]", JSON_EMALFORMED, "an object closed with a bracket");
	refuses("[1}", JSON_EMALFORMED, "an array closed with a brace");
	refuses("}", JSON_EMALFORMED, "a closing brace on its own");
	refuses("]", JSON_EMALFORMED, "a closing bracket on its own");
	refuses("\"unterminated", JSON_EMALFORMED, "a string that never ends");

	/* More than one document. This is where a logged or signed request and
	 * the request that was acted on come apart. */
	refuses("{\"a\":1}{\"b\":2}", JSON_ETRAILING, "two documents");
	refuses("{\"a\":1} junk", JSON_ETRAILING, "text after the document");
	refuses("1 2", JSON_ETRAILING, "two numbers");
	accepts("{\"a\":1}   \n\t ", "trailing whitespace is not trailing text");

	/* Numbers that some reader somewhere accepts. Each of these is a value
	 * to that reader and a refusal here, which is the disagreement. */
	refuses("01", JSON_EMALFORMED, "a leading zero");
	refuses("-01", JSON_EMALFORMED, "a negative leading zero");
	refuses("+1", JSON_EMALFORMED, "a leading plus");
	refuses(".5", JSON_EMALFORMED, "a number with no integer part");
	refuses("1.", JSON_EMALFORMED, "a number with nothing after the point");
	refuses("1e", JSON_EMALFORMED, "an exponent with no digits");
	refuses("1e+", JSON_EMALFORMED, "an exponent with a sign and no digits");
	refuses("0x10", JSON_EMALFORMED, "hexadecimal");
	refuses("-", JSON_EMALFORMED, "a minus on its own");
	refuses("NaN", JSON_EMALFORMED, "NaN");
	refuses("Infinity", JSON_EMALFORMED, "Infinity");
	refuses("-Infinity", JSON_EMALFORMED, "negative Infinity");
	refuses("1x", JSON_EMALFORMED, "a number with a letter stuck to it");

	/* Words. `truex` must not be `true` with something after it: inside an
	 * array a lenient reader would take `[1x]` as `[1]`. */
	refuses("truex", JSON_EMALFORMED, "a word with a letter stuck to it");
	refuses("tru", JSON_EMALFORMED, "a word cut short");
	refuses("True", JSON_EMALFORMED, "a capitalised word");
	refuses("[1x]", JSON_EMALFORMED, "a value with a letter stuck to it, "
	                                 "inside an array");

	/* Strings, and the bytes that must not be in one. */
	refuses("\"a\nb\"", JSON_EMALFORMED, "a raw newline inside a string");
	refuses("\"a\tb\"", JSON_EMALFORMED, "a raw tab inside a string");
	refuses("\"\\z\"", JSON_EMALFORMED, "an escape this does not know");
	refuses("\"\\u12\"", JSON_EMALFORMED, "a \\u escape cut short");
	refuses("\"\\u12g4\"", JSON_EMALFORMED, "a \\u escape that is not hex");
	refuses("\"\\\"", JSON_EMALFORMED, "a string ending in a lone backslash");

	/* Above ASCII, in both forms. See `jsonread.h`: this server cannot
	 * report such a value back, so it does not accept one. */
	refuses("\"\\u00e9\"", JSON_ENONASCII, "a \\u escape above ASCII");
	refuses("\"\\ud800\"", JSON_ENONASCII, "a lone surrogate half");
	refuses("\"caf\xc3\xa9\"", JSON_ENONASCII, "a raw UTF-8 byte");
	refuses("{\"caf\xc3\xa9\":1}", JSON_ENONASCII, "and one in a key");

	/* A NUL anywhere, including where this parser would never look. */
	{
		static const char INSIDE[] = "{\"a\":\"b\0c\"}";
		static const char AFTER[]  = "{\"a\":1}\0";

		ok(json_parse(INSIDE, sizeof(INSIDE) - 1, &doc)
		   == JSON_EMALFORMED, "a NUL inside a string is refused");
		ok(json_parse(AFTER, sizeof(AFTER) - 1, &doc)
		   == JSON_EMALFORMED,
		   "and one after the document, where this would never have "
		   "looked, is refused too");
	}

	/* --- bounds --------------------------------------------------------------- */
	{
		static char DEEP[256];
		/*
		 * Large enough for the longest thing built below, which is the
		 * pool-overflow case: a string of `JSON_TEXT_MAX + 16`
		 * characters plus its quotes. The first version of this was a
		 * kilobyte and overran by half again -- caught not by the
		 * plain `gcc` line used while writing it but by the suite
		 * runner, which builds with `-O2 -Werror` and turns glibc's
		 * fortify warning into a failure. A bound written from the
		 * largest case rather than from a guess.
		 */
		static char WIDE[JSON_TEXT_MAX + 64];
		static char BIG[JSON_INPUT_MAX + 64];
		size_t at;
		int i;

		/* One level past what this reads. A bound reached by running
		 * out of C stack is a bound enforced by a crash. */
		at = 0;
		for (i = 0; i < JSON_DEPTH_MAX + 1; i++)
			DEEP[at++] = '[';
		DEEP[at++] = '1';
		for (i = 0; i < JSON_DEPTH_MAX + 1; i++)
			DEEP[at++] = ']';
		DEEP[at] = '\0';
		refuses(DEEP, JSON_EDEPTH, "nested one level too deep");

		at = 0;
		for (i = 0; i < JSON_DEPTH_MAX; i++)
			DEEP[at++] = '[';
		DEEP[at++] = '1';
		for (i = 0; i < JSON_DEPTH_MAX; i++)
			DEEP[at++] = ']';
		DEEP[at] = '\0';
		accepts(DEEP, "and exactly as deep as it reads");

		/* More values than there are nodes. */
		at = (size_t)sprintf(WIDE, "[");
		for (i = 0; i < JSON_NODES_MAX; i++)
			at += (size_t)sprintf(WIDE + at, "%s1", i ? "," : "");
		sprintf(WIDE + at, "]");
		refuses(WIDE, JSON_ENODES, "more values than this holds");

		/* A document past the input bound, refused before a byte of it
		 * is parsed. */
		for (at = 0; at < sizeof(BIG) - 1; at++)
			BIG[at] = ' ';
		BIG[0] = '[';
		BIG[1] = ']';
		BIG[sizeof(BIG) - 1] = '\0';
		refuses(BIG, JSON_EINPUT, "a document past the input bound");

		/* More decoded text than the pool holds. */
		at = (size_t)sprintf(WIDE, "\"");
		for (i = 0; i < JSON_TEXT_MAX + 16; i++)
			WIDE[at++] = 'y';
		sprintf(WIDE + at, "\"");
		{
			struct json got;

			ok(json_parse(WIDE, at + 1, &got) == JSON_EROOM,
			   "more string than this holds");
		}
	}

	/* --- where a refusal happened --------------------------------------------- */
	{
		ok(parse("{\"a\":1,\"a\":2}", &doc) == JSON_EDUPLICATE,
		   "a duplicate, to look at what is reported");
		ok(doc.where > 6,
		   "and the offset points past the first member, not at the "
		   "start of the document");

		ok(parse("[1,2,x]", &doc) == JSON_EMALFORMED,
		   "a bad value, to look at what is reported");
		ok(doc.where == 5, "and the offset is the byte that was wrong");
	}

	/* --- a refusal leaves nothing usable --------------------------------------- */
	{
		struct json got;

		ok(parse("{\"a\":1,\"b\":", &got) == JSON_EMALFORMED,
		   "a document that is fine until it is not");
		ok(json_root(&got) == 0 || json_count(&got, json_root(&got)) == 0
		   || json_member(&got, json_root(&got), "a") == 0,
		   "and what parsed before the fault is not offered to a caller");
	}

	/* --- every verdict has words ----------------------------------------------- */
	{
		static const int CODES[] = {
			JSON_OK, JSON_EMALFORMED, JSON_EDEPTH, JSON_ENODES,
			JSON_EROOM, JSON_EDUPLICATE, JSON_ENONASCII,
			JSON_ETRAILING, JSON_EEMPTY, JSON_EINPUT
		};
		size_t i;
		int all = 1;

		for (i = 0; i < sizeof(CODES) / sizeof(CODES[0]); i++) {
			const char *say = json_reason(CODES[i]);

			if (!say || strcmp(say, "unknown") == 0)
				all = 0;
		}
		ok(all, "every verdict this file can return has words for it");
		ok(strcmp(json_reason(-999), "unknown") == 0,
		   "and one it cannot return says so rather than crashing");
	}

	/* --- accessors handed the wrong thing --------------------------------------
	 *
	 * A handler reads a document a client sent, so every one of these is a
	 * shape somebody will send. None of them may be a crash. */
	{
		const struct json_node *root;

		ok(parse("[1,2]", &doc) == JSON_OK, "an array, for the accessors");
		root = json_root(&doc);
		ok(json_member(&doc, root, "a") == 0,
		   "asking an array for a member gives nothing");
		ok(json_string(&doc, root, 0) == 0,
		   "asking an array for a string gives nothing");
		ok(json_count(&doc, json_at(&doc, root, 0)) == 0,
		   "counting the children of a number gives zero");
		ok(json_member(&doc, 0, "a") == 0, "and so does asking nothing");
		ok(json_at(&doc, 0, 0) == 0, "and indexing nothing");
		ok(json_string(&doc, 0, 0) == 0, "and reading nothing");
		ok(json_root(0) == 0, "and a null document has no root");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
