"""RIP 相対で特定アドレス/文字列を参照している命令を探す。

usage:
  python tools/xref.py --str "Failed to setup"      文字列を参照する箇所
  python tools/xref.py --rva 0xDA2030               アドレスを参照する箇所
"""
import sys
import os
import re
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE
from dis_exports import MODULE


def find_strings(pe, needle):
    """needle を含む文字列の RVA を返す。"""
    out = []
    b = needle.encode()
    start = 0
    while True:
        i = pe.buf.find(b, start)
        if i < 0:
            break
        start = i + 1
        # ファイルオフセット -> RVA
        for name, va, vs, ra, rs in pe.sections:
            if rs and ra <= i < ra + rs:
                out.append((va + (i - ra), pe.cstr(va + (i - ra)) or ''))
                break
    return out


def xrefs_to(pe, target, lo=0x1000, hi=None):
    """[lo,hi) の範囲で target を RIP 相対参照する命令末尾位置を列挙。"""
    text = next(s for s in pe.sections if s[0] == '.text')
    _, va, vs, ra, rs = text
    hi = hi if hi is not None else va + rs
    lo = max(lo, va)
    hi = min(hi, va + rs)
    hits = []
    base = ra - va
    for rva in range(lo, hi - 4):
        disp = struct.unpack_from('<i', pe.buf, base + rva)[0]
        if rva + 4 + disp == target:
            hits.append(rva + 4)
    return hits


def main():
    pe = PE(MODULE)
    if '--str' in sys.argv:
        needle = sys.argv[sys.argv.index('--str') + 1]
        found = find_strings(pe, needle)
        for rva, s in found:
            print('文字列 .rdata:0x%X  %r' % (rva, s[:90]))
            for h in xrefs_to(pe, rva):
                print('    <- 参照命令末尾 0x%X' % h)
    elif '--rva' in sys.argv:
        t = int(sys.argv[sys.argv.index('--rva') + 1], 0)
        for h in xrefs_to(pe, t):
            print('0x%X を参照 <- 命令末尾 0x%X' % (t, h))


if __name__ == '__main__':
    main()
