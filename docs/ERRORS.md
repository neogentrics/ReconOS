# ReconOS error codes

Every code ReconOS can report, what it means, and how bad it is.

**This file is generated.** The codes live in `include/recon_errors.def`, which
is what the system compiles and what the `errors` command reads. Run
`scripts/make-errors.sh` after adding one; editing this file by hand edits a
copy.

## How a code is put together

```
VT-A001
^^ ^ ^^^
|  | +--- which fault, 001 upward within that area
|  +----- which area of the system
+-------- Void Tower
```

**The letter is the area, not the severity.** The same area produces faults of
every kind — the filesystem can fail to read a file and fail to open at all —
so severity in the letter would scatter one subsystem across the alphabet.
Worse, a code would have to change if a fault were ever reclassified, and a
code that changes is a code nobody can look up. Severity is a property of the
entry instead, listed below.

**I and O are not area letters.** This is a code somebody reads off a screen
and types into a search box, and in that setting `I` is `1` and `O` is `0`.

**A hundred codes per letter.** When an area fills, it continues at a second
letter reserved for it rather than renumbering: the old codes are already
written down somewhere. Twenty-four letters at a hundred each is two thousand
four hundred.

**A number is never reused**, even after the fault it named stops existing.

## What the severities mean

| | |
| --- | --- |
| **STOP** | The system cannot continue. The screen that says so shows the code, and the code and time are written to `/System/Logs`. |
| **fault** | Something failed and the system carried on without it. Reported where it happened. |
| **note** | Recorded, and nothing broke. There because a run of them is worth seeing even though one is not a problem. |

## Looking one up

On the machine that showed it:

```
errors VT-A001
```

`errors` on its own lists them all, and `errors log` is what has actually
happened on that machine.


## A — Startup and shutdown

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-A001** | STOP | **No usable font** — ReconOS draws everything itself and found no font to draw with. It looks for DejaVu Sans, Liberation Sans and Ubuntu in the usual places. Installing any one of them is enough. |
| **VT-A002** | STOP | **The filesystem could not be opened** — ReconOS keeps its files under one folder on the host and could not create or reach it. Check that the folder RECONOS_ROOT names exists and can be written to. |
| **VT-A003** | STOP | **No display to draw on** — The compositor started but found no output. On a real machine this means the graphics driver did not come up; in a window it means the host compositor refused the connection. |
| **VT-A004** | STOP | **The Wayland socket could not be created** — Nothing can connect to ReconOS without it, including its own windows. Usually XDG_RUNTIME_DIR is missing, unwritable, or already holds a socket from a copy that did not shut down. |
| **VT-A005** | fault | **The last run ended unexpectedly** — ReconOS did not shut down cleanly the previous time. Nothing is lost that was saved; anything open at the time was not. |
| **VT-A006** | fault | **A startup check found a problem** — One of the checks run before the login screen did not pass. The check that failed reports its own code alongside this one. |
| **VT-A007** | fault | **A system folder was missing and was rebuilt** — One of the folders ReconOS relies on was not there and has been recreated empty. Anything that was in it is gone. |

## B — Storage and the filesystem

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-B001** | fault | **A file could not be read** — The file is there but could not be opened, or ended sooner than its own length said it would. |
| **VT-B002** | fault | **A file could not be written** — The write was refused. The usual causes are a full disk, a read-only filesystem underneath, or a folder that has been removed since it was opened. |
| **VT-B003** | fault | **A path led outside the filesystem** — Something asked for a path that resolves above the ReconOS root. The request was refused; this is the boundary working, not failing. |
| **VT-B004** | fault | **A name was too long** — Names and paths have limits, and joining these two exceeded one. The operation was refused rather than performed on a shortened name, which would have been a different file. |
| **VT-B005** | fault | **The Recycle Bin could not be reached** — Deleting puts things in the bin, and the bin could not be opened or written to. The file was left where it was. |

## C — Accounts and signing in

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-C001** | STOP | **The account list could not be read** — ReconOS cannot tell who may use this machine, so it will not let anybody in. The list lives in /System/Config; if it is damaged, the machine has to be set up again. |
| **VT-C002** | fault | **An account's folder is missing** — The account exists but the folder holding its files does not. It has been recreated empty; what was in it is not recoverable from here. |
| **VT-C003** | note | **A sign-in was refused** — The password did not match. Recorded because a run of these is worth seeing, not because one is a problem. |
| **VT-C004** | fault | **An account could not be changed** — Adding, removing or altering an account did not take effect. The account list is unchanged. |

