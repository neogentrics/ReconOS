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

### VF-014 -- fifteen suites, none of which could see the worst limitation

- **Found in** the connection pool, 16 September 2026. **Found by** writing the
  suite for it and noticing what the existing ones did not cover.
- **What it was** `docs/WEB.md` called single-connection serving *the most
  serious limitation in this document* -- a denial of service costing the
  attacker one socket -- and 548 checks across fourteen suites had nothing to
  say about it. Not one of them failed, and not one of them could have.
- **Why not** Every suite opens one connection, exchanges, and closes. That is
  the shape that behaves identically whether the server handles one connection
  or a hundred. The limitation was known, written down, and untested, and the
  test coverage was excellent everywhere the limitation was not.
- **The new suite fails 4 of 12 against the old server**, and the eight that
  pass are the interesting half: the stuck client is still served correctly,
  with the whole body, across both halves of its request. **A suite that
  follows one client would call the old server perfectly good.**
- **And the document was wrong about the cause.** It said the fix needed *the
  kernel to report readiness on more than a listener; today `accept` is the
  only call that answers `EAGAIN`*. `recv` reports readiness too -- 0 with
  `errno` 0 -- and always had. It was being read as a closed connection, which
  is VF-013.

  So the missing capability and the silent bug were the same fact seen from two
  sides, and both were written down separately without either being recognised
  in the other. The concurrency was not blocked on the kernel; it was blocked
  on a misreading that had also been quietly breaking every request over 4 KiB.
- **Why it belongs here** Two entries in a row where the answer was already in
  the tree. VF-010 was a fix applied in one file and not the one next to it;
  this is a capability described as absent while the code depended on it
  working. **Reading what is written down is not the same as checking it**, and
  a limitation nobody tests is a limitation nobody has to be right about.

### VF-015 -- the same patch-script fault, four times, finally made impossible

- **Found in** this seat's own working habits, across several days.
- **What it was** A Python patch script turning an escaped newline inside a C
  string literal into a real newline, splitting the literal across two lines
  and breaking the build. It happened four times. Each time the conclusion
  written down was *stop using scripts for C string literals*; each time it
  happened again within hours.
- **The fourth time was the checker.** The tool written to catch this fault was
  itself written by a patch script, and that script broke the newline
  comparisons inside its own scanner. The check existed for ten minutes before
  it could run clean, because the thing it detects had already happened to it.
- **Why it belongs here** A resolution that has failed four times is not a rule,
  it is a hope. This register is full of entries where the answer was a
  structural change rather than more care -- the X-macro that stopped the status
  table drifting, the stamp file that stopped `ROLE` building the wrong image,
  the runner that stopped the suites being run from memory. **This is the same
  answer applied to the person rather than the code.**
- **`scripts/check-c-literals.py`** walks every C source in the role's tree and
  reports a literal left open, and `scripts/server-tests.sh` runs it first --
  before anything is compiled, because a broken literal makes every suite below
  it fail for a reason that has nothing to do with the code.
- **Watched failing, both ways**: clean across 45 files, and against a
  deliberately split literal it names the file and both lines. It also caught a
  real one on its first run after being wired in -- a `printf` broken by the
  very next patch script, before the compiler ever saw it.
- **A false-positive pass mattered here.** The first version walked each line in
  isolation and reported twenty, every one a quotation mark inside a multi-line
  block comment. A checker with twenty false positives is a checker somebody
  switches off, which is worse than no checker because it also looks like
  diligence.

### VF-016 -- two smaller faults, both found by writing a test rather than by running one

- **The connection pool could guard one site with another's secret.** Found
  while writing the check that a guarded route with no policy answers 500,
  which needed a second site to exist. `CONNS` is one static array and
  `conn_step` took the site as an argument, so a connection accepted for one
  site and stepped during another's call would be routed by the wrong table and
  guarded by the wrong policy.

  Nothing did that yet -- the server serves one site. It is four bytes in the
  slot to make impossible, against a paragraph telling the next person not to,
  and it is what virtual hosts will need anyway.

- **A counter the child incremented and the parent read.** The new check that
  the guard policy was *actually consulted* used a plain `int`, which `fork`
  copies -- so the parent read its own zero and reported the policy as never
  asked, on a server asking it correctly every time.

  `test_http_serve.c` carries a comment two hundred lines above explaining
  exactly this trap, about its byte counter, written by this seat. **Reading a
  warning is not the same as applying it**, which is the same shape as VF-010
  and VF-014, and is now three entries in this register.

