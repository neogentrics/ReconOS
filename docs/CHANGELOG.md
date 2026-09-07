# ReconOS change log

What changed in each version, newest first. The version number tracks what
works, not what is planned.

This file is the source. `scripts/make-help.sh` turns it into the pages the
Help application shows, so there is one place to write a change down and no
way for the two to disagree.

---

## v0.4.0

**Why this is 0.4.0 and not 0.3.1.** It was numbered 0.3.1 for seventy commits
and seventy-six entries in this file, and a patch release is not that. The
number is supposed to say what changed: sound, video, a codec registry, a
theme protocol for clients that are not part of this program, mail that can
send, an expression grammar with its own tests, and a filesystem call that
creates a private file rather than tightening one afterwards. None of those is
a fix to 0.3.0 — each is a thing 0.3.0 could not do at all.

Noticed by the user, not by the project, which is the part worth writing down:
nothing here counts commits, so "in progress" stayed true for as long as
somebody kept typing under it.

**Passwords can be kept, and there is a page that shows what is kept.**

Mail's sign-in screen now offers "Remember this password". Ticking it and
connecting successfully puts the password in a keyring; opening Mail after that
fills the field in and leaves a Connect button.

What is behind the tick box is the point. The password is encrypted with
AES-256-GCM under a key derived from your account password with PBKDF2 at
sign-in. **That key is never written anywhere.** Signing out, locking the
screen and shutting down all erase it, and everything kept becomes unreadable
until somebody signs in again. A copy of the disk is a copy of ciphertext.

Three details that are not decoration:

* **The keyring's salt is not the login salt.** The accounts file already
  stores PBKDF2(password, login_salt) so a password can be checked; deriving
  the key with the same salt would make that stored value *be* the key, and
  anybody who could read the accounts file could read every secret without
  knowing the password. The keyring hashes a tag of its own with the login salt
  first, so the two derivations are independent.
* **A fresh random nonce for every write.** GCM with a repeated nonce under one
  key is not weakened, it is broken. There is a test whose whole job is to
  write the same secret twice and check the two files differ.
* **The entry's name is authenticated.** A ciphertext cannot be moved from one
  name to another, so somebody who could write the file could not slide the
  mail password onto a name a different program reads.

**Control Panel -> Passwords** lists what is kept -- names and which program
asked, never values -- and forgets one. `keyring` does the same from the
Terminal. There is no call, no command and no page that prints a secret back,
which is deliberate: no window shows one, so anything that did would be the
only way in the system to get a password out in plain text.

**What it does not do, said plainly.** It does not protect against somebody who
is signed in. The key is in this process's memory while the session is
unlocked, and every module is loaded into this process, so any module can ask
for any secret. There are no separate address spaces to hide a key in without a
kernel. It protects a stolen disk and a stolen backup, and `recon_keyring.h`
says so rather than implying more.

**Text that has to explain something now wraps instead of being cut off.**
`recon_draw_paragraph` draws across as many lines as it needs and returns its
height. `recon_draw_text` clips with an ellipsis, which is right for a window
title in a taskbar button and wrong for a sentence -- the Date and Time page
had an explanation reading "Its own co...", and a note beside it asking for
exactly this. Two pages use it so far; the rest can move as they are touched.

**The look harness can sign in with a real password.** `scripts/look.sh
--password X` sets a known password on every account in its throwaway copy of
the filesystem and types it at the login screen, instead of clearing the
password as it always has. Without it none of the above could be photographed:
an account with no password has no keyring, so every screen that offers to keep
a secret correctly hides the offer, and the harness could only ever see the
version where nothing is there.

**A skin can replace the window buttons' glyphs with pictures.** Put a file at
`window-close`, `window-maximize`, `window-restore` or `window-minimize` in
`/System/Icons` and it is drawn instead of the shape.

There is none by default and none is written, so almost every system draws the
rectangles it always has -- which is right: these have to be there before a
font has loaded and before any file has been read, and a title bar that could
fail to have a close button on it is not a title bar.

The file has to exist before it is used, so a skin that supplies one of the
three gets the drawn version of the other two. It cannot produce a missing
button, only a differently drawn one. Both frames use it -- the built-in and
the client's -- because a skin whose close button is a dot everywhere except on
client windows is exactly the near-miss `recon_decor`'s own header warns
against.

Now that a package can place files, a skin that changes them is a package.

**Control Panel → Programs → File Types** lists the kinds of file somebody has
chosen a program for, and "Use the usual program again" undoes one. An
association set months ago was visible only by right-clicking a file of that
kind and looking for the mark -- a setting you cannot find is a setting you
cannot undo.

On the Programs page rather than a page of its own, because it is about which
program opens what and that is the page about programs.

One round went into a constant standing for two slightly different things. The
registry's listing takes a prefix and checks that what follows it is a
separator or the end, so `"open-with/"` matches nothing -- the character after
it is the dot of the extension. The slash belongs to the key being built, not
to the name of the section. The page listed nothing while the keys were plainly
in the file.

**The compositor was run under the leak checker for the first time**, rather
than only the test suites -- which cover what can be tested without a screen,
and a panel is not one of those things.

`recon_shell_create` makes nine panels and four timers. `recon_shell_destroy`
freed six and three. The tooltip, the All Programs list, the screen blanker and
the slide timer were missed, which is what a list of nine calls that has to
match a list of nine calls somewhere else invites.

**The timer is the serious one and it is not a leak.** A timer left on the
event loop still points at a shell that has been freed, and `on_slide_tick`
dereferences it on its first line. The shell is destroyed by
`services restart Shell` as well as at shutdown, so restarting the shell while
a window was sliding would have written into freed memory. Nobody has hit it
because a restart takes a deliberate command and a slide lasts a quarter of a
second -- the kind of window that stays open for years and then closes on
somebody once.

The checker's total for a session that opens every application and shuts down
cleanly went from **8,097,016 bytes to 4,245,584**, and the only ReconOS frame
left is `wlr_renderer_autocreate`.

Worth recording alongside: **resident memory was flat across thirty shell
restarts both before and after the fix.** The allocator does not hand the pages
back, so this class of bug is invisible from outside the process, and measuring
RSS -- the obvious thing to reach for -- would have said there was nothing
wrong. A checker that looks inside is the only thing that finds it. BG-126.

**A graph can be saved as a picture.** A Save button writes the plane into
Pictures as a PNG, named for the moment it was taken -- a grapher that saved
over `graph.png` would lose the one somebody kept.

The pixels are the ones the window last drew, cropped to the plane, rather than
a second drawing of the same thing into a buffer. A second drawing is a second
copy of every rule about where a curve goes, and the two would drift: the saved
picture would stop being a picture of what was on screen, which is the one
thing it has to be. `recon_panel_read` is new and does exactly that -- a
rectangle of a panel, refusing rather than clamping when the rectangle is not
inside it, because a smaller picture than was asked for is a bug that looks
like a feature.

The save happens at the end of a draw rather than at the click, because a click
has no panel to read from -- and because it makes the picture one of the frame
the person was looking at when they pressed the button.

**A sweep for the whole class: every fixed ceiling, and what it does when it
is reached.** Three of them were dropping data and saying nothing, which is the
same failure three times in three subsystems:

- The help's page cap, at 512 lines, which was hiding two thirds of the change
  log. BG-123.
- The web viewer's six ceilings, which are *correct* -- a page is somebody
  else's file -- and were quiet about it. BG-124.
- File Explorer's folder listing, at 512 entries, which said "512 items" about
  a folder of six hundred. The number that did not fit was already known and
  was being clamped away on the line that knew it. BG-125.

And two caps added earlier the same night had the same fault: a package
manifest silently dropped a seventeenth `place` line. Those refuse the install
and name the limit now, because installing most of a package and reporting
success is how a missing file turns up as something not working weeks later.

The rule this leaves behind: a ceiling is often the right answer, and being
quiet about reaching one never is.

**And the web viewer stopped cutting pages off in silence.** Found by sweeping
for the shape BG-123 had: a fixed ceiling that drops data quietly.
`recon_html.c` has six of them, and unlike the help's they are *right* -- a
page is somebody else's file and can be any size, and a reader that grows to
fit whatever it is handed is one a hostile page can exhaust. The comment beside
them already said as much.

What was wrong was the quiet. A page cut off at four thousand blocks looks
exactly like a page that ended, and nobody scrolls to the bottom of a document
to check whether it finished. Every ceiling records being reached now, and the
status line says the rest is not shown. BG-124.

**The help search finds the word inside the page, not only the page.** Typing
narrows the topic list, and the page that opens is scrolled to where the word
actually is, with every line carrying it marked. A search that filters forty
topics to one and then shows the top of it has done half the job: on a long
page the word may be nowhere on screen, and the answer looks like the search
was wrong.

That found **BG-123**, which is worse than the feature is good. A page held
`char lines[512][200]` and every loop filling it stopped at 512. **One version's
change log is about 73 KB**, which wraps to well over a thousand lines — so the
bottom two thirds of it could not be reached by scrolling, the "N more lines
below" note counted only as far as the cap, and nothing said so. Anybody who has
opened the change log has been reading a third of it.

It also cost a hundred kilobytes per page whichever page it was, twice over, for
a two-line topic and the whole change log alike. The lines are allocated and
grown as they fill now: no cap, and a short page costs what a short page costs.

Nobody scrolls to the bottom of a change log to check it ends where it should —
the failure looks exactly like the document being that long. It took a feature
that *jumps* to a place, and then did not, for the missing part to become
visible.

**Three startup failures report the code that was written for them.** The error
catalogue defines forty-two codes and, counted for the first time, thirty-two
of them were raised by nothing. Some of that is deliberate -- a code is never
reused, so reserving one ahead is fine -- but three were describing failures the
system genuinely hits and reported as log lines instead:

- **VT-A001**, no usable font. `recon_shell_create`'s return was not checked at
  all, so a machine with no font carried on with a NULL shell and fell over
  further in, complaining about whatever dereferenced it first.
- **VT-A003**, no display to draw on, for a backend that will not start.
- **VT-A004**, the Wayland socket could not be created.

A log line saying "failed to create Wayland socket" is true and is not
something anybody can look up. Forced the failure by making XDG_RUNTIME_DIR
unwritable and watched `VT-A004` come out with the catalogue's own description
of the three usual causes.

Twenty-nine codes are still raised by nothing, which is now a counted number
rather than an unexamined one.

**The help was audited against the system.** Everything it claims that can be
checked from outside was checked by driving the desktop and looking: the five
power buttons are named as the help names them, Alt+Tab moves the focus,
Ctrl+Alt+Del opens the security box, Print Screen writes into Pictures, all
thirteen Terminal commands it lists exist, Alt+1..4 switches desktops and
Shift+Alt+N brings the window along, and every one of Notepad's eight editing
shortcuts does what the Writing page says — Ctrl+Z really does undo a word at a
time, and Ctrl+H really is the same bar with a second field.

Two things came out of it:

**The Desktops page said "There are four."** It has been possible to turn them
down to one from the taskbar since v0.3, and the page did not mention it — nor
that turning them off brings every window on the other three to the one that is
left, sliding in from where it was. Written down now.

**BG-122**, found in the photograph taken to prove a *different* sentence true.
Notepad's Replace bar drew a caret in both its fields, and so did the Control
Panel's Add Account form and its Registry key-and-value form. Each has two
flags for one fact — a `..._focused` boolean deciding where keys go, and
`recon_edit.active` deciding where the caret is drawn — and only the first was
kept up to date. BG-118 fixed the drawing, which was enough for every window
with one field and could not be enough for one with two.

