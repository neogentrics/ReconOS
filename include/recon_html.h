/*
 * Reading HTML, as much of it as a viewer needs.
 *
 * This turns a page into a list of blocks, each holding a list of runs of
 * text. That is not a document tree and it is not trying to be: what a viewer
 * has to answer is "what words, in what order, in what shape, and which of
 * them are links", and a flat list answers all four.
 *
 * --- Where the line is ---
 *
 * **No CSS.** Structure decides appearance: a heading is large because it is a
 * heading. A page whose layout lives entirely in a stylesheet will render as
 * its underlying structure, which for a well-written page is readable and for
 * a badly-written one is a column of text. That is the honest failure.
 *
 * **No JavaScript.** A page that builds itself at run time arrives empty, and
 * says so rather than showing a blank window.
 *
 * **No images, tables or forms.** An image becomes its alt text, which is what
 * alt text is for. A table becomes its cells in order, which is wrong for a
 * spreadsheet and right for the tables that are really layout. A form is shown
 * and cannot be submitted -- a viewer that could submit one could change
 * something on somebody's server.
 *
 * Saying all that here rather than in a release note, because the gap between
 * "a viewer for simple pages" and "a browser" is a decade of work and the
 * difference should be legible from the header.
 */

#ifndef RECON_HTML_H
#define RECON_HTML_H

#include <stdbool.h>
#include <stddef.h>

/* What kind of block this is, which is what decides how it is drawn. */
enum recon_html_block {
    RECON_HTML_PARAGRAPH,
    RECON_HTML_HEADING,        /* `level` says 1 to 6 */
    RECON_HTML_LIST_ITEM,      /* `level` says how deeply nested */
    RECON_HTML_PRE,            /* whitespace kept, drawn fixed-width */
    RECON_HTML_QUOTE,
    RECON_HTML_RULE,           /* a horizontal line; no runs */
};

/* How a run of text is drawn, as flags because they combine. */
enum recon_html_style {
    RECON_HTML_PLAIN = 0,
    RECON_HTML_BOLD = 1 << 0,
    RECON_HTML_ITALIC = 1 << 1,
    RECON_HTML_MONO = 1 << 2,
    RECON_HTML_LINK = 1 << 3,
};

/*
 * One stretch of text with one appearance.
 *
 * `text` points into the document's own storage and is not NUL-terminated on
 * its own -- `length` is the truth. Splitting the text into separate strings
 * would mean an allocation per word on a page with a lot of markup.
 */
struct recon_html_run {
    const char *text;
    size_t length;
    unsigned style;
    /* Which link this run belongs to, or -1. An index rather than a pointer,
     * so the array can grow without leaving anything dangling. */
    int link;
};

struct recon_html_link {
    char href[2048];
};

struct recon_html_block_entry {
    enum recon_html_block kind;
    int level;
    int first_run;
    int run_count;
};

struct recon_html_document;

/*
 * Read a page.
 *
 * `html` is borrowed for the duration of the call and may be freed afterwards;
 * everything the document needs is copied. NULL only when there is no memory,
 * because there is no such thing as HTML this refuses -- a page of nonsense
 * parses to a document with no blocks in it, and a viewer showing "there is
 * nothing here" is more use than one showing an error about markup.
 */
struct recon_html_document *recon_html_parse(const char *html, size_t length);
void recon_html_free(struct recon_html_document *document);

/* Everything between <title> and </title>, or "" for a page with none. */
const char *recon_html_title(const struct recon_html_document *document);

int recon_html_block_count(const struct recon_html_document *document);
const struct recon_html_block_entry *recon_html_block_at(
    const struct recon_html_document *document, int index);

const struct recon_html_run *recon_html_run_at(
    const struct recon_html_document *document, int index);

/* The address a link run points at, or NULL. */
const char *recon_html_link_at(const struct recon_html_document *document,
    int link);

/*
 * True when the page had a <script> in it and almost no text.
 *
 * The signature of a page that builds itself at run time. Worth telling apart
 * from an empty page, because "this page needs JavaScript, which this does
 * not have" is an explanation and a blank window is not.
 */
bool recon_html_needs_scripting(const struct recon_html_document *document);

/*
 * Read a page that is not HTML at all: plain text.
 *
 * Same document shape, one preformatted block. text/plain is a large fraction
 * of what is worth reading and it would be strange to fetch it and refuse it.
 */
struct recon_html_document *recon_html_plain(const char *text, size_t length);

#endif /* RECON_HTML_H */
