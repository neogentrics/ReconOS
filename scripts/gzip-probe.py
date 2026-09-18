#!/usr/bin/env python3
#
# A third opinion on the compressor.
#
# --- Why this exists beside the suite ---
#
# `server/tests/test_deflate.c` carries its own inflater, written from RFC 1951
# and deliberately in the opposite shape to the encoder -- it reads the fixed
# Huffman tables by their bit patterns where `deflate.c` writes them by their
# ranges. That is a real second opinion and it is still two implementations by
# the same author, from the same reading of the same document.
#
# This is the third: the bytes go to Python's `zlib`, which was written by
# somebody else entirely and has decompressed rather a lot of gzip. If this
# agrees with the suite, the specification was read correctly; if they
# disagree, the suite is the thing that is wrong.
#
# It is a script rather than a suite because it needs a host with Python and
# the suites must run anywhere.
#
# Usage, from the repository root:
#
#     python3 scripts/gzip-probe.py [files...]
#
# With no arguments it uses this repository's own text, which is what the
# server will actually be sending: documents, source and a page of console.
# Every file is compressed by `deflate.c` and decompressed by `zlib`, and the
# ratio is printed because the numbers are the other thing worth knowing.
#
import os
import subprocess
import sys
import tempfile
import zlib

HARNESS = r'''
#include <stdio.h>
#include "deflate.h"

static unsigned char in[300000];
static unsigned char out[400000];

int main(void)
{
	size_t len = 0;
	long n;
	int c;

	while ((c = getchar()) != EOF && len < sizeof(in))
		in[len++] = (unsigned char)c;

	n = deflate_gzip(in, len, out, sizeof(out));
	if (n < 0) {
		fprintf(stderr, "%ld\n", n);
		return 1;
	}
	fwrite(out, 1, (size_t)n, stdout);
	return 0;
}
'''

DEFAULT = (
    "server/README.md",
    "docs/WEB.md",
    "docs/SERVER.md",
    "server/http/serve.c",
    "server/http/deflate.c",
    "CMakeLists.txt",
)


def build(root, where):
    src = os.path.join(where, "harness.c")
    exe = os.path.join(where, "harness")
    with open(src, "w", encoding="utf-8") as f:
        f.write(HARNESS)

    cmd = ["gcc", "-std=gnu11", "-O2", "-I",
           os.path.join(root, "server", "http"), "-o", exe, src,
           os.path.join(root, "server", "http", "deflate.c")]
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
        return None
    return exe


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sys.argv[1:] or [os.path.join(root, p) for p in DEFAULT]
    bad = 0

    with tempfile.TemporaryDirectory() as where:
        exe = build(root, where)
        if not exe:
            print("the harness did not build")
            return 2

        print("%-28s %9s %9s %6s  %s"
              % ("file", "bytes", "gzip", "ratio", "zlib"))

        for path in files:
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            except OSError as e:
                print("%-28s %s" % (os.path.basename(path), e))
                bad += 1
                continue

            done = subprocess.run([exe], input=raw, capture_output=True)
            if done.returncode != 0:
                print("%-28s compressing failed: %s"
                      % (os.path.basename(path),
                         done.stderr.decode("utf-8", "replace").strip()))
                bad += 1
                continue

            packed = done.stdout
            try:
                # 16 + MAX_WBITS is zlib's way of saying "this has a gzip
                # header on it", including the CRC and the length, both of
                # which it checks.
                back = zlib.decompress(packed, 16 + zlib.MAX_WBITS)
                verdict = "same" if back == raw else "DIFFERENT"
            except zlib.error as e:
                verdict = "REFUSED: %s" % e

            if verdict != "same":
                bad += 1

            ratio = (len(packed) * 100 // len(raw)) if raw else 0
            print("%-28s %9d %9d %5d%%  %s"
                  % (os.path.basename(path), len(raw), len(packed), ratio,
                     verdict))

    if bad:
        print()
        print("%d file(s) did not come back" % bad)
        return 1
    print()
    print("every file came back byte for byte, through a decompressor this "
          "project did not write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