The order caught me: the first attempt set the focus *before* starting the two
fields, and `recon_edit_begin` sets `active` — which is how a single-field
window gets a caret without asking. The photograph after the fix looked
identical to the one before it, and each line read correctly on its own.

**A package can ship a file and a setting, and can be nothing but files.** Two
manifest lines, each repeatable:

    place   = aurora.png /System/Wallpapers
    setting = notes/wrap on

**Where a package may write is an allow-list, not a check on what is
forbidden.** Icons, wallpapers, themes, fonts, sounds -- the directories that
exist to hold content -- and nowhere else. Not `/System/Config`, which holds the
accounts file. Not `/System/Modules`, which is loaded at startup. Not `/Apps`,
because a package that could drop a second thing into the directory scanned at
startup could bring code it did not declare.

The shape matters more than the list: a list of *forbidden* places is one
somebody has to keep complete, and the day it is missing an entry is the day a
package writes there. A list of permitted places is wrong in the safe
direction -- a package that wanted somewhere new fails to install, and somebody
reads the comment.

**A setting is a default, not an override.** A key that already has a value is
somebody's choice, and an install that overwrote it would be an install
rearranging their desk. Only what an install actually wrote goes in the
receipt, so removal takes back exactly that.

**And a package no longer has to bring code.** It required a `module`, which
made a package the wrapper for a program and nothing else -- so a wallpaper
pack or a set of skins had no way to be one, even though placing files is
exactly what they are for. The rule now is that a package must bring
*something*: code, or at least one file. A manifest with neither describes
something that would do nothing on installing and nothing on removal.

Checked against the case that matters, which is two packages naming the same
file. The second claims nothing, because what it named was already there;
removing it leaves the first package's file and setting alone; removing the
first takes back exactly its own; and the wallpaper that shipped with ReconOS
is untouched throughout. A package that tried to place a file into
`/System/Config` was refused with the directory named.

**A skin can move the title bar's buttons, and ask for fewer of them.** Two
metrics: `metric.buttons-left` puts them on the left, and `metric.buttons` is a
sum -- 1 close, 2 maximize, 4 minimize -- so a skin can have just a close
button, or close and minimize, and the icon and title take back the room the
missing ones would have used rather than leaving a hole.

**Close cannot be taken away.** The metric's range refuses zero, and
`recon_titlebar` puts the close bit back regardless of what arrives -- both,
because a range is a promise about what a skin *file* may say and the other is
a promise about what is *drawn*, and only the second is a guarantee. A skin is a
file somebody downloads; a window nobody can close by mouse is not a look
anybody chose. Checked by writing a skin that asks for zero buttons and
photographing the close button it got anyway.

Close is outermost on whichever side they are -- nearest the right edge on the
right, nearest the left edge on the left. The button somebody reaches for
without looking is the one in the corner, and a layout that put maximize there
would close windows by accident from the other direction.

**The layout is worked out in one place now, for both frames.** ReconOS draws
two title bars -- one around its own windows and one around a client's -- and
each worked out its own. They used different insets and different gaps, so a
client window's buttons sat two pixels from a built-in window's. `recon_decor`'s
own header argues against exactly that: *"a client window that looked nearly
like a ReconOS window would be worse than one that plainly does not; near-misses
read as a fault rather than as a difference."* Both ask `recon_titlebar` now,
which is worth more than the two pixels -- it means a skin that moves the
buttons moves them on every window, not on the windows whoever made the change
remembered to look at.

**`scripts/check.sh` runs everything under the sanitizers.** Address and
undefined behaviour, on all eighteen suites -- reading past an array, a double
free, use after free, a shift wider than a word, signed overflow, a misaligned
load. Every one of those is a bug that passes on the machine it was written on
and fails somewhere else.

All eighteen were clean the first time, which is worth saying precisely because
it makes the next thing the script says worth believing.

**The compiler was never asked what it thought.** There were no warning flags
in `CMakeLists.txt` at all -- not through v0.1, not through v0.4 -- so the whole
project had been building at gcc's default level, which says almost nothing.
Switching on `-Wall -Wextra` for the first time turned up four real things in a
few minutes:

- **Two mail header chains written as `a() || b() || c();`** -- an expression
  statement whose value is discarded. It works, and it is exactly the shape a
  real mistake takes, and the compiler cannot tell the two apart. Said as an
  `if` now.
- **A click compared across two different enums.** `event->state` is wlroots'
  `enum wlr_button_state` and it was being tested against Wayland's
  `WL_POINTER_BUTTON_STATE_RELEASED`. They share values today and are under no
  obligation to tomorrow -- the day they stopped, every click in the system
  would be read as the wrong half of a press.
- **Two switches over an enum that had quietly stopped covering it**, so they
  had stopped being able to warn about the next value anybody added. Both now
  name the values handled elsewhere, so the checking continues.
- **Six functions nothing called.** One was `cycle_focus`, the old Alt+Tab,
  left behind when `recon_shell_cycle_windows` replaced it -- dead code that
  looks like a working feature is the kind that costs somebody an hour. The
  others were thin wrappers and an orphaned declaration. One of them,
  `are_twins`, went with a table nothing read: the OCR built a bitset of every
  confusable *pair* at O(n^2) mask comparisons and only ever consulted the
  derived "has any twin at all". The finer question is a real one and is named
  in the code where the answer is still being computed and thrown away.

None of those was going to be found by reading. The build is clean at
`-Wall -Wextra` now, with `-Wunused-parameter` deliberately off -- a callback
signature is fixed by whoever calls it, so a handler ignoring an argument is
the normal case, and twenty-two of those would bury the next real one.

`third_party` is included as a system directory, which is what makes the rest
readable: `stb_truetype.h` alone contributed ninety-odd "defined but not used"
warnings, because a header-only library is mostly functions a program does not
call. The six real ones were hidden in that ninety, which is the whole argument
for the distinction.

**A file type can be chosen, not only inherited.** Right-clicking a file offers
"Open with" for every application that opens files, with the one that would
open it marked. Choosing one remembers the choice for that extension and opens
the file with it -- both, because choosing a program from a menu means "this
one, and from now on", and an Open-with that opened once and changed nothing
would have to be used every single time, which is the thing somebody reached
for the menu to stop doing.

It is offered in File Explorer as well as on the desktop, because the same file
right-clicked in two places should offer the same things. That needed one thing
an application could not do before: mark the entry that is in force. The shell's
own menus have had marks since the clock grew a twelve-hour and a twenty-four-
hour entry; an application's menu could not say it, so a menu an application
built that offered several answers to one question had no way to show which
answer was current.

A choice is a *user's*, so it lives in their settings and beats everything an
application declares -- it is the only rule in `recon_props_opener` that came
from a person rather than from a deduction. "Use the usual program" appears
only when there is a choice to undo, and clears it rather than setting a
different one, which is the same rule the presets follow everywhere else: what
shipped cannot be deleted, what you added can.

The key is the extension lowercased, so choosing a program for a photograph
applies to the next one even if the camera shouts its file names. A chosen
application that has since been uninstalled falls back to the declared answer
rather than leaving a file type pointing at nothing.

Menus hold twenty-four entries now rather than sixteen, and an overflow
complains in the log. The "Open with" list is as long as the number of
applications that open files, which grows every time somebody installs a
module -- a menu that silently stopped at its limit would lose Properties, and
nothing on screen says a menu is short.

**A client window gets its own icon.** A Wayland client hands its compositor an
`app_id` and never a picture, so every client window wore the same generic
icon -- honest, and also a taskbar where six different programs look identical.

The note this replaces said that guessing an icon from a reverse-DNS string
would be wrong more often than right, and that is true of *guessing*. What is
done instead has three rules and every answer has to be confirmed against
something that exists: a mapping written down in `/System/Config/app-icons`, an
application ReconOS has registered under that name, or the last dotted part of
the app_id **if an icon by that name is actually there**. The third looks like
guessing and is not -- it cannot produce a wrong picture, only a right one or
none, because the file has to exist before it is used. The failure mode is "no
better than before", which is the only failure mode worth having.

The written-down mapping comes first, because it is the one rule that is a
statement rather than a deduction: it is how to say "no, this client is not the
thing its name reduces to". It is created once with the format in it and never
overwritten, unlike the help pages -- a line added by hand that the system
replaced on the next boot would be a file nobody edits twice.

Checked by handing the compositor three app_ids and looking at what it drew:
one taken from the mapping file, one resolved to a registered application, and
one that reduces to nothing, which correctly kept the generic icon.

**The grapher draws three curves and can be moved about.** A grapher exists to
compare -- "is x^2 above or below 2^x" is the question, and answering it by
typing one, looking, typing the other and remembering is not answering it. Three
fields, each with a swatch in its curve's colour, so the picture says which
line is where and the swatch says which field drew it.

The colours are the skin's own readout accent with its hue rotated a third and
two thirds of the way round, which keeps the skin's character and keeps the
*lightness* -- the half of a colour that decides whether it can be seen at all.
Three fixed colours would be legible on some skins and invisible on others.

**The wheel zooms about the pointer, and the expressions are remembered.**
Zooming about the middle instead means finding a feature, dragging it to the
centre, zooming, and finding it has moved again -- the arithmetic that keeps
the value under the pointer where it is is one line, and it is the difference
between a grapher somebody explores with and one they fight. The wheel and the
buttons use the same factor, so one notch out undoes one notch in exactly.

The three expressions are written to the user's settings as they are typed
rather than when the window closes -- a window that saves on close loses
everything if it is never closed politely, and the whole point of keeping these
is that somebody spent a minute getting one right.

**And the view can be dragged.** It was always centred on the origin, which
makes a grapher that can only look at one place: every interesting part of
log(x) is to the right of it, and zooming in on something near x=10 moves it
off the screen. The plane comes with the pointer the way a map does, the centre
at the moment the drag began is remembered rather than accumulated so the point
under the pointer stays under it, and Reset puts back the place as well as the
span.

The grid is drawn outward from where zero actually is and clipped, rather than
counted from the middle of the box. Counted from the middle it would slide half
a line at a time as the plane moved under it, which looks like the grid being
wrong rather than like the view moving.

BG-121 is the hour that went into the first version of the drag: `motion`
changed the view and did not ask for a redraw, so the numbers moved, the state
was right, and the picture went on showing where it used to be. It looked
exactly like the drag not being delivered. Two `fprintf`s settled it in one
run.

**Help can be searched.** A box above the topic list, filtering on titles and
on the text of every page. Both, because the change log's topics are titled
with version numbers -- a title-only search would find nothing in two thirds of
the document -- and because somebody typing the name of a page they already
know they want should get it.

The corpus is about a hundred kilobytes across forty-odd files, read once and
kept, because a search box that reads forty files per letter typed is a search
box that feels broken. Ctrl+F puts the caret in it, the same key that finds
text in Notepad. Escape empties it and gives the whole list back. Typing while
a page is open does not move off that page unless the search filters it away,
so looking something up does not lose the place of somebody who was reading.

**And F1 now opens the page it says it will.** The help has been promising that
it opens "at the page about whatever you are looking at", and it kept that
promise for four applications out of ten. Web and Mail both asked for a page
called "Networking" that has never existed, so F1 from either left whatever was
already showing; Photos, the player and the Calendar each asked for "Writing",
which exists and is about Notepad -- which is worse, because it looks like an
answer.

Seven pages written that did not exist, every application pointed at its own,
and a topic that does not exist is now said out loud in the log and falls back
to the beginning rather than being passed over quietly. The name is declared
beside the application and the pages are written somewhere else, and nothing
made the two agree; this is what makes the disagreement visible. BG-120.

