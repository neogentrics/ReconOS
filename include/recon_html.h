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
 * **CSS, as far as it decides what is readable.** A stylesheet can hide an
 * element, colour it, weight it, slant it, size it and align it -- see
 * recon_css.h for exactly how much and why no more. Structure still decides
 * most of it: a heading is large because it is a heading, and a stylesheet
 * that only moves things about changes nothing here, because there is nothing
 * to move things about in.
 *
 * The one that matters is `display: none`. Most of what makes a real page
 * unreadable in a structural reader is not layout, it is the parts of the
 * page that were never meant to be seen at once.
 *
 * **No JavaScript.** A page that builds itself at run time arrives empty, and
 * says so rather than showing a blank window.
 *
 * **Images are named, not fetched.** An `<img>` becomes a block carrying the
 * address of the picture and its alt text. Whether the picture is ever asked
 * for is the viewer's business, not the parser's -- and a block that keeps its
 * alt text is one that degrades to exactly what it used to be when the picture
 * does not arrive, which is what alt text is for.
 *
 * **Forms, and they submit.** This used to say a form was shown and could not
 * be submitted, on the reasoning that a viewer able to submit one could change
 * something on somebody's server. That reasoning was half right and it was
 * costing the whole of the readable web that has a search box on it -- and a
 * viewer that cannot search is a viewer you leave to go and use a browser.
 *
 * What survives of it is the split between the two methods, which is a real
 * one rather than a caution. **GET is a question**: the parameters go in the
 * address, the address is the request, and asking twice is asking once twice.
 * **POST is a statement**: the body is not in the address, it is not in the
 * history, and asking twice may have done a thing twice. So a GET goes when
 * it is asked for, and a POST says what it is about to send and to whom, and
 * waits to be told again. That is one dialogue between somebody meaning to
 * log in and somebody's first click on a page they have not read.
 *
 * This file's share is the reading: which controls a page has, what they are
 * called, what they start out holding, and which form each belongs to. What
 * somebody types into one and where it is sent are the viewer's, because they
 * are facts about a session rather than about a document.
 *
 * Saying all that here rather than in a release note, because the gap between
 * "a viewer for simple pages" and "a browser" is a decade of work and the
 * difference should be legible from the header.
 */

#ifndef RECON_HTML_H
#define RECON_HTML_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_css.h"

/* What kind of block this is, which is what decides how it is drawn. */
enum recon_html_block {
    RECON_HTML_PARAGRAPH,
    RECON_HTML_HEADING,        /* `level` says 1 to 6 */
    RECON_HTML_LIST_ITEM,      /* `level` says how deeply nested */
    RECON_HTML_PRE,            /* whitespace kept, drawn fixed-width */
    RECON_HTML_QUOTE,
    RECON_HTML_RULE,           /* a horizontal line; no runs */
    /*
     * A picture. `source` says where it is; the runs are its alt text, drawn
     * when the picture is not there -- which is the whole of how this
     * degrades to what it did before.
     */
    RECON_HTML_IMAGE,

    /*
     * One row of a table.
     *
     * A kind of its own rather than a paragraph, because a viewer that wants
     * to line the columns up has to know which blocks belong to one table --
     * and consecutive rows is what a table is, once the tags are gone.
     *
     * The cells are marked on the runs, by `starts_cell`, rather than being
     * blocks of their own. A cell is not a paragraph: it is a piece of a line,
     * and making each one a block would give every cell in a table its own
     * line, which is exactly the thing tables exist not to do.
     */
    RECON_HTML_ROW,
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

    /*
     * What a stylesheet said to write this in, as 0xRRGGBB. Unset means the
     * skin decides, which is what should happen for the overwhelming majority
     * of text -- a reader that took every page's colours would be a reader
     * whose dark skin is undone by the first page written for a light one.
     */
    bool has_colour;
    unsigned colour;

