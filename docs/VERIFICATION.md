# Verification findings

Claims about ReconOS that the repository contradicts.

`KF` is a fault in the kernel. `BG` is a bug in the OS. Neither covers the
thing this register is for: **a statement made as fact, in prose, that source
does not support** — a number in a handoff, a branch named in an instruction, a
limitation that stopped being true while nobody re-read it.

These are worth naming separately because of how they propagate. A `BG` is
found by something breaking. A `VF` is found only if somebody checks, and if
nobody does it is not inert — it gets built on. Every entry below was inside a
document about to be handed to a session that would have acted on it.

## How a finding is named

`VF-` and a number, assigned in the order the finding was **made**:

```
VF-001 ... VF-999
```

Numbers are never reused and never renumbered.

**On the collision this file could cause.** `docs/BUGS.md` records what
happened on 6 September 2026, when two sessions each took "the next number"
from the copy of the register in front of them and twelve faults were named
twice. A third register is a third chance at that. `VF` avoids it by not being
a shared sequence: numbers are assigned here and nowhere else, and nothing in
`BUGS.md` ever becomes a `VF` or the reverse. If a `VF` turns out to describe a
real defect in the system rather than a claim about it, it gets a `BG` or `KF`
of its own and this entry cites it — two numbers for two different things, not
one number moved.

## Open

None.

## Closed

### VF-001 — The correction to the syscall numbers was itself the error

- **Found in** a handoff prompt for the server role, before it was sent.
  **Found by** parsing the enum rather than re-reading it.
- **What it was** A previous statement gave `SYS_SOCKET` through `SYS_CONNECT`
  as 27–31. That was amended to 26–30 as a correction. The original was right;
  the correction was wrong, and shifted every number in the block by one.
- **How the wrong number was arrived at** By counting `grep "SYS_"` output.
  That output is not the enum. Lines like `SYS_WRITE to make room for
  SYS_OPEN would silently turn every` match the pattern and are prose inside a
  comment — the enum has 32 members and the grep returned considerably more.
  Stripping comments first and parsing the braces gives 27–31.
- **What it would have cost** The same amendment restated `SYS_WRITE` as 0,
  `SYS_CLOSE` as 9 and `SYS_READ` as 10. They are 1, 10 and 11 — `SYS_EXIT`
  holds 0. So eight numbers were wrong in a document whose stated purpose was
  to be the numbers somebody built against, under the words "verified against
  the enum today."
- **`scripts/check-syscall-numbers.py` exists to catch exactly this** and could
  not, because the fault was in prose. The script compares two headers; nothing
  compares a sentence to a header.
- **Confirmed by** running the checker (32 calls, both headers and 42
  hand-written numbers agree) and then injecting a one-place shift into
  `userland/include/recon.h` to watch it fail, before its pass was believed.

### VF-002 — The handoff told the new session to branch from a tree without sockets

- **Found in** the same document. **Found by** checking whether the base branch
  contained the calls the task depends on.
- **What it was** "Branch from `main`, call it `server`." `origin/main` declares
  no socket system calls at all — `SYS_SOCKET` appears zero times in its copy
  of `kernel/include/recon/kernel/user.h`. The session would have started on a
  tree where the thing it had been asked to build does not exist.
- **Where the work actually is** `origin/kernel` is 12 commits ahead of `main`,
  `origin/userland` 19, and neither has merged. `origin/userland` contains every
  commit on `origin/kernel` plus seven, and carries both the kernel socket
  implementation and the C library. That is the correct base.
- **A second copy of the same hazard** The local `ReconOS` checkout is 84
  commits behind `origin/main` — which is why `check-syscall-numbers.py` was
  absent from disk there while present on the branch. Any session judging the
  project by that working tree is reading a stale one.

### VF-003 — It described a raw syscall interface that nothing should use

- **Found in** the same document. **Found by** looking for a caller before
  documenting a calling convention.
- **What it was** The handoff presented the five syscall numbers as the
  interface to write against. `userland/libc/socket.c` is 259 lines of working
  BSD sockets over those calls — `socket`, `bind`, `listen`, `accept`,
  `connect`, `send`, `recv`, `setsockopt`, `getsockopt`, `shutdown` — with
  `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` and `netdb.h` beside it, and
  `userland/tests/test_libc_socket.c` holding it against the host's.
- **Why it was missed** The numbers were checked against the kernel header and
  agreed, so they looked verified. Correct numbers for an interface nobody
  should be calling is still the wrong instruction, and no check on the numbers
  would ever have said so.

### VF-004 — A limitation that had stopped being true

