"""discord_krisp.node 解析用の最小 PE リーダ。"""
import struct


class PE:
    def __init__(self, path):
        self.path = path
        self.buf = open(path, 'rb').read()
        b = self.buf
        e = struct.unpack_from('<I', b, 0x3C)[0]
        self.machine = struct.unpack_from('<H', b, e + 4)[0]
        nsec = struct.unpack_from('<H', b, e + 6)[0]
        optsz = struct.unpack_from('<H', b, e + 20)[0]
        opt = e + 24
        self.image_base = struct.unpack_from('<Q', b, opt + 24)[0]
        self.sections = []
        for i in range(nsec):
            o = e + 24 + optsz + 40 * i
            name = b[o:o + 8].rstrip(b'\x00').decode('latin1')
            vs, va, rs, ra = struct.unpack_from('<IIII', b, o + 8)
            self.sections.append((name, va, vs, ra, rs))
        self.dirs = [struct.unpack_from('<II', b, opt + 112 + 8 * i) for i in range(16)]

    def rva_to_off(self, rva):
        for name, va, vs, ra, rs in self.sections:
            if rs and va <= rva < va + rs:
                return ra + (rva - va)
        return None

    def section_of(self, rva):
        for name, va, vs, ra, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                return name
        return None

    def cstr(self, rva, limit=400):
        o = self.rva_to_off(rva)
        if o is None:
            return None
        end = self.buf.find(b'\x00', o, o + limit)
        if end < 0:
            return None
        s = self.buf[o:end]
        try:
            t = s.decode('utf-8')
        except UnicodeDecodeError:
            return None
        return t if all(32 <= ord(c) < 127 or c in '\r\n\t' for c in t) else None

    def wstr(self, rva, limit=400):
        o = self.rva_to_off(rva)
        if o is None:
            return None
        end = o
        while end < o + limit * 2 and self.buf[end:end + 2] != b'\x00\x00':
            end += 2
        try:
            t = self.buf[o:end].decode('utf-16-le')
        except UnicodeDecodeError:
            return None
        return t if t and all(32 <= ord(c) < 127 for c in t) else None

    def exports(self):
        """{name: rva} を返す。"""
        rva, size = self.dirs[0]
        if not rva:
            return {}
        o = self.rva_to_off(rva)
        nfunc, nname = struct.unpack_from('<II', self.buf, o + 20)
        addr_rva, namep_rva, ord_rva = struct.unpack_from('<III', self.buf, o + 28)
        ao, no, oo = map(self.rva_to_off, (addr_rva, namep_rva, ord_rva))
        out = {}
        for i in range(nname):
            nr = struct.unpack_from('<I', self.buf, no + 4 * i)[0]
            idx = struct.unpack_from('<H', self.buf, oo + 2 * i)[0]
            fn = struct.unpack_from('<I', self.buf, ao + 4 * idx)[0]
            out[self.cstr(nr)] = fn
        return out


def imports(pe):
    """IAT の RVA -> "DLL!関数名" を返す。"""
    import struct as _s
    rva, size = pe.dirs[1]
    if not rva:
        return {}
    o = pe.rva_to_off(rva)
    out = {}
    i = 0
    while True:
        d = _s.unpack_from('<IIIII', pe.buf, o + 20 * i)
        if d[3] == 0 and d[4] == 0:
            break
        dll = pe.cstr(d[3]) or '?'
        lookup, iat = d[0] or d[4], d[4]
        lo, io = pe.rva_to_off(lookup), iat
        j = 0
        while True:
            v = _s.unpack_from('<Q', pe.buf, pe.rva_to_off(lookup) + 8 * j)[0]
            if v == 0:
                break
            if v >> 63:
                nm = 'ord_%d' % (v & 0xFFFF)
            else:
                nm = pe.cstr((v & 0xFFFFFFFF) + 2) or '?'
            out[iat + 8 * j] = '%s!%s' % (dll, nm)
            j += 1
        i += 1
    return out


def func_bounds(pe, rva):
    """.pdata から rva を含む関数の (begin, end) RVA を返す。"""
    import struct as _s
    prva, psize = pe.dirs[3]
    o = pe.rva_to_off(prva)
    n = psize // 12
    lo, hi = 0, n
    while lo < hi:
        mid = (lo + hi) // 2
        b, e, u = _s.unpack_from('<III', pe.buf, o + 12 * mid)
        if rva < b: hi = mid
        elif rva >= e: lo = mid + 1
        else: return (b, e)
    return None


def find_callers(pe, target_rva):
    """.text 内で E8 rel32 により target_rva を call する命令の RVA 一覧。"""
    import struct as _s
    tx = next(s for s in pe.sections if s[0] == '.text')
    _, va, vs, ra, rs = tx
    base = ra - va
    out = []
    for rva in range(va, va + rs - 5):
        if pe.buf[base + rva] == 0xE8:
            disp = _s.unpack_from('<i', pe.buf, base + rva + 1)[0]
            if rva + 5 + disp == target_rva:
                out.append(rva)
    return out