### VF-017 -- "DNS is blocked" was half true, and the wrong half was believed

- **Found in** `docs/SERVER.md` and `docs/WEB.md`, 16 September 2026. **Found
  by** reading the kernel's own source before accepting this role's own note.
- **What it said** DNS and DHCP were both **blocked** on "an unconnected
  datagram socket", with a section headed *What is blocking the first two
  services, exactly* explaining that an unconnected datagram socket cannot be
  opened from user mode.
- **What is actually true** That covers DHCP, which must broadcast, and a DNS
  *server*, which must reply to whoever asked. **A resolver is a connected
  datagram** -- one known server, one question, one answer -- and
  `kernel/core/socket_file.c` says in as many words: *a connected UDP socket
  works through `write` today; an unconnected one is refused rather than
  half-served.*
- **Measured before anything was built on it**, because this register exists
  for exactly the opposite mistake:

  ```
  udp probe: fd=5 connect=0 write=29 read=61 tries=2619
  udp probe: id=1234 flags=8180 qd=1 an=2 last=104.20.23.154
  ```

  A real query to 10.0.2.3:53 and a real answer for example.com, on the
  machine, from a temporary probe that was removed once the resolver replaced
  it.
- **Why it belongs here, and why it is the fourth of its kind.** VF-004 was a
  limitation that had stopped being true. VF-009 was a capability that was
  never true. VF-014 was a capability described as absent that the code already
  depended on. This is a blocker **written at the right time about the right
  thing, whose scope grew in the writing**: two services were grouped under one
  sentence, the sentence was true of one of them, and nobody asked which.
- **What it cost** A resolver that could have been written any time after the
  socket layer landed. `docs/SERVER.md` now carries two rows where it carried
  one, because the built half and the blocked half of a protocol are not one
  status.
- **The habit that keeps working** is reading the source of the thing said to
  be missing. That is how VF-014 was found and how this was. A document
  describing another seat's code is a claim about it, and it ages.

### VF-018 -- a failure that reported a number no server had given

- **Found in** `GET /api/resolve` on the machine, minutes after it first
  worked. **Found by** asking for a name the encoder refuses and reading the
  whole reply rather than the field that was being tested.
- **What it was** `dns_resolve` returns before sending anything when a name
  cannot be encoded, and left the caller's `struct dns_result` untouched. The
  endpoint printed `"rcode":3` -- a plausible *no such name* -- from whatever
  the previous request had left on the stack. The number looked exactly like an
  answer and no server had been asked.
- **The suite could not have caught it as written.** Every check in
  `test_dns.c` passed a fresh structure and read only the fields the call fills
  in, which is the natural way to write a check and the one shape that cannot
  see this. The check added for it **dirties the structure first**, which is
  what a real caller does by reusing one.
- **Why it belongs here** The parse path cleared the structure and the early
  returns did not, so the safety was in the common case and missing from the
  exceptional one. That is where this register keeps finding things: 0.10.0's
  early return that skipped a stylesheet, VF-011's handler failures wearing the
  client's status. **An exceptional path is the one nobody exercises twice.**

### VF-019 -- a merge that fixed one fault and broke another, caught in one boot

- **Found in** kernel 0.2.48, merged from `origin/kernel` at 95fd008 on 16
  September 2026. **Found by** re-running this role's own endpoints after the
  merge rather than only its suites.
- **What the merge fixed** KF-244, exactly as announced: `connect` no longer
  answers `SYS_OK` for a port nothing is listening on. The standing
  measurement changed from `connect(closed port)=0 write=-1` to an honest *in
  flight*, which closes the reporting half of VF-009.
- **What it broke** `connect` on a **datagram** socket began answering
  `SYS_EIO` every time. That is the only shape of UDP a program can use, and
  it is what this role's resolver -- written hours earlier, as VF-017 -- is
  built on. DNS went from real addresses to `resolved:false` on the first boot
  after the merge.
