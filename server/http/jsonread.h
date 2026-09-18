/*
 * Reading JSON, for an API that takes documents rather than form fields.
 *
 * --- Why this arrives now ---
 *
 * `docs/SERVER.md` has *structured REST / RPC management API* at **partial**,
 * and `docs/WEB.md` has said since 0.14.0 that the JSON writer *needs a parser
 * next*. Everything this server accepts today is `application/x-www-form-
 * urlencoded` or `multipart/form-data`, which are what an HTML form sends and
 * not what a management client sends. A Server Manager that can be driven by
 * another program needs to be able to read a document.
 *
 * `json.c` writes; this reads. They are separate files because escaping text
 * and parsing structure are different jobs with different failure modes, and
 * because the writer is used by handlers that never parse anything.
 *
 * --- The rule, which is the same rule as everywhere else here ---
 *
 * **Refuse rather than guess.** JSON has a specification and a much larger set
 * of things parsers accept anyway, and every one of those is a place where the
 * client's parser and the server's parser read one document differently. That
 * is the same fault as request smuggling with a different syntax: a signed
 * request whose signature is computed over one reading and whose meaning is
 * taken from another.
 *
 * So: no comments, no trailing commas, no single quotes, no unquoted keys, no
 * `NaN` or `Infinity`, no leading `+`, no leading zeros, no bare `.5` or `1.`,
 * no control bytes inside a string, no trailing text after the document.
 *
 * **And no duplicate keys**, which is the one worth naming. RFC 8259 says the
 * behaviour is undefined, and real parsers differ: some take the first, some
 * the last. `{"role":"reader","role":"admin"}` is then genuinely two documents
 * depending on who reads it, which is exactly the shape the rest of this server
 * refuses. It is refused here too.
 *
 * --- Two narrowings that come from this machine rather than from taste ---
 *
 * **1. No floating point, because this machine has none.** The kernel and the
 * init program are built with `-mno-80387 -mno-sse -mno-sse2 -mno-mmx`: there
 * is no floating-point unit available to this code, and a `double` in here
 * would not survive the link. So a number is **kept as its text** and read back
 * only as an integer, by `json_int`, which refuses anything that is not one.
 *
 * That is not a limitation being hidden. A parser that converted `1e400` or
 * `0.1` to a double would be answering with a number the document did not
 * contain, and every JSON parser that does this quietly loses precision on
 * integers past 2^53. Keeping the text means a caller that one day needs a
 * fraction has the bytes to work from, and nothing here has silently rounded
 * anything in the meantime.
 *
 * **2. ASCII only, to stay symmetric with the writer.** `json.h` refuses to
 * emit any byte above 0x7F, because it does not validate UTF-8 and ASCII JSON
 * is always valid JSON. A reader that accepted UTF-8 would accept a machine
 * name this server **cannot report back** -- the request would be taken and the
 * reply would fail, further from the cause and harder to explain than a refusal
 * at the door.
 *
 * So a byte above 0x7F is refused, and so is a `\u` escape above `007F`. The
 * two move together: the day there is a UTF-8-aware writer, this relaxes with
 * it, and not before.
 *
 * --- Bounded, and iterative ---
 *
 * Fixed arrays, no allocation. Depth is bounded by `JSON_DEPTH_MAX` and the
 * parser keeps its own stack rather than recursing, because the init program's
 * stack is not large and a nesting bound enforced by running out of it is a
 * bound enforced by a crash.
 */

#ifndef RECON_HTTP_JSONREAD_H
#define RECON_HTTP_JSONREAD_H

#include <stddef.h>

/*
 * Bounds. Each is a refusal.
 *
 * `JSON_NODES_MAX` counts every value in the document, including every member
 * of every object -- a management request is a handful of fields and this is
 * generous for that. `JSON_TEXT_MAX` is the room for decoded strings and keys
 * together, which is not the same as the document's length: escapes shrink.
 */
#define JSON_NODES_MAX     64
#define JSON_DEPTH_MAX      8
#define JSON_TEXT_MAX    2048
#define JSON_INPUT_MAX  16384

/* What a value is. */
#define JSON_NULL      0
#define JSON_TRUE      1
#define JSON_FALSE     2
#define JSON_NUMBER    3
#define JSON_STRING    4
#define JSON_OBJECT    5
#define JSON_ARRAY     6