    /*
     * This run begins a cell of a table row.
     *
     * Only meaningful inside a RECON_HTML_ROW block. It is what lets a viewer
     * measure a column: the widest first-run-of-cell-two across every row is
     * how wide column two has to be.
     *
     * A flag on the run rather than a list of offsets on the block, because
     * the runs are already the thing being walked and a parallel list is a
     * second thing to keep in step with them.
     */
    bool starts_cell;

    /*
     * Which form control this run *is*, or -1 for the overwhelming majority
     * of runs, which are text.
     *
     * A control is a run rather than a block for the same reason a cell is
     * not a block: an input sits *in* a line, between words -- "Search for
     * [____] in [Everything v]" is one sentence with two controls in it, and
     * giving each its own block would put every field of every form on a line
     * of its own and lose which words belong to which.
     *
     * The run's own text is empty. What is drawn there is a control, and how
     * wide it is is a question for whoever has the font.
     */
    int field;
};

struct recon_html_link {
    char href[2048];
};

/* --- Forms --- */

/*
 * What a control is, which is what decides how it is drawn and what it
 * contributes when the form is sent.
 *
 * Deliberately fewer kinds than HTML has types. `type=email`, `type=search`,
 * `type=tel`, `type=url` and `type=number` are all a box you type into: they
 * differ in what a browser *validates* and in which keyboard a telephone
 * offers, and this has no soft keyboard and refuses to invent validation
 * rules the server is going to apply anyway. An unknown type is a text box,
 * which is what the HTML standard itself says to do with one.
 */
enum recon_html_field_kind {
    RECON_HTML_FIELD_TEXT,       /* one line, typed into */
    RECON_HTML_FIELD_AREA,       /* <textarea>: several lines */
    RECON_HTML_FIELD_CHECKBOX,
    RECON_HTML_FIELD_RADIO,
    RECON_HTML_FIELD_CHOICE,     /* <select>, with options */
    RECON_HTML_FIELD_SUBMIT,     /* sends the form */
    RECON_HTML_FIELD_RESET,      /* puts every field back to its default */

    /*
     * `<button type=button>` and `<input type=button>`, which do nothing at
     * all without script.
     *
     * Kept and drawn rather than dropped, and drawn *disabled*. A page whose
     * "Show more" button is simply absent looks like a page missing a
     * feature; one whose button is there and visibly dead says what is
     * actually true, which is that this viewer runs no script.
     */
    RECON_HTML_FIELD_BUTTON,

    /*
     * `type=hidden`: sent, never drawn.
     *
     * Not an oversight and not a courtesy to the page. Half the search forms
     * on the web carry a hidden token that says which section is being
     * searched, and a viewer that dropped them would send a request the
     * server has never seen the like of.
     */
    RECON_HTML_FIELD_HIDDEN,
};

/* GET asks. POST tells. See the note at the top of this file. */
enum recon_html_method {
    RECON_HTML_GET,
    RECON_HTML_POST,
};

struct recon_html_form {
    /*
     * Where it is sent, exactly as the page wrote it and not resolved.
     *
     * Unresolved for the same reason the stylesheet list is: this does not
     * know where the page came from. An empty action means the page's own
     * address, which the viewer knows and this does not.
     */
    char action[2048];
    int method;                  /* enum recon_html_method */

    /*
     * The form's name, for a message about what is being sent.
     *
     * Not used in the request -- a form's name is not a parameter. It is here
     * so the confirmation before a POST can say "the sign-in form" rather
     * than "a form", when the page has bothered to say.
     */
    char name[64];
};

/* One choice inside a `<select>`. */
struct recon_html_option {
    char label[96];              /* the words between the tags */
    char value[192];             /* what is sent */
    bool selected;

    /*
     * Whether the page wrote a `value` at all.
     *
     * An option with none sends its own words, which is why this is not just
     * "is the value empty": `<option value="">Any department</option>` sends
     * an empty value deliberately, and falling back to the label there would
     * send "Any department" to a server expecting nothing.
     */
    bool has_value;
};

