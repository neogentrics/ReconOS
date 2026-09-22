# What each application does when its window is closed

Closing a built-in application does not destroy its window. `recon_apps.c`
calls `recon_appwin_hide`, the window keeps everything it had, and opening the
application again shows the same one back — which is deliberate and worth
having: a browser that forgot its tabs every time would be a worse browser.

What it means is that **a destructor runs at shutdown and never when somebody
actually closed the thing.** Four faults came out of that in one week, and
`scripts/closed-not-destroyed.py` now asks the question of every application at
once. This file is where the answers live, so that a settled question stays
settled instead of being re-asked every run.

## The two hooks, and which question each answers

| | fires on | the question it answers |
| --- | --- | --- |
| `visibility(user, bool)` | show, hide, minimize, restore | *should I still be doing work?* |
| `closed(user)` | `recon_appwin_hide` alone | *is this over?* |

**They are not interchangeable, and the difference has consequences.** A
browser that ended its session on `visibility(false)` would sign you out every
time you minimised the window; a Task Manager that only stopped sampling on
`closed` would go on reading the process table while minimised.

## What was found, and what was done

| application | found | ruling |
| --- | --- | --- |
| `recon_web.c` | a session cookie outlived the browser closing | **fixed**, v0.4.74 — `closed` saves the jar and drops session cookies |
| `recon_player.c` | the music kept playing with no window | **fixed**, v0.4.75 — `closed` stops playing |
| `recon_mailwin.c` | the connection and the password outlived the window | **fixed**, v0.4.75 — `closed` disconnects and wipes the password |
| `recon_control_panel.c` | the registry stayed unlocked, having said it would not | **fixed**, v0.4.80 — `closed` re-locks and wipes both secrets |
| `recon_taskmgr.c` | *nothing* — the instrument was wrong | already correct: `visibility` disarms the timer, and takes two samples on the way back because CPU is a rate |
| `recon_notepad.c` | the document, its undo history and a loaded font survive a close | **keep.** That is what somebody wants back. A Notepad that forgot your text when you closed it would be losing work, not tidying up |
| `recon_terminal.c` | the command session survives a close | **keep.** Coming back to the shell you left is the point. Worth knowing: a long-running command keeps running, which is the same answer a terminal gives everywhere else |
| `recon_calendar.c` | a half-typed appointment survives a close | **keep**, for Notepad's reason. Work in progress is work |
| `recon_help.c` | the topic list is freed at shutdown | **keep.** Freeing it on close would only make reopening slower; nothing outside the struct is affected |
| `recon_photos.c` | a dialog is cancelled at shutdown, so one open at the moment of a close may outlive its window | **open** — see below |

## The one still open: a dialog whose window has gone

`photos_destroy` opens with `recon_shell_cancel_dialog(ph->shell, ph)`, and its
comment says why: *"Photos is the first application here that can both ask
something and be closed while it asks."*

If that state is reachable, then closing the window while the question is up
leaves a dialog on screen belonging to nothing — and answering it calls back
into a window that is hidden rather than freed, so it would not crash, it would
simply act on a window nobody can see.

**It is not settled because I could not reach the state to photograph it.** The
run that would settle it:

```
./scripts/look.sh --password reconos --pause 2 \
    "apps Photos" "ui click <the control that asks>" "state" \
    "ui click <the window's X>" "state"
```

The second `state` says whether `dialog:` is still open with no window under
it. If it is, Photos needs a `closed` that cancels the question — and the fix
is one line, the same shape as the other four.

Recorded as open rather than guessed at, because the alternative is a row in
this file that says *keep* about something nobody checked.
