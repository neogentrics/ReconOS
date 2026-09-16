/*
 * Putting text into HTML without it becoming markup.
 *
 * --- Why this exists, written from the fault it removes ---
 *
 * `server_init.c` puts the machine's name into the dashboard twice: once as a
 * heading and once as the value of an input. Neither was escaped, and that was
 * *safe* -- because `handle_set_name` runs every candidate name through
 * `server_name_split`, which admits letters, digits and the hyphen and refuses
 * everything else. A `<` could not reach the page.
 *
 * That safety came from a function three files away, and the comment saying so
 * had to be written in the page's own source because nothing else recorded it.
 * **The day the name rules loosen -- a dot, for a fully qualified name -- that
 * page becomes a cross-site scripting hole, and neither the compiler nor any
 * suite says a word.** A dependency that is only true by coincidence, and only
 * documented in prose, is a dependency waiting to be broken by somebody
 * improving something else.
 *
 * So text is escaped where it is written, and the coupling goes away.
 *
 * --- What is escaped, and why the list is longer than it looks it needs ---
 *
 * `<` and `&` are the ones everybody knows. The other three are why escapers
 * that only handle those two still produce holes:
 *
 *   `>`  closes a tag somebody else opened. Cheap, and there is no reason not
 *        to.
 *   `"`  ends an attribute's value. Without it, `value="NAME"` with a name
 *        containing a quote gains whatever attributes the name likes --
 *        `onmouseover`, most usefully for an attacker. This is the one the
 *        dashboard's input would have fallen to.
 *   `'`  the same, for a page that quotes attributes with apostrophes. This
 *        one does not, and escaping it costs nothing and removes a question
 *        the next reader would have to answer.
 *
 * Refused rather than handled: escaping for a `<script>` body, for a URL, or
 * for a CSS value. Each needs a different escape and **using the wrong one is
 * not safer than using none** -- HTML-escaping inside a script block produces
 * `&lt;` where the script expected `<`, which either breaks the page or, worse,
 * survives a round trip and becomes markup later. A caller wanting those needs
 * functions that do not exist yet, and should not reach for this one.
 */

#ifndef RECON_HTTP_ESCAPE_H
#define RECON_HTTP_ESCAPE_H

#include <stddef.h>

/*
 * Escape `in` into `out`.
 *
 * Returns the number of bytes written, not counting the terminator, or -1 when
 * it would not fit.
 *
 * **Refuses rather than truncates, and that is the whole of why it returns a
 * length.** Text cut in the middle of an entity is text that ends `&l` -- and
 * a browser reading the next thing after it as part of an entity is the exact
 * confusion escaping exists to prevent. A caller must check.
 *
 * `in` is read to its terminator. A caller with bytes rather than a string --
 * a form value, which may hold any byte -- should use `http_escape_n`.
 */
long http_escape(const char *in, char *out, size_t room);

/* The same, for `len` bytes rather than a terminated string. */
long http_escape_n(const char *in, size_t len, char *out, size_t room);

#endif
