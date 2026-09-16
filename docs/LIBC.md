# The C library

*Lifted out of `README.md` on 15 September 2026, when it had grown to 1228
lines. **The text below is unchanged** -- it was moved rather than rewritten,
so every line still reads as whoever wrote it left it.*

---

## The C library

Everything in `src/` calls into glibc today. On the ReconOS kernel there is no
glibc, so `userland/libc/` is what those calls will reach instead: strings and
memory, `snprintf` and `vsnprintf`, the character classes, numbers out of text,
`qsort`, and the eleven stdio functions the desktop actually uses.

**2,473 of the 3,113 library calls in `src/` are answered by it.** That number
is counted, by walking every `.c` and `.h` with comments stripped, and its
shape is the shape of this system: `snprintf` 915 times, `strcasecmp` 357,
`strlen` 211, `strcmp` 139. A desktop is mostly text being compared and
formatted.

### Why it is tested the way it is

These functions fail quietly or not at all. A `strcmp` returning the wrong
*sign* still sorts, backwards. A `snprintf` that rounds `%.2f` the wrong way at
a half is wrong in a way nobody writing a test from memory would think to
check. So there is no test here asserting what these functions should do.

Both libraries are compiled into one program -- ReconOS's renamed by a
`-include prefix.h` so the two can coexist -- and every call is made twice and
the answers compared. **511,000 checks, with the library being replaced as the
referee.** Where ReconOS differs on purpose, the difference is asserted rather
than skipped.

The file layer gets the same treatment through `userland/tests/hostsys.c`,
which answers the five primitives `stdio.c` is built on with POSIX calls. So
the same buffering code that will run on the kernel reads a real file here and
is held against the host's stdio for the same file: the same bytes, the same
counts, the same position after every operation.

### Ten desktop sources build with no glibc under them

`scripts/check-userland.sh` compiles them with `-nostdinc`, the compiler's own
headers and `userland/include`, and nothing else -- so it is a compile rather
than a claim, and it is the fifth pass of `scripts/check.sh` so it cannot rot.

### Both figures above come from the linker, because two written by hand were wrong

`scripts/measure-libc.py` runs `nm` over the desktop's object files. Every one
of them carries a table of the symbols it needs and a table of the symbols it
has, and the difference is the external surface -- exactly, and without ever
having heard of a list.

The list is the part that was wrong. A grep for the functions the library was
expected to need finds every call of a function on the list and **none of a
function that is not on it**, so the numerator and the denominator came out
short by the same amount and the fraction looked right (BG-182). Corrected by
hand, it then missed `gmtime_r`, `localtime_r` and sixteen of the twenty
floating-point functions.

And it could never have found `puts`. `src/main.c` calls `printf` with a string
literal containing no conversions, the compiler rewrites that into `puts`, and
**the desktop needs a function whose name appears nowhere in its source.**

### What is missing, by what it needs

| | symbols | call sites |
| --- | --- | --- |
| an allocator | 5 | 430 |
| files and directories | 21 | 96 |
| sockets | 19 | 44 |
| an errno | 1 | 43 |
| loading a module at run time | 4 | 12 |
| processes and signals | 6 | 8 |
| floating-point maths | 20 | 5 |
| an environment | 1 | 2 |
| the rest of stdio | 3 | 0 |

`malloc` is the wall: five symbols and 430 call sites. The shape of the rest is
the linker's doing -- **floating-point maths is twenty functions**, not the four
a grep found, and it is a self-contained afternoon that needs nothing from the
kernel.

`userland/include/stdlib.h` has no `malloc` declaration at all, so a caller
fails to *link* rather than getting a stub that returns nothing and crashes
somewhere else an hour later. The ask is first on the list in
[docs/KERNEL-WANTS.md](docs/KERNEL-WANTS.md).

```bash
cmake --build build --target recon_libc_tests recon_libc_files_tests
./build/recon_libc_tests && ./build/recon_libc_files_tests
```
