# ReconOS help

What the Help application shows. One topic per `##` heading, in the order they
appear here.

This file is the source. `scripts/make-help.sh` turns it into the pages Help
reads, so a change is written once. Anything added to the system that a person
has to be told about belongs here, in the same commit that adds it.

---

## Getting around

The **Apps** button at the bottom left opens the Start menu. The left column
is the programs you open most; **All Programs** at the foot of it lists
everything installed. The right column is your folders, the Control Panel, and
this help.

**Type with the menu open** to narrow the list to names containing what you
typed. The arrow keys move the highlight and **Enter** opens it — three
letters and Enter is usually enough. **Escape** steps back: first what you
typed, then the long list, then the menu itself.

Along the bottom of the menu are five buttons: lock the screen, sign out,
switch user, restart, and shut down. Point at one and its name appears on the
left.

The bar along the bottom of the screen lists every window that is open on this
desktop. Click one to bring it forward; click the one in front to put it away.

**Alt+Tab** steps through the open windows without touching the mouse.

## Windows

Drag a window by its title bar. Drag any edge or corner to resize it.

Drag a window until the pointer touches the **left or right edge** of the
screen and let go, and it fills that half. Drag it to the **top** and it fills
the screen. The middle button in the title bar puts it back where it was.

The three buttons at the right of a title bar are minimize, maximize and
close.

A program written for Wayland rather than for ReconOS gets the same title bar
as everything else, so it drags, resizes, minimizes, maximizes and closes the
same way. Its edges are grabbed from just **outside** the window, where the
cursor changes to show which way it will move.

A program that insists on drawing its own frame is left alone rather than
given a second one.

## Desktops

Four of them, or one. The numbers at the right of the taskbar say which you are
on, and a dot under a number means that desktop has windows on it.

**Alt+1** to **Alt+4** switches. Hold **Shift** as well and the window you are
using comes with you.

A window on another desktop is not on this taskbar. That is deliberate: it is
how four desktops stay four separate places rather than one crowded one.

**Right click the taskbar** to turn the four down to one, or back. It is
remembered for your account, so it is your choice and not the machine's — and
somebody who does not want four desktops should not have four numbers taking up
room on their taskbar.

Turning them off does not lose anything. Every window on the other three comes
to the one that is left, and slides in from where it was so you can see what
arrived rather than finding windows you did not put there.

## Files

The **File Explorer** shows your folders. Type a path into the bar at the top,
or use the drop-down at its right for the places you go often.

Click something once to select it, again to open it. A folder opens; a text
file opens in Notepad. Something ReconOS has nothing to open with says so.

**Right-click** anything for what can be done to it, including **Properties**,
which says what it is, how big it is and when it last changed.

Deleting puts things in the **Recycle Bin** rather than destroying them, and
each item remembers where it came from, so restoring puts it back. **Shift and
Delete** skips the bin, after asking.

From the Terminal the bin is the **bin** command: `bin` lists it, `bin <name>`
puts something in, `bin restore <name>` takes it back out, and `bin empty`
clears it. Note that **del** is not the same thing — it removes a file rather
than binning it, and there is no undoing that.

The Control Panel's **Storage** page says how much room the bin is holding,
and can empty it.

## The desktop

Icons on the desktop are the contents of your Desktop folder, so anything that
writes a file there puts it on the desktop.

**Drag an icon** and it stays where you put it. Where you put things is
remembered per account, as a position on a grid, so it survives the screen
changing size.

**Right-click the desktop** to make a new folder, a new file, or a shortcut to
a program.

## Writing

**Notepad** opens, edits and saves text.

- **Ctrl+Z** undoes, a word at a time. **Ctrl+Y** redoes.
- **Ctrl+C**, **Ctrl+X** and **Ctrl+V** copy, cut and paste. They work between
  Notepad, every text box in the system, and the Terminal.
- **Ctrl+A** selects everything. Hold **Shift** with the arrow keys to select
  by hand.
- **Ctrl+F** finds text. It ignores capitals and carries on from the top when
  it reaches the end. **F3** finds the next one.
