#!/usr/bin/env python3
"""Lists the engine's command-line options with their descriptions.

The options are sysParam objects built by static constructors from two constant string addresses; this reads those addresses
from the generated C (gen/) and the strings from the wasm's data segments, placed where __wasm_init_memory puts them.
usage: scripts/params.py <game.wasm> [filter-regex]
"""
import re, sys

def leb(b, i, signed=False):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if not c & 0x80: break
    if signed and c & 0x40: r -= 1 << s
    return r, i

def sections(b):
    i = 8
    while i < len(b):
        sid = b[i]; n, i = leb(b, i + 1)
        yield sid, i, i + n
        i += n

def memory_image(b):
    secs = {sid: (lo, hi) for sid, lo, hi in sections(b) if sid}
    # data segments (all passive in this module)
    lo, hi = secs[11]
    count, i = leb(b, lo)
    segs = []
    for _ in range(count):
        flag, i = leb(b, i)
        assert flag == 1, 'expected passive data segments'
        n, i = leb(b, i)
        segs.append((i, n)); i += n
    # imports: count the functions, to find __wasm_init_memory (the start function)
    lo, hi = secs[2]
    count, i = leb(b, lo); nfunc = 0
    for _ in range(count):
        for _ in range(2):
            n, i = leb(b, i); i += n
        kind = b[i]; i += 1
        if kind == 0: _, i = leb(b, i); nfunc += 1
        elif kind == 2:
            flags, i = leb(b, i); _, i = leb(b, i)
            if flags & 1: _, i = leb(b, i)
        else: raise SystemExit('unexpected import kind %d' % kind)
    start, _ = leb(b, secs[8][0])
    lo, hi = secs[10]
    count, i = leb(b, lo)
    for k in range(start - nfunc + 1):
        n, i = leb(b, i); body = (i, i + n); i += n
    code = b[body[0]:body[1]]
    # i64.const dest; i32.const 0; i32.const size; memory.init seg 0
    image = {}
    for m in re.finditer(rb'\x42', code):
        try:
            j = m.start() + 1
            dest, j = leb(code, j, True)
            if code[j] != 0x41: continue
            off, j = leb(code, j + 1, True)
            if code[j] != 0x41: continue
            size, j = leb(code, j + 1, True)
            if code[j:j + 2] != b'\xfc\x08': continue
            seg, j = leb(code, j + 2)
            if seg < len(segs) and segs[seg][1] == size and off == 0: image[seg] = dest
        except IndexError:
            pass
    return sorted((dest, b[segs[s][0]:segs[s][0] + segs[s][1]]) for s, dest in image.items())

def main():
    b = open(sys.argv[1], 'rb').read()
    image = memory_image(b)
    def string_at(addr):
        for dest, data in image:
            if dest <= addr < dest + len(data):
                o = addr - dest
                return data[o:data.index(b'\0', o)].decode('latin1')
        return None
    src = open('gen/gta_122.c', encoding='utf-8', errors='replace').read().split('\n')
    pat = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
    out = set()
    for n, line in enumerate(src):
        if 'sysParam0x3A0x3AsysParam' not in line or '(instance, var_j0, var_j1, var_j2)' not in line: continue
        vals = {}
        for back in src[n - 8:n]:
            m = re.match(r'\s*var_j([12]) = (\d+)ull;', back)
            if m: vals[m.group(1)] = int(m.group(2))
        if len(vals) == 2:
            name, desc = string_at(vals['1']), string_at(vals['2'])
            if name and (not pat or pat.search(name + ' ' + (desc or ''))): out.add((name, desc or ''))
    for name, desc in sorted(out): print('-%-34s %s' % (name, desc))
    print('%d options' % len(out), file=sys.stderr)

if __name__ == '__main__':
    main()