- **Where** `sys_connect` asks `socket_connect_progress` about every socket,
  and that function opened `if (!s || s->type != SOCK_STREAM || s->conn < 0)
  return SOCKET_PROGRESS_FAILED;`. For a datagram, `socket_connect` had just
  succeeded and set `connected` -- and the progress check then reported a
  failure, because it read *not a stream* as *did not make it*. **Those are
  different facts**, and collapsing them is the same shape as VF-011, where a
  server-side failure wore the client's status.
- **Diagnosed by measurement, not by reading.** Three controls separated it:
  the machine still served inbound TCP (200), the kernel's own DHCP exchange
  still completed, and the network came up identically on both boots. So the
  card and the stack were fine and it was a program's UDP specifically.
- **The fix was then proved rather than asserted** -- applied, rebuilt, booted,
  and the same request answered with real addresses again. Reported in
  `docs/SIGNALS.md` with the diagnosis and the patch. **No KF number claimed;
  it is the kernel session's to number**, which is the standing rule here since
  the collision on 6 September.
- **Why nothing else caught it** Nothing else in the tree connects a datagram
  socket. The kernel's own socket probe covers streams, and its DHCP client is
  inside the kernel and never goes through `sys_connect`. The resolver that
  found this was two hours old. **A capability with exactly one user is a
  capability whose regressions depend on that user still running.**
- **Why it belongs here** The suites all passed. 684 checks, seventeen suites,
  green before and after the merge -- because every one of them is a host
  suite and the fault is in the kernel. A merge verified by running the suites
  alone would have been called clean.

### VF-020 -- outbound TCP had never worked, and the reason was two bytes of arithmetic

- **Found in** kernel 0.2.48 after the merge, 16 September 2026. **Found by**
  capturing packets on the virtual NIC rather than reasoning about the code.
- **What it was** Every outbound SYN carried a **wrong TCP checksum**, so every
  correct peer discarded it without a word -- no SYN+ACK, and not even a RST
  for a port with nothing listening. `dial.c` reported *timed out* for every
  target, which is exactly what a silently dropped segment looks like from
  above.
- **How it was pinned** The capture gave the bytes, and recomputing both
  checksums showed the IP one correct and the TCP one wrong -- **by the same
  amount on both packets, 0x0C0F**. That is `0x0A00 + 0x020F`, the two halves
  of this machine's own address. A constant difference is a missing constant
  term, and the missing term named itself.
- **Why** `socket_connect` binds to `IPV4_ANY` before opening, so `tcp_open`
  was handed 0.0.0.0 as the local address and summed the pseudo-header over a
  source of zero -- while the IP layer wrote the device's real address into the
  header on the way out.
- **Why it had never been noticed** An accepted connection takes its local
  address from the packet that arrived, so inbound was always correct. Every
  test of this kernel's sockets before today was inbound: a client on the host
  talking to the machine's web server. **Outbound had one user, written three
  versions ago, that could not run until `connect` stopped lying about its
  result.** VF-009 hid this behind a bug of its own.
- **Fixed and proved on the wire, not argued.** Before: a SYN and silence.
  After: a RST for the closed port, a full three-way handshake for the open
  one, and the listener on the host logging `ACCEPTED`.
- **And it revealed a second fault** underneath, which is the kernel session's
  and is reported with its timings: the handshake now completes on the wire and
  `connect` still never reports it. Replies arrive in about a millisecond and
  are not acted on for six seconds, until the peer retransmits. Thirty seconds
  and 7.8 million polls give the same answer as two.
- **Why it belongs here** Three sessions have written about this kernel's
  sockets and none had looked at a packet. The code review that would find a
  missing pseudo-header term is possible and nobody did it; the capture took
  four minutes and named the byte. **When something is dropped silently by
  everything downstream, the only witness left is the wire.**

### VF-021 -- a round trip of zero milliseconds to the other side of the internet

- **Found in** the NTP client, 17 September 2026, minutes after it first
  measured a real offset. **Found by** reading the whole line rather than the
  number that was being worked on.
- **What it said** `the clock: behind by 1778 ms, round trip 0 ms, stratum 3`.
  The offset was the figure under test and it looked right. The round trip
  beside it was zero, to a server several thousand kilometres away, and **no
  packet makes that trip in under a millisecond.**
- **What it was** `SYS_WALLTIME` is declared in nanoseconds and counts whole
  seconds. Measured directly: five reads gave `1789646397000000000` every time,
  low nine digits zero, and 200000 yields moved it by exactly 1000 ms.
