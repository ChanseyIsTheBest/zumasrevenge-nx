#!/usr/bin/env python3
"""zumasrevenge-nx (2026-10-27): Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp) at Zuma's Revenge's
addresses (XDK 2.0.20871). One-off, run once on the tree whose Direct3D layer came from bejeweled3-nx 1.0.1 (its hooks at
Bejeweled 3's XDK 20764 addresses); docs/zumasrevenge-notes.md, "Renut's hooks at Zuma's Revenge's addresses", has the
evidence.

Each active hook's Bejeweled 3 address is paired with this game's:
  - the render and sampler state functions from the two state tables ({getter, setter, default} rows, stride 12):
    Bejeweled 3's 0x82AD01E8 (101 rows) / 0x82AD06A8 (20) and this game's 0x82974578 / 0x82974A38, whose default
    columns are the same word for word: the same row and column is the same function;
  - the other functions by the anchor alignment of the two libraries (out/renut_map/anchor.txt: exact-sequence
    anchors, the functions between them aligned on their shape);
  - the seven the alignment left unpaired, read side by side (MANUAL).

  python -I tools/renut_hook_remap_zr.py [--dry-run]
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')
ANCHOR = os.path.join(ROOT, 'out', 'renut_map', 'anchor.txt')
ZR_IMAGE = os.path.join(ROOT, 'out', 'default_image.bin')
BJ3_IMAGE = os.path.join(os.path.dirname(ROOT), 'bejeweled3-nx', 'out', 'default_image.bin')
BASE = 0x82000000
TABLES = ((0x82AD01E8, 0x82974578, 101), (0x82AD06A8, 0x82974A38, 20))

# Bejeweled 3 address -> (Zuma's Revenge address, evidence): the hooks the alignment left unpaired.
MANUAL = {
    0x822F4E78: (0x8230AF08, 'D3DDevice_Clear: (dev, count, rects, flags, color, z, stencil) wraps ClearF'),
    0x822F4D40: (0x8230ADD0, 'D3DDevice_ClearF'),
    0x822F6618: (0x82309B78, 'D3DDevice_Present: resolves, then calls Swap sub_82309410'),
    0x822F5EB0: (0x82309410, 'D3DDevice_Swap: the only VdSwap caller'),
    0x822F05A8: (0x8230AFC8, 'D3D_SetSurfaceClip'),
    0x822F8568: (0x8231AD10, 'D3DDevice_SetRingBufferParameters'),
    0x82309B18: (0x8231B0D8, 'D3DDevice_EndIndexedVertices'),
}
HOOK = re.compile(r'^(\s*BANJO_RENUT_HOOK(?:_RAW|_SHARED)?\(\s*sub_)([0-9A-Fa-f]{8})\b')
DECL = re.compile(r'(?<![0-9A-Za-z])((?:__imp__|renut_native_)?sub_)([0-9A-Fa-f]{8})\b')


def state_pairs():
    z = open(ZR_IMAGE, 'rb').read()
    j = open(BJ3_IMAGE, 'rb').read()
    pairs = {}
    for ja, za, n in TABLES:
        for i in range(n):
            jr = struct.unpack_from('>3I', j, ja - BASE + 12 * i)
            zr = struct.unpack_from('>3I', z, za - BASE + 12 * i)
            if jr[2] != zr[2]:
                raise SystemExit('state table row %d: defaults differ (%08X, %08X)' % (i, jr[2], zr[2]))
            for c in range(2):
                if pairs.get(jr[c], zr[c]) != zr[c]:
                    raise SystemExit('%08X pairs with both %08X and %08X' % (jr[c], pairs[jr[c]], zr[c]))
                pairs[jr[c]] = zr[c]
    return pairs


def anchor_pairs():
    pairs = {}
    for line in open(ANCHOR):
        p = line.split()
        if len(p) >= 4 and p[2] != '-':
            pairs[int(p[2], 16)] = int(p[0], 16)
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    states, anchors = state_pairs(), anchor_pairs()
    used = {}
    missing = []
    counts = {'state': 0, 'anchor': 0, 'manual': 0}

    def target(bj3):
        if bj3 in MANUAL:
            counts['manual'] += 1
            return MANUAL[bj3][0]
        if bj3 in states:
            counts['state'] += 1
            return states[bj3]
        if bj3 in anchors:
            counts['anchor'] += 1
            return anchors[bj3]
        return None

    mapping = {}
    files = [os.path.join(HOOKS, f) for f in sorted(os.listdir(HOOKS)) if f.endswith('.cpp')]
    for path in files:
        disabled = 0  # inside `#if 0` (hooks of functions an earlier port's XDK does not link)
        for line in open(path, encoding='utf-8'):
            if line.startswith('#if'):
                disabled += 1 if (disabled or line.startswith('#if 0')) else 0
                continue
            if line.startswith('#endif'):
                disabled = max(0, disabled - 1)
                continue
            m = HOOK.match(line)
            if not m or disabled:
                continue
            bj3 = int(m.group(2), 16)
            z = target(bj3)
            if z is None:
                missing.append('%s: %s' % (os.path.basename(path), line.strip()[:120]))
                continue
            mapping[bj3] = z
            used.setdefault(z, set()).add(bj3)
    dupes = {z: s for z, s in used.items() if len(s) > 1}
    for z, s in sorted(dupes.items()):
        print('  %08X is the target of %s' % (z, ', '.join('%08X' % x for x in sorted(s))))
    for m in missing:
        print('  unmapped: ' + m)
    print('renut_hook_remap_zr: %d hooks (%d state table, %d anchor, %d manual), %d unmapped, %d shared targets'
          % (len(mapping), counts['state'], counts['anchor'], counts['manual'], len(missing), len(dupes)))
    if missing or dupes or a.dry_run:
        return 1 if (missing or dupes) else 0
    # Rewrite every sub_XXXXXXXX / renut_native_sub_XXXXXXXX naming a mapped address in the code part of each hook
    # file line (comments keep the addresses they cite), in one pass (no chained replacement). banjo_shared_hooks.h
    # already names this game's Swap (sub_82309410).
    for path in files:
        raw = open(path, encoding='utf-8', newline='').read()
        lines = raw.split('\n')
        for i, line in enumerate(lines):
            cut = line.find('//')
            code, rest = (line, '') if cut < 0 else (line[:cut], line[cut:])
            lines[i] = DECL.sub(lambda m: m.group(1) + ('%08X' % mapping[int(m.group(2), 16)]
                                                       if int(m.group(2), 16) in mapping else m.group(2)), code) + rest
        out = '\n'.join(lines)
        if out != raw:
            open(path, 'w', encoding='utf-8', newline='').write(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
