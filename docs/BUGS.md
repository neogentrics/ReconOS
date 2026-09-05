# Bugs

Every fault found in ReconOS, what it actually was, how it surfaced, and what
was done about it.

This exists because a list of commits is not a track record. A commit says
what changed; it does not say that something was broken, that somebody hit it,
or that it is fixed now. Sixty-odd faults have been found in this system so
far and until today none of them had a name. They do now.

## How a bug is named

`BG-` and a number, assigned in the order the fault was **found**:

```
BG-001 ... BG-999
```

Numbers are never reused, never renumbered, and never retired. A closed bug
keeps its number forever, because the number is how the fix is referred to
afterwards — in a commit, in a change log, in a conversation six months from
now about whether this has happened before.

BG-001 through BG-059 were assigned retroactively on 4 September 2026, from
the commit history, and are in the right order relative to each other. From
BG-060 on, a number is taken when the fault is found.

### Why not lettered, like the error codes

The [error codes](ERRORS.md) are `VT-A001`, `VT-G004` — a letter for the area
of the system, then a number. That is right for them and wrong here.

An error code is a **category**. It is written into the source once and raised
every time that kind of thing goes wrong, on any machine, forever, so it wants
grouping: all the storage faults together, all the network faults together.

A bug is an **event**. It happened once, in one place, and was fixed. Grouping
events by subsystem sounds tidy and buys nothing: nobody ever needs "all the
bugs in the display layer" as a contiguous range, and the moment one bug spans
two subsystems — as several below do — the letter has to be guessed at. So:
one sequence, in the order things were found, and the area is a label rather
than part of the name.

## What each entry records

- **Found in** — the version that was running when it surfaced.
- **Found by** — who or what surfaced it. `Joshua` means it was reported from
  actually using the system, which is how most of the interesting ones arrive.
  The rest name the instrument: a screen capture, the compiler, injected
  input, a measurement.
- **Was** — what was actually wrong. Not the symptom: the cause. A register
  that records symptoms is a register nobody can learn anything from.
- **Fixed in** — the version, and the commit.

Bugs still open say **Open** and why.

## Where this is tracked