**An application says what it opens, and a module can say it too.** The answer
to "what opens a .png?" was a list of extensions inside one function in the
system -- a long way from the application that opens one, and with no way for a
module to answer it at all. A module could bring an application that reads a
format, and the file would sit on the desktop correctly named and correctly
drawn, and double-clicking it would do nothing.

Registering an application and registering what it opens are now the same act.
A module's claim beats the system's own default, because somebody who installs
a picture editor and finds pictures still opening in the viewer has installed
something that does not work. Turning an application off in the Control Panel
hands its file types straight back -- an "off" that leaves the associations
behind is not off -- and every application's row now says what it opens, because
"this program will now open your pictures" is a consequence of installing
something rather than a detail.

The one association that stays dynamic is the player's: what it opens is
whatever can be decoded, and that changes when a module brings a decoder.
Written down once it would be a list that is wrong from the moment somebody
installs anything.

**Every built-in lost its version number to that change, for about an hour.**
The field went into the middle of a public struct and the twelve built-in
registrations were positional, so each version string slid one place and
`version` became NULL. It compiled without a warning; the only sign anywhere
was a dash in one column of `apps`. The version is the number the whole
applet-update decision rests on. The table uses named fields now. BG-119.

**Mail can attach files.** Up to eight, twelve megabytes together, chosen with
the same file picker Notepad uses. The message becomes multipart/mixed; without
an attachment it is written exactly as it always was, one plain part with no
boundary anywhere in it -- which is the point rather than an optimisation,
because every mail reader in the world handles the simple shape better than the
complicated one.

**The boundary is checked, not assumed.** This is the one genuinely dangerous
part of multipart and it is dangerous quietly: a boundary that also occurs
inside the message splits it there instead, so the letter arrives cut in half
-- or somebody who can put text in the body ends the message early and appends
parts of their own. The usual answer is a long random string and the argument
that a collision is unlikely, which is fine against accident and worthless
against somebody who has read the source. So the string is built, searched for
in everything it will separate, and rebuilt with a different number if it is
found. Certain rather than probable, for the cost of one pass.

A filename is a header value twice over, so it can carry the same attack a
subject can and one more: it is written inside quotes, and a quote in it ends
the value early. Line breaks, quotes and backslashes are refused.

The files are read at Send, not when they are chosen. A file picked at nine and
sent at eleven should be the file as it is at eleven -- and reading it then is
the only way to notice it has been deleted since, which is a thing to say out
loud rather than to send an empty part about.

**Mail takes Cc and Bcc.** Three address fields, each a comma-separated list,
and one rule that is the whole reason there are three of them rather than one:
**every address reaches the server, and only To and Cc reach the message.**

Getting that backwards is the failure that has embarrassed real mail software
repeatedly. The letter still sends, still arrives, and quietly hands every
recipient the list of people who were meant to be hidden -- and nothing about
it is visible from the sender's side. So the guarantee is built as something
`recon_smtp_compose` *cannot* do rather than something it remembers not to:
there is one function that writes an address header, it is handed the name to
write, and Bcc never calls it.

A letter addressed only in Cc, or only in Bcc, is a real letter and is sent --
with no To header at all, because there is honestly nobody to put in it. A bad
address anywhere in any of the three lists stops the whole send and the message
says which one: a letter that reached four of five people and reported success
is worse than one that failed, because nobody goes looking for the fifth.

**`state` says who has the input.** The login screen takes every pointer event
while it is up, which is correct -- but `apps <name>` would still open a window
and `state` would still call it open and focused. It is neither, and that cost
an afternoon of measuring a Calculator whose buttons were fine. `session` knew
all along, which was not enough: it has to be said by the command somebody
actually runs. BG-117.

**One caret, in the field being typed into.** Every text field drew one at
once, and every field with a default value drew it selected -- so the Mail
setup screen showed six carets and two highlights, none of which meant
anything. `struct recon_edit` has carried the flag that answers this the whole
time and the drawing never read it. BG-118.

**The Calculator opens at a size it can be used at.** Six mode tabs on one
row, each sized to its own label; a keypad whose columns divide the width
exactly rather than throwing the remainder away; labels centred by the line's
own height instead of by a constant that only worked at one font size; and
keys drawn with the button edge, so they round with the skin and read as forty
buttons rather than one slab with lines scored in it.

The window opened at 430 wide with a minimum of 520 — a size the resize code
would refuse to let anybody choose. That is fixed in `recon_appwin_create`
rather than in the Calculator: an opening size below the minimum is raised to
it, for every application, because it is a mistake any of them can make and
none of them can see. See BG-116.

**`scripts/look.sh` photographs the desktop from a script.** Every "does it
look right?" question in this project has been settled by a picture, and
getting to one takes four steps that are not obvious — two of which cost an
afternoon each. The login screen takes the whole screen's pointer input, so a
window opened while it is up reports itself as focused and cannot be clicked;
and `capture` resolves its path inside the ReconOS filesystem rather than the
host's, so an absolute host path reports success and writes nowhere useful.
The harness signs in first, to a copy of the filesystem with the password
removed, and refuses to continue if it did not reach the desktop.


**ReconOS makes a sound.** `recon_audio` is a new boundary of the same kind as
the filesystem and the network: one file knows how sound reaches hardware and
nothing above it does. It speaks ALSA rather than a sound server, deliberately
— ALSA is the lowest thing on Linux that is still an interface, so the header's
shape is close to the shape a real driver has, and replacing it when there is a
ReconOS driver is replacing a file rather than rethinking an idea.

It is **pulled, not pushed**: the device asks for samples rather than being
handed them. A device runs at its own rate and wants a fixed number of frames
at fixed moments, and an interface where the caller decides when to write makes
the caller responsible for a clock it does not own.

Without a sound library at build time, or a sound card at run time, every
function is still there and says there is no audio. That is the same code a
machine with no card takes, which is the path that otherwise gets tested least.

**Codecs are a registry, and the registry is the deliverable.** A decoder says
which extensions it handles and how to recognise its files, and everything
above asks rather than knowing a list — so a module can bring a decoder and
nothing in the player changes. Contents are checked before the name: a file's
bytes are what it is, and its name is what somebody called it.

ReconOS ships the ones it can honestly write. **WAV** is written here, because
it is a header and then the samples: 8, 16, 24 and 32-bit, integer and float,
mono and stereo. **MP3** is minimp3, which is a format parser and therefore on
the permitted side of `THIRD_PARTY.md`'s line. Writing an MP3 decoder here was
considered and rejected — the format is a hundred pages of psychoacoustics, and
one that is ninety-five per cent correct does not sound nearly right, it sounds
broken.

Every decoder is checked against ffmpeg, sample for sample, on real music
rather than a tone. All four WAV widths match **exactly**; MP3 is off by at
most one across eight million frames, which is rounding. That comparison found
a real fault: the float conversion scaled by 32767 instead of 32768 and
truncated instead of rounding, so every sample came out fractionally quiet and
biased towards silence.

**MP4 files play their sound**, and the split that makes that work is the
answer to what a codec pack is here.

ReconOS demuxes the container itself. An MP4 is a tree of boxes and three
run-length tables saying where each compressed piece lives — that is structure,
and structure is what this system writes for itself. Verified by pulling every
audio frame out of a real video and comparing it byte for byte with the frames
ffmpeg extracts from the same file: **all 3307 of them match exactly.**

The *decoding* is a module. `CodecPack.rts` wraps libavcodec to register an AAC
decoder, and it is genuinely optional — built only where libavcodec is present,
removable afterwards, and the desktop plays WAV and MP3 without it. A codec
pack that cannot be taken off again is not a pack, it is a decision somebody
made for you.

Which means a file that cannot be played says **which decoder is missing**:
*"that file holds H.264 video and AAC sound. There is no AAC decoder installed,
and nothing here shows video yet."* That sentence is the entire reason the two
halves are separate, and it was wrong until it was tested — `open()` returned
NULL for three unrelated reasons with no way to tell them apart, so the player
guessed, and guessed "or it is damaged" about a perfectly good file.

**Video plays.** H.264 and H.265 decode through the codec pack; everything
around them is ReconOS's own, and the split is deliberate rather than
convenient. libavcodec is handed compressed bytes and gives back three planes.
It is not asked to demux, to convert colour, to scale, or to decide when a
picture should be shown -- it can do all four, and all four are structure or
arithmetic, which are the things this project writes for itself. Keeping the
borrowed part exactly the size of the thing that justifies it is what keeps the
justification checkable: "libavcodec, because H.264 is seven hundred pages" is a
claim somebody can weigh, and "libavcodec" is not.

**Colour conversion carries the two things that are usually guessed.** Which
coefficients (BT.601 or BT.709) and which range (0-255 or the broadcast 16-235)
are read from the file and only guessed when the file does not say -- and then
guessed the same way everything else guesses, by picture height. Treating studio
range as full range gives grey blacks and washed-out whites, which is the
commonest way a home-made player looks subtly wrong and never looks like a bug.

**Converting and scaling are one pass, and there are two resamplers.** Going
straight from the planes to the window's size means the colour conversion runs
once per pixel that will be *seen* rather than once per pixel in the file: a
1080p frame in a 400-pixel box is nine tenths less work, every frame. And the
resampling changes with direction -- interpolation when enlarging, an average of
every source pixel when reducing. That second one was not a preference. Bilinear
reads four pixels, which is most of the source at 1:1 and a twelfth of it at
3.4x, and what gets thrown away comes back as aliasing that crawls from frame to
frame. Measured against ffmpeg: 5.05 mean difference per channel with bilinear,
3.59 with the average, and 1.15 at native size where neither runs.

**The sound leads and the picture follows.** The device is the clock, as it has
been since sound arrived -- it consumes samples at a fixed rate whether or not
anybody is watching, and asking how many it has played is a measurement rather
than an estimate. Pictures have no such thing, so a frame that arrives late is
*dropped* rather than shown late. A file with no sound track runs on wall time,
which is a worse clock and is still a real one; counting frames and assuming
each took as long as it should have is not.

**Seeking starts from the last frame that stands on its own.** Video frames are
mostly descriptions of how they differ from earlier ones, so landing anywhere
and decoding forward gives a second of coloured smears. The sync-sample table
says where the self-contained frames are; everything between there and the
target is decoded for what it teaches the decoder and never shown.

**Verified against ffmpeg on a real file, frame by frame**, at five scales and
seven seek targets rather than one of each -- which is what caught both faults.
The full accounts are BG-097 and BG-098, and the short version of each is worth
carrying: a test whose inputs are all round numbers is testing round numbers,
and a comparison against a reference decoder proves agreement about the one file
it was run on.

**A Media Player** with a playlist, transport, a draggable position bar and its
own volume — applied to the samples rather than to the device's mixer, because
turning the whole machine down when somebody wants one track quieter is
reaching past your own edges. The position is asked of the device rather than
counted, since what has been handed over is up to a buffer ahead of what
somebody is hearing.

**Every kind of file has its own icon.** They are all the same sheet with one
mark — a level meter for sound, film perforations and a play triangle for
video, a horizon for a picture, angle brackets for markup, a brace for
structured text, a letter for a typeface, a banded box for an archive. One
icon for every file says nothing, and saying what a thing is before its name is
read is most of what an icon is for. The Type column and the desktop ask the
same question the explorer does, so a file looks the same everywhere.

**A window frame you can see through, and a skin built on it.** A skin can now
say how solid the chrome is, and *Glass* says 210 of 255 -- see-through enough
that the wallpaper moves under a title bar, solid enough that a filename stays
readable over a photograph.

The mechanism is one pass over a finished rectangle rather than an alpha on
every fill. The title bar is drawn exactly as it always was, by code that has
not changed, and then faded once at the end -- so its text, its icon, its
buttons and its rounded corners all become see-through together and none of them
had to learn a new rule. The alternative was touching every drawing primitive in
the system, where getting one wrong leaves an opaque patch inside a translucent
bar.

