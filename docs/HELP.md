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

**Control Panel → Appearance** lists the skins. Choosing one puts it on
immediately, for your account alone; other accounts keep theirs.

Ten ship. Four of them are for how people actually see: **Deuteran**,
**Protan** and **Tritan** for the three kinds of colour vision deficiency, and
**Contrast** for low vision. **Reading** softens the contrast and warms the
background.

**Control Panel → Reading** changes the spacing between letters and lines, the
typeface and its size. Extra space between letters is the single adjustment
with the most evidence behind it for a dyslexic reader.

A skin is a text file in `/System/Themes`. `theme install` adds one somebody
else wrote.

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

## This help, and what changed

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

## What is not built yet

Several Control Panel pages are there and say plainly that they are not
built: Power, Update, Troubleshoot and Recovery, and part of Storage. Nearly
all of it needs a kernel of ReconOS's own, which is the next phase of the
project — a program running on Linux cannot suspend a machine, partition a
disk, or reinstall itself.

They are shown rather than hidden on purpose. A gap nobody can see is a gap
nobody remembers.
