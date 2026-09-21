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
# Every file goes through **both** compressors -- the whole-buffer one that
# `send_response` uses and the streaming one that files go through -- and is
# decompressed by `zlib`. Both ratios are printed, because the question the
# streaming form has to answer is not only "is it correct" but "is it as small
# as the one it is standing in for".
#
import os
import subprocess
import sys
import tempfile
import zlib

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "deflate.h"

static unsigned char in[300000];
static unsigned char out[600000];
static struct deflate_stream Z;

/*
 * With no argument, the whole-buffer compressor. With `stream`, the
 * incremental one, fed in eight-kilobyte pieces -- `HTTP_SEND_CHUNK`-sized,
 * which is the shape the file handler actually produces.
 */
int main(int argc, char **argv)
{
	size_t len = 0;
	size_t at = 0, total = 0;
	long n;
	int c;

	while ((c = getchar()) != EOF && len < sizeof(in))
		in[len++] = (unsigned char)c;

	if (argc > 1 && strcmp(argv[1], "stream") == 0) {
		n = deflate_stream_begin(&Z, out, sizeof(out));
		if (n < 0) {
			fprintf(stderr, "begin: %ld\n", n);
			return 1;
		}
		total = (size_t)n;

		while (at < len) {
			size_t take = len - at > 8192 ? 8192 : len - at;

			n = deflate_stream_write(&Z, in + at, take,
			                         out + total,
			                         sizeof(out) - total);
			if (n < 0) {
				fprintf(stderr, "write: %ld\n", n);
				return 1;
			}
			total += (size_t)n;
			at += take;
		}

		n = deflate_stream_end(&Z, out + total, sizeof(out) - total);
		if (n < 0) {
			fprintf(stderr, "end: %ld\n", n);
			return 1;
		}
		total += (size_t)n;
		fwrite(out, 1, total, stdout);
		return 0;
	}

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


def run(exe, how, raw):
    """Compress `raw` one way. Returns (bytes, complaint)."""
    args = [exe] if how == "whole" else [exe, "stream"]
    done = subprocess.run(args, input=raw, capture_output=True)
    if done.returncode != 0:
        return None, "%s failed: %s" % (
            how, done.stderr.decode("utf-8", "replace").strip())

    packed = done.stdout
    try:
        # 16 + MAX_WBITS is zlib's way of saying "this has a gzip header on
        # it", including the CRC and the length, both of which it checks.
        back = zlib.decompress(packed, 16 + zlib.MAX_WBITS)
    except zlib.error as e:
        return packed, "%s REFUSED: %s" % (how, e)

    if back != raw:
        return packed, "%s DIFFERENT (%d bytes back, %d in)" % (
            how, len(back), len(raw))
    return packed, None


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sys.argv[1:] or [os.path.join(root, p) for p in DEFAULT]
    bad = 0

    with tempfile.TemporaryDirectory() as where:
        exe = build(root, where)
        if not exe:
            print("the harness did not build")
            return 2

        print("%-22s %8s   %8s %6s   %8s %6s  %s"
              % ("file", "bytes", "whole", "ratio", "streamed", "ratio",
                 "zlib"))

        for path in files:
            name = os.path.basename(path)[:22]
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            except OSError as e:
                print("%-22s %s" % (name, e))
                bad += 1
                continue

            sizes = {}
            complaints = []
            for how in ("whole", "stream"):
                packed, complaint = run(exe, how, raw)
                sizes[how] = len(packed) if packed is not None else 0
                if complaint:
                    complaints.append(complaint)

            if complaints:
                bad += 1

            def pct(n):
                return (n * 100 // len(raw)) if raw else 0

            print("%-22s %8d   %8d %5d%%   %8d %5d%%  %s"
                  % (name, len(raw),
                     sizes["whole"], pct(sizes["whole"]),
                     sizes["stream"], pct(sizes["stream"]),
                     "; ".join(complaints) if complaints else "same"))

    if bad:
        print()
        print("%d file(s) did not come back" % bad)
        return 1
    print()
    print("every file came back byte for byte, both ways, through a "
          "decompressor this project did not write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