**Premultiplied, which is the part that fails invisibly.** Wayland's ARGB8888
has the colour channels already scaled by the alpha, so half-opacity white is
half-grey. A version that set only the alpha byte would give chrome that is
see-through *and* too bright -- which reads as a deliberate glow rather than as
a fault, and would be found by somebody wondering why the glass looks lit from
inside. It is pinned by a test rather than by looking at it, in a file that
needs no compositor to run.

**The taskbar and the Apps menu take it too, and the menu takes half.** At the
same opacity as a window frame, the menu put "Recon Core" directly on top of
another window's "Line 1, Column 1". That is not a tuning problem: a title bar
carries one short label, and a menu is a column of a dozen somebody is scanning.
Halved rather than given a second setting, so a skin still says how much glass
it wants once and the rule that follows is a sentence -- chrome you read a list
from gets half. The tooltip and the dim behind a dialog stay solid, for the same
reason stated the other way round.

**And a second icon set, lit.** Every generated icon is written twice: flat, and
again into `/System/Icons/Glossy` with a curved-glass treatment -- a vertical
ramp that makes a flat shape read as curved, the lower arc of a large ellipse
centred above the icon for the specular, and one lighter row along the top of
the shape for the rim. It follows the icon's own alpha rather than a rectangle,
which is the whole difference between a glossy icon and an icon with a glossy
box behind it.

Two sets rather than one treatment applied everywhere, because the flat idiom is
what Classic and Recon are *for* and 95 did not gleam. Two sets rather than a
gloss applied on the way to the screen, because these are files precisely so any
one of them can be replaced -- and a gloss at draw time would be applied to the
replacement too.

A skin says which it wants with `metric.icon-gloss`, and the glossy set falls
back to the flat one rather than switching to it: an icon that only exists flat,
because somebody added or replaced it, still appears under a glossy skin. And
the icon cache now watches the theme's generation counter, because which file a
name resolves to is no longer decided by the name alone -- BG-089's shape
exactly, caught before it shipped this time.

**Screenshots read now.** A whole 1280x720 desktop -- windows, their menus, the
status bar and the clock -- comes back as 101 of 125 marks at confidence 97,
where a day earlier it came back as nothing.

The missing stage was in front of the others. Finding lines by looking for rows
with no ink is right for a picture that is only writing and wrong for a screen:
a window border puts ink on every row it spans, so the display collapsed into a
few enormous bands and every mark in one was a blob.

**The first guess about why was wrong, and measuring said so immediately.** I
assumed the wallpaper was being thresholded as ink and drowning everything. A
whole desktop measures 1.9% ink -- *less* than a crop of plain text at 3.9%. The
threshold was never involved. It was geometry.

`recon_ocr_regions` cuts the picture on whitespace, alternating between rows and
columns and recursing into each piece: a screen into windows, a window into its
bar and its contents, the contents into paragraphs. **A cut has to be wide
relative to the piece being cut**, which is what stops the recursion running
past a paragraph and separating the words in it -- a fixed number of pixels
cannot do that, because the gap between two words at forty point is wider than
the gap between two paragraphs at eight.

**And a line drawn across a whole block is a place to cut, not something to
read.** That is what gets inside a window at all: its border is one column of
ink on every row, so no run of blank will ever be wide enough there. Text never
spans its own extent; a frame, a rule and an underline always do. Only where the
block is over 48 pixels, because at the bottom of the recursion a block is a
single line and a tall letter genuinely does span it.

Smaller blocks then exposed the other end of BG-100. The taskbar clock read as
`9 / 5 / 2 0 2 6`: its gaps are one and two pixels, and two is twice one, so the
proportional rule found a word break between every character. That fault was a
threshold taken from the band's height alone; its fix took one from the
distribution alone. Both halves were needed -- **one pixel of difference is not
evidence at any size**, and a quarter of the height is the floor under it.

**Photos saves what it opens.** It could read seven picture formats and write
none of them, so a JPEG stayed a JPEG. **Save as PNG** writes the picture out
beside the original, under a name nothing else has -- a converter that
overwrites is a converter that loses the thing it converted.

PNG out and nothing else, deliberately. The direction people want is almost
always this one: a photograph arrives compressed and is wanted lossless to work
on. Offering the other direction would mean a button that costs a little of the
picture every time somebody presses it.

The whole feature was the join between two things that already existed -- the
decoder Photos opens files with, and the encoder written for screenshots -- and
the interesting part was what checking it found. **The encoder had never been
tested.** It had been looked at, on screenshots, by a person deciding they
looked right; a picture can have red and blue swapped or an alpha channel
quietly flattened and still look right at a glance. The new suite encodes with
the writer and decodes with stb_image, so what is being checked is agreement
with a different implementation by a different author.

That found a refused encode leaving the caller's length variable untouched. No
live caller was bitten -- each starts its own at zero and checks the pointer --
which is exactly why it was worth closing: the next caller is the one that
reuses the variable, and a stale length beside a NULL pointer is an overrun
waiting for a skipped check.

Checked on a running system as well as in memory. A 3900-byte JPEG opened in
Photos, the button pressed, a 7485-byte PNG beside it; both then decoded by
ffmpeg, which wrote neither. 375 of 79800 samples differ and **every one of
them by exactly one step** -- stb_image and libjpeg rounding the IDCT
differently, within what the JPEG standard permits. The conversion moved
nothing.

**A Read Text button in Photos.** It reads the picture on screen, writes the
text to a new file in Documents named after the picture, and opens it in
Notepad. Verified end to end on a running system: 46 of 46 marks, confidence
100, a file on disk with a provenance line at the top of it.

**Three of the four outcomes write nothing, and that ratio is the feature.**
The only case that saves without asking is the one the engine itself calls a
reading. A picture where marks were found and none could be named says so and
saves nothing -- a file whose entire contents are replacement characters is not
a result. A guess is held and asked about, with the real numbers, and
**deliberately without showing a sample of the text**: an excerpt reads as
evidence, and the whole reason to ask is that the engine cannot tell whether it
is. There is no setting to skip the question and no remembered answer, because
it was a guess every time.

The provenance line goes on **every** file, not only the doubtful ones. A note
that appears solely on uncertain output makes its absence a claim of
confidence, and absence is erased by one edit.

**The reading happens on a timer rather than on the click.** It is synchronous
and the desktop is single-threaded, so doing it inside the click handler would
freeze with the *previous* status showing -- the one moment the machine is busy
would be the one moment it had not said so. Forty milliseconds is enough for
"Reading." to reach the glass first. Above twelve megapixels it asks before
starting, and that threshold comes from measurement rather than caution: a
1920x1080 screenshot reads in about 40ms and a 2400-pixel page in about 100.

**And the header now says what this cannot read.** A screenshot of a *whole
desktop* comes back as almost nothing, and it is worth stating plainly because
it is the picture people will try first: `recon_ocr_lines` treats a row with any
ink as part of a line, which is true of a cropped page and false of a screen
where window borders put something on nearly every row. The display collapses
into a few enormous bands and every mark in them is a blob, correctly refused.
Fixing it needs a stage that finds text regions before finding lines, and that
stage does not exist yet. Until it does, this reads a picture of some text and
not a picture of a screen.

**Reading text out of a picture, finished.** On a real screenshot of ReconOS's
own text -- through the compositor and a PNG encoder, not a buffer the test drew
for itself -- it reads **52 characters of 52, refuses nothing, and reports a
confidence of 100.** The premise held.

The matching is baseline-anchored template comparison. Each line's size and
baseline are fitted from all its marks at once, because no single mark contains
either -- an 'o' does not know where the baseline is and a 'T' does not know the
x-height. Then every candidate has exactly one place it could sit, so three
comparisons on position and width discard about nine tenths of the alphabet
before a pixel is looked at. **Those three comparisons are what separate o from
O from 0, and a comma from an apostrophe** -- and they are the payoff for
refusing to normalise each mark into a common box, which is the standard move
and which throws away exactly the height and width those pairs differ by.

**The engine measures whether it can tell two letters apart, rather than
assuming.** For every pair of candidates at a size it records whether they are
the same shape, and refuses to name either when they are. In DejaVu Sans, `l`
and `I` score exactly 1000 against each other at 10, 12, 14, 16 and 20 pixels.
That is not a similar shape; it is the same shape, and picking one would be
right about half the time and look identical to being right.

Two faults found before shipping, both by looking rather than reasoning:

The same letter came out **9x10 from the page and 7x9 from the rasteriser** --
the mark systematically fatter, and every score sitting at 850 where it should
have been a thousand. The two sides were cut at different thresholds: the page
by Otsu, which on black-on-white lands near 200, and the candidate at a flat
128. The mirrored cut is exact rather than tuned -- a page pixel is ink when its
value is at or below the threshold, and coverage `c` is drawn as `255 - c`, so
the candidate cut is `255 - threshold`.

And it read `"the lazy dog"` as `"the Iazy dog"`, which is the one thing the
design exists to prevent. The twins rule should have caught it and did not,
because it compared the winner only against the runner-up. Fixed to refuse any
candidate with a twin -- which is what `recon_ocr_match.h` already promised, and
the header being stronger than the code is the worse direction for those two to
disagree in.

**Reading text out of a picture, begun.** ReconOS has no word processor and
cannot yet run anybody else's, so a screenshot of a document is currently text
that cannot be got at. That makes this worth more here than it would be
elsewhere.

**It is not a research project, and the reason is that ReconOS draws its own
text.** General optical character recognition -- photographs, perspective,
handwriting, unknown faces -- is a field. But most of what anybody wants read is
text that was *rendered*: a screenshot, a saved page, a scan of ordinary print.
A system with a font rasterizer can produce the shapes it is trying to
recognise, which turns "what letter is this" into a comparison against shapes it
can draw on demand. That is the whole idea and it is also the whole limit.

Three of the five stages are built and tested: deciding which pixels are ink,
finding the bands text sits in, and splitting a band into marks. All three are
arithmetic on a bitmap, so they need no font and are tested without one -- on
images built a rectangle at a time, because an OCR test that runs on a
photograph can only be checked by reading its output, which means it passes
whenever the output looks plausible.

Two things settled early because they are the honest part rather than the last
part. The threshold is chosen from the image by Otsu's method rather than fixed,
and it also decides **which way round the page is** -- light text on dark is as
ordinary as dark on light in a screenshot, and reading it backwards does not
fail: it finds the *gaps* between letters, which are marks, in rows, of
plausible size, and produces a confident answer made entirely of holes. And the
result carries a confidence and a count of marks it could not name, because a
reader that cannot say "I am guessing" is one that lies on every picture it was
not built for.

**The matching is not written**, and there is deliberately no whole-picture
entry point yet. A function that finds marks and can name none of them works and
returns nothing, which is a worse thing to ship than a header saying which half
is finished.

**The glass can be a colour.** Six tints -- Blue, Amber, Rose, Jade, Violet,
Graphite -- chosen beside the skin rather than as skins of their own. Eleven
skins times six colours is sixty-six entries in a list somebody has to read,
every one differing from its neighbours in a single respect.

**A tint keeps the lightness and moves only the hue**, which is the whole
design. A palette's *structure* is in its lightness -- which surfaces sit above
which, which text reads against which -- and only its appearance is in its hue.
Recolouring by rotating hues changes what a skin looks like; doing it the other
way round changes whether it works. So the tint is used as a hue reference: a
colour at the tint's own lightness comes out as the tint, darker ones as darker
versions, lighter ones as lighter. Pinned by a test across the whole range,
because the two sides of that calculation are different code and an error in
either is invisible from the other side of the branch.