struct recon_html_field {
    int kind;                    /* enum recon_html_field_kind */

    /*
     * Which form this belongs to, or -1.
     *
     * A control outside any form is real and reasonably common -- pages put
     * one there for script to read. It is drawn, it can be typed into, and it
     * is never sent, because there is nowhere to send it.
     */
    int form;

    char name[128];
    char value[256];             /* what it starts out holding */

    /*
     * What to show when it is empty, or the words on a button.
     *
     * One field for both because they are the same thing from the drawing
     * side: the text that appears in the control when the control has no text
     * of its own to show.
     *
     * The same size as `value`, because a button's words *are* its value when
     * it has one -- and a label held in a smaller buffer would mean a button
     * whose face says less than what it sends.
     */
    char label[256];

    bool on;                     /* checked, for a checkbox or a radio */
    bool disabled;
    bool secret;                 /* type=password, drawn as dots */
    bool required;

    /*
     * How wide the page asked for, in characters, or 0.
     *
     * Honoured as a hint and bounded by the viewer, because `size=200` on a
     * page written for a wide screen is a field wider than this window and a
     * line that runs off the side.
     */
    int width_chars;

    /* For a `<select>`: where its options are, in the option table. */
    int first_option;
    int option_count;
};

struct recon_html_block_entry {
    enum recon_html_block kind;
    int level;
    int first_run;
    int run_count;

    /*
     * For an image, where the picture is -- as an index into the same table
     * the links use, and -1 for every other kind of block.
     *
     * The same table because the two are the same thing: an address written
     * in a page, resolved against the page it was written in. A second table
     * of strings differing only in which attribute they came from would be a
     * second set of bounds to get right.
     */
    int source;

    /* What a stylesheet said about the block as a whole. 0 means unset, and
     * then the kind decides -- which is what happens on every page that has
     * no stylesheet, and on most blocks of every page that has one. */
    int size_percent;