- **Why the offset still looked plausible** Two of its four terms are the
  *server's* timestamps, which are fine-grained, so it reads 1170 ms rather
  than a whole number of seconds. Only the round trip is computed from this
  machine's two coarse readings alone, so only the round trip collapsed --
  **the broken number was the one nobody was looking at, and the number being
  tested was the one that hid it.**
- **Why it belongs here** A figure that cannot be true is worth more than a
  figure that looks right, and this one was sitting beside the figure under
  test for two boots before it was read. The suite could not have caught it:
  `test_ntp.c` drives the arithmetic with timestamps it supplies itself, and
  the arithmetic is correct. The fault is in what the machine's clock can
  express, which no pure check can see.
- **What changed** The round trip is not printed. The offset is printed with
  its real uncertainty -- *about 1170 ms, give or take a second, this machine's
  clock counts whole ones* -- rather than to a millisecond it cannot support.
  Reported in `docs/SIGNALS.md`; a finer wall clock makes it useful and nothing
  else has to change.
- **And a smaller one beside it**: the first version of this service pointed at
  QEMU's gateway for NTP and got silence, because slirp answers DNS on 10.0.2.3
  and answers NTP nowhere. It resolves `time.cloudflare.com` with this role's
  own resolver now, which is what a real machine does and is the first thing
  here to use one built capability to reach another.

### VF-022 -- the guard broke the console's own form, and curl hid it for three versions

- **Found in** the dashboard, 17 September 2026. **Found by** reading the page
  while adding rows to it, and noticing what its form posts to.
- **What it was** `GET /` has offered a rename form since long before there was
  a guard. 0.17.0 marked `POST /api/name` guarded. **A browser form cannot send
  an `Authorization` header**, so from that moment the form answered 401 to
  every submission -- on the one page a person actually looks at.
- **Why three versions of testing missed it** Every check of that endpoint was
  `curl -H 'Authorization: Bearer ...'`, which is the client that *can* send
  the header. The endpoint was tested exhaustively and the page it sits on was
  never submitted. **Testing the API is not testing the console**, and the
  console is the part with a person in front of it.
- **Confirmed rather than assumed**: a POST shaped exactly as a browser sends
  one -- form encoding, no `Authorization` -- answered `401 Unauthorized`, while
  the page continued to render the form that produces it.
- **Fixed** by accepting the token from a form field as well as the header. The
  policy hook now receives the body, which it needs and did not have.
- **And the line that was already drawn stays drawn.** A token in a **query
  string** is still refused: it is written into this server's own access log,
  sent onward in `Referer`, and kept in history. A **POST body** is none of
  those -- it is exactly as exposed as the header beside it and no more. That
  distinction is in `auth.h` so the next person does not have to rediscover
  which half of it was the objection.
- **Why it belongs here** A guard is a change to every caller of the thing it
  guards, and one of the callers was this project's own page. The suites could
  not have caught it: `test_auth.c` is pure and `test_http_serve.c` builds its
  own site. Nothing in the tree knew the dashboard and the route were connected.

### VF-023 -- four boots spent on a kernel I had built for the other role

- **Found in** my own verification procedure, 17 September 2026.
- **What it was** The last step of landing 0.20.0 was a check that *both* roles
  build -- `make ROLE=server`, then `make ROLE=workstation`. That leaves the
  **workstation** kernel in `kernel/build/`, and the next thing done was to boot
  it expecting a server. It booted, ran its self-tests, and never served, which
  is exactly correct behaviour for a workstation.
- **What it looked like** A machine that hangs after the socket self-test. Three
  identical stuck boots, byte-for-byte the same log length, which read as a
  deterministic fault in freshly committed work.
- **Why it took four boots** Because the symptom is indistinguishable from a
  real hang, and because `/tmp` is shared with other sessions -- the reference
  logs that would have shown the difference had been cleaned up, along with the
  16 GB test disk, which sent the diagnosis down a second wrong path first.
- **Why it belongs here** This is VF-008's family: the `ROLE` stamp means the
  **last build wins**, and a verification order of *server, workstation, boot*
  guarantees booting the wrong one. The fix is procedural and is written here
  rather than in a comment nobody reads at the right moment: **build the role
  immediately before booting it**, never as the second half of a two-role check.