**Only the chrome moves, and only a skin that opts in.** Text roles are excluded
so a tint can never be the reason a label became hard to read; the accent, the
selection and the warning colour are excluded because they mean something, and a
meaning that changes hue with the decor is one nobody can learn. And a skin has
to say `metric.tintable` -- which the colour-vision skins never will, because
their palettes are chosen so particular pairs stay distinguishable and moving
the hues would undo exactly that, silently, to whoever picked the colour.

The first six screenshots of six different tints came out pixel-for-pixel
identical. The setting was stored, the lens was applied at the point a colour is
asked for, and nothing had asked for a colour since -- `recon_shell_restyle` was
missing. A change nothing is told about is a change that did not happen, which
is the same shape as the icon cache keyed on a name after the name stopped
deciding the file.

**A `dialog` command on the control socket**, which exists because of how the
change below was tested. Getting a dialog on screen to look at cost four
attempts at aiming a click, and every one failed silently -- a click that misses
a close button by three pixels does nothing, reports nothing, and is
indistinguishable from a dialog that never opened.

The useful half is not `dialog ask`. It is the bare `dialog`, which prints what
is being asked and **where each button is**, so nothing driving the desktop from
outside has to guess a coordinate. Every guessed coordinate is a test that can
fail for a reason unrelated to what it tests.

`dialog press <label>` answers by name, and does it by clicking the button at
its real position rather than by calling the answer callback -- calling the
callback would test the callback, and what is worth testing is that the button
is where it is drawn and that its hit region agrees. `dialog ask` is gated
behind `RECONOS_ALLOW_SPAWN` like `raise`, because it puts a question on screen
that nothing asked.

**Glass reaches the dialogs, and stops at one of them.** The context menu and
All Programs take the same half-strength glass the Apps menu does. A dialog
takes it *only on its title strip*, which is the rule window frames already
follow: the bar that says what this is fades, and the part somebody has to read
does not.

That was found by looking rather than by reasoning. Faded whole, the delete
confirmation had the file list's selected row -- a solid blue bar -- running
directly behind the words "Move 'notes.txt' to the Recycle Bin?". Legible, and
not what a question about somebody's file should look like.

**The security box stays solid, and the skin gets no say.** It asks somebody to
approve something they cannot undo, and it dims the whole desktop behind itself
so that being asked is unmistakable. See-through would work directly against
what it is for: "it looked like part of the window behind" is the beginning of
every story about somebody approving the wrong thing. Same rule as there being
no switch to turn a safety check off.

**ReconOS has its own Wayland protocol.** The applications that ship with it
read the palette directly because they are in this process; one that arrives
later is a client in its own process and cannot. The decision recorded in
`docs/APPLICATIONS.md` is that installed applications become clients, so this is
on the path to that rather than beside it.

Without being told, a client can guess and be the one window that does not
match, ship its own theme and be the one window that does not change when the
desktop does, or read the registry behind the compositor's back — which works
until the format changes and is not a boundary at all.

**What it does not do is say how to draw.** No frame, no button shape, no font.
A client that wants to look like it belongs uses the colours; one that wants to
look like itself ignores them, and the test client keeps its fallback colours to
prove that ignoring them is allowed.

**The first version of the test client passed and proved nothing.** It recorded
the palette and stopped, so the client was told the skin had changed and went on
showing white content inside a dark frame. Everything about the protocol worked;
the claim was that a client *following* it ends up the right colour, and that
was false. A test that stopped at "the bytes arrived" would have called it a
pass. Measured properly: the pixels inside the client's window are `#FFFFFF` on
Recon and `#1E2024` on Midnight — exactly the two surface colours the compositor
said it had sent.

**The Calculator's sixth mode.** Graphing was the one of six never built, and it
needed an expression evaluator first — the Calculator is button-driven and had
no way to read one.

**Two things make a grapher honest rather than merely working**, and both are
about refusing to draw a line that is not there. A column where the function has
no value breaks the stroke, so 1/x is two branches rather than two branches
joined by a vertical wall down the y axis. And a jump wider than the whole
window between neighbouring columns is a break rather than a slope, because no
honest curve crosses the window in one pixel of x — get that wrong and tan(x) is
drawn as a row of walls.

That is why the evaluator has **three results rather than two**. Dividing by
zero and the root of a negative are not errors: the expression is fine and has
no value at that x. A grapher told "error" gives up on the whole curve because
of one point.

**Pixels are square**, with the height following from the width and the shape of
the box. Letting them differ draws a circle as an ellipse, which is a quieter
lie of the same kind. The cost is that sin(x) looks flat at this scale — because
it is, and a grapher that silently stretched it would be answering a question
nobody asked.

**My test was right and the code wrong**, for a change: `-2^2` came to 4. A power
binds tighter than a minus on its left and looser than one on its right, and the
paragraph at the top of the file said exactly that while the grammar underneath
did the opposite.

**STARTTLS, with the upgrade required.** Port 587 works now, so a provider that
offers only STARTTLS is usable. It was deferred on purpose — the easy half is
connecting and the hard half is the one that ships a system sending passwords in
the clear.

**Three things something in the middle can do, and all three are refused.**

It can *delete* STARTTLS from the server's offer, and a client that carries on
has sent everything in the open because one line was removed. So the upgrade is
**required**: no offer means the session ends. A server that genuinely cannot
encrypt and a middle that stripped the offer look identical from the client's
side — and that is the point, because a client able to tell them apart is one
that could be argued out of encrypting.

It can answer *"ready to start TLS"* and put more commands in the same packet.
Those bytes arrived before anything was proved, and a client that reads them
takes an attacker's commands as though they came from inside the encrypted
session. That is how STARTTLS has been broken in real mail clients. Everything
buffered is discarded the moment the upgrade starts.

And it can lie in the first EHLO. Nothing learned before the upgrade survives
it — EHLO is sent again afterwards, so what the server can do is heard from the
party whose certificate has been checked.

**Tested against a server built to attack it** rather than one built to work,
because the well-behaved path ends at a certificate a fake server cannot produce
and everything worth checking happens before that. The no-offer server saw
exactly `EHLO reconos` and nothing else; the injecting server saw `EHLO reconos`
and `STARTTLS` and nothing else. No AUTH, no MAIL FROM, no letter, in either.

**BG-113 came out of the same fake server, and is not about SMTP.** A server
that accepts and then says nothing left the client waiting forever: the connect
deadline was dropped the moment the socket connected, and *connected* and
*usable* are the same moment for a plain stream and not for an encrypted one.
Every outgoing TLS stream had it — reading mail, the web viewer, checking the
time.

**Mail can send.** Both halves of an account on one form, because reading and
sending are separate protocols on separate servers and the same account to the
person filling it in. The sending half is allowed to be empty — somebody who
only wants to read should not be stopped at a field asking for a server they do
not have.

**Write is reachable without connecting first.** Sending and reading are
different servers, and requiring a successful connection to one before a letter
can be handed to the other is a coupling with nothing behind it: a mail server
being down should not stop somebody writing.

The body is the first multi-line field in the system, so `recon_edit` gained a
flag — Enter puts a newline in rather than finishing, on that field only. There
is no way to send from the keyboard, deliberately: a letter should not leave
because somebody finished a line. And no wrapping, stated rather than faked — a
long line runs off the edge and is still there; wrapping means deciding where
words break and mapping the caret through it, which is a text engine rather than
a text box.

**A failure leaves the letter exactly as it was.** A failure that also loses what
somebody wrote is two failures, and the second is the one they remember.

**BG-112, found by filling the form in and noticing the port had gone.**
`recon_edit_begin(&field, field.text, …)` hands `snprintf` a source and a
destination that overlap — undefined, and here an empty string. Not new: the
mail form has moved focus that way since it was written, and the Web viewer's
address bar did it on a click, so **clicking the address bar cleared the address
it was showing**. Unnoticed in both places because a field about to be typed
into looks the same cleared as selected. The form only exposed it by gaining two
fields with defaults worth keeping.

**SMTP, the protocol underneath it.** It could receive on two
protocols and not send at all. This is the transport and the message; the window
to write one in is next.

Encrypted from the first byte, with no plaintext path in the file and no setting
that produces one — and **the cost of that is named**: a provider offering only
STARTTLS on 587 is a provider this cannot send through. STARTTLS is absent
because it means starting in the clear and asking to be upgraded, and `recon_net`
has no way to upgrade a stream, deliberately. Building it has to make the
upgrade *required*, because the easy half is connecting and the hard half is the
one that would ship a system sending passwords in the open.

**The rules live in their own file so they can be tested without a network
stack**, and they are where the mistakes are. A newline in a subject is how one
message becomes two — whoever wrote it gets to add headers, and what arrives is
not what was on screen. Refused at composing as well as at checking, because
those are separate entry points and a caller could reach the second without the
first. The *sender's* address is checked too: it comes from the settings rather
than the message, so it is the one nobody thinks about.

A line that is exactly a dot ends a message, so one inside a letter is doubled.
A body with no final newline gets one, or the dot that follows lands on the end
of a sentence. And a message too long for the buffer is refused whole — a
truncated one is still valid SMTP, so it would be accepted, delivered, and
arrive missing the end with nobody told.

base64 went in beside hex in `recon_crypt`, checked against RFC 4648's vectors —
not mine, so agreeing with them is evidence about the encoder rather than
evidence I wrote the test and the code the same way.

**Photos can make a picture a different size.** Half and Double rather than a
box to type a size into — a dialog taking two numbers has to explain what
happens when they do not match the picture's shape, and the honest answer (the
shape is kept and one of them is ignored) means the second box was never real.

Nothing is written. What is on screen changes and the file does not, so a resize
is something to look at before deciding to keep, and keeping it is **Save as
PNG**, which already refuses to overwrite. That gave Photos unsaved state for
the first time, so the title bar carries the same star Notepad uses —
deliberately not a different sign.

**One resampler, where there were two halves and a gap.** I said this would be
small because `recon_video` already area-averages down and interpolates up.
That was wrong: `recon_video_render` takes YUV planes and cannot be pointed at a
photograph. What existed was a private RGBA shrinker in the wallpaper loader
that could only make things smaller, a good two-way resampler welded to video,
and nothing at all in the application where somebody would ask.

`recon_image` is the shared one, and it picks its resampler **per axis** — a
picture made narrower and taller at once wants an average across and an
interpolation down, and one rule for both gets one of them wrong. Tested against
properties rather than a fixture, because a fixture for a scaler is a picture
the scaler made and proves only that it agrees with itself.

**The clock's menu opens the page, not the panel it lives on.** Also two things
the screenshot caught that the change itself caused: the header said "Central
(UTC-6)" while the row highlighted underneath was Eastern, because the
summer-time hour was being folded into the match and Central-plus-an-hour is
Eastern's offset. And the note explaining why there are no buttons to set the
clock ran off the end of the window as "Its own co...".

**The clock tells the right time.** It was an hour slow: Central selected, the
host reading 4:25 am, ReconOS reading 3:25 am. The page said plainly that
nothing here follows daylight saving — true, and it did not stop the clock being
wrong for most of the year for most of the people in the list.

The original reasoning is not reversed. A rule engine for the world's
daylight-saving legislation is a database with politics in it, revised by
parliaments with no interest in this clock, and getting it wrong twice a year is
worse than not having it. So **summer time is a switch rather than a rule**, and
it starts from what the host thinks: `tm_isdst` says whether summer time is in
force and the offset says by how much, so the two come apart into a standard
zone and a switch without any rules being carried. Written with standard time
arithmetic rather than `tm_gmtoff`, which is a BSD extension C11 does not have —
a system that intends to run on its own kernel should not lean on what glibc
adds to a standard structure.

