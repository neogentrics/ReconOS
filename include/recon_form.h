/*
 * What a form sends.
 *
 * Split out of recon_web.c for the same reason recon_url.c was split out of
 * recon_http.c: this is arithmetic on bytes, it decides what leaves the
 * machine, and testing it there meant linking a compositor. A rule about which
 * controls contribute to a request is exactly the kind of thing that is wrong
 * in one case out of eight and never noticed, because the seven that work look
 * like all of them working.
 *
 * --- The split with the viewer ---
 *
 * The *document* says what a control is called and what it starts out holding.
 * The *viewer* holds what somebody has typed since. Neither is enough on its
 * own, so this takes both: the document from recon_html, and one
 * `recon_form_value` per control from whoever is showing it.
 *
 * Nothing here opens anything, allocates a window, or knows what a page looks
 * like. It turns two tables into the bytes of a request.
 */

#ifndef RECON_FORM_H
#define RECON_FORM_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_html.h"
#include "recon_http.h"

/*
 * The live state of one control.
 *
 * `text` is borrowed for the length of the call. Which field means anything
 * depends on the control's kind, and the kind comes from the document rather
 * than from here -- a value that carried its own idea of what it was could
 * disagree with the page, and then two answers would exist for "is this a
 * checkbox".
 */
struct recon_form_value {
    const char *text;            /* a text box or a textarea */
    bool on;                     /* a checkbox or a radio */
    int chosen;                  /* which option, for a menu */
};

/*
 * One string, encoded as `application/x-www-form-urlencoded`.
 *
 * Returns the number of bytes written, not counting the terminator, or 0 when
 * it would not fit -- which callers have to treat as a failure rather than as
 * a short answer, because a half-encoded value is a different value.
 *
 * The unreserved set is RFC 3986's. A space is "+" rather than "%20", which is
 * this encoding's one deliberate difference from ordinary percent-encoding and
 * the one that surprises people.
 */
size_t recon_form_encode(const char *text, char *out, size_t size);

/*
 * What one control contributes, or NULL when it contributes nothing.
 *
 * The rules are HTML's, and each has a page that breaks without it:
 *
 *   - no name, no request. A control with no name exists for script.
 *   - disabled means not sent, which is what disabled means.
 *   - an unchecked box is absent entirely -- not empty, not "off". A server
 *     counts the ones that arrived.
 *   - a checked box with no value of its own sends "on".
 *   - a menu sends its chosen option's *value*, not the words shown.
 *   - of all the submit buttons on a form only the one pressed is sent, which
 *     is how a page tells Save from Delete. `submitter` of -1 -- Enter pressed
 *     in a text box rather than a button clicked -- sends none of them.
 *   - a reset button and a button that only script could work send nothing.
 */
const char *recon_form_field_sends(const struct recon_html_document *page,
    int index, const struct recon_form_value *values, int value_count,
    int submitter);

/*
 * Build the bytes a form sends, in document order.
 *
 * Document order because the standard says so and because servers rely on it:
 * two controls sharing a name is how a page sends a list, and the order is the
 * list.
 *
 * The result is the caller's to free, NUL-terminated, and never longer than
 * `limit` including the terminator. NULL when it will not fit -- a refusal
 * rather than a shortened request, because a form arriving with its last two
 * answers missing is worse than one not arriving: the server accepts it.
 *
 * `out_count` and `out_secret` are for whatever has to describe this before it
 * goes; either may be NULL.
 */
char *recon_form_body(const struct recon_html_document *page, int form,
    int submitter, const struct recon_form_value *values, int value_count,
    size_t limit, int *out_count, bool *out_secret);

/*
 * Put a GET's answers into the address.
 *
 * The answers *replace* whatever query the action already had, which is what
 * the standard says: `action="/s?lang=en"` submitted as a GET sends the form's
 * fields and not `lang`. A page that needs both puts the other in a hidden
 * field, which is what hidden fields are for.
 *
 * False when the address it would make is longer than an address can be, and
 * then `where` is left as it was.
 */
bool recon_form_get_address(struct recon_http_url *where, const char *body);

#endif /* RECON_FORM_H */