    /*
     * Read and recorded; not yet drawn. Centring a line means knowing its
     * width before placing the first word on it, which is a second pass the
     * flow does not make -- so this is here, honest and unused, rather than
     * absent and then discovered to be missing by whoever adds the pass.
     */
    int align;                 /* enum recon_css_align, 0 unset */
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
/*
 * A page's bytes as UTF-8, when they are not already.
 *
 * Returns a NUL-terminated copy the caller owns, or NULL when `bytes` is
 * already usable and should be read as it stands.
 *
 * --- How it decides ---
 *
 * By looking, not by being told. If the bytes are valid UTF-8 they are UTF-8:
 * that is not a guess, because the encoding is self-checking -- a sequence of
 * bytes that decodes cleanly as UTF-8 is essentially never anything else. And
 * if they are not, they are read as Windows-1252, which is what every browser
 * does with a page that declares Latin-1 and is what the remaining
 * single-byte web actually is.
 *
 * `declared` -- from the Content-Type header -- is used only to skip the
 * check when it says UTF-8, which is most of the time. It is deliberately not
 * trusted the other way: a page that *says* Latin-1 and is really UTF-8 is
 * common enough that believing the label would break pages that currently
 * work.
 */
char *recon_html_to_utf8(const char *bytes, size_t length,
    const char *declared, size_t *out_length);

struct recon_html_document *recon_html_parse(const char *html, size_t length);

/*
 * The same, with stylesheets already in hand.
 *
 * `sheet` is added to as the page's own `<style>` elements are met, in the
 * order the page names them -- so a caller fetches the `<link>` sheets from a
 * first pass, puts them in a sheet in that order, and parses again. Order is
 * half of the cascade, which is why this takes a sheet rather than a list of
 * texts: the caller already knows the order and this does not.
 *
 * NULL `sheet` is exactly recon_html_parse.
 */
struct recon_html_document *recon_html_parse_styled(const char *html,
    size_t length, struct recon_css_sheet *sheet);

/*
 * The stylesheets a page asked for, so a caller can fetch them.
 *
 * `<link rel=stylesheet href=...>` only, unresolved -- resolving an address
 * against the page it was written in is the viewer's job, and this does not
 * know where the page came from.
 */
int recon_html_stylesheet_count(const struct recon_html_document *document);
const char *recon_html_stylesheet_at(const struct recon_html_document *document,
    int index);
void recon_html_free(struct recon_html_document *document);

/* Everything between <title> and </title>, or "" for a page with none. */
const char *recon_html_title(const struct recon_html_document *document);

/*
 * What the page paints behind everything, if it said so.
 *
 * False when it did not, which is most pages of the old web and no page of
 * the new one. A viewer that gets false should keep its own paper rather than
 * assume white: the paper belongs to whoever is reading, until the page has
 * an opinion.
 */
bool recon_html_page_background(const struct recon_html_document *document,
    unsigned *colour_out);

int recon_html_block_count(const struct recon_html_document *document);

/*
 * Whether the page was bigger than this reader will hold.
 *
 * The ceilings in recon_html.c are deliberate -- a page is somebody else's
 * file and can be any size, and a reader that grows to fit whatever it is
 * handed is one a hostile page can exhaust. Truncating is the right answer.
 *
 * Being quiet about it is not. A page cut off at four thousand blocks looks
 * exactly like a page that ended, and somebody reading it has no way to know
 * the rest is missing -- the same failure the help had, where the change log
 * stopped a third of the way through and said nothing.
 */
bool recon_html_was_truncated(const struct recon_html_document *document);
const struct recon_html_block_entry *recon_html_block_at(
    const struct recon_html_document *document, int index);

/*
 * How many runs and how many links the document has.
 *
 * A caller walking the runs of one block knows where they end, because the
 * block says. A caller walking the document does not, and had no way to ask:
 * `recon_html_run_at` returns NULL past the end, so the only way to find the
 * end was to run off it and be told. That is fine for a loop and useless for
 * a bounds check, which is what wanted them -- an index into these tables is
 * exactly the kind of thing that is wrong by one and reads whatever follows
 * the array.
 */
/*
 * Which block a named place is in, or -1.
 *
 * `name` is what followed the "#" in a link, without it. This is what makes
 * "#install" go somewhere: an id is not a thing to draw, it is a place to
 * arrive at, and a block is the finest thing a viewer can scroll to.
 */
int recon_html_anchor_block(const struct recon_html_document *document,
    const char *name);
int recon_html_anchor_count(const struct recon_html_document *document);

int recon_html_run_count(const struct recon_html_document *document);
int recon_html_link_count(const struct recon_html_document *document);

const struct recon_html_run *recon_html_run_at(
    const struct recon_html_document *document, int index);

/* The address a link run points at, or NULL. */
const char *recon_html_link_at(const struct recon_html_document *document,
    int link);

/*
 * The forms, the controls, and the choices inside a `<select>`.
 *
 * Read-only views of the document's own tables. A control's *current* value
 * is not here and is not this file's: `value` is what the page said it starts
 * out holding, and what somebody has typed since belongs to whoever is
 * showing the page. Keeping the live value here would mean a document that
 * changes as it is read, and every consumer would have to know which of the
 * two it was looking at.
 */
int recon_html_form_count(const struct recon_html_document *document);
const struct recon_html_form *recon_html_form_at(
    const struct recon_html_document *document, int index);

int recon_html_field_count(const struct recon_html_document *document);
const struct recon_html_field *recon_html_field_at(
    const struct recon_html_document *document, int index);

int recon_html_option_count(const struct recon_html_document *document);
const struct recon_html_option *recon_html_option_at(
    const struct recon_html_document *document, int index);

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