**The time zone list had a scrollbar and no way to move it.** Twenty-six zones,
about nine rows of room, seventeen unreachable. The note directly above the
fault describes the fault: it explains that every list in Appearance takes the
wheel now, because one of them *"said 'scroll for the rest' under a list that
could not be scrolled — a page telling somebody to do something it would not let
them do."* The same sentence was true one page over.

**And checking against a time server told people to edit the Registry**, which
is not a setting, it is an instruction to go around one. It is a button now.

**Windows arrive from the desktop they came from.** The first thing in ReconOS
that moves on its own. Turning four desktops into one moves windows nobody asked
to have moved, and a thing that happens without being explained looks like a
fault — so they slide in from the side they were on. Desktop 3 is to the right
of desktop 1 on the pager, so its windows come in from the right; the direction
is the explanation.

The offset is a **display** offset and never touches the window's position. A
window animating in from off the right edge is, as far as everything else is
concerned, already exactly where it belongs — hit testing, snapping, the taskbar
and the clamp that keeps a title bar reachable all go on working on a position
that never moved. Ease-out cubic in integer arithmetic, a quarter of a second,
and the last step sets zero explicitly so the end of an animation is the same
pixel as no animation at all.

**Desktop labels read on the wallpaper they are on.** Reported from a
screenshot. Three faults stacked: the shadow was drawn at one offset, so seven
of a glyph's eight edges sat on the picture; the shadow roles carry alpha and
were written into a premultiplied buffer unscaled, which is see-through and too
bright; and a ring drawn where it is not needed blends with the label's own
antialiased edges and costs the glyph weight for nothing.

So the ring goes all the way round, premultiplied, and is drawn **only when it
is far enough from the wallpaper to be separating anything**. The wallpaper's
lightness is measured into a 16×9 grid where the picture is decoded — a grid
rather than one number, because a wallpaper is often pale at the top and dark at
the bottom and an average of that is wrong in both halves.

**The measurement agreed with the code and disagreed with the eye, and the eye
was right.** The contrast ratio came back 17:1 for a label that was barely
readable, because it was answering "is there a dark pixel near a light one" —
true throughout. What had changed was the weight of the strokes.

**Four desktops, or one, per account.** Somebody who does not use four desktops
should not have four buttons taking up the corner of their taskbar. Right-click
the bar and choose.

Off is genuinely *one desktop*, not four with the buttons hidden, and that
distinction is the whole design. Hiding the pager while leaving Alt+2 working
would let somebody arrive on a desktop with nothing on screen saying where they
are and no button to come back with — a worse place to be than a taskbar with
four squares on it. Everything that can reach another desktop was already
guarded by one count, so making that count answer the setting turns the buttons,
Alt+1..4 and Alt+Shift+1..4 off together, with no second rule to fall out of
step with the first.

**And everything comes to the desktop you are standing on.** You do not move:
once there is one desktop it is the one you were already on, so nothing jumps
and nothing has to be gone looking for. The setting and the windows move in one
function rather than the registry being written at the call site — after the
switch there is nothing left to reach a stranded window with.

A screenshot caught the bug the numbers could not. Every reading said "1 of 1"
and every one was correct, while the corner still held a single square marked
**1** — the drawing loop is bounded by the count, so answering the setting drew
one button instead of none. A button saying which of your one desktops you are
on is the exact clutter the setting exists to remove.

**A put-away window's button recedes into itself.** The last thing asked for on
the taskbar and not built. `minimized` reached the drawing code, was cast to
void, and then reached exactly one thing six lines later -- the bevel direction.
So a put-away window and a background window differed by one pixel of light and
one of shadow, on a 28-pixel button, at the bottom of the screen.

There are three states, and there are now two signals, so every pair differs by
at least two things:

| | fill | bevel | contents |
|---|---|---|---|
| focused | active | pressed | full |
| open, behind | plain | raised | full |
| minimized | plain | pressed | **washed** |

The contents wash towards the button's **own fill**, which is the original idea
corrected rather than a new one. It used to be the whole button filled with the
bar's colour so a put-away window sank into the bar, and that inverted on the
skin whose bar is deep blue and whose buttons are near-white. Receding into its
own button is right on every skin because the *surface* defines the direction --
and it is the only such rule a skin cannot defeat, because every other way of
saying "less prominent" needs two colours to stay apart and a skin may put them
anywhere.

**Measured on the same button, open and then put away**, so nothing but the
state differs. All eleven skins land between 0.574 and 0.588 against an
arithmetic prediction of 0.569.

**Three instruments were wrong before that number was right**, and the code was
right every time. Mean colour per button compared against two *other* buttons
as a noise floor — a button is mostly fill, and the floor was measuring
different words and icons. The pointer was in the photograph, its tooltip and
cursor sitting on the very button being measured, which made three skins report
a put-away button with *more* ink than an open one. And the taskbar was assumed
four pixels higher than it is, so the window straddled the bevel — which flips
with the state, and swamped what it was meant to measure around.

A fourth found nothing wrong with the code and something wrong with the
measurement: one skin read 0.74 where ten read 0.577, the gradient looked like
the culprit, the fix changed the number by nothing at all, and the fault was
that a graded button's own spread was being counted as contents. A wash cannot
remove that, because washing a colour towards itself does nothing.

**The wash strength's ceiling was measured with this system's own reader.**
Washed past about 140 of 255 the title stops being separable from the button at
all — the engine finds zero marks where it found nine or twelve, on every skin.
It is set to 110.

**A menu on the clock.** Clicking it opened the Control Panel at its root: the
right application at the wrong page, and four more clicks for somebody who only
wanted to stop reading fourteen thirty. It now offers the two things people
want from a clock in a corner — which way it writes the hour, and how to get at
the rest.

Both choices are shown and the one in force is **marked**, rather than one entry
that toggles. "Show am and pm" says what will happen and not what is happening,
so the state would have to be read off the clock itself — which is the thing
somebody was looking at when they could not tell. It writes the setting the
Control Panel writes; two places that can change one thing and two records of
what it is set to is how a preference starts disagreeing with itself.

**The clock goes in the corner, and gains the date.** The clock and the desktop
pager were the wrong way round. That is a mistake rather than a preference: the
corner is where a clock goes, and four numbered squares sitting in the place a
clock goes are four squares somebody has to look at twice to identify.

The clock showed the time and not the date, so the corner answered half of what
people look there for. The full date is four times too wide for a taskbar, so
this writes a short one from the same numbers, on a second line, in a smaller
face. Two lines do not fit a 28-pixel button at the bar's own size, and a clock
is glanced at rather than read -- competing with the window titles beside it
would be wrong even if it fitted.

**Twelve-hour time needed nothing built.** `clock/twenty-four-hour` already
existed, `recon_clock_short` already honoured it, and the Control Panel's Date
and Time page already toggled it; the new date line follows whatever the time
line is doing. Clicking the clock still opens the Control Panel, though at the
root rather than at Date and Time -- that needs the panel to accept which page
to show, and is not done.

**And the pager rounds.** It drew a plain bevel where every other button goes
through `recon_draw_button_edge`, so it stayed square under every skin. Those
four were the last square things on a desktop whose windows and task buttons
have rounded since v0.2.10 -- and being in the corner is exactly where that
gets noticed.

Checked by clicking rather than by reading the diff: pager button 3 selects
desktop 3, button 4 selects desktop 4, and the clock still opens the Control
Panel from its new position. Moving a button and breaking its click is the
whole risk in that change.

**Four wallpapers made for this system rather than drawn by it.** The
four that existed are two colours, a ramp and some stars, generated at first run
-- the right default, and not artwork. Glass is see-through *to* the wallpaper,
which makes the wallpaper matter more than it did before it existed.

*Aurora* is installed at first run alongside the drawn ones, into the same
directory, listed by the same scan, chosen the same way. It also cannot be
deleted, and not because anything new says so: removal already refuses anything
with no recorded origin, and only a wallpaper somebody *added* has one. The
preset rule fell out of the existing design rather than needing a second one.

How it was made is written down in THIRD_PARTY.md, because "made for ReconOS"
and "drawn by hand" are different claims and only the first is true.

**No backdrop blur, and that is the honest gap.** Real Aero blurred what was
behind the glass, which is what let it be far more transparent than this is. A
window's buffer cannot see what is under it, so blurring would mean either
sampling only the wallpaper -- correct over the desktop and a lie over another
window -- or reading back the composited scene every frame. Neither is worth
doing badly, so the opacity is set where it is legible without it.

**A web viewer.** Not a browser, and it is called a viewer everywhere: HTTP
and HTTPS, HTML structure, links, back and forward — and no CSS, no
JavaScript, no images, no forms. That boundary is stated rather than
discovered, because "a viewer for simple pages" and "a browser" are a weekend
and a decade apart.

Structure decides appearance, since there is no stylesheet: a heading is large
because it is a heading. A page whose layout lives entirely in CSS renders as
its underlying structure — readable for a well-written page, a column of text
for a badly-written one. That is the honest failure rather than a hidden one,
and a page that builds itself with JavaScript says so instead of showing blank.

`text/plain` is read as text and not as markup, so an RFC or a README keeps its
own line breaks and its own column alignment. A large fraction of what is worth
reading is a plain file, and running one through an HTML parser eats every `<`
in it.

A redirect may go from http to https and **may not go the other way**. A server
answering an encrypted request with "now ask me again in the clear" is broken
or hostile, and following it would silently undo what the encryption was for.

An image becomes its alt text, which is what alt text is for. A link is
underlined as well as coloured, because a link that is only a different colour
is invisible to a reader who cannot see that difference.

It opens a file from this machine as well as one from the network. `.html` and
`.htm` open in the viewer; `.xml`, `.json`, `.csv`, `.ini` and `.conf` open in
Notepad, which shows them as what they are. XML is deliberately *not* given to
the viewer: it reads HTML's tag vocabulary, so an XML document would come out
as its text with every tag silently dropped — which looks like a viewer that
works rather than one that does not understand the file.

The Type column in File Explorer knows about all of them now. It had been
saying "File" for a JPEG that Photos would open perfectly happily, which is the
column disagreeing with the rest of the system about what it is looking at.

The layout runs while the page is drawn, and the link regions are registered as
the words are placed — so what is clickable is, by construction, exactly what
was drawn. A separate layout pass is a second set of arithmetic that can
disagree with the first, and the disagreement shows up as a link a few pixels
from where it looks.

---

## v0.3.0

**Mail.** IMAP and POP3, both over TLS, both through the encrypted streams
above — so the certificate of whatever answers is checked before a password
goes near it. There is no unencrypted option for either. Those ports exist,
the passwords they carry are readable by anything in between, and a switch for
it is a switch somebody eventually flicks.

Both protocols, because they answer different questions. POP3 collects: it
downloads what is waiting and the mail is yours, on one machine. IMAP reads:
the mail stays on the server and this is a view onto it, so the same account
opened from two machines shows the same thing.

**ReconOS never deletes on the server, on either protocol.** `DELE` is not
sent, and the IMAP fetches use `BODY.PEEK` rather than `BODY`, so opening the
list does not mark forty messages read. A young mail client with a bug that
alters somebody's mailbox is a young mail client nobody uses twice, and there
is no undo on the far end.

**The password is not stored.** It is asked for when a connection is made and
kept in memory until the window closes. That is an inconvenience and it is the
honest position: the registry would mean anything that can read a file can read
somebody's mail password, obfuscating it is worse because it looks like
protection, and doing it properly needs a keyring — a key that exists only
while somebody is signed in — which is a subsystem rather than a field. It is
written down as work to do. The window says so on the screen where the
password is typed, because somebody typing one into a new program is entitled
to know what happens to it.