- **Ctrl+H** is the same bar with a second field. **Replace** changes the
  match in front of you and moves to the next; **All** changes every one from
  the top of the document. Tab moves between the two fields, and either can
  be undone.

## Accounts

The **Control Panel** has an Accounts page. An administrator can add an
account, set or clear its password, give it a picture, and make it an
administrator or a limited account.

Removing an account asks what to do about its folder: keeping the files and
deleting them are separate decisions, and only one of them can be undone.

A **limited** account cannot change anything in `/System` and cannot read
another account's files.

**Lock** covers the screen without ending your session — your windows stay
open and only you can unlock it. **Sign Out** ends the session.

## How it looks

**Control Panel → Appearance** has three sections: **Themes**, **Colours** and
**Wallpapers**. Choosing a skin in the list picks it out and shows what it is;
**Use This Skin** puts it on. It is yours alone -- other accounts keep theirs,
and the one in use says so beside its name.

Ten ship. Four of them are for how people actually see: **Deuteran**,
**Protan** and **Tritan** for the three kinds of colour vision deficiency, and
**Contrast** for low vision. **Reading** softens the contrast and warms the
background.

**Control Panel → Reading** changes the spacing between letters and lines, the
typeface and its size. Extra space between letters is the single adjustment
with the most evidence behind it for a dyslexic reader.

### Making a skin of your own

A skin is a text file in `/System/Themes`. `theme install` adds one somebody
else wrote.

**Customize Skin**, under the list, makes your own copy of the chosen skin. It
asks first, then for a name and a line describing it, and then opens the **Skin
Editor** in a window of its own -- beside the list rather than over it, because
changing a colour is something you do while looking at the result.

A copy rather than a blank file: a skin answers forty-eight questions about
colour, and having them all answered already is a much easier place to start
than an empty file. The skin that ships cannot be changed, only copied; a copy
is yours and can be edited or deleted.

The editor lists every colour with a swatch of what it currently is. Pick one,
press **Change Colour**, and type a colour as RRGGBB — or AARRGGBB where it
needs to be see-through. The change is immediate and is written to the file
straight away; there is no separate save.

The **Colours** section shows the colours of whichever skin is on. If that skin
is one that ships, changing a colour offers to make your copy first.

A skin can also change the shape of a window, by writing lines like these into
its file:

- `metric.title-height` and `metric.border` — how tall the bar is, and how
  thick the frame around the contents.
- `metric.corner` and `metric.button-corner` — how far the window's top corners
  and the buttons are rounded. Zero is square.
- `metric.button-size` — how big the three buttons are.
- `metric.buttons-left` — `1` puts the buttons on the left of the bar instead
  of the right. Close stays in the corner either way.
- `metric.buttons` — which buttons there are, as a sum: 1 close, 2 maximize,
  4 minimize. `7` is all three and is the usual. `5` is close and minimize.

**Close is always there.** A skin that leaves it out gets it back, because a
window nobody can close is not something a skin is allowed to make.

A skin can also replace what is drawn *inside* the buttons. Put a picture named
`window-close`, `window-maximize`, `window-restore` or `window-minimize` in
`/System/Icons` and it is used instead of the shape. Supply one and the others
keep theirs.

Some colours are a *ramp* from one to another, shown as two halves of the
swatch and a second value. **Remove Ramp** makes one flat.

The ten skins that ship cannot be edited — they are built into ReconOS, so a
change to their files would be ignored. Copy one first; the copy is yours.

From the Terminal, `theme copy <name>` does the same thing.

## Programs

**Control Panel → Programs** installs and removes programs, and **Modules**
shows what is loaded and why anything refused to load.

A program arrives as a **package**: a folder ending in `.rpk` with a
`package.txt` inside saying what it is and what to place where. Installing
writes down everything it put in place, so removing it takes back exactly
that and nothing else.

A package does not have to contain a program. One that only places files -- a
set of wallpapers, some skins, a few fonts -- is a package too. It may write
into the folders that hold that kind of thing and nowhere else, so a package
cannot put a file among the system's settings or its loaded code.

