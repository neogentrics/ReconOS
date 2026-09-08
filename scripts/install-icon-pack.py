"""Install an icon pack from E:\\Icons\\<Pack> into assets/icons/<Pack>.

Handles what a folder of downloads actually looks like: several sizes of the
same picture, a `web` subfolder the browser dropped things into, "-liquid-glass"
in some names and not others, and "-2"/"-3" suffixes where the same file was
fetched twice.
"""
import io
import os
import re
import shutil
import sys

SRC_ROOT = 'E:/Icons'
DST_ROOT = 'E:/Github Projects/ReconOS/assets/icons'

# ReconOS's own names, mapped onto what a pack calls the same picture.
# A name absent here is installed under whatever the pack calls it.
SYSTEM = {
    'folder':          'folder',
    'file':            'file',
    'file-sound':      'audio',
    'file-image':      'image',
    'file-data':       'document',
    'file-archive':    'archive-folder',
    'file-web':        'website',
    'notepad':         'edit-pencil',
    'explorer':        'browse-folder',
    'system':          'tools',
    'control-panel':   'settings',
    'apps':            'squared-menu',
    'help':            'info',
    'recovery':        'reset',
    'update':          'refresh',
    'clock':           'clock',
    'photos':          'pictures-folder',
    'mail':            'mailbox',
    'player':          'musical-note',
    'trash':           'trash',
    'trash-full':      'trash-can',
    'accounts':        'user',
    'troubleshoot':    'wrench',
    'display':         'increase-font',
    'programs':        'package',

    # Filled by the second batch.
    'terminal':        'terminal',
    'calculator':      'calculator',
    'taskmanager':     'tasklist',
    'keyring':         'password-book',
    'calendar':        'calendar',
    'network':         'network',
    'firewall':        'firewall',
    'web':             'internet',
    'modules':         'device-manager',

    # Skins and wallpaper: a grid of pictures is what that page shows.
    'appearance':      'thumbnails',

    # The Explorer's parent-folder button. A bare chevron, for the same
    # reason: the pack's `up` is an arrow inside a circle, and at this size
    # the circle is all that survives.
    'up':              'chevron-up',
}

# What nothing in a pack can answer, and why -- so the gap is a decision on
# record rather than something that looks forgotten.
#
#   shutdown    nothing in the pack means *power*. The drawn red symbol is
#               instantly recognisable and stays: meaning beats matching.
#   application a generic "some program" picture; `squared-menu` is the Apps
#               button and the rest are specific things.
#   file-video  }  no source, and a wrong picture on a file type is worse
#   file-font   }  than the plain sheet the generator draws.
#   window-close / window-maximize / window-minimize / window-restore
#               all three drawn shapes stay, and this is a decision about the
#               *set* rather than about any one of them.
#
#               The pack's maximize and minimize are a glyph inside a box, and
#               a caption button is already a box: measured at twelve pixels
#               both come out a featureless blob. Its `multiply` is a bare X
#               and survives the shrink -- but putting it in leaves one soft
#               grey icon between two solid drawn ones, and three buttons in a
#               row that do not match each other look worse than three that
#               are merely plain. They are drawn at the size they are shown,
#               which no downscale can match.

SIZE = re.compile(r'-(\d+)(?:-\d+)?\.png$')


def catalogue(pack_dir):
    """Every picture in a pack, by plain name, at the largest size found."""
    best = {}
    for root, _dirs, files in os.walk(pack_dir):
        for name in files:
            if not name.lower().endswith('.png'):
                continue

            m = SIZE.search(name)
            if m is None:
                continue
            size = int(m.group(1))

            plain = name[:m.start()]
            if plain.startswith('icons8-'):
                plain = plain[len('icons8-'):]
            if plain.endswith('-liquid-glass'):
                plain = plain[:-len('-liquid-glass')]

            path = os.path.join(root, name)
            if plain not in best or size > best[plain][0]:
                best[plain] = (size, path)
    return best


def install(pack):
    src = os.path.join(SRC_ROOT, pack)
    dst = os.path.join(DST_ROOT, pack)
    if not os.path.isdir(src):
        print('no pack at %s' % src, file=sys.stderr)
        return

    have = catalogue(src)
    if not os.path.isdir(dst):
        os.makedirs(dst)

    written = 0
    for plain, (_size, path) in sorted(have.items()):
        shutil.copyfile(path, os.path.join(dst, plain + '.png'))
        written += 1

    aliased = []
    for target, source in sorted(SYSTEM.items()):
        if source not in have:
            continue
        shutil.copyfile(have[source][1], os.path.join(dst, target + '.png'))
        aliased.append(target)
        written += 1

    print('%s: %d pictures, %d of them under ReconOS names'
          % (pack, written, len(aliased)))

    missing = [t for t in sorted(SYSTEM) if SYSTEM[t] not in have]
    if missing:
        print('  no source for: %s' % ', '.join(missing))
    return have


if __name__ == '__main__':
    packs = [d for d in sorted(os.listdir(SRC_ROOT))
             if os.path.isdir(os.path.join(SRC_ROOT, d))
             and d != 'user account icons']
    print('packs found: %s' % ', '.join(packs))
    for p in packs:
        install(p)