- **Found in** the same document. **Found by** reading the file the claim was
  about.
- **What it was** "Nothing has ever proved a socket fd works with `read`/`write`.
  You will be the first thing that actually tests this." `recon_init.c` already
  contains `exercise_a_socket()`, which holds a socket descriptor in ring 3 and
  covers creation, distinct descriptors, close, double-close, and a listener's
  documented `EAGAIN`. The header of `libc/socket.c` names the ring-3 program
  by path.
- **What is still true, narrowly** No bytes have moved over an accepted
  connection on this kernel. The host differential test says plainly that it
  cannot cover the kernel's behaviour — on the host those calls reach Linux.
  So the data path is the open question, not the descriptor.
- **The shape of this one** The claim was accurate when written and decayed.
  Nothing edits a sentence when the code beneath it changes.

### VF-005 — The reason given for taking a worktree was false

- **Found in** the same document. **Found by** running `git worktree list`
  while cleaning up.
- **What it was** "Take your own git worktree — the sessions share one checkout
  on disk and will collide otherwise." They do not share one. `ReconOS-kernel`,
  `ReconOS-userland` and `ReconOS-matrix` already exist beside the main
  checkout as registered worktrees.
- **Why it still matters that the instruction was kept** The advice was right
  and the reasoning was wrong, which is the harder version to catch — it reads
  as verified because the conclusion is sound. It also missed the convention:
  the existing names say the new one is `ReconOS-server`.

### VF-006 -- A reply that looked exactly like success came from the wrong process

- **Found in** the first attempt to reach the web server on the machine.
  **Found by** reading the body rather than the status line.
- **What it was** `curl` answered `HTTP/1.1 200 OK` with a dashboard and
  `Server: ReconOS/0.1`. It looked like the thing the whole day had been spent
  building.
- **What it actually was** A leaked child of this role's own host test suite,
  orphaned when an earlier run segfaulted, still holding the port. **QEMU had
  never started at all** -- `Could not set up host forwarding rule` -- and the
  serial log was one line long.
- **What gave it away** The body. That HTML is `DASHBOARD[]` from
  `server/tests/test_http_serve.c`, not anything `server_init.c` produces. The
  status line, the headers and the server name were all consistent with
  success; only the content was from the wrong program.
- **Fixed by** bounding the test child with `alarm(20)`. An orphan that answers
  is worse than one that hangs, because it cannot be told apart from a pass.
- **The general shape** Every signal said yes except the one nobody thinks to
  check. A result that arrives from *somewhere* is not a result that arrived
  from the thing under test.

### VF-007 -- A measurement that compared two different things

- **Found in** the check that the truncation fix had worked. **Found by** the
  numbers being off by exactly one.
- **What it was** `/` reported `declared=1194 received=1193`. That looked like
  the fix had left a one-byte bug behind, and a real one would look identical.
- **What it actually was** Two `curl` invocations -- one for the headers, one
  for the body -- against a page that embeds `requests served` and `bytes
  sent`. The counters moved between the two calls, so the declared length came
  from a different response than the body did.
- **Measured properly** Header and body from a single response: 1193 and 1193,
  and five consecutive runs of the largest page all matching.
- **Why it belongs here** The instrument was wrong, not the system, and an
  instrument that is wrong in the direction of "there is still a bug" wastes
  exactly as much time as one that is wrong the other way -- and is much more
  likely to be believed.

### VF-008 -- "Rebuilt" was not the same claim as "built the right thing"

- **Found in** switching `ROLE` to compare a workstation boot against a server
  boot. **Found by** the two producing byte-identical images, which they
  cannot.
- **What it was** `make` compares timestamps and knows nothing about variables,
  so changing `ROLE` left the init newer than every source. The build printed
  *Nothing to be done*, exited 0, and the machine booted the other role's
  program. A comparison meant to settle a question was silently between a thing
  and itself.
- **And the first fix was worse.** A stamp file named after the role does force
  the rebuild -- but written above `all` in the Makefile it became make's
  **default goal**, so `make ARCH=x86_64` touched a file, printed "is up to
  date", exited 0 and built no kernel whatsoever. Two consecutive builds
  reported success and produced nothing.
- **Both are recorded in `kernel/Makefile`** beside the rule, where the next
  person changing it will read them.
- **Why it belongs here** Twice in a row the build system reported success for
  work it had not done, and both times the only thing that noticed was a figure
  that could not be true. `rc=0` is a claim like any other.

### VF-009 -- "`connect` exists, so this is buildable today"

