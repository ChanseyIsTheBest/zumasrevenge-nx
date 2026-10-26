#!/usr/bin/env python3
"""zumasrevenge-nx (2026-10-27): Renut's Direct3D device offsets, from Bejeweled 3's values (the d3d layer this tree took
from bejeweled3-nx 1.0.1) to Zuma's Revenge's (XDK 2.0.20871), which are Peggle 2's (21256).

Measured on the aligned function pairs of the two libraries (out/renut_map/anchor.txt, same-length pairs): Zuma's
Revenge's device fields are Bejeweled 3's + 24 from about 11560 to 13536 (67 offsets, 342 accesses) and + 16 from 13584
on (140 offsets, 508 accesses), the rest unchanged (141 offsets). bejeweled3-nx's notes: Bejeweled 3 is Peggle 2's
layout 24 bytes lower from 11584 and 16 bytes lower from 13600. So Zuma's Revenge's layout is Peggle 2's, and every
site of bejeweled3-nx's list (out/renut_port/device_offset_sites_bj3.json: N&B's, Peggle 2's and Bejeweled 3's value of
each) takes its Peggle 2 value here; the text sites (out/renut_port/device_offset_sites_zr_extra.json) take
peggle2-nx's own lines. bejeweled3-nx's tools/renut_device_offsets_bj3.py with the value columns switched.

  python -I tools/renut_device_offsets_zr.py [--dry-run]
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
SITES = os.path.join(ROOT, 'out', 'renut_port', 'device_offset_sites_bj3.json')
EXTRA = os.path.join(ROOT, 'out', 'renut_port', 'device_offset_sites_zr_extra.json')
WINDOW = 150
NUM = re.compile(r'(?<![0-9A-Za-z_.])(0x[0-9A-Fa-f]+|\d+)(?![0-9A-Za-z_.])')


def values(v):
    if v is None:
        return []
    if isinstance(v, str):  # '{a, b, c}' (kDeviceOffsetsMinusOne)
        return [int(x) for x in re.findall(r'[0-9]+', v)]
    return list(v) if isinstance(v, list) else [v]


def ToPeggle2(bj3):
    if bj3 is None:
        return None
    if bj3 >= 13584:
        return bj3 + 16
    if 11560 <= bj3 < 13560:
        return bj3 + 24
    return bj3


def split(line):
    cut = line.find('//')
    return (line, '') if cut < 0 else (line[:cut], line[cut:])


def remap(code, mapping):
    """Every literal of the code part (not the comment) whose value is a key of `mapping` gets the mapped value, in
    one pass (no chained replacement), keeping hex or decimal spelling."""
    head, tail = split(code)

    def sub(m):
        t = m.group(1)
        v = int(t, 16) if t.lower().startswith('0x') else int(t)
        if v not in mapping:
            return t
        n = mapping[v]
        return ('0x%X' % n) if t.lower().startswith('0x') else ('%d' % n)
    return NUM.sub(sub, head) + tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    sites = json.load(open(SITES, encoding='utf-8'))
    files = {}

    def lines_of(rel):
        path = os.path.join(GPU, rel)
        if path not in files:
            raw = open(path, encoding='utf-8', newline='').read()
            files[path] = [raw, raw.replace('\r\n', '\n').split('\n')]
        return files[path][1]

    done = already = same = 0
    problems = []
    groups = {}
    for s in sites:
        groups.setdefault((s.get('p1_tree_site') or s['site'], s['code'].strip()), []).append(s)
    for (site, code), group in groups.items():
        rel, line = site.rsplit(':', 1)
        line = int(line)
        old_to_now, now_to_new = {}, {}
        for s in group:
            b, now = values(s['banjo']), values(s.get('bj3'))
            # A site bejeweled3-nx recorded no Peggle 2 value for: the measured rule (Bejeweled 3's offsets + 24 from
            # 11560 to 13559, + 16 from 13584 on; 17104 -> 17120 is the word bejeweled3-nx's notes name in Peggle 2).
            new = values(s.get('peggle')) if s.get('peggle') is not None else [ToPeggle2(x) for x in now]
            if not new or not now or any(x is None for x in new + now):
                problems.append('%s %s: no Peggle 2 value (left as it is)' % (site, s.get('name') or ''))
                continue
            for vb, vn, vj in zip(b, now, new):
                old_to_now[vb] = vn
                if vn in now_to_new and now_to_new[vn] != vj:
                    problems.append('%s: %d maps to both %d and %d on one line' % (site, vn, now_to_new[vn], vj))
                now_to_new[vn] = vj
        if not now_to_new:
            continue
        if all(k == v for k, v in now_to_new.items()):
            same += 1
            continue
        now_code = split(remap(code, old_to_now))[0].strip()
        want_code = split(remap(code, {k: now_to_new[v] for k, v in old_to_now.items()}))[0].strip()
        lines = lines_of(rel)
        lo, hi = max(0, line - WINDOW), min(len(lines), line + WINDOW)
        cand = [i for i in range(lo, hi) if split(lines[i])[0].strip() == now_code]
        if not cand:
            if any(split(lines[i])[0].strip() == want_code for i in range(lo, hi)):
                already += 1
            else:
                problems.append('%s: line not found: %s' % (site, now_code))
            continue
        for best in sorted(cand, key=lambda i: abs(i + 1 - line))[:1]:
            lines[best] = remap(lines[best], now_to_new)
            done += 1
    extra_done = extra_already = 0
    for e in json.load(open(EXTRA, encoding='utf-8')):
        rel, line = e['site'].rsplit(':', 1)
        line = int(line)
        lines = lines_of(rel)
        lo, hi = max(0, line - WINDOW), min(len(lines), line + WINDOW)
        cand = [i for i in range(lo, hi) if e['find'] in lines[i]]
        if not cand:
            if any(e['replace'] in lines[i] for i in range(lo, hi)):
                extra_already += 1
            else:
                problems.append('%s: extra line not found: %s' % (e['site'], e['find'][:100]))
            continue
        best = min(cand, key=lambda i: abs(i + 1 - line))
        lines[best] = lines[best].replace(e['find'], e['replace'], 1)
        extra_done += 1
    for path, (raw, lines) in files.items():
        nl = '\r\n' if '\r\n' in raw else '\n'
        out = '\n'.join(lines).replace('\n', nl)
        if out != raw and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(out)
    print('renut_device_offsets_zr: %d lines rewritten, %d already at Peggle 2\'s values, %d lines whose values '
          'Bejeweled 3 shares with Peggle 2; extras: %d rewritten, %d already' % (done, already, same, extra_done,
                                                                             extra_already))
    for p in problems:
        print('  ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
