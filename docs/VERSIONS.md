# Version history

*Lifted out of `README.md` on 15 September 2026. **The rows below are
unchanged** -- moved, not rewritten.*

**This is the file to edit when a version lands.** It was in the README, which
is the one file both sessions restructure, so every landing risked a conflict
in it. Here it belongs to whoever is shipping.

> **This file is the index; `docs/CHANGELOG.md` is the record.**
>
> They cover the same versions, and for a while the rows here had grown into
> paragraphs that said the same thing twice -- twenty-one of forty-four were
> over four hundred characters against a median of two hundred and seventy-five.
> That is one entry written twice per landing, which is how two records of one
> fact start disagreeing.
>
> So a row is a headline and a sentence. The reasoning, the measurements and
> the faults found along the way are in the change log, which is where somebody
> goes to find out *why*; this is where they go to find out *when*.
>
> **Trimmed on the strength of a measurement rather than a feeling.** Every
> figure in every row was compared against that version's change log entry:
> forty-three of the forty-four repeated their entry entirely, and the one that
> did not -- v0.4.22, which held `2,473 of 3,113` and `511,000 checks` and
> nothing else did -- had those three moved into the change log first. A
> measurement with one home is a measurement that goes when the home is tidied.
>
> The `0.4.6 - 0.4.21` row is left alone: it is one line standing in for sixteen
> versions, so it is already an index entry rather than a paragraph, and there
> is no single change log entry behind it to compare it against.

---

## Version history

Newest first. The number tracks what works, not what is planned.