### VF-024 -- the README claimed 736 checks and its own table summed to 735

- **Found in** `server/README.md`, 17 September 2026. **Found by** the userland
  session sending word, through Joshua, about blunt search-and-replace edits --
  and naming the gap as *doing the careful thing for code and not for `docs/`*.
  Adding the table up took a minute.
- **What it was** The summary box said 736 and the suite table summed to 735.
  One row said 93 where the suite had grown to 94. That number had been quoted
  in the README, the board and three commit messages.
- **Why** Several documentation edits here were bare `s.replace(old, new)` with
  no assertion on the match count, on the unexamined grounds that they were
  "just docs". The code edits beside them all asserted. **The discipline was
  applied where a compiler would have caught the mistake anyway, and dropped
  where nothing would.**
- **Why it belongs here** The table was a **third list**, beside
  `CMakeLists.txt` and the suites themselves. This register already carries two
  entries about lists nobody derives -- the status table that drifted twice
  before becoming an X-macro, and the suites run from memory for thirteen
  versions before `server-tests.sh`. This is the same fault in the one document
  that carries numbers, and it was written by the seat that wrote both fixes.
- **What it is now** `scripts/server-tests.sh` checks the README against the
  run it just did: every row, the total, and the suite count. **Watched
  failing**, a row edited to 71 against a real 72:
  `server_dns: the table says 71, the run gave 72`, exit 1.
- **And it uncovered a latent fault of its own.** The runner has parsed target
  names out of `CMakeLists.txt` since the day it was written, and that file is
  checked out with **CRLF** endings here -- so every name carried a trailing
  carriage return. It never mattered while the name was only a filename. It
  became visible the moment the same name was compared against text in a
  document. **A fault that waits for a second reader**, which is precisely the
  shape the userland session's message described.
- **And a second mistake in the fix itself**, worth recording because it is the
  same class: the first version *derived* the README's row name from the
  executable name, turning `recon_server_serve_tests` into `server_serve` while
  the table correctly says `server_http_serve`. An invented mapping is a fourth
  list with extra steps. It reads CMake's own `add_test(NAME ...)` now.

### VF-025 -- an upload endpoint that had never been shown to keep anything

- **Found in** the test rig rather than the code, 17 September 2026. **Found
  by** rebuilding a disk that had been lost, and noticing what became testable
  again.
- **What it was** `POST /api/upload` has existed since 0.15.0 and had been
  verified in every way except the one that matters: **nothing had ever checked
  that an uploaded file was still there after a reboot.** Every test of it ran
  inside one boot, where a write that never reached the volume and a write that
  did look identical.
- **Why** The 16 GB disk the check needs lived in `/tmp`, which is shared with
  the other sessions on this machine and is cleaned without warning. It
  disappeared twice on 17 September, taking every reference boot log with it --
  and the second loss sent a diagnosis down the wrong path for four boots,
  because the logs that would have shown a workstation kernel booting instead of
  a server were gone too (VF-023).
- **So the answer was not a test, it was somewhere durable to put the disk.**
  `scripts/server-disk.sh` builds the role, makes the image under
  `kernel/build/` where nothing else reaches, installs onto it and prints the
  line to boot it with. Regenerated rather than committed.
- **Now measured, across two boots on one volume:** upload answers 201, the same
  name answers 409 in the same boot, the machine is restarted with a **new token
  and a new boot**, the web root reports *already there* rather than being
  written again, the same name still answers 409 -- and a name never used
  answers 201. The file survived, the layout was not clobbered, and the refusal
  to overwrite held across a restart.
- **Why it belongs here** *Accepted* and *kept* are different claims, and only
  one of them had evidence for two versions. A durability property tested inside
  a single boot is not tested at all -- and the reason it went untested was not
  difficulty but a missing disk, which is the least interesting possible cause
  and cost the most time.

### VF-026 -- the request counter had been three times the truth since the pool landed

- **Found in** `/api/status` and the dashboard, 17 September 2026. **Found by**
  a different number not adding up: 211 requests served against only two log
  segments, when six were due.
- **What it was** `FACTS.served` was incremented whenever `http_serve_once`
  answered "something happened". Before the connection pool that meant one
  connection served; **after 0.16.0 it means one step of one connection** --
  accepting it, reading part of a request, answering it. Same call, same
  return value, different meaning.
