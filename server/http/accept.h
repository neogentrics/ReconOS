/*
 * Choosing what to send, from what the client will take.
 *
 * --- Why this exists, and why it had to come before the thing that needs it ---
 *
 * `docs/WEB.md` has carried this row since 0.12.0:
 *
 *     **The access log as JSON** | unblocked | ... it would make one endpoint
 *     serve two formats, and two representations of one thing drift exactly
 *     like two lists do. It needs a decision about content negotiation first,
 *     not a few more lines.
 *
 * This is that decision. `GET /api/log` answers text today; a management client
 * wants JSON; and the wrong way to give it both is a second endpoint, because
 * then there are two pieces of code rendering one thing and they drift the
 * first time either is edited. The right way is one endpoint that renders the
 * same entries two ways, chosen by what the client asked for.
 *
 * --- What `Accept` actually decides ---
 *
 * More than a preference. With `q=0` and wildcards, this header decides whether
 * a request is answered **at all**: a client that will take only what this
 * server cannot produce gets 406. A header that decides an outcome is a header
 * that has to be parsed properly rather than scanned for a substring, which is
 * how it is usually done.
 *
 * --- Strict about shape, silent about parameters, and that is measured ---
 *
 * Every other header this project reads is held to its grammar, and this one is
 * too: a media range that is not `type/subtype`, a subtype wildcard or a full wildcard, a comma
 * with nothing after it, a `q` outside 0 to 1 -- each is a refusal, and the
 * request is answered 400 rather than served something nobody asked for.
 *
 * **But parameters other than `q` are accepted and ignored**, and that is not
 * the *skip what you do not understand* fault this project refuses elsewhere.
 * Their shape is checked; only their meaning is ignored. The reason it must be
 * that way is what Chrome sends on every request:
 *
 *     text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,
 *     image/webp,image/apng,<a full wildcard>;q=0.8,application/signed-exchange;v=b3;q=0.7
 *
 * `v=b3` is a parameter that is not `q`. A parser that refused it would answer
 * **400 to every request a browser makes**, which is a strictness with no
 * safety in it: unlike a framing header, misreading `Accept` cannot make two
 * readers disagree about where a message ends. The cost of ignoring a
 * parameter is sending a representation that is right about the type and
 * indifferent to a refinement of it. The cost of refusing one is the console.
 *
 * --- Vary ---
 *
 * A response whose body depends on a request header **must** say so, or a cache
 * between here and the client serves the JSON it stored to the next person who
 * asked for HTML. `serve.c` sends `Vary: Accept` on any response produced by a
 * negotiated route, and that is the server's job rather than each handler's --
 * for the same reason the security headers and the access log are: a header
 * every handler must remember is a header the next handler will not send.
 */

#ifndef RECON_HTTP_ACCEPT_H
#define RECON_HTTP_ACCEPT_H

#include <stddef.h>

/*
 * How many media ranges one header may carry.
 *
 * Chrome sends eight. Sixteen leaves room and is a refusal rather than a
 * truncation: a header cut short is a client's preferences read as something
 * it did not say, and the ranges at the end are the low-weight ones that
 * decide whether the answer is 406.
 */
#define ACCEPT_RANGES_MAX   16

/* A media range, a parameter name and a parameter value are each bounded for
 * the reason `http.h` gives about headers. */
#define ACCEPT_TOKEN_MAX    64

/*
 * Pick one of `offers`, or refuse.
 *
 * `header` is the raw `Accept` value, or NULL when the client sent none.
 * `offers` are the representations this route can produce, **in the server's
 * own order of preference**, and `count` is how many.
 *
 * Returns the index of the offer to send. Returns:
 *
 *   `ACCEPT_ANY`        no header, or a header that accepts anything -- take
 *                       the first offer, which is the server's preference.
 *                       (Returned as 0, so a caller that ignores this
 *                       distinction is still correct.)
 *   `ACCEPT_NONE`       the client will not take anything on offer: **406**.
 *   `ACCEPT_EMALFORMED` the header does not parse: **400**.
 *
 * Selection is by RFC 9110: the most specific matching range decides an
 * offer's weight -- an exact type beats a subtype wildcard beats a full wildcard -- and the
 * highest weight wins. A tie is broken by the server's order, because a client
 * that says two things are equally good has said it does not mind, and the
 * server does.
 *
 * Pure. No allocation, no clock, no sockets -- so the suite hands it every
 * hostile header as a string literal.
 */
#define ACCEPT_NONE       (-1)
#define ACCEPT_EMALFORMED (-2)
#define ACCEPT_ANY          0

int http_accept_pick(const char *header, const char *const *offers,
                     size_t count);

/*
 * Does this media range match this type? Exposed because it is a rule rather
 * than a step, and a rule with a suite is a rule somebody can check.
 *
 * Returns how specific the match is -- 3 for `type/subtype`, 2 for a subtype wildcard,
 * 1 for `*` /`*`, 0 for no match -- which is the number the selection above is
 * built on.
 *
 * Case-insensitive, because a media type is a token.
 */
int http_accept_matches(const char *range_type, const char *range_sub,
                        const char *type);

#endif