/* Verdicts. Zero is a document with exactly one meaning. */
#define JSON_OK            0
#define JSON_EMALFORMED  (-1)	/* syntax */
#define JSON_EDEPTH      (-2)	/* nested past JSON_DEPTH_MAX */
#define JSON_ENODES      (-3)	/* more values than JSON_NODES_MAX */
#define JSON_EROOM       (-4)	/* more text than JSON_TEXT_MAX */
#define JSON_EDUPLICATE  (-5)	/* the same key twice in one object */
#define JSON_ENONASCII   (-6)	/* a byte, or an escape, above 0x7F */
#define JSON_ETRAILING   (-7)	/* text after the document ends */
#define JSON_EEMPTY      (-8)	/* nothing but whitespace */
#define JSON_EINPUT      (-9)	/* the document is over JSON_INPUT_MAX */

/*
 * One value.
 *
 * A flat array with indices rather than pointers, so the whole document is one
 * structure a caller can hold by value and nothing points anywhere that could
 * go away. `-1` is *none*.
 */
struct json_node {
	int    kind;

	/*
	 * For a string: where its **decoded** text sits in the document's
	 * pool, and how long it is. For a number: where its text sits in the
	 * *input*, and how long -- unescaped, because a number has no escapes.
	 * For everything else: both zero.
	 */
	size_t at;
	size_t len;

	/* For a member of an object: its key, decoded, in the pool. `name_len`
	 * is zero for anything that is not one -- including a value inside an
	 * array, which has a position rather than a name. */
	size_t name_at;
	size_t name_len;

	int    first;		/* first child, or -1 */
	int    next;		/* next sibling, or -1 */
};

struct json {
	const char      *text;		/* the input, for numbers */
	struct json_node nodes[JSON_NODES_MAX];
	size_t           count;

	char             pool[JSON_TEXT_MAX];
	size_t           pool_used;

	/*
	 * Where a refusal happened, as a byte offset into the input, and zero
	 * on success. Filled in for every refusal -- a parser that reports a
	 * position for some faults and not others is one whose caller has to
	 * know which.
	 */
	size_t           where;
};

/*
 * Parse `text` into `into`.
 *
 * `len` is how many bytes are valid. The text need not be NUL-terminated, and
 * a NUL inside it is a refusal rather than an early end: a document whose
 * meaning depends on whether the reader stops at a zero byte is a document two
 * readers read differently.
 *
 * Returns `JSON_OK`, or one of the refusals above with `into->where` set. On a
 * refusal nothing in `into` is usable -- the whole document is taken or none of
 * it is, for the reason `config.h` gives about half-applied files.
 *
 * Pure: no allocation, no syscalls, no clock.
 */
int json_parse(const char *text, size_t len, struct json *into);

/* A few words for a verdict, for a console line or an error body. Never NULL:
 * an unrecognised verdict gives "unknown". */
const char *json_reason(int verdict);

/* The document's top value, or NULL when there is none. */
const struct json_node *json_root(const struct json *doc);

/*
 * A member of an object, by name, or NULL.
 *
 * `name` is compared byte for byte against the decoded key. Not
 * case-insensitively: JSON keys are not, and a server that matched `Name` to
 * `name` would accept a document a strict client thinks it did not send.
 */
const struct json_node *json_member(const struct json *doc,
                                    const struct json_node *object,
                                    const char *name);

/* The nth value of an array, or NULL. */
const struct json_node *json_at(const struct json *doc,
                                const struct json_node *array, size_t index);

/* How many children an object or array has. Zero for anything else. */
size_t json_count(const struct json *doc, const struct json_node *value);

/*
 * A string's decoded text, NUL-terminated, or NULL if this is not a string.
 *
 * `*len` receives the length when it is not NULL. The text is terminated **and**
 * counted because a decoded string may legitimately contain a NUL -- `\u0000`
 * is valid JSON -- and a caller that only has the terminator would read a
 * shorter string than was sent.
 */
const char *json_string(const struct json *doc, const struct json_node *value,
                        size_t *len);

/*
 * A number, as an integer, or 0 if it is not one this can give.
 *
 * Refuses a fraction, an exponent, and anything outside the range of a `long`:
 * there is no floating point on this machine, and a parser that rounded would
 * be answering with a number the document did not contain. The text is still
 * there for a caller that needs it -- `at` and `len` index the input.
 *
 * Returns 1 and fills `*out` on success, 0 otherwise.
 */
int json_int(const struct json *doc, const struct json_node *value, long *out);

/* Whether a value is exactly `true` or exactly `false`, as a C truth value.
 * Returns 1 and fills `*out` for a boolean, 0 for anything else -- including
 * `null`, a number and the string "true", none of which are booleans and all
 * of which some parser somewhere treats as one. */
int json_bool(const struct json *doc, const struct json_node *value, int *out);

#endif
