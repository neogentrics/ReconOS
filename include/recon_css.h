/*
 * Reading CSS, as much of it as a viewer needs.
 *
 * --- What this is for ---
 *
 * The HTML reader turns a page into a flat list of blocks and runs, and says
 * in its own header that a page whose layout lives entirely in a stylesheet
 * renders as its underlying structure. That is honest and it is also why so
 * many real pages read as a column of navigation, cookie notices and footer
 * links with the article somewhere in the middle.
 *
 * Most of that is not layout. It is `display: none` -- the parts of the page
 * that were never meant to be seen at once -- plus a colour, a weight and a
 * size. Those four are most of the difference between a page and a readable
 * page, and none of them needs a layout engine.
 *
 * --- Where the line is ---
 *
 * **Selectors are one compound each, with descendants.** `p`, `.footer`,
 * `#nav`, `p.lead`, `div.body p` all match. Child (`>`), sibling (`+`, `~`),
 * attribute and pseudo-class selectors do not, and a rule using one is kept
 * out rather than half-matched -- a rule that fires when it should not is
 * worse than one that never fires, because the first hides text.
 *
 * **No cascade beyond specificity and order.** No inheritance walk, no
 * computed values, no units but px and %. `em` is read as a multiple of the
 * parent's size where the caller knows one and ignored where it does not.
 *
 * **Nothing is executed.** `@media` blocks are skipped whole -- their
 * condition needs a viewport this does not model, and applying a print
 * stylesheet to a screen is worse than applying neither. `@import` is not
 * followed.
 *
 * Saying all that here rather than in a release note, because the gap between
 * "a stylesheet subset" and "CSS" is a decade of work and the difference
 * should be legible from the header.
 */

#ifndef RECON_CSS_H
#define RECON_CSS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * What a rule can say.
 *
 * Every field carries "unset" as a distinct state rather than as a default,
 * because a rule that says nothing about colour must not overwrite one that
 * did. That is the whole of the cascade this implements, and getting it wrong
 * makes the last rule in the file win everything.
 */
struct recon_css_style {
    bool has_colour;
    unsigned colour;              /* 0xRRGGBB */

    bool has_background;
    unsigned background;

    /* 0 unset. Otherwise the value. */
    int weight;                   /* 400 normal, 700 bold */
    bool italic_set;
    bool italic;
    bool mono_set;
    bool mono;

    /* Per cent of the size this element would otherwise have. 0 unset. */
    int size_percent;

    /* 0 unset, then left, centre, right. */
    enum { RECON_CSS_ALIGN_NONE = 0, RECON_CSS_ALIGN_LEFT,
           RECON_CSS_ALIGN_CENTRE, RECON_CSS_ALIGN_RIGHT } align;

    /* 0 unset, 1 shown, 2 gone. Two rather than a bool for the same reason as
     * everything else here: "not mentioned" and "display: block" are
     * different answers and only one of them overrides a hide. */
    int display;
};

#define RECON_CSS_SHOWN 1
#define RECON_CSS_NONE 2

/*
 * One element, as the matcher needs to see it.
 *
 * `classes` is the attribute as written -- space separated, in the page's own
 * order. Split at match time rather than stored split, because an element has
 * one class attribute and a sheet has thousands of selectors, so the work
 * belongs on the side there is less of.
 */
struct recon_css_element {
    const char *tag;
    const char *id;
    const char *classes;
};

struct recon_css_sheet;

/* An empty set of rules, to add sheets to. NULL only when out of memory. */
struct recon_css_sheet *recon_css_new(void);
void recon_css_free(struct recon_css_sheet *sheet);

/*
 * Read one stylesheet into the set.
 *
 * Called once per `<style>` element and once per fetched `<link>`, in the
 * order the page named them, because order is half of the cascade. False only
 * when the set is full -- there is no such thing as CSS this refuses, and a
 * sheet of nonsense contributes no rules.
 */
bool recon_css_add(struct recon_css_sheet *sheet, const char *text,
    size_t length);

int recon_css_rule_count(const struct recon_css_sheet *sheet);

/*
 * What applies to the element at the end of `stack`.
 *
 * `stack` is the open elements, outermost first, so `depth - 1` is the
 * element being asked about and everything before it is its ancestry. `out`
 * is filled in from lowest specificity to highest, so a caller can seed it
 * with what it already knows and have the sheet override only what it says.
 */
void recon_css_match(const struct recon_css_sheet *sheet,
    const struct recon_css_element *stack, int depth,
    struct recon_css_style *out);

/*
 * One `style="..."` attribute.
 *
 * Applied after everything a sheet said, which is what the specification
 * requires and is also what somebody writing one on an element means.
 */
void recon_css_inline(const char *declarations, size_t length,
    struct recon_css_style *out);

/* A colour by CSS name, #rgb, #rrggbb, or rgb(...). False when it is none of
 * those -- including `transparent` and `inherit`, which are not colours. */
bool recon_css_colour(const char *text, unsigned *out);

#endif
