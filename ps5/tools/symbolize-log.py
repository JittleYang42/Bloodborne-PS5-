#!/usr/bin/env python3
"""bbport PS5: thread stacks of a last_run.log with function names.

    symbolize-log.py last_run.log BUILD_DIR [--at SECONDS]

Title code (0x400000 + offset in BUILD_DIR/link/llvm-pie.elf) is named by llvm-symbolizer;
game code is shown as GAME+offset (the prepared image), system modules as sys+address.
"""
import collections, re, subprocess, sys
log, build = sys.argv[1], sys.argv[2]
at = sys.argv[sys.argv.index('--at') + 1] if '--at' in sys.argv else None
lines = open(log, errors='replace').read().split('\n')
starts = [i for i, l in enumerate(lines) if 'thread stacks at' in l and (not at or f'at {at} s' in l)]
if not starts:
    sys.exit('no stack dump in the log')
first = starts[-2] if len(starts) > 1 and 'GPU library' in lines[starts[-1]] else starts[-1]
names, stacks, current = {}, collections.OrderedDict(), None
for l in lines[first:]:
    if l.startswith('PS5: ---- end'):
        break
    m = re.match(r'thread (.*):$', l)
    if m:
        current = m.group(1)
        continue
    m = re.search(r'tid=([0-9a-f]{16}) rip=([0-9a-f]{16})', l)
    if m:
        tid = int(m.group(1), 16)
        names.setdefault(tid, current)
        stacks.setdefault(tid, []).append(int(m.group(2), 16))
title = sorted({a for v in stacks.values() for a in v if 0x400000 <= a < 0x8000000})
out = subprocess.run(['llvm-symbolizer-18', f'--obj={build}/link/llvm-pie.elf', '--functions=short']
                     + [hex(a - 0x400000) for a in title], capture_output=True, text=True).stdout
blocks = [b.split('\n')[0] for b in out.strip().split('\n\n')]
symbol = dict(zip(title, blocks))
def name(a):
    if 0x400000 <= a < 0x8000000:
        return symbol.get(a, '?')
    if 0x2800000000 <= a < 0x2810000000:
        return 'GAME+%x' % (a - 0x2800000000)
    return 'sys+%x' % a
for tid, v in stacks.items():
    print(f'{names.get(tid) or "?"} (tid {tid:x}):')
    for a in v:
        print('    ' + name(a))