- **Measured rather than estimated.** Eleven requests were made against a fresh
  boot and the counter moved by **thirty-three**. The log ring, which is
  incremented once per finished response, held twelve -- correct, and the
  contradiction that made the fault visible.
- **Why it survived five versions** Nothing compares the two. The dashboard
  shows the counter, `GET /api/log` shows the ring, and no check has ever
  looked at both in one breath. Each was individually plausible: a busy-looking
  number on a page nobody was auditing, next to a list that was right.
- **Why it belongs here** The pool changed what `http_serve_once` *returns* and
  every caller was reviewed for whether it still compiled, not for whether the
  value still meant the same thing. **A function whose meaning changes while
  its signature does not is a change no compiler reports** -- which is the same
  shape as VF-019, where a progress check was applied to a socket type it was
  never written for.
- **Fixed** by counting in `note`, which runs exactly once per response --
  including the ones refused before any handler saw them, which are requests
  the machine answered and should be counted. It is the same event the log
  records, so the page's figure and the log's length can no longer drift.

### VF-027 -- the access log now survives a reboot, and the reason it could not was wrong

- **What was recorded** From 0.12.0: the log is in memory because the C library
  drops `O_APPEND`. It does drop it, and **appending needs the ability to
  append, not that flag** -- `SYS_SEEK` exists, so seek-to-end then write is
  the other route. The stated blocker was never the real one.
- **The real one**, measured against a file that exists: plain `OPEN_WRITE` is
  refused; `OPEN_CREATE`, documented *it must not already exist*, **succeeds**
  on a file that does; `OPEN_REPLACE`, documented *it must exist*, is
  **refused** on one. The only door that opens contradicts its own
  documentation, and a log built on that is a log built on a fault.
- **So the shape changed instead of the door.** `SYS_CREATE` is documented,
  works, writes a file whole in one transaction and refuses to overwrite --
  because ReconFS writes whole files. A log that appends is impossible here; a
  log that **rotates** is natural, which is how log-structured systems are
  built on purpose elsewhere and was arrived at here because nothing else was
  available.
- **The two mistakes it would have had**, both watched failing at 10 of 34:
  names without zero padding, so `10.log` sorts before `9.log` and every reader
  that lists a directory gets the log out of order; and a segment number
  starting from zero each boot, which -- because `SYS_CREATE` refuses to
  overwrite -- would not clobber the previous run but would **fail every write
  from the second boot onwards, silently.** A log that has stopped looks
  exactly like a server with nothing to report.
- **Measured across two boots on one volume:** boot one reported *continuing at
  segment 000001*, answered seventy-two requests, and boot two reported
  *continuing at segment 000003* -- two segments written and the numbering
  carried across the restart, while the in-memory ring came back empty, which
  is the contrast the whole thing exists for.
- **What it costs, written down rather than discovered:** an entry is durable
  when its segment is written, not when it is recorded. Up to
  `LOGFILE_FLUSH_EVERY` entries live only in the ring, and a machine that loses
  power loses them. That is the weakness an append-only log would not have.

### VF-028 -- the header virtual hosts dispatch on had never been enforced

- **What was being built** Name-based virtual hosts: a site carries a name, the
  first whose name matches a request's `Host` answers, and a name nobody claims
  gets 421.
- **What was found on the way** `request.c` accepted **HTTP/1.1 with no `Host`
  at all**, and accepted **two `Host` headers** -- in the file that has refused
  two `Content-Length` headers since 0.0.2, with the doctrine written above it:
  *agreement is not the property that makes a message safe, being unambiguous
  is.* The rule had been applied to the header that says how long a message is
  and never to the header that says **which machine it is for**, which is the
  one a dispatch is about to make a decision with.
- **Measured against the parser, before and after:**

```
before   HTTP/1.1 with no Host    verdict=0    status=0
         two different Hosts      verdict=0    status=0

after    HTTP/1.1 with no Host    verdict=-2   status=400
         two different Hosts      verdict=-8   status=400
         HTTP/1.0 with no Host    verdict=0    status=0    (unchanged)
```

- **And on the machine**, a server-role boot with the console on :80, six
  requests over the loopback forward:

```
ordinary request, Host present                HTTP/1.1 200 OK
HTTP/1.1 with no Host at all                  HTTP/1.1 400 Bad Request
two different Host headers                    HTTP/1.1 400 Bad Request
two identical Host headers                    HTTP/1.1 400 Bad Request
HTTP/1.0 with no Host (predates the field)    HTTP/1.1 200 OK
a name this machine was never given           HTTP/1.1 200 OK
```

  The last two are the ones worth reading. HTTP/1.0 predates the field and is
  untouched. And the console answers to a name it was never given **on
  purpose**: its site has no name, which claims everything, and that is what
  every server here did before this version. A machine that starts refusing
  names after an upgrade nobody asked for is a worse outcome than the fault.

  Two identical `Host` headers are refused as well. Not an oversight: a
  duplicate that agrees is still a message with two answers to one question,
  and a reader that resolves it by comparison is a reader that resolves it.
  `Content-Length` has been refused on the same grounds for twenty-three
  versions.

- **The dispatch, watched failing** against `pick_site` rewritten to answer with
  the head of the chain every time -- the exact shape of a server that ignores
  `Host`: **9 of 59 checks in `test_http_serve`**. The fifty that still passed
  are the interesting half. Every 200 passed, because the head of the chain
  serves the same routes; every check of a *name* passed when the name happened
  to be the head's. What failed was the second site's context, the second site's
  `Server` header, every 421, and the pipelined pair.
- **So the body of the test response is the site's own `ctx`**, not the route's.
  Both sites share one route table deliberately: a check that read the path
  would pass with the dispatch deleted.
- **What is not proved on the machine** A second named site. `server_init.c`
  configures one site with no name, so what a boot shows is that adding the
  dispatch changed nothing for a server that has one site. The chain itself is
  proved over a real socket in the host suite, against `serve.c` unmodified --
  which is the same standard every other property of this server is held to.

### VF-029 -- the server dispatched on a host name it had never been given

- **What was claimed, by the version that shipped it** 0.24.0's note says a
  site with no `host` claims every name, and that this is what a single-site
  machine does. True of `serve.c`. **Not true of the machine**, because the
  init program never gave that field a value at all.
- **The shape.** `server_init.c` declared `struct http_site site;` on the stack
  in `main` and then assigned ten fields one at a time. `host` and `next` --
  added to the struct that same version -- were not among them, so the server
  walked a chain of sites starting from whatever the stack held.
- **Measured, on the machine**, with the previous shape instrumented to print
  the two fields immediately before the first assignment:

```
  site before assignment: host=0 next=0
```

  Zero on that boot, and on the boots the 0.24.0 verification was done on,
  which is why every one of them passed. That is the fault's whole danger: it
  is not that the server misbehaved, it is that **it behaved correctly for a
  reason nobody chose.**
- **What "indeterminate" means here**, shown on a host rather than asserted --
  the same shape, in a frame that other work has already used:

```c
static void dirty(void)  { const char *junk[8]; ... }
static void configure(void) { struct site s; s.routes = ...; print(s.host); }
```

```
(dirtied 0x5b63691d7008)
host=0x5b63691d7008 next=0x5b63691d7008
```

  `host` holding a pointer to a string the previous call left behind. On the
  machine that is a server comparing `Host:` against text from somewhere else
  in its own memory, and answering **421** to every request the moment it does
  not match -- a console that has stopped answering to its own address, with no
  fault anywhere a reader would look.
- **The fix is the shape, not the two fields.** The site is now a file-scope
  `static const struct http_site SITE = { ... }` with designated initializers.
  Everything it does not mention is zero by the language, and stays zero for
  every field added after the line was written. Assigning the two missing
  fields would have fixed this instance and left the next one.
- **And the same shape was in the three test files**, where it was harmless:
  the compiler refuses a positional initializer with the wrong number of
  members, so each new field broke the build instead of the server. They are
  designated now too -- the point is that the loud version and the silent
  version were the same mistake, and only one of them announced itself.
- **`scripts/check-site-init.py` runs with every suite** and refuses a
  `struct http_site` declared without an initializer. Watched failing: the old
  declaration put back gives

```
server/init/server_init.c:2094: `site` is declared and then assigned field by field
```

  No suite can see the init program's stack, so the check has to read the
  source. It is narrow on purpose -- it is about one struct that is known to
  grow, not a general rule about uninitialised variables.