Every bug here is also a [GitHub issue](https://github.com/neogentrics/ReconOS/issues),
titled with its `BG-` number and labelled `bug` plus its area. The issue is
where discussion happens; this file is the durable record. If they disagree,
this file is wrong and should be corrected — the issues carry the timestamps.

Features, patches and releases are tracked the same way: see
[Labels](#labels) at the foot of this file.

---

## Open

None. Every bug below was found and closed; the list is kept in full
rather than pruned, because a register that only shows what is currently
broken says nothing about the work.

---

## Fixed

### BG-080 — A font inside ReconOS could be set and would never load

[#233](https://github.com/neogentrics/ReconOS/issues/233)

- **Found in** v0.2.17. **Found by** screen capture, testing the new font
  picker: the page said which font was on and every letter on the screen was
  still the old one.
- **Was** `recon_font_load` opens the file itself, so it wants the path the
  *host* keeps it at. `recon_access_apply` passed it whatever the setting held
  -- and a font installed into ReconOS is named by its place inside ReconOS,
  which is not that. Everything above the loader reported success: the key was
  written, the page named the font, and `recon_font_reload` was documented to
  leave the old typeface in place when a file cannot be read, which is exactly
  what it did. The terminal's `access font /System/...` had the same fault and
  nobody had tried it, because until this version there was nowhere inside
  ReconOS for a font to be.
- **Fixed in** v0.2.17. `recon_access_apply` resolves the setting to a host
  path before loading, and passes anything that does not resolve through
  untouched so a host path somebody typed still works. The Control Panel
  checks through the same resolution, so what it reports is the loader's
  answer rather than a second opinion about a path the loader never sees.

### BG-078 — A dialog asked for four buttons and silently got three

[#231](https://github.com/neogentrics/ReconOS/issues/231)

- **Found in** v0.2.17. **Found by** screen capture, building the New Skin
  question: the dialog came up with Light, Dark and High Contrast and no
  Cancel.
- **Was** `recon_shell_ask` clamped to `RECON_DIALOG_BUTTONS_MAX`, which was
  three, by cutting the tail. The tail is the way out: the contract is that
  callers put the safe answer last and Enter and Escape both choose it. So a
  question with one button too many did not lose an answer -- it lost its
  escape hatch, and Escape silently started confirming instead of declining.
  Three had been enough for every question there was, which is why a cap that
  cannot be right had never been wrong.
- **Fixed in** v0.2.17. The maximum is four, and truncation now drops from the
  middle: the last slot always keeps the caller's last button. Losing an
  answer is visible; losing the safety is not.

### BG-079 — A chosen wallpaper outlived the skin change it was promised to

[#232](https://github.com/neogentrics/ReconOS/issues/232)

- **Found in** v0.2.17. **Found by** screen capture, testing New Skin: the
  system went dark and the desktop stayed light.
- **Was** the Wallpapers page says a picture chosen there "stays until the
  skin changes again". `recon_wallpaper_current` preferred the account's
  choice over the skin's suggestion unconditionally, so the first picture
  anybody chose was the last one they would ever see -- no skin could put its
  own on again. The page and the code had disagreed since wallpapers became
  choosable, and the page is the promise.
- **Fixed in** v0.2.17. `recon_wallpaper_set` records the skin in force
  alongside the picture, and the choice applies only while that skin is still
  on. Changing the skin is also somebody saying what they want, and it is the
  more recent of the two.

### BG-062 — What's New reopens after a shell restart

[#2](https://github.com/neogentrics/ReconOS/issues/2)

- **Found in** v0.2.16. **Found by** injected input, testing BG-061.
- **Was** the "what changed in this version" notice fires from the same code
  path a sign-in runs, and restarting the desktop shell runs that path again.
  Somebody who has not yet dismissed the notice gets it raised in front of
  them a second time. Harmless, and wrong: a shell restart is a repair, not a
  new session, and it should not put a window in front of what somebody was
  doing.
- **Fixed in** v0.2.17. `adopt_signed_in_user` takes an `arriving` flag. A
  sign-in passes true; a shell restart passes false, and only the true case
  raises the notice. The notice stays due either way -- it is still unread,
  and it will be shown the next time somebody actually signs in.

### BG-063 — The first row on the Services tab was drawn through the header rule

[#81](https://github.com/neogentrics/ReconOS/issues/81)

- **Found in** v0.2.17. **Found by** screen capture: the tab had been built and
  never looked at.
- **Was** `recon_draw_text` takes a **baseline**, not a top edge.
  `draw_service_rows` computed `ry + (ROW_HEIGHT - ascent) / 2`, which is a top
  edge, so every row sat about ten pixels high and the first one had the column
  header's rule through the middle of it. Every other row-drawing function in
  the file already used `ry + (ROW_HEIGHT + ascent) / 2 - 2`.
- **Fixed in** v0.2.17. The same formula as its neighbours.

### BG-064 — Clicking a row on the Services tab selected nothing

[#82](https://github.com/neogentrics/ReconOS/issues/82)

- **Found in** v0.2.17. **Found by** injected input, in the same look as
  BG-063: the row did not highlight and the button stayed on Start.
- **Was** the row-click handler has a branch per tab and Services had none, so
  a click fell through to the process branch. That looked the row up in the
  process list -- the wrong list -- and set `selected_pid` rather than
  `selected_row`, which is what the Services tab reads. With nothing ever
  selected, Start, Stop and Restart could not act on anything.
- **Fixed in** v0.2.17.

### BG-060 — The Appearance page lists two wallpapers out of five

[#1](https://github.com/neogentrics/ReconOS/issues/1)

- **Found in** v0.2.16. **Found by** Claude, reviewing the page after Joshua
  asked for it to be split into sections.
- **Was** the skin list and the wallpaper list were on one page, and the skin
  list took `(height - y) / ROW_HEIGHT` rows — everything left. With ten skins
  installed there was room for two wallpapers out of five, and the other three
  were drawn past the bottom edge where nothing could see them. Adding Copy
  and Edit buttons between the two lists is what pushed it over.
- **Fixed in** v0.2.17, `4f84508`. Appearance is three sections and each has
  the whole window, so neither list can be squeezed by the other growing. Not
  fixed by arithmetic: an arithmetic fix would hold until the next thing was
  added between them.

### BG-065 — The shell would hold only eight windows

[#147](https://github.com/neogentrics/ReconOS/issues/147)

- **Found in** v0.2.17. **Found by** reading `struct recon_shell` while making
  the Control Panel open a window per item.
- **Was** `struct recon_appwin *apps[8]`. Seven built-ins and a Calculator is
  eight, so a ninth window was refused — and refused quietly, from the
  application's point of view: the window was built and drawn and had no
  taskbar button, took no clicks, and could not be reached by Alt+Tab. It
  looked like a window and behaved like a picture.
- **Fixed in** v0.2.17, `4f84508`. `RECON_SHELL_WINDOWS_MAX`, thirty-two,
  named rather than written into one array declaration — the second array that
  had to agree with it was the one that would have been missed.

### BG-066 — Every window of one application shared a remembered position

[#148](https://github.com/neogentrics/ReconOS/issues/148)

- **Found in** v0.2.17. **Found by** three Control Panel windows opening at
  exactly the same coordinates, three times running, after being told to
  cascade.
- **Was** `geometry_key` built the registry key from `win->impl->title` — the
  *application's* name. That is the same as the window's name for an
  application with one window, which until now was all of them. Every Control
  Panel item is built from one impl, so all fourteen shared a single saved
  position: they opened on top of each other, and moving any one of them wrote
  that position for all the rest.
- **Fixed in** v0.2.17, `4f84508`. The key comes from the window's own title,
  falling back to the application's.

### BG-067 — The title bar drew the application's name, not the window's

[#149](https://github.com/neogentrics/ReconOS/issues/149)

- **Found in** v0.2.17. **Found by** screen capture: the taskbar button said
  Firewall and the window's own title bar said Control Panel.
- **Was** `recon_appwin_set_title` existed and the taskbar read it. The title
  bar read `win->impl->title` directly. A header comment three functions away
  claimed both read through `recon_appwin_title` "so they stay in step", which
  was true of one of them.
- **Fixed in** v0.2.17, `4f84508`.

### BG-068 — A click that opened a window left the keyboard behind

[#150](https://github.com/neogentrics/ReconOS/issues/150)

- **Found in** v0.2.17. **Found by** injected input: the new window arrived in
  front and `state` still reported the old one focused.
- **Was** after offering a click to an application, the shell raised and
  focused the window that had been clicked — unconditionally, including when
  handling the click had deliberately focused something else. A Control Panel
  tile opened its window, focused it, and had focus taken straight back to the
  tile that opened it.
- **Fixed in** v0.2.17, `4f84508`. The shell notes which window held focus
  before the click, by identity rather than by index, and only focuses the
  clicked one if the application did not move focus itself.

### BG-069 — The issue script made 126 duplicate issues

[#211](https://github.com/neogentrics/ReconOS/issues/211)

- **Found in** v0.2.17. **Found by** the run's own output: entries it had
  created an hour earlier came back as new.
- **Was** `scripts/make-issues.py` reads the existing issues with
  `subprocess.run(..., text=True)`, which decodes using the platform's
  preferred encoding. On Windows that is the locale codepage, not UTF-8, so
  every em dash in a title — and every title in the register has one — came
  back as mush. No title ever matched, every entry looked new, and the script
  made a second copy of the whole register. Twice.
- **Fixed in** v0.2.17. `encoding='utf-8'` on every `gh` call, and a check
  that refuses to run at all if the listing comes back with no `BG-` titles in
  it: the failure is silent by nature, so the guard has to be about the shape
  of the answer rather than about the error that was never raised. The 126
  duplicates were deleted, and every bug kept its original issue number.
- **Note** the register exists to show that faults get found and fixed. A
  tool that fills it with noise is worth an entry of its own.

### BG-070 — Nothing in the Help window was clickable

[#212](https://github.com/neogentrics/ReconOS/issues/212)

- **Found in** v0.2.17. **Found by** Joshua: *"now nothing in the window is
  clickable. You can't even scroll."*
- **Was** the region that lets the sidebar take the mouse wheel covers every
  row in it, and was registered *after* them. The last region added wins, so
  it swallowed every click on the list: the topics were drawn, highlighted
  under the pointer, and could not be chosen. The window frame kept working,
  which is what made it look like only the inside was broken.
- **Fixed in** v0.2.17, `b9745f9`. A region that exists to catch what the
  others miss goes down before them, not after.
- **Note** introduced by the fix for BG-047, which added the scrolling. The
  fix for one fault is where the next one comes from more often than is
  comfortable.

### BG-071 — Markdown subheadings appeared with their hashes

[#213](https://github.com/neogentrics/ReconOS/issues/213)

- **Found in** v0.2.16. **Found by** screen capture, while confirming BG-070:
  the page read `### Making a skin of your own`.
- **Was** the help is written as Markdown and read from it, and the reader
  drew every line as plain text. Nobody writing the help asked for the hashes
  to appear on screen.
- **Fixed in** v0.2.17, `b9745f9`. A line beginning with hashes is drawn as a
  subheading, in the accent colour, with the hashes taken off.

### BG-072 — "Scroll for the rest" above a list that could not scroll

[#214](https://github.com/neogentrics/ReconOS/issues/214)

- **Found in** v0.2.17. **Found by** Joshua: *"under appearance or colors, it
  says scroll for the rest, but you can't scroll."*
- **Was** `panel_scroll` handled the skin editor and the registry and nothing
  else. The Colours section printed a line telling somebody to do something
  the page would not let them do, which is worse than not offering it.
- **Fixed in** v0.2.17, `b9745f9`. Every list in Appearance takes the wheel,
  clamps where it is drawn rather than where the wheel is turned, and shows a
  bar when there is more than fits. Themes and Wallpapers got it at the same
  time: neither overflows today and both will.
- **Note** it survived because nothing could turn the wheel from outside, so
  nothing tested it. `ui scroll` exists now. This is BG-038 again.

### BG-073 — An application's icon did not follow its windows

[#215](https://github.com/neogentrics/ReconOS/issues/215)

- **Found in** v0.2.17. **Found by** Joshua: *"Control panel's icon isn't
  system wide. It's only in the start menu."*
- **Was** two separate faults with one shape. The Control Panel was registered
  with `RECON_ICON_CONTROL_PANEL` in the Start menu's table and
  `RECON_ICON_SYSTEM` in its own window description, so the menu drew the
  sliders and the title bar and taskbar drew a generic square. And a window
  opened for a Control Panel item carried the Control Panel's icon rather than
  the item's — fourteen taskbar buttons nobody could tell apart.
- **Fixed in** v0.2.17, `b9745f9`. `recon_appwin_set_icon` alongside
  `recon_appwin_set_title`, each page window carries its own, and the title
  bar and taskbar both read through `recon_appwin_icon` so there is one answer
  rather than two that can disagree.
- **Note** the same shape as BG-067, a day apart: a thing with two names is a
  thing that will eventually be two different things.

### BG-074 — Every folder in the Start menu opened the same folder

[#222](https://github.com/neogentrics/ReconOS/issues/222)

- **Found in** v0.2.17. **Found by** Joshua: *"if I click documents, it should
  be loading me into... the documents folder. Instead, it just takes me to the
  user folder."*
- **Was** `recon_fs_user_dir` returns a pointer into one shared static buffer.
  The menu built the path, then called `recon_shell_open_named` on the next
  line to make sure the File Explorer existed — and building the explorer
  calls `recon_fs_user_dir(NULL)` to decide where to start, overwriting
  `/Users/Joshua/Documents` with `/Users/Joshua` before the pointer was ever
  read. Every place opened the account's own folder.
- **Fixed in** v0.2.17, `7426d91`. The path is copied into a local before
  anything else runs.
- **Note** the fault is not the shared buffer, which is a reasonable thing for
  a path accessor to have. It is holding the pointer across a call that could
  reach the same accessor. Worth checking the other callers for the same
  shape.

### BG-075 — Clicking the Start menu's search box closed the menu

[#223](https://github.com/neogentrics/ReconOS/issues/223)

- **Found in** v0.2.17. **Found by** Joshua: *"you can't click in it. When I
  click on it, it just closes the app's menu."*
- **Was** the box had a hit region and nothing handled it, so a click on it
  fell through to "somewhere in the menu that is not an entry", which closes
  the menu. Typing had always worked; what was missing was the box not
  throwing the menu away when somebody did the obvious thing and clicked it
  first.
- **Fixed in** v0.2.17, `7426d91`. The box takes the click and keeps the menu.
  Its phantom text was also drawn in the disabled ink — the colour of a thing
  that cannot be used — which on a warm skin made the box read as switched
  off; it is the dim surface ink now.
- **Note** introduced with the box itself, one commit earlier. A region added
  without a handler is a control that looks alive and is not.

### BG-076 — The firewall's rule list could not be scrolled

[#224](https://github.com/neogentrics/ReconOS/issues/224)

- **Found in** v0.2.17. **Found by** Joshua: *"Doesn't have the scroll
  ability, so you can't scroll through the different rules."*
- **Was** the same gap as BG-072, in the one list that had not been swept:
  `panel_scroll` did not know about the firewall page. With nine rules it
  happened to fit; the moment more shipped it did not.
- **Fixed in** v0.2.17, `7426d91`. It scrolls, with a bar, and the list
  follows the selection only when the selection moves — following it on every
  draw pinned the list and made the wheel do nothing at all, which was the
  first attempt.

### BG-077 — Nothing in the Start menu could be right-clicked

[#230](https://github.com/neogentrics/ReconOS/issues/230)

- **Found in** v0.2.17. **Found by** Joshua: *"You can't right click them. If
  you try to right click, it right clicks the desktop in the background."*
- **Was** `recon_shell_handle_right_click` closed the Start menu as its first
  act, before testing where the click had landed. So a right-click on an
  application fell through to whatever was behind the menu -- the desktop --
  and the desktop's own menu appeared instead.
- **Fixed in** v0.2.17, `e045523`. The menu is tested first, and closing it
  happens only once nothing in it has claimed the click. A right-click on a
  gap in the menu now leaves the menu open, which is the same fault in a
  smaller place.

### BG-001 — The screen stayed blank

[#3](https://github.com/neogentrics/ReconOS/issues/3)

- **Found in** v0.1.0 (pre-release). **Found by** running it: the compositor
  started, took the display, handled input, and drew nothing.
- **Was** `wlr_scene_attach_output_layout()` keeps scene outputs positioned in
  step with the layout; it does not create them. Without a scene output,
  `wlr_scene_get_scene_output()` returned NULL every frame and the frame
  handler returned early — silently, which is precisely why the fault was
  invisible.
- **Fixed in** v0.1.0, `cd6f9c3`. A scene output per output, plus reporting
  the failure instead of returning early in silence.

### BG-002 — The render loop never started

[#4](https://github.com/neogentrics/ReconOS/issues/4)

- **Found in** v0.1.0 (pre-release). **Found by** the same investigation as
  BG-001.
- **Was** nothing asked for the first frame. Every later frame is scheduled by
  the one before it, so with no first frame the loop never began.
- **Fixed in** v0.1.0, `cd6f9c3`. An initial frame is scheduled once the
  output is up.

### BG-003 — The screen rendered as bands of stale image

[#5](https://github.com/neogentrics/ReconOS/issues/5)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, on the Hyper-V VM.
- **Was** partial redraws assume the driver hands back a buffer still holding
  the previous frame. `hyperv_drm` does not, so every region the scene
  considered unchanged was never filled in.
- **Fixed in** v0.1.0, `7263845`. Existing damage is widened to the whole
  output before committing — widened, never created, because marking the
  output damaged unconditionally makes every frame commit and every commit
  schedule another, which is a compositor redrawing forever at full speed.
  `RECONOS_PARTIAL_DAMAGE=1` opts back in where buffers are preserved.

### BG-004 — The wallpaper was rescaled on every frame

[#6](https://github.com/neogentrics/ReconOS/issues/6)

- **Found in** v0.1.0 (pre-release). **Found by** measurement, while
  investigating BG-003.
- **Was** a 4256×2832 image resampled to screen size during compositing, in
  software, sixty times a second.
- **Fixed in** v0.1.0, `7263845`. Downscaled once at load to the size it will
  be drawn at, so compositing copies rather than resamples. Idle RSS fell from
  59MB to 16MB. This is why the background is built after the first output
  reports its resolution rather than at startup.

### BG-005 — The cursor swallowed every click

[#7](https://github.com/neogentrics/ReconOS/issues/7)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: windows could not be
  dragged, resized, or closed, while typing worked fine.
- **Was** the pointer was a red rectangle in the scene graph, raised above
  everything else. Hit testing asks for the topmost node at the pointer's
  position, so the answer was always the cursor. No window was ever found,
  pointer focus was cleared on every motion, and no client received a mouse
  event. Typing still worked because keyboard focus is assigned when a window
  maps and does not depend on hit testing — which is what made the fault look
  like a mouse problem rather than a hit-testing one.
- **Fixed in** v0.1.0, `4cce403`. wlroots' own cursor, drawn on the pointer's
  layer rather than in the scene, so it cannot intercept input.

### BG-006 — Black rectangles flickered over the shell's own windows

[#8](https://github.com/neogentrics/ReconOS/issues/8)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua. Worst on a maximized
  Watchtower, and visible with nothing moving on screen.
- **Was** every buffer handed to the scene graph pointed back at the panel's
  single pixel block. The compositor may still be reading a committed buffer
  while the next frame is drawn into that same memory, so frames were
  overwritten mid-read.
- **Fixed in** v0.1.0, `e0b7fc5`. A committed buffer owns its pixels. The cost
  is a copy per commit, which is nothing next to how rarely a panel commits.

### BG-007 — Resizing a panel freed pixels that committed buffers still held

[#9](https://github.com/neogentrics/ReconOS/issues/9)

- **Found in** v0.1.0 (pre-release). **Found by** the investigation into
  BG-006: maximizing made the flicker dramatically worse, and maximizing
  resizes.
- **Was** a use after free. The panel's pixel block was freed outright on
  resize while committed buffers still referenced it.
- **Fixed in** v0.1.0, `e0b7fc5`, by the same change: a buffer that owns its
  pixels cannot have them freed underneath it.

### BG-008 — Apps, then Shut Down, closed the menu and did nothing

[#10](https://github.com/neogentrics/ReconOS/issues/10)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, pressing it.
- **Was** the apps menu is raised above every window, but windows were offered
  clicks first. A maximized window covering the same pixels swallowed the
  click meant for the menu.
- **Fixed in** v0.1.0, `e0b7fc5`. Click routing follows what is drawn on top
  of what.

### BG-009 — Built-in windows were pinned above every other window

[#11](https://github.com/neogentrics/ReconOS/issues/11)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: a client window
  could never be brought in front of Watchtower.
- **Was** built-in windows were raised to the top of the scene and kept there.
- **Fixed in** v0.1.0, `c0a361e`.

### BG-010 — Watchtower's Processes tab listed everything on every tab

[#12](https://github.com/neogentrics/ReconOS/issues/12)

- **Found in** v0.1.0 (pre-release). **Found by** reading the window.
- **Was** the process list was not filtered by which tab was showing.
- **Fixed in** v0.1.0, `c0a361e`.

### BG-011 — Clicking a window brought a different one forward

[#13](https://github.com/neogentrics/ReconOS/issues/13)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: clicking the
  Calculator where it overlapped Watchtower raised Watchtower.
- **Was** built-in windows were offered clicks in the order they were
  *created*. Watchtower was created first, so it answered for every click
  landing inside its rectangle, including clicks on windows stacked above it.
- **Fixed in** v0.1.0, `1b0d820`. The scene graph is asked what is actually on
  top. "Contains this point" is a different question from "is on top here".

### BG-012 — A terminal would never minimize

[#14](https://github.com/neogentrics/ReconOS/issues/14)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, pressing the button.
- **Was** the shell was offered every click before client windows were
  considered at all, so a built-in window underneath the terminal took the
  click and the taskbar branch was never reached. The log confirmed minimize
  was never called.
- **Fixed in** v0.1.0, `1b0d820`.

### BG-013 — Frames were corrupted on a machine with no GPU

[#15](https://github.com/neogentrics/ReconOS/issues/15)

- **Found in** v0.1.0 (pre-release). **Found by** measurement, after four
  reasoned explanations turned out to be wrong. Dumping panel pixels and
  capturing composited frames showed both were already correct, which placed
  the fault below compositing.
- **Was** with no render node, the OpenGL path renders through a software
  implementation and then copies the finished frame back out for the display
  to scan. That copy was corrupting frames.
- **Fixed in** v0.1.0, `5d38fa3`. The pixman renderer rasterizes straight into
  the display's buffer, so the copy never happens. Selected automatically when
  the machine has no render node; an explicit `WLR_RENDERER` still wins.
- **Note** this is the bug that changed how ReconOS is debugged. Four wrong
  explanations in a row were all reasoned from the code. The fix came from
  looking at pixels. `capture` and the `look.sh` harness exist because of this
  entry.

### BG-014 — The Applications tab listed processes

[#16](https://github.com/neogentrics/ReconOS/issues/16)

- **Found in** v0.1.0 (pre-release). **Found by** reading the window: it was
  the Processes tab twice.
- **Was** the Applications view enumerated processes rather than windows.
- **Fixed in** v0.1.0, `b3ceff4`.

### BG-015 — Focus had two owners that disagreed

[#17](https://github.com/neogentrics/ReconOS/issues/17)

- **Found in** v0.1.0 (pre-release). **Found by** code review while adding
  per-window memory.
- **Was** both the compositor and the shell tracked which window had focus,
  and nothing kept them in step.
- **Fixed in** v0.1.0, `1ab1143`. One owner.

### BG-016 — The compositor repainted on damage that had already been drawn

[#18](https://github.com/neogentrics/ReconOS/issues/18)

- **Found in** v0.1.0 (pre-release). **Found by** measurement: an idle desktop
  was not idle.
- **Was** repaints were triggered by leftover damage rather than by anything
  changing.
- **Fixed in** v0.1.0, `6c6fb0e`.

### BG-017 — Every context menu entry had silently done nothing, ever

[#19](https://github.com/neogentrics/ReconOS/issues/19)

- **Found in** v0.1.0 (pre-release). **Found by** injected input — pressing
  the buttons rather than reading the code.
- **Was** the button handler closed the menu *before* dispatching the click,
  so `context_open` was already false when the shell looked at it and the
  branch that turns a click into a choice never ran. Rename, Delete, Cut,
  Copy, Paste, New Folder, New File and New Shortcut had never been reachable
  since menus were added. The menu appeared, the click landed, the menu
  closed, nothing happened.
- **Fixed in** v0.1.0, `8b4ebc3`.
- **Note** this fault survived for as long as it did because there was no way
  to press a button without a person doing it. `recon_inject_pointer/button/
  key`, the `ui` command and the `state` command were all built here, and
  found BG-018 through BG-021 the same afternoon.

### BG-018 — Renaming was destroyed in the same breath it was started

[#20](https://github.com/neogentrics/ReconOS/issues/20)

- **Found in** v0.1.0 (pre-release). **Found by** injected input, immediately
  after BG-017 made Rename reachable at all.
- **Was** every context action ends with a shell refresh, which reloads the
  desktop, and the reload cancelled any rename in progress.
- **Fixed in** v0.1.0, `8b4ebc3`. A rename is carried across a reload by name.

### BG-019 — Renaming a file selected its extension too

[#21](https://github.com/neogentrics/ReconOS/issues/21)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** the whole name was selected, so the first keystroke took the
  extension with it.
- **Fixed in** v0.1.0, `8b4ebc3`. The stem is selected and `.app` survives,
  with Ctrl+A, shift-free arrow keys and Backspace behaving accordingly.

### BG-020 — Right-clicking the taskbar did nothing unless you hit a button

[#22](https://github.com/neogentrics/ReconOS/issues/22)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** the taskbar answered right-clicks only on a window button. Empty bar
  did nothing, which reads as broken.
- **Fixed in** v0.1.0, `8b4ebc3`. Empty bar offers Task Manager, Show Desktop
  and Refresh.

### BG-021 — Opening Watchtower opened the Calculator

[#23](https://github.com/neogentrics/ReconOS/issues/23)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** applications were opened by position in a list. The index for
  Watchtower pointed at the Calculator, and any application failing to
  construct would have shifted every index after it.
- **Fixed in** v0.1.0, `8b4ebc3`. Applications are found by name.

### BG-022 — The file dialog filled the window it belonged to

[#24](https://github.com/neogentrics/ReconOS/issues/24)

- **Found in** v0.1.0 (pre-release). **Found by** screen capture.
- **Was** the dialog was sized to its parent rather than to its contents.
- **Fixed in** v0.1.0, `05799c4`.

### BG-023 — The desktop was empty

[#25](https://github.com/neogentrics/ReconOS/issues/25)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: no icons.
- **Was** the marker recording that default shortcuts had been written lived
  in `/System/Config`. When user folders arrived the shortcuts moved to the
  user's folder, but the marker did not, so a system that had already written
  them once never wrote them to the new location.
- **Fixed in** v0.1.0, `c7fbde2`. The marker belongs to the user and lives in
  their folder.

### BG-024 — ReconOS refused to start if its root was not writable

[#26](https://github.com/neogentrics/ReconOS/issues/26)

- **Found in** v0.1.0 (pre-release). **Found by** running it somewhere else.
- **Was** an unwritable root was treated as fatal.
- **Fixed in** v0.1.0, `a15ea26`. It falls back to a writable root.

### BG-025 — The setup screen offered eight skins out of ten

[#27](https://github.com/neogentrics/ReconOS/issues/27)

- **Found in** v0.1.0. **Found by** counting them on screen.
- **Was** the grid drew a fixed eight.
- **Fixed in** v0.1.1, `0fd4f66`.

### BG-026 — One account could read another account's files

[#28](https://github.com/neogentrics/ReconOS/issues/28)

- **Found in** v0.1.0. **Found by** code review of the new account boundary.
- **Was** writing into another account's folder was refused, but
  `recon_fs_list` and `recon_fs_read` were not, which is most of the way to no
  protection at all.
- **Fixed in** v0.1.1, `07ee8e8`. Both refuse, tested including the case where
  one account's name is a prefix of another's. The system's own folder stays
  readable — an account has to be able to load its settings.
- **Security.**

### BG-027 — The address bar's drop-down appeared behind things

[#29](https://github.com/neogentrics/ReconOS/issues/29)

- **Found in** v0.1.1. **Found by** Joshua, using the File Explorer.
- **Was** the drop-down was built as a panel, so it took part in scene
  stacking as an ordinary surface rather than as a menu.
- **Fixed in** v0.1.3, `b4560cd` and `f2690d6`. It is a menu.

### BG-028 — The refresh arrow was not redrawn

[#30](https://github.com/neogentrics/ReconOS/issues/30)

- **Found in** v0.1.1. **Found by** screen capture.
- **Was** the button's own redraw was never triggered after the first draw.
- **Fixed in** v0.1.3, `f2690d6`.

### BG-029 — Lock offered every account on the screen it put up

[#31](https://github.com/neogentrics/ReconOS/issues/31)

- **Found in** v0.1.1. **Found by** Joshua, locking the session.
- **Was** anybody could sign in over a locked session, which is the one thing
  locking exists to prevent.
- **Fixed in** v0.1.3, `a0dee0c`. Locked to whoever locked it: their account
  and no other, the arrow keys cannot step off it, and the check is made when
  signing in rather than only when drawing — a lock that is only a picture is
  not a lock. Switch User ends the session first, and that screen still offers
  everybody, because then there is nothing left to protect.
- **Security.**

### BG-030 — Double-clicking the Recycle Bin did nothing

[#32](https://github.com/neogentrics/ReconOS/issues/32)

- **Found in** v0.1.1. **Found by** Joshua, double-clicking it.
- **Was** the desktop's "what does opening this mean" switch had no case for
  the bin, while the context menu's Open — a second copy of the same logic —
  did.
- **Fixed in** v0.1.3, `a0dee0c`. One copy; the other calls it.

### BG-031 — Icons fell apart when shrunk

[#33](https://github.com/neogentrics/ReconOS/issues/33)

- **Found in** v0.1.1. **Found by** screen capture at a small icon size.
- **Was** nearest-neighbour sampling on the way down.
- **Fixed in** v0.1.3, `db72de7`.

### BG-032 — The control socket was open to every account on the machine

[#34](https://github.com/neogentrics/ReconOS/issues/34)

- **Found in** v0.2.1. **Found by** code review.
- **Was** `/tmp/reconos.sock` was created with default permissions. Anything
  that can write to that socket can drive the whole desktop.
- **Fixed in** v0.2.2, `f239432`, mode 0600.
- **Security.**

### BG-033 — Desktop labels were white text on a light wallpaper

[#35](https://github.com/neogentrics/ReconOS/issues/35)

- **Found in** v0.2.1. **Found by** screen capture, immediately after the
  Beacon skin was added.
- **Was** desktop label colour was fixed rather than asked of the skin.
- **Fixed in** v0.2.2, `58cf832`.

### BG-034 — The Start menu did not redraw itself

[#36](https://github.com/neogentrics/ReconOS/issues/36)

- **Found in** v0.2.1. **Found by** Joshua: the menu showed stale contents.
- **Was** the menu panel was drawn once and never invalidated when what it
  listed changed.
- **Fixed in** v0.2.2, `fd2654b`.

### BG-035 — Every gradient role showed the wrong label

[#37](https://github.com/neogentrics/ReconOS/issues/37)

- **Found in** v0.2.2. **Found by** screen capture, while adding gradients.
- **Was** the label lookup was offset by the roles that had no gradient.
- **Fixed in** v0.2.3, `6d694d4`.

### BG-036 — Two source files contained raw NUL bytes where `'\0'` was meant

[#38](https://github.com/neogentrics/ReconOS/issues/38)

- **Found in** v0.2.3. **Found by** the compiler: *null character(s)
  preserved in literal*.
- **Was** `recon_net.c` and `recon_theme.c` each had a literal NUL byte inside
  single quotes, from an edit that lost a backslash passing through a shell.
- **Fixed in** v0.2.4, `27cc5f8`.
- **Note** this keeps happening — it happened again on 4 September 2026 while
  fixing BG-061. Patches are written to a file and run by path now rather than
  piped through a shell heredoc, which is the only reliable fix.

### BG-037 — A previous fix claimed a bug that was never a bug

[#39](https://github.com/neogentrics/ReconOS/issues/39)

- **Found in** v0.2.3. **Found by** re-reading `f239432` while fixing BG-036.
- **Was** `f239432` claimed that comparing the gateway against a raw NUL byte
  meant a machine with no gateway did not print "(none)". That claim is wrong:
  a character constant holding a single NUL byte has the value 0, exactly as
  `'\0'` does, so the comparison had always been correct. The compiler warns
  about the form, not about a change in meaning. The warning was read and the
  conclusion assumed.
- **Fixed in** v0.2.4, `27cc5f8`, by correcting the claim rather than leaving
  it standing.
- **Documentation.**

### BG-038 — No system shortcut could be driven from outside, and Alt+Tab had stopped working

[#40](https://github.com/neogentrics/ReconOS/issues/40)

- **Found in** v0.2.5. **Found by** building the means to test shortcuts, then
  using it.
- **Was** two faults. Injected keys went straight to the shell, bypassing the
  compositor's shortcut handler entirely — so Alt+Tab, Alt+Q, Ctrl+Alt+Del and
  Print Screen had never once been tested. Sending one the route a real key
  takes immediately found that Alt+Tab cycled the compositor's list of
  *client* windows. That was correct when the only windows were clients, and
  became a shortcut that did nothing the moment ReconOS drew its own: a
  desktop with a Notepad and a Terminal on it has no clients in that list.
- **Fixed in** v0.2.6, `380b996`.
- **Note** a shortcut nothing can press is a shortcut nobody notices the loss
  of.

### BG-039 — A clean build had 36 warnings

[#41](https://github.com/neogentrics/ReconOS/issues/41)

- **Found in** v0.2.6. **Found by** the compiler, after a claim that the build
  was warning-free turned out to be true only of the one file being rebuilt.
- **Was** among them: five `implicit declaration of strcasecmp` — `recon_users.c`
  and `recon_session.c` called it without `<strings.h>`, so the compiler was
  assuming a signature. It happens to work on this target and is undefined
  behaviour everywhere. Four were paths built with `snprintf` from a folder
  and a name where the two together can exceed the buffer; a truncated path is
  not a shortened name, it is a different file.
- **Fixed in** v0.2.7, `666ee5e`.

### BG-040 — The roadmap claimed a warning-free build that did not exist

[#42](https://github.com/neogentrics/ReconOS/issues/42)

- **Found in** v0.2.6. **Found by** BG-039.
- **Fixed in** v0.2.7, `edd4653`.
- **Documentation.**

### BG-041 — An unfocused window's title was drawn in the focused colour

[#43](https://github.com/neogentrics/ReconOS/issues/43)

- **Found in** v0.2.7. **Found by** a screenshot that appeared to show a
  window with an empty title bar. That turned out to be occlusion — but
  looking into it found this underneath.
- **Was** `RECON_THEME_TITLE_TEXT_INACTIVE` was defined, answered by all ten
  skins, and read by nothing. On skins whose two title bars are near-identical
  greys it looked fine, which is why it survived; on Beacon it is white on
  light blue.
- **Fixed in** v0.2.8, `14ab3db` and `c3bbf62`.

### BG-042 — The accessibility contrast test measured the wrong bar

[#44](https://github.com/neogentrics/ReconOS/issues/44)

- **Found in** v0.2.7. **Found by** BG-041.
- **Was** title text was measured against the *active* title bar for both
  states, so the unreadable combination was never tested.
- **Fixed in** v0.2.8, `14ab3db`.

### BG-043 — The font drew no dashes

[#45](https://github.com/neogentrics/ReconOS/issues/45)

- **Found in** v0.2.15. **Found by** screen capture of the Update page.
- **Was** the glyph cache held 32..126 only, and an em dash is not in that
  range. See BG-044, which is the same fault seen properly.
- **Fixed in** v0.2.16, `020af35`.

### BG-044 — Any text that was not ASCII drew nothing at all

[#46](https://github.com/neogentrics/ReconOS/issues/46)

- **Found in** v0.2.15. **Found by** every em dash in the help coming out as a
  hole in the sentence.
- **Was** text was walked a byte at a time and glyphs were cached only for
  32..126, so a UTF-8 sequence was three or four bytes each of which drew
  nothing. Not a box, not a question mark: nothing. The typeface had the
  glyphs the whole time. The first attempted fix folded the punctuation to
  ASCII on the way into the system, which was a patch on the symptom.
- **Fixed in** v0.2.16, `189aa0f`. The walk decodes UTF-8, and the glyph cache
  has a second half for what is above the ASCII range.

### BG-045 — Deleting one byte of a two-byte character broke the field

[#47](https://github.com/neogentrics/ReconOS/issues/47)

- **Found in** v0.2.15. **Found by** typing an accented character into a name
  field and pressing Backspace, after BG-044 made such characters visible.
- **Was** the caret stepped by byte. Backspace over a two-byte character left
  a broken sequence — a name that had been typed correctly and could no longer
  be read. `recon_edit_key` also accepted only ASCII, so a keyboard laid out
  for a language with accents in it could not name a file the system would
  happily have stored.
- **Fixed in** v0.2.16, `0f295b9`. The caret is still a byte offset, because
  that is what the text is; arrow keys and Backspace step over whole
  characters by walking off the continuation bytes.

### BG-046 — The Help window could not be closed or moved

[#48](https://github.com/neogentrics/ReconOS/issues/48)

- **Found in** v0.2.16. **Found by** Joshua: *"None of the buttons work in the
  top right corner for the help window, so you can't close it once you press
  F1."*
- **Was** `help_draw` called `recon_hit_clear()` *after* the window frame had
  registered its own buttons, wiping every hit region the frame had just added
  — the close button, the minimize and maximize buttons, and the title bar
  that a window is dragged by.
- **Fixed in** v0.2.16, `7d7e36c`. The same fault was in the What's New window
  and was fixed with it.

### BG-047 — The Help sidebar could not be scrolled

[#49](https://github.com/neogentrics/ReconOS/issues/49)

- **Found in** v0.2.16. **Found by** Joshua: *"there's no scroll in the
  listing panel, no scroll bar."*
- **Was** around forty topics in an eighteen-row panel, with no scroll offset
  and no bar. Most of the help was unreachable.
- **Fixed in** v0.2.16, `7d7e36c`.

### BG-048 — "N more lines below" was drawn over the last line of text

[#50](https://github.com/neogentrics/ReconOS/issues/50)

- **Found in** v0.2.16. **Found by** Joshua: *"the text at the bottom is
  jumbled."*
- **Was** the visible-line count was computed before the heading had taken its
  room, and without reserving a line for the footer that says there is more.
- **Fixed in** v0.2.16, `7d7e36c`.

### BG-049 — F1 did not close the Help window

[#51](https://github.com/neogentrics/ReconOS/issues/51)

- **Found in** v0.2.16. **Found by** Joshua: *"Pressing F1 again doesn't close
  the help app. It just resets it back to the top."*
- **Was** F1 always opened help on whatever was in front, and opening help
  when help is in front reopened it at its first topic.
- **Fixed in** v0.2.16, `7d7e36c`. F1 with help in front closes it.

### BG-050 — Opening the Calculator crashed the whole system

[#52](https://github.com/neogentrics/ReconOS/issues/52)

- **Found in** v0.2.16. **Found by** Joshua, twice: *"I tried to open the
  calculator app, and it crashed it. The whole system just crashed."*
- **Was** v0.2.15 added a `const char *help;` field to
  `struct recon_appwin_impl` without bumping `RECON_MODULE_ABI`. A
  `Calculator.rex` built before that change passed the version gate with a
  struct one field short, and `recon_appwin_create` read `impl->help` past the
  end of it. SIGSEGV.
- **Fixed in** v0.2.16, `d61a9a3`. ABI bumped to 2, with the rule that was
  missed written into the header beside it.
- **Note** proved rather than assumed: the ABI was reverted to 1, the system
  rebuilt, `apps Calculator` run, and the segfault reproduced before the fix
  was claimed.

### BG-051 — A stale module was never replaced by the one that shipped with the build

[#53](https://github.com/neogentrics/ReconOS/issues/53)

- **Found in** v0.2.16. **Found by** BG-050: the mechanism that should have
  prevented it.
- **Was** shipped modules were installed only if absent, so an out-of-date
  `.rex` in the filesystem outlived every rebuild.
- **Fixed in** v0.2.16, `d61a9a3`. Shipped modules are compared byte for byte
  and replaced when they differ.

### BG-052 — The control socket aborted on the connection after a `quit`

[#54](https://github.com/neogentrics/ReconOS/issues/54)

- **Found in** v0.2.16. **Found by** the next connection failing, while
  building remote access.
- **Was** a use after free. `handle_line` closed the client from inside
  itself, freeing the struct the read loop was standing on.
- **Fixed in** v0.2.16, `abb00f4`. `handle_line` returns a verdict and the
  caller closes.

### BG-053 — The remote key was echoed back as a failed command

[#55](https://github.com/neogentrics/ReconOS/issues/55)

- **Found in** v0.2.16. **Found by** authenticating over the network port.
- **Was** the key line was consumed by the authentication check and then also
  handed to the command interpreter, which did not recognise it and said so —
  printing the key into the transcript.
- **Fixed in** v0.2.16, `abb00f4`.
- **Security.**

### BG-054 — The Terminal was documented as unavailable when it was installed

[#56](https://github.com/neogentrics/ReconOS/issues/56)

- **Found in** v0.2.16. **Found by** running `weston-terminal` to check, after
  the roadmap had recorded it as a blocker for some time.
- **Was** a documented blocker that had never been verified.
- **Fixed in** v0.2.16. Corrected in the roadmap.
- **Documentation.**

### BG-055 — `recon_spawn` had never been executed

[#57](https://github.com/neogentrics/ReconOS/issues/57)

- **Found in** v0.2.16. **Found by** adding the `spawn` command, which called
  it for the first time.
- **Was** code written, compiled, shipped and never once run. It did not work.
- **Fixed in** v0.2.16.
- **Note** one of several this session. Building the instrument that can reach
  a code path is how the code path gets found to be broken; see also BG-017
  and BG-038.

### BG-056 — `recon_help_show_topic` had never been executed

[#58](https://github.com/neogentrics/ReconOS/issues/58)

- **Found in** v0.2.16. **Found by** wiring F1 to it.
- **Was** as BG-055.
- **Fixed in** v0.2.16, `75c331c`.

### BG-057 — CMake added a source file to every target that mentioned another

[#59](https://github.com/neogentrics/ReconOS/issues/59)

- **Found in** v0.2.16. **Found by** the build: test targets acquired the
  firewall and everything it needs.
- **Was** a `sed` over `CMakeLists.txt` matched every target listing
  `recon_fs.c` rather than the one intended.
- **Fixed in** v0.2.16, `5d395f1`. Trimmed to the main target plus
  `recon_net_tests`.

### BG-058 — The stop screen was unreadable at a glance

[#60](https://github.com/neogentrics/ReconOS/issues/60)

- **Found in** v0.2.16. **Found by** Joshua, on seeing the first one.
- **Was** the error screen used the ordinary window palette, so a system that
  had stopped looked like a system that was fine.
- **Fixed in** v0.2.16, `ed41702`. Purple — the Recon Towers hub colour — with
  the code in amber, and a counter while it collects what it knows.

### BG-059 — Killing the build by pattern killed the shell running the command

[#61](https://github.com/neogentrics/ReconOS/issues/61)

- **Found in** v0.2.16. **Found by** the shell dying.
- **Was** `pkill -f "build/ReconOS"` matched its own invoking command line.
- **Fixed in** v0.2.16. The harness kills by recorded PID.
- **Note** a tooling fault rather than a ReconOS one, recorded because it cost
  an afternoon twice.

### BG-061 — Restarting the desktop shell segfaulted the system

[#62](https://github.com/neogentrics/ReconOS/issues/62)

- **Found in** v0.2.16. **Found by** injected input, testing the new Services
  tab: `services restart Desktop shell` with a Notepad open, three times out
  of three.
- **Was** two faults, one behind the other.
  1. The UI font was owned by the shell and freed with it. Application windows
     deliberately outlive a shell restart — that is what makes restarting the
     shell a repair rather than a loss — and every one of them holds that
     font pointer, several caching a copy of their own. The first frame after
     the restart was a segmentation fault inside the glyph rasteriser, five
     frames deep in stb_truetype.
  2. Registering a built-in application twice was refused as a name collision.
     A second shell registers the same seven built-ins, so a restarted desktop
     had no Notepad and no File Explorer while the old windows were still on
     screen — a desktop you cannot open anything from.
- **Fixed in** v0.2.17, `c0a43bc`. The font is the system's, loaded once per
  size for the whole run and freed after nothing is left that could draw. A
  built-in re-registering itself is an update in place; a module taking a
  built-in's name is still refused.
- **Note** found under gdb only after the harness was made to sign a user in
  first — without a signed-in account the restart took the other branch and
  did not crash, which is why the first three gdb runs looked clean.

---

## Labels

The same register covers everything else that happens to this system, because
"what changed and why" is one question:

| Label | For |
|---|---|
| `bug` | A fault. Titled `BG-nnn — …`. |
| `security` | A fault with a security consequence. Always also `bug`. |
| `regression` | Worked before, does not now. Always also `bug`. |
| `feature` | Something the system cannot do yet. |
| `patch` | A correction that is not a fault — wording, documentation, a claim. |
| `release` | One per version, closed when it ships. |
| `blocked-on-kernel` | Real, specified, and waiting on Phase 2. |

Areas, matching the [error code](ERRORS.md) letters where they apply:
`startup`, `storage`, `accounts`, `display`, `programs`, `network`,
`firewall`, `settings`, `applications`, `input`, `skins`, `help`, `build`,
`docs`.

## Adding a bug

1. Take the next number. The highest in this file is the last one used.
2. Write the entry — **Found in**, **Found by**, **Was**, and either
   **Fixed in** or **Open** and why.
3. Add its area to the `AREA` table in `scripts/make-issues.py`.
4. Run the script, which opens the issue with the same title and body:

   ```
   python scripts/make-issues.py --dry-run
   python scripts/make-issues.py
   ```

   Entries that already have an issue are left alone, so running it again is
   safe. One with a **Fixed in** line is created and then closed.
5. Paste the issue link under the heading, and reference the number in the
   commit that fixes it.

The **Was** field is the one that matters. A register full of symptoms is a
list of complaints; a register full of causes is something to learn from.
