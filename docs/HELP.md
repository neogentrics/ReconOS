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

There are four. The numbers at the right of the taskbar say which you are on,
and a dot under a number means that desktop has windows on it.

**Alt+1** to **Alt+4** switches. Hold **Shift** as well and the window you are
using comes with you.

A window on another desktop is not on this taskbar. That is deliberate: it is
how four desktops stay four separate places rather than one crowded one.

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
