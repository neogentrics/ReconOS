"""Name the Colored Glass tiles after what they are, into assets/icons.

The pack arrives as icon_01 .. icon_50, which says nothing. Each number was
identified by looking at all fifty at once on a contact sheet, and the mapping
below is that reading written down -- it is the only record of it, so it says
what the picture is as well as which ReconOS name it answers.

The files copied are the ones `cutout.py` produced, not the originals: every
file in the pack is RGB with no alpha channel, so the rounded tile sits on an
opaque near-black rectangle and would draw as a black square on the wallpaper.
"""
import os
import shutil
import sys

# number: (ReconOS name, what the picture is)
MAP = {
    1:  ('control-panel', 'a gear'),
    2:  ('folder',        'a blue folder'),
    3:  ('troubleshoot',  'a magnifying glass -- find what is wrong'),
    4:  ('photos',        'a camera'),
    5:  ('web',           'a wire globe'),
    7:  ('mail',          'an envelope'),
    8:  ('calendar',      'a calendar page'),
    9:  ('clock',         'a clock face'),
    11: ('appearance',    'a wheel of colours'),
    12: ('player',        'a musical note'),
    13: ('programs',      'a letter A built out of tools'),
    14: ('terminal',      'a prompt on black'),
    15: ('recovery',      'an arrow coming down into a tray'),
    16: ('trash',         'a waste basket'),
    18: ('network',       'the radio fan'),
    19: ('modules',       'the Bluetooth rune -- a device, which is what that '
                          'page manages'),
    20: ('accounts',      'two people'),
    22: ('file-sound',    'a waveform'),
    23: ('notepad',       'a ruled pad'),
    24: ('file-data',     'a list with coloured markers: rows of values, which is what a data file is'),
    27: ('help',          'an open book'),
    28: ('taskmanager',   'a rising chart -- Watchtower graphs what the machine is doing, and shares this name'),
    31: ('calculator',    'a calculator'),
    32: ('file-web',      'a compass'),
    36: ('file-video',    'a video camera'),
    37: ('file-image',    'a framed picture'),
    41: ('firewall',      'a shield with a padlock in it'),
    44: ('display',       'a monitor'),
    46: ('update',        'two arrows round a circle'),
    48: ('file-archive',  'a filing box'),
    49: ('system',        'a wrench and a screwdriver crossed'),
    50: ('shutdown',      'the power symbol, in red'),
}

# What the pack has no answer for, so the gap is a decision rather than an
# oversight. All of these fall back to the Glass set, which Smoked also gets:
#
#   apps        nothing in the pack is a grid of squares, and the Apps button
#               is drawn at sixteen pixels where a tile would be a blob.
#   application these two are drawn instead, as Glass-set masks -- see
#   file-font   scripts/draw-glass-gaps.py. Nothing in this pack means "some
#               program", and a page with a letter on it is a picture the
#               pack does not have either.
#   keyring     nothing is a key. Glass has a password book, which is better
#               than a shield -- the shield went to the firewall, where a
#               shield is the convention.
#   explorer    Glass's browse-folder is a folder with a glass over it, which
#               says "look inside this" more plainly than anything here.
#   file, file-data, file-font, application, trash-full, recon-towers,
#   window-close / -maximize / -minimize / -restore
#               no source, or drawn at a size no downscale survives.

SRC = ('C:/Users/neoge/AppData/Local/Temp/claude/E--Github-Projects/'
       '23247a64-07aa-4fbb-b4d4-40c15a4723dc/scratchpad/cut')
DST = 'E:/Github Projects/ReconOS/assets/icons/Colored Glass'


def main():
    os.makedirs(DST, exist_ok=True)
    written = 0
    for number, (name, _what) in sorted(MAP.items()):
        src = os.path.join(SRC, 'icon_%02d.png' % number)
        if not os.path.exists(src):
            print('missing %s' % src, file=sys.stderr)
            return 1
        shutil.copyfile(src, os.path.join(DST, name + '.png'))
        written += 1
    print('%d named' % written)
    return 0


sys.exit(main())