- **Found in** `docs/WEB.md`, written by this seat on 15 September. **Found by**
  measuring `connect` on the machine a day later.
- **What it was** The reverse-proxy row read *`connect` exists, so this is
  buildable today*, and the discovery design in `docs/SERVER.md` was built on
  the same assumption -- a TCP sweep, described as the way through precisely
  because it "works with exactly the five calls that exist today".
- **What is actually true** `connect` returns `SYS_OK` for a port nothing is
  listening on. Measured: `connect(closed port)=0`, and a `write` straight after
  a connect that *should* have worked answers -1, because the handshake has not
  finished and there is no way to wait for it. A sweep cannot tell who answered.
  Neither a proxy nor discovery is buildable.
- **How the wrong claim was arrived at** By checking that the *call* existed
  rather than what it did. The five syscalls were verified against the enum --
  carefully, in the entry that opens this file -- and "exists" was then read as
  "works as a caller would expect". A present call with surprising semantics
  passes every check that asks whether it is present.
- **Why it belongs here** It is the same shape as VF-004, and by the same seat.
  That one was a limitation that had stopped being true; this is a capability
  that was never true. Both were written down as fact after a check that could
  not have distinguished them.
- **Filed** as its own entry at the top of `docs/KERNEL-WANTS.md`, with the
  measurement. No number claimed; it is the kernel's.
- **Resolved, 16 September 2026.** The kernel session fixed it as KF-244, and
  said so in the same message that reported this entry's subject. Worth
  recording what happened next, because it is what this register is for: **the
  announcement carried three numbers and all three were wrong.** `SYS_CONNECT`
  was given as 30 and is 31; `SYS_EAGAIN` as -12 and is -4; `SYS_EIO` as -4 and
  is -9. The names were right every time.

  Checking them before writing code took one command. Building against them
  would have swapped *in flight* and *refused* exactly -- so every connection
  that was merely unfinished would have been abandoned, and every permission
  refusal retried for ever. That is the precise failure the announcement itself
  warned about, reached by trusting its own table.

  `server/dial.c` therefore compares no numbers at all. The kernel session's
  own conclusion, unprompted: *"building against the names is correct and I'd
  keep doing it."*

### VF-010 -- the JSON path kept the coincidence that had just been removed from HTML

- **Found in** `server/init/server_init.c`, 16 September 2026. **Found by**
  reading back a refusal made one version earlier.
- **What it was** `/api/status` and the reply from `POST /api/name` wrote the
  machine's name into a JSON string with no escaping. Safe -- because
  `server_name_split` admits letters, digits and the hyphen and refuses
  everything else, so a quote cannot reach it.
- **Why that is the finding and not the defence.** That is exactly the argument
  that had been true of the dashboard's `value="..."` until it was not, and
  `server/http/escape.c` exists in 0.9.0 to stop depending on it. The same
  dependency was still load-bearing in the JSON path, one directory away, for
  four versions.
- **How it surfaced.** In 0.12.0 `/api/log` was refused as JSON, and the
  refusal named the hazard precisely: a log line holds a request target, which
  is text a client chose. Two endpoints were already emitting client-adjacent
  text as JSON while that refusal was being written. The reasoning was right
  and its scope was assumed rather than checked.
- **Why it belongs here** A fix applied where a fault was found is not a fix
  applied where the fault lives. Nothing asked the obvious follow-on question --
  *where else does this pattern appear* -- and the answer was in the same file
  as the thing being protected.
- **Fixed** in 0.14.0. `server/http/json.c`, and every string in every JSON
  endpoint now goes through it, **including the ones that cannot hold a quote
  today**: a rule with an exception for known-safe values is a rule the next
  person to add a field has to apply correctly and silently.
- **Measured on the machine**, not just in the suite. A quote and a newline
  were put into a service description, the kernel rebuilt and booted, and the
  bytes on the wire read `\"` and `\n` as two-character escapes, which a strict
  parser then read back as the original string. Without the wiring the quote
  would have closed the string. The instrumentation was reverted afterwards.

### VF-011 -- a server-side failure answered 400, which blames the client

- **Found in** `http_status_for`, while wiring VF-010's escaper. **Found by**
  needing a status for "this server could not build its own answer" and
  discovering there was not one.
- **What it was** `http_status_for` ends `default: return 400`. Every JSON
  handler already returned `HTTP_EBODY_LONG` when its own `snprintf` overran
  its own buffer -- which is 413, *Content Too Large*, about the request. Any
  code it did not recognise became 400, *Bad Request*, also about the request.
  There was no way for a handler to say the fault was the server's.