It does not send. SMTP is a separate protocol with its own ways of losing a
message, and this does not pretend otherwise.

**A clock, bottom right.** It shows the time, and asking it shows the date,
the day, the zone and the region. It keeps twenty-six zones, in minutes rather
than hours, because three-quarters of an hour is a real offset that half the
world's software still cannot represent. It can be set to check an NTP server
and will report what the server said — and will not set the clock from it,
because a machine that quietly moves its own clock is a machine whose logs
cannot be trusted about when anything happened.

The tick is on the minute boundary, not once a second. A clock showing minutes
that redraws sixty times for each one is fifty-nine drawings nobody asked for.

**Photos and a Calendar.** Photos shows one picture at a time, fitted to the
window and never enlarged past its actual size, on a dark mat so a bright
image is not sitting in a bright frame. The Calendar is a month grid with
what is on each day; the entries are a text file, so they can be read and
edited by anything, including a person with a Notepad.

**The Calculator has five modes.** Standard, Scientific, Programmer, Date and
Convert. Programmer works in whole numbers rather than doubles, because a
calculator showing you a bit pattern has to be exact about all sixty-four of
them and a double is exact to fifty-three. Convert covers twelve families with
factors that are exact where an exact value exists — an inch has been exactly
0.0254 metres since 1959, and writing 0.0254001 would be inventing a
disagreement with the rest of the world.

Currency is deliberately absent. Every other family is a ratio fixed by
definition; an exchange rate is a fact about this afternoon, and a calculator
that shipped with one baked in would be confidently wrong about the one
question where being wrong costs money.

**Applets can be updated on their own.** A system application is no longer
welded to the release that shipped it. Install a module registering a name
that is already taken, at a higher version, and it takes the name; the one it
displaced is kept and comes back if the replacement is removed. An update you
cannot back out of is not an update.

The built-in applications carry the system's version as their own, which gives
this a property worth knowing about: when ReconOS is updated past an installed
applet, the built-in wins again and the installed one steps aside. An applet
update applies until the release that catches up with it.

Versions are compared as numbers and not as text, because the obvious
implementation says 1.10 is older than 1.9 — correct for nine releases of
anything, and wrong on the tenth in the direction where an update system
refuses the update it exists to install.

**The terminal is fixed-width, and has colours.** The interpreter has always
written its tables with `%-20s` and the terminal had always thrown that work
away; in a proportional face the columns wandered by a character or two on
every row. It uses a fixed-width font now, so `apps` and `ls` and `firewall`
read as tables.

Failures are drawn in red. The text stays plain and what each line *means* is
carried beside it, because the interpreter's output goes two places — a window
that has colours and a socket that has none — and an escape code in the middle
of the text would be something the socket has to know to strip.

Four schemes: Recon, which follows the system skin and is still the default;
PowerShell, which is that console's blue and near-white exactly; Green Screen;
and Paper. `scheme` lists them and `scheme <name>` chooses, and the choice
survives a restart. They are presets and cannot be removed.

The one colour not copied faithfully is PowerShell's error red, which on that
blue is close to unreadable and is the single most complained-about thing
about the console. Being faithful to a mistake is not a service to the person
reading it.

**The network port is encrypted.** Remote access over TCP 7420 speaks TLS. The
key is offered over the encrypted channel rather than in front of it, and
every warning saying otherwise has come down.

There is no certificate authority and no expiry theatre. A machine that owns
itself has no upstream to ask for an identity, so it asserts its own: a
self-signed certificate made the first time the port opens. What stands in for
an authority is the **fingerprint** — `remote` prints it, and so does Control
Panel → Network — which the client pins on first connect and checks from then
on. That is how SSH does it, for the same reason: a chain answers "did
somebody vouch for this name", and nobody has vouched for this machine.
Pinning answers "is this the same machine as last time".

**Outgoing connections are encrypted too**, and this is the opposite problem
with the opposite answer. Listening, the identity question is "is this the same
machine as last time", and pinning a self-signed certificate answers it
honestly. Connecting out, the question is "is this really imap.example.com" —
which is exactly what a certificate authority is for, and somebody *has*
vouched for that name. So going out verifies: a chain to a trusted root, and
the hostname checked against the certificate.

There is no switch to turn that off. A verify-off switch is a switch that ends
up on, and the failure it causes is silent — an encrypted connection to
whoever answered, which is not the same thing as an encrypted connection to
who you asked for.

When it refuses it says *which* check failed. "Certificate error" leaves
somebody with three very different possibilities and no way to tell them
apart: a wrong clock, a short bundle, or somebody sitting in the middle. The
last of those is the reason the code exists and it gets its own sentence.

The trusted roots are copied into `/System/Config` on first use from wherever
the host keeps its bundle — borrowed once, owned afterwards, the same as the
icons. Nothing at runtime looks at a host path, so the day this boots on its
own kernel the roots are already a file it owns.

Applications get this as an **encrypted stream**, handshaked a step at a time
between turns of the event loop rather than inline — a handshake is several
round trips to somebody else's network, and doing it in one go would freeze
the screen for as long as they took to answer. A stream's `opened` does not
fire until the handshake finishes, so nothing can write a password into a
socket that has proved nothing.

A refused certificate is its own outcome and not "unreachable". They mean
opposite things to the person reading them: "your mail server is unreachable"
sends somebody to check their connection, when the machine is reachable and
what is wrong is a clock, a missing root, or something answering in its place.

The port still ships closed and the firewall still decides whether it may open
at all. Encryption removes one reason it is off by default; it does not make
opening a port to the world a default.

**Screen resolution**, the last row on Display Settings that was not built.
The page lists the sizes a display offers and sets one, and a display that
cannot be changed says so rather than offering a control that could only fail
-- which is every nested and headless backend, where ReconOS is whatever size
the window is.

How it is built matters more than that it is. Nothing above
`include/recon_display.h` knows wlroots exists: the page asks ReconOS what the
screen can do, and ReconOS asks wlroots today and its own kernel later. The
same shape `recon_volume_*` already uses for three directories that will one
day be partitions. The point is that swapping what is underneath is one file
rather than every page that ever asked a question.

Phase 2 has also started. The kernel, in `kernel/`, boots on x86_64 and
aarch64 under BIOS, UEFI and device tree, reports the firmware underneath it,
and manages physical memory. It is built by its own Makefile against no libc
and no wlroots. It runs nothing of the desktop yet, and this file will say so
plainly when it does.

---

## v0.2.17

**An application can be turned off.** Programs → System Apps → Disable. It
stays registered and listed there, marked, and is offered nowhere else: not in
the menus, and not openable by any route. The list of turned-off names lives
in the system registry, so it survives a restart. This is distinct from
removed on purpose -- a built-in cannot be removed, it is compiled in, and "I
do not want this and cannot delete it" is a real thing to want.

**Repair says what it found.** For a program installed from a package, the
receipt names every file the install placed and each is checked. For one that
arrived as a bare `.rex` in /Apps, whether that module is still there and
loaded is the whole question. Neither puts a file back: that needs the package
it came from, and nothing keeps one, which is worth saying rather than having
a button that quietly does nothing.

**Install a Program opens the File Explorer.** It was a path to type. The same
right-click that adds a picture or a font now installs a program, on `.rex`,
`.rts` and `.rpk` -- three ways of saying "take this file into the system",
said one way.

**Storage and Disk Cleanup line up.** Figures are drawn from their right-hand
edge, because sizes are read against each other and that comparison is made on
the digits. A share bar's track is a groove rather than an empty text field,
and a category holding nothing draws no track at all. The volume selector is
the same tab bar Appearance and Network use -- these were buttons, and
choosing which of three spaces you are looking at is choosing a view, not
acting.

**One click highlights, everywhere.** Three pages forced a selection into
existence every frame, so they opened with the first row lit and the buttons
that act on it already armed. Switching lists, switching hives and removing a
row all reset to row zero, which is a real row -- so a Remove button came back
armed against whichever neighbour slid up into the gap.

**Tooltips, on anything with a clickable region.** A control says what it is
when somebody stops on it for half a second. The Control Panel's fourteen
icons carry one -- which is where the line of description under each name
went, because that line had to fit under an icon and so was cut off mid-word.
So do the window buttons, whose middle one says which of its two meanings it
currently has; the taskbar's window buttons, which lose their titles as more
windows open; the pager; and the Apps button.

**Network is four sections**: Status, Adapters, Data Used, Applications. It
was one page holding the machine's name, every interface, the gateway, every
resolver, the last test and two buttons, which fitted only because this
machine has two interfaces.

Adapters is the hardware question: every interface, and for the one picked,
its address, netmask, state and kind. Data Used is what each has carried since
it came up, with a total that leaves loopback out because loopback never left
the machine. Applications is which programs may open a connection, and Allow
and Block -- the sharing decision somebody actually gets to make, given
ReconOS has no stack of its own to share. Status keeps the rest and gains a
Firewall button, because the firewall belongs to the network and reaching it
meant going back to the front page to find a second icon.

**Fonts can be chosen and installed.** Display Settings lists what is
installed, where each came from, and which one is being drawn with. Installing
one is a right-click on any `.ttf`, `.otf` or `.ttc` anywhere -- the way a
picture becomes the background. It installs without switching to it: changing
every letter on the desktop without being asked is a different act from being
asked to keep a file.

**Wallpapers can be added and removed.** A picture anywhere becomes the
background from its right-click menu, and joins the list. The list says where
each one came from, so two folders that both hold a Sunset.png can be told
apart. One somebody added can be removed; one that ships cannot, and neither
can the one currently showing.

**New Skin**, which does not need a skin selected first. That is the whole
point of it: Customize Skin copies the row you are pointing at, and somebody
who wants to make their own had to work out that pointing at somebody else's
was how. It asks which of three starting points -- light, dark or high
contrast -- because a skin has forty-eight roles and every one has to hold a
colour, so there is no blank to start from.

**One click chooses, two acts**, in the lists that were doing both at once.

**Reading is Display Settings**, which is what the page is. Screen resolution
is still the one thing on it that is not built.

**About is System Information**: what the machine is, what ReconOS is, and
what is underneath. Three groups, kept apart on purpose, because the third is
what explains how the first is readable at all. The processor and its core
count are read from the host rather than described in the abstract.

**Programs splits into Installed and System Apps.** They are different things
and the buttons that apply to them are different buttons.

**All Programs is a fly-out**, beside the menu rather than replacing what is
in it. Anything in it can be pinned to the Start menu from its right-click
menu, and anything pinned can be unpinned the same way.

**Disk Cleanup**, as its own Control Panel item: a space to clean, categories
with sizes and counts and a tick box each, View Files, and Clean Up System
Files. Each row says what it costs to tick it, because a size is not a
consequence. Cleaning removes rather than binning -- moving scratch into the
bin would free nothing -- and the question it asks first says so.

**Storage is three spaces**: System, Programs and User, each measured on its
own and each with its own recycle bin. Deleting routes itself -- a file goes to
the bin belonging to the space it came from -- and emptying one names the space
it is emptying, because deleting a document and deleting a system file are not
the same act.

Real partitions still need a kernel. This is the layer above them, and it is
the part that has to be right before anything can be moved onto a partition
later.

**The screen blanks when nothing is happening.** A timeout on the Power page
-- never, 1, 2, 5, 10, 15, 30 minutes or an hour -- and an option to ask for
your password when it wakes. It covers the screen rather than switching the
display off, which would need a kernel, and the page says so.

The key or click that wakes it is spent on waking: somebody coming back to
their desk presses a key to see what is there, and typing that key into a
document they cannot see yet is not what they asked for.

