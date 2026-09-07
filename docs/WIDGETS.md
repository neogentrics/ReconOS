# The widget layer

One place that owns what a control looks like and how it behaves when a person
points at it.

The code is `include/recon_widget.h` (what a caller says), `src/recon_widget.c`
(the drawing) and `src/recon_widget_state.c` (the rules, kept apart from the
compositor so they can be tested without a screen).

---

## Why it exists

Every application in ReconOS drew its own buttons. Not "drew them differently"
— drew them *itself*, five or six lines at a time:

```c
recon_fill_rect(p, x, y, w, h, COLOR_BG);
recon_draw_button_edge(p, x, y, w, h, false, COLOR_BAR);
recon_draw_text(p, font, x + 8, baseline, w, label, COLOR_TEXT);
recon_hit_add(p, x, y, w, h, HIT_SOMETHING);
recon_hit_tip(p, "What it does");
```

That block appeared around two hundred times across this repository. Writing it
out is not the problem. The problem is what it means: **a behaviour that has to
be written two hundred times is a behaviour that gets written zero times.**

Nothing in ReconOS highlighted under the pointer. Nothing sank when it was
pressed. The close button on every window did not go red on approach, the way
close buttons have on every desktop for twenty years — not because anyone
decided against it, but because there was no single place to put it, and
nobody was going to add it to two hundred call sites by hand.

The copies had also drifted, as copies do. The Photos toolbar clipped its
labels at the button's full width measured from an indented start, so a long
label ran over the button next to it. Several applications registered no click
target at all for a disabled button, so the click fell through to whatever was
behind it. The login screen used the *pressed* colour for hover, and so had
nothing left to say for a press.

An application should say **this is a button, here, called that, and pressing
it means this** — and be told nothing about bevels, corner radii, hover tints
or hit regions. Those are the system's business.

---

## Why it carries a version

Modules are built separately from the system that loads them, and the
calculator is one of them. If what a button looks like can change, a module
compiled against one generation of that answer and loaded by another has to be
something the system can **notice**, rather than something that shows up as a
window whose buttons are subtly the wrong shape.

So the layer has a number, and it is deliberately **not** the module ABI's
number:

| | `RECON_MODULE_ABI` | `RECON_WIDGET_VERSION` |
|---|---|---|
| **Asks** | can this be loaded at all? | will its controls match the screen? |
| **Changes when** | a struct changes shape | the look or behaviour of a control changes |
| **Getting it wrong is** | a crash | a window that looks foreign |
| **A mismatch is** | refused | reported, and loaded anyway |

Refusing on a widget mismatch would be worse than the drift it prevents: a
module a version behind still draws working buttons, and a module that will not
load draws nothing. The loader says so once into the log and carries on.

`RECON_MODULE` fills the number in from the headers a module was compiled
against, so a module author cannot state it wrongly.

---

## Version history

### Version 1 — v0.4.0

The first. Everything below is what "drawn against widget version 1" means.

**States.** A control is in exactly one of four, and the panel knows which
without the application being asked:

| State | When |
|---|---|
| `NORMAL` | nothing is happening to it |
| `HOT` | the pointer is over it |
| `ACTIVE` | the pointer is over it *and* the button is down |
| `DISABLED` | drawn, explains itself, cannot be pressed |

Two ids on the panel carry this — the *hot* id from pointer motion, and the
*held* id set on press and cleared on release. Both survive a redraw, because
they describe the pointer and the pointer does not move when a window repaints.

Held **and** hovered is what counts as pressed. Press a button, think better of
it, slide off, let go — nothing happens. Held alone would have made that
impossible, and `held == id` reads as correct in the code, which is why the
rule has a test rather than a comment.

While anything is held, nothing else lights up.

**Looks.** A caller says what a button *means* and the skin decides what that
looks like:

| Look | For |
|---|---|
| `PLAIN` | the ordinary button |
| `ACCENT` | the one Return would press; carries the accent at rest |
| `DANGER` | close, delete, end task — warning-coloured **under the pointer**, not at rest |
| `TAB` | one of a strip; shows chosen through `checked` |

Danger is red on approach and not before. A close button that is red all the
time is a close button somebody stops seeing.

**Shades.** Hover moves a surface 40/255 toward the skin's selection colour;
press moves it 90/255. Derived rather than named as a role, so a skin somebody
wrote this afternoon has a working highlight the moment it has a selection
colour. Checked on every built-in skin by `tests/test_widget.c`: hover differs
from rest, press differs from hover, and press is always the further of the
two.

**Disabled controls became inert.** A disabled button keeps its region and its
tooltip — pointing at it can still say why it is unavailable — and stops
answering clicks, and does not let the click fall through to what is behind it.
Set from `disabled` alone, so an application that says a button is unavailable
has said the whole of it. `recon_hit_test` still answers "what is there";
`recon_hit_test_active` answers "what would take a click", and every click path
asks the second.

**Caption buttons act on release.** Minimize, maximize and close used to act on
the press, which is why they never appeared to do anything: the window was
closed or minimized before the frame showing the button sunk had been drawn.
They now wait for the button to come back up, over the same control it went
down on. Built-in windows and client windows share one implementation, so this
is true of both.

**Rounding became the default.** `metric.corner` went from 0 to 5 and
`metric.button-corner` from 0 to 4, with the ceiling on the button radius
raised from 8 to 12. Zero had been chosen when Beacon and Glass were the only
skins that rounded anything; it left nine of the eleven built-in skins drawing
square buttons not because anything had decided they should, but because nobody
had written a number for them. Classic, Reading and Contrast now say `0` out
loud — 95 was square, and a soft edge drawn in a colour between the two a
high-contrast skin promises is the opposite of what somebody who turned that on
asked for.

**The skin list shows shape, not only colour.** Each row in Appearance draws a
sample of that skin's own button, in that skin's numbers -- so two skins that
share a palette and differ entirely in whether their buttons round can be told
apart without putting either one on. Drawn rather than described: a row saying
"rounded corners" in words would be a second description of the skin that could
disagree with it, and this one cannot, because it is the same arithmetic the
skin will use on every button in the system. `recon_theme_metric_of` is the
metric twin of `recon_theme_color_of`, added for it.

---

## What this layer will not do

**It does not lay controls out.** Where a button goes is a question about the
window it is in, and a layout engine here would have to be told about every
window in the system to be any good at it. Applications keep their own
arithmetic and pass rectangles.

**It is not a retained widget tree.** ReconOS redraws a panel from scratch
whenever anything about it changes, and a tree of parents and invalidation
would be a second description of the window that could disagree with the first.

---

## Adding a control kind

1. Add it to `recon_widget.h` with a comment saying what it is *for*, not what
   it looks like.
2. Draw it in `recon_widget.c`, reading `recon_widget_state_of`.
3. If it has a rule that can be checked without pixels, put the rule in
   `recon_widget_state.c` and a check in `tests/test_widget.c`.
4. Raise `RECON_WIDGET_VERSION` only if an existing control changed how it
   looks or behaves. A new kind nothing yet uses changes nothing anybody can
   see.
5. Add a section here saying what changed.