A file that is already there is left alone, and a package that finds one does
not claim it: removing that package will not take away something that was
already yours. The same is true of settings a package brings -- they are
defaults, and one you had already chosen is left as you chose it.

Installing a program is an administrator's decision. A program runs inside
ReconOS with everything ReconOS can do, which is closer to installing a driver
than to saving a file.

## The Terminal

The Terminal runs ReconOS commands. It is not a Unix shell: nothing here
reaches the machine underneath, and nothing outside ReconOS can be run from
it.

**help** lists every command. Some worth knowing:

- **dir**, **cd**, **type** — look around and read a file
- **copy**, **move**, **rename**, **del** — change things
- **theme**, **wallpaper** — how it looks
- **net** — what the network is doing
- **capture** — a picture of the screen
- **reg** — the settings the system remembers
- **bin** — the Recycle Bin: list it, fill it, empty it

## Pictures

**Photos** opens PNG, JPEG, BMP, GIF and TGA. Double-clicking a picture
anywhere opens it here.

It shows one picture at a time, fitted to the window. **Half** and **Double**
change the size, and **Save as PNG** writes the result out under a new name --
it refuses to overwrite an existing file rather than asking, so nothing is ever
lost to a mistyped name. A `*` in the title means there are changes that have
not been saved.

What is not on offer is making a small picture sharp. Enlarging shows the
pixels that are there, larger. Inventing detail that was never in the picture
needs a trained model, and a button promising a sharper photograph while
delivering a confident invention is not a feature this wants.

## Sound and video

**Media Player** plays what ReconOS can decode: WAV and MP3 for sound, and MP4
for video. Double-clicking a file that can be played opens it here.

A file it cannot decode is refused with a sentence saying so, rather than
opening an empty window. Which formats can be played changes when a module
brings a decoder -- **Control Panel → Modules** lists what is loaded.

Sound goes out through the machine underneath. On a machine with no sound card
everything still works and says there is nothing to play through.

## The web viewer

**Web** fetches a page over HTTP or HTTPS and shows the text in it. It reads
HTML's structure -- headings, paragraphs, lists, links -- and follows a link
when you click one. An `.html` file on this machine opens here too.

It is a reader, not a browser. No JavaScript, no images, no styling, no forms.
A page that needs any of those shows what it says without them, which is
sometimes everything and sometimes very little.

Every address is checked before it is fetched, and **Control Panel → Firewall**
decides whether it may go out at all.

## Choosing what opens a file

Right-click a file, on the desktop or in File Explorer, and the menu offers
**Open with** for every program that reads files. Choosing one opens the file
*and* makes that program the one for every file of that kind -- so this is the
way to say "always open these in that", not the way to open one thing once.
**Use the usual program**, on the same menu, undoes it.

**Control Panel -> Programs -> File Types** lists every choice you have made
and undoes any of them, which is where to look when something opens in a
program you do not remember choosing.

**If the program does not say it opens that kind of file, you are asked
first.** It is a warning and not a refusal: opening a picture in Notepad to
look at what is inside it is a perfectly good reason, and so is sending a `.log`
somewhere that is not the usual program. The dialog says which program, which
file, and that the choice applies to every file of that kind -- because the
surprise is not the file that opens as nonsense, it is the next one.

## Passwords

Some programs offer to keep a password for you. Mail does, on the screen where
it asks for one: tick **Remember this password** and it is kept once the
connection has worked.

Kept is not the same as written down. It is encrypted with a key made from
your account password, and that key exists only while you are signed in --
it is never written anywhere. Signing out, locking the screen, or shutting
down makes every kept password unreadable until you sign in again.

**An account with no password has no keyring.** There would be nothing to make
a key from, so nothing offers to keep anything, and the tick box is not shown
rather than shown and ignored.

**Control Panel -> Passwords** lists what is being kept, by name and by which
program asked. It does not show the passwords themselves and there is no way
to make it: nothing in ReconOS will print one back to you. Choose one and
press **Forget** and it is gone; whatever kept it will ask again next time.