**The firewall can be changed from its page.** Add Rule offers nine presets --
web server, mail, a database, a run of the ports games take, and the two blunt
ones that refuse everything in or out -- because most people adding a rule want
one of those. "Something Else" takes a name, a port or a range, and three
buttons that cycle through direction, protocol and what to do. Remove Rule too.

**A search box in the Start menu.** Typing there has narrowed the list since
v0.2.15 and nothing said so, which meant nobody found out. There is a box in
the footer now with "Search programs" in it when it is empty.

**Icons** for Appearance, Programs, Modules, Network, Firewall and Recovery,
which had been sharing one generic red square. Update is the last one without
its own.

**The Control Panel is icons.** Fourteen of them, each with a line under it
saying what it is for. Clicking one opens it in a window of its own, named for
the item and stepped clear of whatever opened it -- so a wallpaper and a set of
colours can be worked on side by side. They are still the Control Panel: one
application, one entry in the menus, one window per item and no more.

**Appearance is three sections**: Themes, Colours, Wallpapers. Each gets the
whole window, which is why the wallpaper list shows all five now instead of the
two it had room for when the skins were above it.

Choosing a skin no longer puts it on. Clicking down the list to read the
descriptions restyled the desktop nine times on the way; there is a **Use This
Skin** button, and the row says which one is in use.

**Customize Skin**, not Copy This Skin. It asks first, then takes a name and a
line describing it, then opens the editor **as its own window** -- because
changing a colour is something you do while looking at the result, and an
editor covering the thing it is changing was the worst place to put it. The
desktop, the skin list and the colours are all on screen at once, and a colour
changes under all three.

Four faults underneath, all of which had to be fixed for any of it to work:
the shell held eight windows and quietly dropped the ninth; every window of an
application shared one remembered position, so they opened on top of each
other; the title bar drew the application's name while the taskbar drew the
window's; and a click that opened a window left the keyboard talking to the
window that was clicked. BG-065 to BG-068.

**Services.** The parts of ReconOS that run are a list now, in Watchtower
beside Applications and Processes: the desktop shell, the control socket,
remote access, the firewall and networking. Each one says whether it is
running, stopped, or failed and with which error code, and how many times it
has been started this run -- one means a normal system, more than one means
somebody has been repairing something. Start, Stop and Restart. `services` in
the Terminal is the same list and the same registry.

Multitasking used to be a Control Panel page describing behaviour nobody could
change. It is a service now, which is what it always was.

**The desktop shell can be restarted.** It rebuilds the taskbar, the desktop,
the menus and the window management, and leaves your application windows open
-- so a taskbar can be repaired without costing you the document you were
writing. Whoever is signed in stays signed in: restarting the shell is a
repair, not a sign-out.

Two faults found by doing that, both of which had been waiting:

The UI font belonged to the shell and was freed with it, while every surviving
window still held the pointer. The first frame after a restart was a crash
inside the glyph rasteriser. The font belongs to the system now, loaded once
per size for the whole run.

Registering a built-in application twice was refused as a name collision, so a
restarted desktop had no Notepad and no File Explorer while the old windows
were still on screen. A built-in re-registering itself is an update in place;
a module trying to take a built-in's name is still refused.

**Bugs have numbers.** Every fault found in ReconOS -- sixty-two of them so
far -- is written down in `docs/BUGS.md` with an ID (`BG-001` upward), what it
actually was, how it surfaced, who found it, and what was done about it. The
ones found before today were numbered retroactively from the commit history.
They are GitHub issues too, so the record is public and dated.

---

## v0.2.16

**Errors have codes now.** When something goes wrong ReconOS names it —
`VT-A001`, `VT-G005` — so it can be written down and looked up. `errors` in
the Terminal says what a code means, `errors log` is what has happened on this
machine, and `docs/ERRORS.md` is the whole list for when you have a code and
not a working machine.

Three kinds: **STOP** (the system cannot continue and shows a screen with the
code on it), **fault** (something failed and the rest carried on), and **note**
(written down, nothing broke). If a run stops, the next start says so once,
with the code.

**A firewall.** Control Panel → Firewall, and `firewall` in the Terminal. It
decides what ReconOS itself opens and accepts: outgoing allowed, incoming
blocked, and a list of rules where the first match decides. Nine rules ship —
the incoming ones written down and switched off, so opening one is a switch
rather than remembering a port number. It is not the host's firewall and does
not touch it.

**The startup screen checks the system** instead of only counting it. Every
folder the system needs, the skins, the accounts, the programs, the firewall —
each line says what it found, and says the code when what it found is wrong.
Missing folders are rebuilt rather than only reported.

**Remote access, two ways.** Over SSH by forwarding the control socket, which
is encrypted and needs nothing from ReconOS; or over TCP 7420 with a key, which
is off by default and says plainly that the key crosses the network in the
clear. The firewall has to allow it either way.

## v0.2.15

**Help.** A new application, beside the Control Panel in the Start menu, with
a page for each part of the system and this change log underneath. The text is
written out of the system's own files every time it starts, so help describing
a version that is no longer running cannot survive an update.

**What changed, after an update.** The first time an account reaches the
desktop on a new version, a window says what that version brought, with an OK
button. Each account is told once. The whole log is in Help at any time,
back to the first version.

**The Control Panel and the Start menu have their own icons**, instead of
sharing the plain window that stood in for everything without one.

**Storage says where the room went.** The page used to be four notes about
things that do not work. It now measures what ReconOS owns — the system, the
programs, each account by name, the scratch space and the Recycle Bin — and
can empty the bin. How much room is left is still the host's answer, because
there is one filesystem and no volume layer under it.

**The Recycle Bin from the Terminal.** `bin` lists it, `bin <name>` puts
something in, `bin restore <name>` takes it back out, and `bin empty` clears
it. Before this the bin could only be reached from the File Explorer.

**Text that is not plain English draws properly.** Accents, dashes,
quotation marks and other alphabets used to be dropped silently — a sentence
would arrive with a hole where its punctuation should be. They are read as
UTF-8 now, in file names as well as in the help. A character the font has no
drawing for shows as an empty box rather than as nothing. A keyboard laid out
for a language with accents in it can type them, and Backspace removes a whole
character rather than a piece of one.

**The Update page says what version this is and what it brought**, with a
way through to the whole change log. It used to be three notes about things
that do not work; one of them stopped being true when the change log arrived.

**Notepad can replace.** Ctrl+H opens the find bar with a second field:
*Replace* changes the match in front of you and moves to the next, *All*
changes every one. Both undo.

**Programs written for Wayland get a ReconOS title bar.** A program that
was not written for ReconOS used to arrive with a frame of its own, in
somebody else's colours, with its own buttons. Now it is framed the way every
other window is — same colours, same buttons, same corners — and can be
dragged, resized, minimized, maximized and closed like one. A program that
insists on drawing its own frame still may; it is not given a second one.

**F1 opens the help about whatever you are looking at.** From Notepad it opens
Writing; from a Control Panel page it opens the page about that page.

**Three quiet bugs, found by clearing the compiler's warnings.** The File
Explorer's folder drop-down could point at the wrong place for a deeply nested
folder; a wallpaper or account picture with a very long file name was listed
but could not be loaded; and a file with a very long name could be moved to
the wrong place on its way to the bin. The build now compiles with nothing to
say, so the next warning will be one somebody notices.

**Skins can be written here.** *Copy This Skin* on the Appearance page writes
the chosen one out under a name of your own; *Edit This Skin* lists every
colour with a swatch and lets you change it. The change is immediate and is
saved as you make it. The ten that ship are built in and cannot be edited —
copy one first. `theme copy <name>` does the same from the Terminal.

**Type in the Start menu to find something.** The list narrows to names
containing what you typed; the arrows move the highlight and Enter opens it.
Escape steps back one thing at a time. The menu took no keys at all before
this, not even Escape.

**Every button is the same shape.** The rounded corners a skin asks for were
written in three places and every button that had not been rewritten stayed
square. On Beacon that meant two shapes of button in one window.

## v0.2.14

**Programs arrive as packages.** A `.rpk` is a folder with a manifest in it,
so a program can bring an icon and other files rather than being one file of
code. Installing writes a receipt naming everything it placed, and removing
takes back exactly that. The receipt is written before the program is loaded,
so one that refuses to load can be rolled back instead of leaving files
nothing knows about.

**Find in Notepad.** Ctrl+F opens a bar above the status line. It ignores
case and wraps around the end of the document.

## v0.2.13

**Removing an account can keep or delete its files.** They are two decisions,
and only one of them can be undone.

**The Start menu's power buttons are icons**, in the bottom right. The name of
whichever one you point at appears on the left.

**Buttons have rounded corners** where the skin asks for them, and the startup
screen is slower, because it went past faster than it could be read.

## v0.2.12

**A text clipboard.** Cut, copy and paste work in Notepad, in every text field
in the system, and into the Terminal. Before this, text could not be moved
from one place to another at all.

**Notepad can select text** — with the mouse, or Shift and the arrow keys.

## v0.2.11

**Notepad can undo**, by word rather than by keystroke, and has an Edit menu
so the shortcuts are findable.

## v0.2.10

**Skins can change the shape of a window**, not only its colours: the height
of a title bar, the thickness of its border, how far its corners are rounded,
and how big its buttons are.

## v0.2.9

**Desktop icons can be dragged**, and stay where they are put.

**Files open when you click them.** A text file opens in Notepad. Before this,
nothing in the system opened a file by being clicked.

## v0.2.8

**Properties** tells you what something is: its name, kind, size, where it
lives and when it last changed. It had been in the menu and greyed out since
the menus existed.

## v0.2.7

**Four desktops.** Alt+1 to Alt+4 switches between them; the numbers on the
right of the taskbar do the same. Alt+Shift and a number takes the current
window with you.

## v0.2.6

**Windows snap to the edges.** Drag one to the left or right edge for that
half of the screen, or to the top to fill it.

**Alt+Tab works again.** It had quietly stopped working when ReconOS began
drawing its own windows.

## v0.2.5

**The registry can be changed from the Control Panel**, not only read. Saving
redraws the system, so a setting takes effect while you watch.

## v0.2.4

**Skins can be installed.** `theme install` takes a skin file and adds it to
the list; `theme remove` takes it away.

## v0.2.3

**Gradients.** A skin can ask for a surface to fade from one colour to
another, which is what the Beacon skin needed to look like the era it is
reaching for.

**A startup screen**, with the Recon Towers mark and a count of what the
system found as it came up.

## v0.2.2

**All Programs** at the foot of the Start menu lists everything installed. The
column above it is what this account opens most.

## v0.2.1

**Connections that carry data**, with a rule about which programs may open
one. **Screen capture** on Print Screen. **Installing and removing programs.**
**Wallpapers the system draws for itself.**

## v0.2.0

**The network, seen but not implemented.** ReconOS has no kernel, so it has no
network stack of its own; it reports the host's, and says so on screen.

## v0.1.3

**Choose an account, then sign in** — two screens rather than one, so nothing
about how an account signs in is shown before one is chosen.

**Updates announce themselves** on the first start after one.

## v0.1.2

**Setup that looks like it belongs to something**, with the Recon Towers mark
and a picture of each skin. **Account pictures.**

## v0.1.1

**One account at a time, properly.** Each account gets its own settings, its
own folders and its own windows.

**A Control Panel** with the shape of the whole system in it, including the
parts that are not built, each saying what has to exist first.

## v0.1.0

The first version that was a usable desktop: it sets itself up on first run,
asks who you are on every run after, and gives that person a desktop of their
own. A taskbar, a Start menu, windows, a file explorer, a terminal, a task
manager, Notepad and a calculator.
