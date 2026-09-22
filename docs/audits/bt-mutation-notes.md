# Reading the mutation survivors

`scripts/mutate-bluetooth.py` replaces every comparison, logical operator and
boolean return in the nine Bluetooth files in turn, builds and boots each one,
and lists the breakages the self-tests sat through. This file is the reading
of that list, which is the part the tool cannot do.

A survivor is **not** the same as a bug. Four kinds of thing land in the list,
and telling them apart is the work:

1. **An untested decision.** The interesting kind, and the reason the tool
   exists.
2. **An equivalent mutation**, where the change cannot alter behaviour — a
   comparison against a bound nothing reaches, or an arm of a condition the
   other arm already implies.
3. **A guard against a caller that does not exist**, defensive rather than
   reachable.
4. **A path a guard above it makes unreachable.**

Anything left in `bt-mutation-survivors.txt` should have a line here saying
which it is. **An entry with no line is an entry nobody has read yet**, which
is the state the whole list started in.

---

## The run, over five passes

Every row is a run, not an estimate.

| pass | what had been written since the one above | mutants | killed | compiler | **survived** |
|---|---|---|---|---|---|
| 1 | — | 333 | 198 | 26 | **109** |
| 2 | the attach path, `get_be32`, the first boundaries | 345 | 237 | 26 | **82** |
| 3 | `bt_pair`'s `outmax` guards, `bt_link`, `bt_stack` | 345 | 267 | 26 | **52** |
| 4 | the `l2cap` boundaries and its three replies | 345 | 288 | 26 | **31** |
| 5 | the SDP refusals and the field map | 345 | 297 | 26 | **22** |
| 6 | a budget of zero, a continuation after a finished PDU | 345 | 299 | 26 | **20** |

The first run had 333 rather than 345 because it excluded `bt_hid.c`, which
had been swept separately, and because the script was still generating a
mutant for every `<` and `>` inside an `#include` line.

`bt_hid.c`, `bt_mouse.c` and `bt_stack.c` are at zero.

The 26 the compiler refuses are counted as caught: `-Werror` is a check like
any other, and a mutation it rejects is one that could not have shipped.

---

## What is left, and why

### Equivalent — the change cannot alter an answer

- **`bluetooth.c:186`** `len < HCI_EVENT_HEADER` → `<=`. The discriminating
  input is a two-byte event, and both branches below need six. Refused either
  way, one line apart.
- **`bt_link.c:70`** `len < 3` → `<=`. A three-byte Inquiry Result declares
  zero devices, and `n >= count` refuses it whatever this line does.
- **`bt_pair.c:128`** `len < HCI_EVENT_HEADER` → `<=`. Every case in that
  switch needs at least eight bytes to have an address in it.
- **`hid_report.c:341`** `depth > collection_depth_max` → `>=`. Assigning the
  same value it already holds.
- **`hid_report.c:444`** `bit_offset + bit_size > end_bits` → `>=`. Same:
  assigning `end_bits` the value it already has.
- **`sdp.c:156`** `while (at < len)` → `<=`. The extra iteration parses zero
  bytes, which fails, and the function returns false — which is exactly what
  falling out of the loop does.
- **`sdp.c:173`** `at >= len` → `>`. `at == len` then reaches
  `sdp_element_parse` on zero bytes, which fails and returns false one line
  later.
- **`sdp.c:228`** `at < list.data_len` → `<=`. The extra iteration returns
  false at the parse; the loop ending returns false at the bottom. Same answer,
  different line.
- **`l2cap.c:124`** both mutations. By the time this is reached, a *complete*
  PDU has already been cleared at the top of the function — so `active` implies
  `have < want`, and each arm implies the other.
- **`l2cap.c:293`** `at + 2 <= len` → `<`. The only option this skips is a
  trailing two-byte header with no value, which could never have carried an
  MTU.

### Defensive against a caller that does not exist

- **`bluetooth.c:110`** `plen && params` → `||`, and **`l2cap.c:228`**
  `dlen && data` → `||`. A caller passing a non-zero length with a null
  pointer is a programming error inside this kernel, not something the wire can
  produce. Testing it would mean writing the bug on purpose at the call site.
- **`hid_report.c:552`** `bit_offset > 0xFFFFFF00u` → `>=`. The one offset that
  distinguishes them is `0xFFFFFF00` exactly, which needs a report of half a
  gigabyte.

### Unreachable because of a guard above

- **`hid_report.c:604`, `:613`, `:617`, `:622`** — the four `return false`
  paths in `hid_mouse_decode` when a field will not extract.
  `len < m->report_bytes` is checked first, and `report_bytes` is computed
  from the highest `bit_offset + bit_size` of the very fields these calls
  read. So a layout that was built at all cannot contain a field outside the
  report it describes.

  **They stay.** If one ever fires, the fault is in
  `hid_report_mouse_layout` having produced a field the report cannot hold,
  and a driver that returned a decoded value instead would be reading bytes
  that are not there.

### Undefined, and unobservable here

- **`hid_report.c:574`** `is_signed && bit_size < 32` → `<=`. At 32 bits there
  is nothing to sign-extend, and the mutant's `1u << 32` is undefined
  behaviour. On this target it shifts by zero and produces the same value, so
  no test can see it. **Not equivalent in principle**, which is why it is here
  and not in the section above.

### Real, and hidden by the state being zeroed

- **`hid_report.c:432`** `i < info->field_count` → `<=`. This reads
  `fields[field_count]`, one entry past the last one stored. `hid_report_parse`
  zeroes the whole structure, so that entry is all zeros: `bit_size` 0 keeps
  `end_bits` where it is, `constant` false, `usage` 0, and no branch matches.
  The read is genuine and the answer is unchanged.

  At `field_count == HID_MAX_FIELDS` it reads past the array and into
  `field_count` itself, still inside the structure. **Worth fixing if that
  loop is ever changed to do more than read.**

---

## Two lessons this list taught about its own tests

**An overflow can forge the evidence that it did not happen.**
`info.fields` is followed in its structure by `field_count` and
`fields_truncated` — the two values that report the overflow. The first test
for `hid_report.c:304` used one-bit fields, so the thirty-third field written
one past the end had `bit_offset` 32 and `bit_size` 1, which writes
`field_count = 32` and `fields_truncated = true`: the same answers the correct
code gives. The test passed against the broken parser. It uses eight-bit
fields now, so the clobbered count is 256 and no descriptor here could produce
it.

**A refusal that does not write its output cannot be told from a mismatch.**
`sdp_uint` refuses a non-integer without touching `*out`, and
`sdp_find_attribute` initialises that to nought — so a version accepting
everything still reports a UUID as attribute **0**, and a search for `0x0206`
gets the same answer either way. The case searches for attribute zero now.

Both were found by applying the mutation by hand and watching, after
reasoning had said they should already have been caught. **Reasoning about why
a test should have worked is not a substitute for running it broken.**