From the Terminal, `keyring` lists them, `keyring forget <name>` removes one,
and `keyring keep <name> <secret>` adds one. There is no command that prints a
secret, for the same reason.

**What this protects against, and what it does not.** It protects a disk or a
backup that somebody else has: without your password, what is stored is
unreadable. It does not protect against somebody using your account while you
are signed in -- at that point the key is in memory and the programs running
are yours. Lock the screen when you walk away; that puts the key back.

## Mail

**Mail** reads over IMAP or POP3 and sends over SMTP. Both halves of an account
are on one form, because they are one account to the person filling it in; the
sending half may be left blank if you only want to read.

**The connection is always encrypted and the far end is always checked.** There
is no unencrypted option and no setting that produces one. On port 587 the
upgrade to encryption is required to succeed: a server that does not offer it,
or something in the middle that removes the offer, ends the session rather than
carrying on in the open.

Writing a letter takes **To**, **Cc** and **Bcc**, each a list separated by
commas. Everyone named in all three receives it; only To and Cc appear in the
letter itself, which is what a blind copy means. **Attach a file** adds up to
eight files, twelve megabytes together. Files are read when you press Send, so
what goes is the file as it is then.

**The password is not saved.** It is used for the connection and forgotten when
the window closes. Somewhere safe to keep it needs a key that exists only while
somebody is signed in, and that does not exist yet.

## What is running

**Watchtower** lists the windows that are open and the memory the system is
using, and can close a window that has stopped answering. **Ctrl+Alt+Del**
opens it.

It is not a process list. ReconOS asks the machine underneath to create every
process it runs, so what a process is belongs to that machine and not to this
one -- and a list that showed some of them and called itself complete would be
worse than one that does not pretend.

## The Calculator

Six modes, chosen along the top.

- **Standard** is arithmetic.
- **Scientific** adds trigonometry, logarithms, powers and factorials.
- **Programmer** works in hexadecimal, decimal, octal and binary at once, with
  the bitwise operations, in whole numbers rather than decimals -- a number
  shown as a bit pattern has to be exact about every one of those bits.
- **Date** is how far apart two dates are.
- **Convert** changes units.
- **Graph** draws up to three expressions in *x* at once, each in its own
  colour with a swatch beside its field. Drag the picture to move about in it
  and use the wheel to zoom in on whatever is under the pointer. Zoom in, Zoom
  out and Reset are also buttons, and Reset puts back the place as well as the
  size. **Save** writes the picture into your Pictures folder, named for the
  moment you took it. What is typed here is remembered, so closing the window
  does not lose it.

Every key has a keyboard equivalent, including the number pad.

## Dates

**Calendar** shows a month at a time, with today marked. Clicking the clock on
the taskbar opens the same view without opening a window.

**Control Panel → Date and Time** sets the time zone, chooses between a
twelve- and twenty-four-hour clock, and says whether daylight saving is in
force. Checking against a time server is a separate switch, off until it is
turned on, because reaching out to a machine on the internet is not something
to start doing without being asked.

## What opens a file

Double-clicking a file opens it in whichever program handles that kind. Right
click one and **Open with** lists the programs that can, with a mark beside the
one that would. Choosing a different one opens the file **and remembers it**,
so every file of that kind opens there from now on.

**Use the usual program** appears once you have chosen something, and puts it
back. It only ever removes your choice -- the program a file type came with
cannot be deleted, only overridden.

**Control Panel -> Programs -> File Types** lists every kind of file you have
chosen a program for, in one place, and undoes any of them. Everything not
listed there opens in whatever handles it.

## Pictures of the screen

**Print Screen** saves a picture of the whole screen into your Pictures
folder. So does the **capture** command, which can also be given a path.

## Help and updates

**F1** opens this help at the page about whatever you are looking at. From
Notepad it opens Writing; from a Control Panel page it opens the one about
that page. With nothing in front it opens at the beginning.

**Help** is in the Start menu, under the Control Panel. The list down the left
is these pages; under the rule at the bottom of it is the change log, one
entry for every version back to the first.