## D — Display, windows and drawing

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-D001** | STOP | **The renderer could not be created** — ReconOS could not get a renderer from the graphics stack. On a virtual machine, WLR_RENDERER_ALLOW_SOFTWARE=1 is usually what is missing. |
| **VT-D002** | fault | **A window could not be drawn** — There was not enough memory for the window's pixels, or the buffer was refused. The window is not shown; the rest of the system is unaffected. |
| **VT-D003** | fault | **The screen could not be captured** — Print Screen or the capture command could not read the screen back, or could not write the picture out. |

## E — Programs, modules and packages

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-E001** | fault | **A program would not load** — The file is there and is a module, and loading it failed. Modules is the Control Panel page that says why for each one. |
| **VT-E002** | fault | **A program was built for a different ReconOS** — Modules declare which version of the interface they were built against, and this one does not match. It has to be rebuilt. |
| **VT-E003** | fault | **A package could not be read** — The folder is not a package, or its package.txt is missing, or says something the installer does not understand. |
| **VT-E004** | fault | **An install was rolled back** — The files were placed and the program then refused to load, so everything the install put down has been taken back. Nothing was left behind. |
| **VT-E005** | fault | **A program could not be removed** — Some of what the install placed could not be deleted. The receipt says what it was. |

## F — Network

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-F001** | fault | **The network could not be read** — ReconOS reports the host's network rather than implementing one, and the host would not say what it has. |
| **VT-F002** | fault | **A connection could not be opened** — The address was reached and refused, or was not reached at all. |
| **VT-F003** | fault | **A name could not be resolved** — No address was found for that name. Either the name is wrong or nothing is answering name lookups. |

## G — Firewall and remote access

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-G001** | note | **The firewall refused a connection** — A rule blocked it, or nothing allowed it and the default for that direction is to block. Control Panel, Network, Firewall shows the rules. |
| **VT-G002** | fault | **The firewall rules could not be read** — The rules file is missing or damaged. The built-in defaults are in force: outgoing allowed, incoming blocked. |
| **VT-G003** | fault | **The firewall rules could not be saved** — The change is in force for this session and will not survive a restart. |
| **VT-G004** | fault | **Remote access could not be opened** — The port could not be listened on. Something else is using it, or the system would not allow it. |
| **VT-G005** | note | **A remote connection was refused** — It arrived without a key, or with one that does not match. Recorded with the address it came from. |

## H — Settings and the registry

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-H001** | fault | **Settings could not be read** — A settings file is missing or damaged. What could not be read falls back to the defaults rather than to nothing. |
| **VT-H002** | fault | **Settings could not be saved** — The change is in force now and will not survive a restart. |
| **VT-H003** | note | **A setting was ignored** — The value is not one that setting can take, so the default is being used. The setting is left as it is rather than corrected, because the value somebody wrote is evidence of what they meant. |

## J — Applications

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-J001** | fault | **A program would not open** — The window could not be built. There was not enough memory, or the program refused to start. |
| **VT-J002** | fault | **A program stopped answering** — It is still running and is not responding. Watchtower can end it. |

## K — Input

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-K001** | STOP | **The keyboard could not be set up** — No keyboard layout could be built, so nothing typed would reach anything. This is usually a missing or damaged xkeyboard-config. |

## L — Skins, icons and wallpapers

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-L001** | fault | **A skin could not be read** — The file is not a skin, or names a colour that is not one. The skin is not offered; the one in use is unaffected. |
| **VT-L002** | fault | **A picture could not be drawn** — An icon or wallpaper file could not be read, or is not in a format ReconOS knows. It is left out rather than drawn wrong. |
| **VT-L003** | note | **A colour nothing answered** — Something asked for a colour by a name no skin defines, so it was drawn in the fallback. Visible on screen as a colour that does not belong. |

## M — Help and documentation

| Code | Severity | What it means |
| --- | --- | --- |
| **VT-M001** | fault | **The help could not be written out** — The help pages are copied into /System/Help at every start and this one failed. Whatever was there from last time is still there. |

