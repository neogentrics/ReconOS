#!/usr/bin/env python3
"""Generate userland/libc/two_over_pi.inc -- the bits an exact sin needs.

    python3 scripts/gen-two-over-pi.py

--- Why a table of two thousand bits ---

Reducing an angle modulo pi/2 by subtracting a rounded pi/2 costs one bit of
the remainder for every bit of the quotient. The first run of the maths suite
measured what that means: **860,948,872,375 units in the last place at three
pi**, which is not an inaccurate answer but a wrong one, and by 1e15 there is
nothing left at all.

The cure is to do the reduction in as many bits as the argument demands. A
double's exponent reaches 2^1024, and after throwing that much of 2/pi away
the fraction still has to be good to 53 bits -- so the table holds 2,048.

--- Why it is generated and not typed ---

Because it is checked. The first eight words of 2/pi are a published number,
and the script refuses to write a table that disagrees with them. A constant
of this shape is wrong in its tenth word or not at all, and being wrong in its
tenth word makes every large angle wrong in a way no test of small angles
could ever see.

`scripts/check.sh` regenerates it and compares, the same as the help pages
and the error catalogue, so the file in the tree cannot drift from the script
that made it.
"""
import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, 'userland', 'libc', 'two_over_pi.inc')

BITS = 2400
WORDS = 64

# The published hexadecimal expansion of the fraction of 2/pi. Any reference
# carries these; they are what makes this script a check rather than a claim.
KNOWN = [0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0,
         0xDB629599, 0x3C439041, 0xFE5163AB, 0xDEBBC561]


def machin_pi(bits):
    """pi * 2^(bits+32), by Machin's formula and integer division only."""
    one = 1 << (bits + 32)

    def arccot(x, unity):
        total = term = unity // x
        x2 = x * x
        n = 1
        sign = -1
        while term:
            term //= x2
            total += sign * term // (2 * n + 1)
            sign = -sign
            n += 1
        return total

    return 4 * (4 * arccot(5, one) - arccot(239, one))


def words_of_two_over_pi():
    pi_scaled = machin_pi(BITS)
    target = (2 << (WORDS * 32 + BITS + 32)) // pi_scaled
    return [(target >> ((WORDS - 1 - i) * 32)) & 0xFFFFFFFF
            for i in range(WORDS)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='verify without writing')
    args = ap.parse_args()

    words = words_of_two_over_pi()

    if words[:len(KNOWN)] != KNOWN:
        sys.stderr.write('the computed table disagrees with the published '
                         'expansion of 2/pi\n')
        sys.stderr.write('  computed: %s\n'
                         % ' '.join('%08X' % w for w in words[:8]))
        sys.stderr.write('  known   : %s\n'
                         % ' '.join('%08X' % w for w in KNOWN))
        return 1

    lines = []
    for i in range(0, WORDS, 6):
        lines.append('\t' + ' '.join('0x%08XU,' % w
                                     for w in words[i:i + 6]))

    text = ('/*\n'
            ' * 2/pi to %d bits, as %d words of 32. **Generated** by\n'
            ' * scripts/gen-two-over-pi.py -- do not edit.\n'
            ' *\n'
            ' * That script checks what it computes against the published\n'
            ' * hexadecimal expansion before writing, because a table of this\n'
            ' * shape is wrong in its tenth word or not at all, and being\n'
            ' * wrong in its tenth word makes every large angle wrong in a way\n'
            ' * no test of small angles could see.\n'
            ' */\n'
            'static const unsigned int TWO_OVER_PI[%d] = {\n%s\n};\n'
            % (WORDS * 32, WORDS, WORDS, '\n'.join(lines)))

    if args.check:
        if not os.path.exists(OUT):
            sys.stderr.write('%s does not exist\n' % OUT)
            return 1
        if io.open(OUT, encoding='utf-8', newline='').read() != text:
            sys.stderr.write('%s is not what this script generates\n' % OUT)
            return 1
        print('two_over_pi.inc matches its generator')
        return 0

    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(text)
    print('wrote %s (%d words, %d bits)' % (OUT, WORDS, WORDS * 32))
    return 0


if __name__ == '__main__':
    sys.exit(main())