The text is written out of the system's own files each time it starts, so it
always describes the version you are running. Editing a page under
`/System/Help` changes what you see until the next update puts the shipped
one back.

The first time you reach the desktop after an update, a window says what that
version brought. Pressing **OK** records that you have read it, and it does
not come back for that version. Each account is told separately, so somebody
who has not signed in for a while still hears about the version they arrive
on. **All changes** opens Help at that version's entry, with every earlier
one under it.

## Text and letters

Text is read as UTF-8, so accents, dashes, quotation marks and other alphabets
draw the way they are written — in file names as well as in this help.

A character the font has no drawing for appears as an empty box. That is the
font saying so rather than the character being lost: the file still has it,
and it will keep it.

A keyboard laid out for a language with accents in it types them, and the
arrow keys and Backspace step over a whole character rather than a byte of
one.

## The firewall

**Control Panel → Firewall** decides what ReconOS may open and what may be
opened to it. It is not the firewall of the machine underneath and does not
touch that one.

Outgoing connections are allowed by default and incoming ones are blocked,
which is right almost always. Under that is a numbered list of rules, and
**the first rule that matches decides** — so the order is part of the rule,
and moving one changes what it does.

Nine rules ship. The incoming ones are written down and switched **off**: the
useful state for a rule about remote access is there, correct, and not in
force until you want remote access. Turning one on is a switch rather than
having to remember a port number.

A rule that is off is drawn dim. That is deliberate: written down and not in
force is the most important thing about such a rule.

Adding and removing rules is `firewall` in the Terminal for now. The rules are
a plain text file in `/System/Config`, so they can be read without ReconOS.

## Reaching this machine from elsewhere

Two ways, and they are not equally safe.

**Over SSH.** ReconOS listens on a socket that cannot leave the machine, and
SSH can carry it:

```
ssh -L /tmp/recon-there.sock:/tmp/reconos.sock user@this-machine
```

Then `nc -U /tmp/recon-there.sock` from the other end. SSH does the encryption
and decides who you are, both of which it is much better at than ReconOS.

**Over the network.** `remote key` makes a key and shows it once. `remote on`
opens TCP 7420, and every connection is asked for that key before it can run
anything. The firewall rule has to be on as well — turning remote access on is
not enough, and the refusal says which rule to turn on.

**The connection is encrypted.** ReconOS makes itself a certificate the first
time the port opens, and the key is offered over that rather than in front of
it.

Nothing vouches for that certificate — there is no authority to ask, and a
machine that owns itself has none to ask. What stands in for one is the
**fingerprint**: `remote` prints it, and so does Control Panel → Network. Check
it matches what your client shows the first time you connect from somewhere,
and the client should refuse from then on if it ever changes. That check is
the whole protection, and it only works if somebody does it once.

This is how SSH does it, for the same reason.

`remote` on its own says which way in is open, and what the fingerprint is.

## When something goes wrong

ReconOS names its faults so you can look one up. A code reads like this:

```
VT-A001
```

`VT` is the system. The letter says which part of it — A is startup, B is
storage, C is accounts, and so on. The number says which fault.

**`errors VT-A001`** in the Terminal says what a code means. **`errors`** on
its own lists every code there is, and **`errors log`** is what has actually
happened on this machine.

There are three kinds:

- **STOP** — the system cannot continue. It shows a screen with the code on it
  and writes the code and the time into `/System/Logs`. The next start says
  what happened.
- **fault** — something failed and the rest carried on. Reported where it
  happened.
- **note** — written down, and nothing broke.

If the last run stopped, the first screen after the startup screen says so
once, with the code. The machine in front of you is working; the message is
about the run before it.

The full list is also in `docs/ERRORS.md` in the repository, for when you have
a code and not a working machine.

## What is not built yet

Several Control Panel pages are there and say plainly that they are not
built: Power, Update, Troubleshoot and Recovery, and part of Storage. Nearly
all of it needs a kernel of ReconOS's own, which is the next phase of the
project — a program running on Linux cannot suspend a machine, partition a
disk, or reinstall itself.

They are shown rather than hidden on purpose. A gap nobody can see is a gap
nobody remembers.