| Version | What it brought |
| --- | --- |
| **0.4.52** | **A form can carry a file.** The picker, the policy behind it — a page never names a file, and learns the name and bytes but never the path — and a `multipart/form-data` encoder whose boundary is checked against the content rather than drawn at random. Verified end to end: driven headless, sent over a socket, and read back by a parser that is not ours. |
| **0.4.51** | **A keyboard and something to read with.** The kernel delivers USB HID codes, which name *positions*; the desktop wants meanings, and there is no xkbcommon on a ReconOS machine. That layout is written and mutation-tested. And the medium carries two fonts with their licence, because a machine with no `/usr/share` and no font of its own draws a blank screen. |
| **0.4.50** | **The host must not show through.** Five functions of `recon_fs.c` had no suite, and the row describing them named work that was already finished. `guest_path` keeps Linux out of the sentences a person reads — and my first test for it went down a path that never reaches it, so it proved nothing. 48.94% to 58.25%, nothing at zero. |
| **0.4.49** | **A file field was a text box.** `type=file` fell through to the default, so a page asking for a picture got a box somebody could type a filename into — and the form would have gone url-encoded to a server expecting multipart. Drawn dead with a reason now, and a form that asks for multipart is refused rather than sent in a shape nothing can read. |
| **0.4.48** | **A `#` that ended the command.** Comments put inside a shell line continuation ended it, so the freestanding check spent three versions compiling without `-Werror` while reporting that it had not. The answer was right anyway, because a second instrument had the flags on one line. Also: the library had no `<limits.h>`, and vendored headers were being held to the project’s warnings. |
| **0.4.47** | **Signals.** The kernel had four signal calls and nothing in userland could reach them. The restorer is the difficulty — machine code, because there is no C for “return through a system call” — and the layering said where it goes. 30 checks against the host’s, and two files left off the port list on purpose, because compiling is not what that check asks. |
| **0.4.46** | **The same shape a third time.** Five applications were including the whole compositor for one field — two of them for nothing at all — and the freestanding check turned out to be stricter than the build, rejecting a file over unused callback parameters. 46 of 80 sources build with no Linux under them; 52 do now. |
| **0.4.45** | **The other side of the seam.** A panel drawn straight onto a screen — what a program on the ReconOS kernel does — and it takes no system calls, so the same code runs against a plain buffer and is tested there. The seam needed nothing changed to take a second implementation, which is the argument that it is in the right place. |
| **0.4.44** | **The last inch.** `recon_ui.c` was 3,100 lines of which 28 mentioned wlroots, and those 28 held the rest off a compiler with no Linux under it. A panel is a pixel buffer; the inch that hands it to a screen is behind a table of function pointers now, and the desktop’s whole drawing layer builds freestanding. |
| **0.4.43** | **It is the workstation, and its text stays in the box.** `docs/ROLES.md` has five roles and this is one of them, so the screen says which. Photographing that found BG-207 in the same frame — the heap line leaving the panel and running off the edge of the screen, invisible to 306,797 checks because none of them measured the box. |
| **0.4.42** | **One include held twenty-four files.** `include/recon_ui.h` reached `<xkbcommon/xkbcommon.h>` for a single typedef, and that kept twenty-four sources off a compiler with no Linux under them. `include/recon_key.h` is ReconOS’s own names for the same numbers, held to xkbcommon’s by a sweep of 224,517 comparisons — and the desktop went from 20 to 42 sources that build with no libc at all. |
| **0.4.41** | **Four sentences nobody was checking.** `recon_tls.c` had fifteen functions no suite ran, all of them the half that decides whether to believe somebody else — so the suite mints a certificate authority and five certificates with a different fault built into each, and reads what a person would be told. Fifteen at zero, now three. |
| **0.4.40** | **A cell can cover several columns.** `colspan` is read, measured across the columns it covers and drawn across them — and finding it turned up BG-206, where an empty cell took no column and put every heading in a two-row header under the wrong one. The README’s check-count badge was measured for the first time while it was being edited, and it had drifted from 1,971 to 4,474,929. |
| **0.4.39** | **The Recycle Bin, which nothing had ever run.** `coverage.sh --zero` listed fourteen functions of `recon_fs.c` that no suite reached, and they were the whole of the bin — the one part of the filesystem whose entire job is that deleting is *recoverable*, and which fails silently when it is not. |
| **0.4.38** | **Sockets, and the claim nobody could test.** The kernel took five socket numbers and they had no caller; this is the caller. |
| **0.4.37** | **A cell wraps inside its column.** The board said a long cell ran the row off the side; half of that was already fixed, and what remained was worse — a cell that overran its column pushed the pen along, so every column after it on that row stopped lining up. |
| **0.4.36** | **What installing a package actually does.** `recon_package` had 21 checks on reading a manifest and none on doing anything with it — the install path is the one that takes somebody else's shared object and loads it into this process. |
| **0.4.35** | **Five of them were not text.** The board said fourteen places where a release build warns a string may be cut, *"every one builds a string to display rather than to open"*. |
| **0.4.34** | **Every error code has something that can raise it.** The board asked for seven codes to be wired; `git log -S` says all seven were wired on 12 September, and the board had gone on calling them work for three days because *34 of 43 reachable* was counted once by hand. |
| **0.4.33** | **Memory a program can ask for.** `SYS_MAP` with an fd of -1 returns a demand-paged anonymous range -- the first entry in `KERNEL-WANTS.md`, answered, with nothing in `userland/` rebuilt for it: `mem_recon.c` had been sending that call since the allocator was written. |
| **0.4.32** | **The nine that need nothing from the kernel.** Byte order, `inet_pton`, `inet_ntoa`, `gai_strerror`, and the stdio readers `feof`, `ferror` and `ungetc`. |
| **0.4.31** | **`sscanf`.** Fourteen call sites, and what the desktop asks of it was measured by reading all fourteen before any of it was written -- those formats open the suite, verbatim. |
| **0.4.30** | **Files by name, by number, and by directory.** The eleven of twenty-one that have a system call underneath them already; the other ten are declared and deliberately not defined, so a caller fails to link rather than getting a plausible wrong answer. |
| **0.4.29** | **`errno`, and two numberings that meet in one place.** One symbol, 43 call sites, and the last group needing nothing from the kernel. |
| **0.4.28** | **The allocator.** The last large piece of the C library and the one the other 430 call sites were behind -- boundary tags, coalescing, segregated free lists, and regions handed back when nothing in them is in use. |
| **0.4.27** | **The kernel starts the system instead of containing it.** `recon_init` was 121 KiB of `.rodata` inside the kernel image and the installer put nothing on the volume it formatted, so changing a string on the first-boot screen meant reflashing a kernel. |
| **0.4.26** | **The volume has a shape, and a second boot finds it.** `SYS_MKDIR`, and the ten directories a ReconOS machine has, laid down on first boot and found already there on every boot after. |
| **0.4.25** | **ReconOS is on the screen.** The first program on its own kernel that is a system rather than a self-test: it asks the machine what it is, the display how it is arranged and the volume what is on it, and draws a screen somebody can read. |
| **0.4.24** | The maths: twenty functions, 3,613,874 checks against the host's, none failing. Eight held to bit-for-bit equality and twelve to a bound in units in the last place that the suite measures and prints rather than merely asserts. |
| **0.4.23** | Dates: the two clocks, the calendar and `strftime`, held against the host's across two and a half centuries. Two more desktop sources compile with no glibc under them. |
| **0.4.22** | A C library of ReconOS's own, for the day there is no glibc underneath: strings and memory, `snprintf`, the character classes, numbers out of text, and the file layer. 2,473 of the 3,113 library calls in `src/` are answered by it, counted rather than estimated. |
| **0.4.6 - 0.4.21** | The web viewer becomes a browser: a parser checked against fifteen hundred cases nobody here wrote, stylesheets, tables with columns, text alignment, links to a place on a page, History and Bookmarks as pages, forms that submit, and cookies with four things they are not allowed to do. |
| **0.4.5** | A framework for how every control looks and behaves, versioned on its own so it can be fixed without arguing about the system's number. The taskbar in a scene layer of its own, above every window, because it is how you reach everything else. |
| **0.4.0** | ReconOS makes a sound, shows a picture, and can be seen through: `recon_audio`, a codec registry, WAV written from the specification, our own MP4 demuxer, and video playing with the sound device as the clock. Colour conversion and scaling are ours; H.264 is borrowed, in a module. |
| **0.3.0** | TLS both ways — the port, and outgoing with the far end verified. Mail over IMAP and POP3. A clock, Photos, a Calendar. The Calculator gains five modes. Applets update on their own. A fixed-width terminal with colour schemes. Screen resolution. The kernel begins, alongside |
| **0.2.17** | The Control Panel becomes icons, one window per item. Appearance, Network and Programs split into sections. Storage becomes three spaces with a bin each, plus Disk Cleanup. Tooltips. Fonts and wallpapers installable from a right-click. Presets that cannot be deleted |
| **0.2.16** | Error codes with a screen and a log. A firewall. Remote access, two ways. A startup screen that checks rather than counts |
| **0.2.15** | Help and the change log, in the system. Skins writable from inside it. Storage. Typing in the Start menu |
| **0.2.14** | Packages — a manifest and a receipt, so removing takes back exactly what installing placed. Find |
| **0.2.13** | Removing an account can take its files. The menu footer becomes icons |
| **0.2.12** | A text clipboard, which the system did not have at all |
| **0.2.11** | Notepad can undo, and a menu to find it in |
| **0.2.10** | A skin can set the *shape* of a window frame, not only its colours |
| **0.2.9** | Desktop icons move and are remembered. Files open when you click them |
| **0.2.8** | Properties — and the explorer and the box agreeing about what a file is |
| **0.2.7** | Four desktops, the last thing the Multitasking page said was missing |
| **0.2.6** | Window snapping, and Alt+Tab — which had been quietly dead for months |
| **0.2.5** | The registry can be changed, not only read |
| **0.2.4** | Skins can be installed, which the file format had been waiting for |
| **0.2.3** | Gradients, a boot splash, and the unreadable labels that finding them revealed |
| **0.2.2** | All Programs. The control socket restricted to the account that owns it |
| **0.2.1** | Connections that carry data. Screen capture. Installing programs. Wallpapers |
| **0.2.0** | The network — seen and reached across, not implemented |
| **0.1.3** | Choose an account, then sign in |
| **0.1.2** | Setup that looks like it belongs to something. Accounts with faces |
| **0.1.1** | One account at a time, properly. The shape of the rest, in the Control Panel |
| **0.1.0** | The milestone defined as *a usable desktop* |

[docs/ROADMAP.md](docs/ROADMAP.md) has what each version did in full, and what
is known to be missing. [docs/BUGS.md](docs/BUGS.md) has every fault ever
found — what it actually was, how it surfaced, who found it, and what was done
about it. Every entry is also a
[GitHub issue](https://github.com/neogentrics/ReconOS/issues).

Still early. The desktop still runs on Linux, and its account roles are
enforced by ReconOS inside ReconOS rather than by anything underneath it. What
is here works and is tested; what is not here is listed at the end.

**There is now a kernel of its own**, built alongside the desktop and not yet
underneath it — see [The kernel](#the-kernel) below. It boots on two
architectures under three firmwares, on its own bootloader, off its own
filesystem, installed by its own installer, and it reads a keyboard and a
mouse. It has a network stack of its own now too. What it does not have yet is
the **display** a desktop needs, and a way for a program to reach that network
— which is exactly the list that section ends with.
---