- **What that costs** A client told 400 or 413 acts on it: it shortens the
  request, drops a header, stops retrying. None of that can help, because the
  request was never the problem. The one signal that would have been useful --
  *try again, this end is broken* -- was unreachable.
- **Why the default is not itself wrong.** For a request-side verdict nobody
  has mapped yet, 400 is the right guess. It only became wrong when a verdict
  arrived that was not about the request.
- **Fixed** in 0.14.0. `HTTP_EINTERNAL` answers 500, named explicitly in the
  switch rather than left to the default, with a check in the suite.

### VF-012 -- thirteen suites and no way to run them

- **Found in** this seat's own habits, 16 September 2026. **Found by** writing
  the runner that should have existed at three suites.
- **What it was** Every suite had been compiled and run by hand, one `gcc` line
  at a time, for thirteen versions. That works exactly as long as the person
  typing remembers every suite, and the failure when they do not is silent: the
  suites that ran pass, the report says the suites passed, and the one that was
  skipped is the one that would have failed. The check-count figures in the
  README were assembled the same way.
- **What the runner found in its first minute** Two suites did not build under
  its flags. `-std=c11` defines `__STRICT_ANSI__`, glibc hides `kill` behind
  `__USE_POSIX`, and the two socket suites -- which fork a child and signal it
  -- got an implicit declaration each.
- **And the suites were right.** `CMakeLists.txt` sets `CMAKE_C_EXTENSIONS ON`,
  which is `-std=gnu11`. The runner was measuring against a dialect the project
  does not build with, which is its own version of this register's recurring
  fault: a check that cannot distinguish the thing from a near neighbour. The
  script was fixed, not the suites.
- **Why it belongs here** The first honest total this project has had is 474
  checks across thirteen suites, produced by one command. The previous figure,
  445 across twelve, was assembled by hand and was not wrong -- but nothing
  except memory made it right.
- **`scripts/server-tests.sh`** reads its target list out of `CMakeLists.txt`
  rather than keeping one, for the reason the status table is generated from an
  X-macro: two lists drift, and the drift is invisible until something is
  already wrong.

### VF-013 -- the 64 KiB body limit had never once been reachable

- **Found in** the upload endpoint, 16 September 2026. **Found by** posting a
  4 KiB file and getting an empty reply.
- **What it was** `serve.c` read a zero from `recv` as end-of-stream. On this
  kernel a zero means *nothing has arrived yet* -- `errno` is 0 and the
  connection is open. So every request whose bytes did not all arrive in the
  first read was dropped without an answer.
- **The threshold was about 4 KiB of total request**, which is why nothing had
  noticed. Every request this server had ever been sent -- a page, a status
  poll, a rename with a twelve-character name -- fits in one read. The first
  thing that did not was the first upload.
- **The same file already knew.** `send_all`, in the same source, carries a
  stall counter and a comment saying in as many words that a zero from `send`
  means the buffer is full rather than that anything is wrong. The receive side
  is the mirror image and had never been looked at. **A fault understood in one
  direction is not a fault found in the other**, and this is the third entry in
  this register with that shape -- VF-010 was the same, one file over.
- **The first fix was wrong in an instructive way.** Retrying the zero, bounded
  at 200000 attempts, still failed: `have` stayed at 2880 through every one of
  them. It would have been easy to conclude the connection was dead. Sending
  the same bytes slowly proved it was not -- 20 KB arrived complete in 400-byte
  pieces while 3.7 KB in one burst did not. **The size was never the limit;
  the pacing was.**
- **What it cost to find** was one measurement from each end. The instrumented
  `recv` said the server was awake and getting nothing; the paced client said
  the kernel was capable of more. Neither alone distinguishes a dead connection
  from a stalled one.
- **Filed** as its own entry at the top of `docs/KERNEL-WANTS.md`, with both
  tables. No number claimed; it is the kernel's.
- **Fixed here** in 0.15.0: a zero is *not yet* mid-request and *nothing more*
  between requests, the site supplies whatever its system does to let other
  work run, and the wait is bounded by a **clock** rather than an attempt
  count. Fifteen seconds, which on the measured rate is about fifteen
  kilobytes.
- **And the deadline found one more thing.** A request cut off at it was
  closed silently and left no log entry -- which contradicts what `log.h` says
  the log is for, in the exact case somebody would go looking. It now answers
  **408** and records it.
- **What is still wrong, and is written down rather than rounded off:**
  `HTTP_BODY_MAX` says 64 KiB and the deadline admits about fifteen. The two
  numbers measure different things and both are true; they will agree again
  when the kernel entry is answered.
