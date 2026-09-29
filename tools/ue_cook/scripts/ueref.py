"""UE 5.6 shipping exe から静的にリフレクション表(UHT の Statics::PropPointers 等)を読む。ゲームは起動しない。"""
import struct, sys, capstone

class Exe:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        e = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, e + 6)[0]
        optsz = struct.unpack_from('<H', d, e + 20)[0]
        opt = e + 24
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        so = opt + optsz
        self.secs = []
        for i in range(nsec):
            name, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, so + i * 40)
            self.secs.append((name.rstrip(b'\0').decode(), vs, va, rs, rp))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.limit = self.base + max(va + max(vs, rs) for _, vs, va, rs, _ in self.secs)

    def rva2off(self, rva):
        for n, vs, va, rs, rp in self.secs:
            if va <= rva < va + rs:
                return rp + (rva - va)
        return None

    def off(self, va):
        if not (self.base <= va < self.limit):
            return None
        return self.rva2off(va - self.base)

    def isptr(self, v):
        return self.base <= v < self.limit and self.off(v) is not None

    def u64(self, va, add=0):
        o = self.off(va)
        return struct.unpack_from('<Q', self.data, o + add)[0]

    def u32(self, va, add=0):
        return struct.unpack_from('<I', self.data, self.off(va) + add)[0]

    def u16(self, va, add=0):
        return struct.unpack_from('<H', self.data, self.off(va) + add)[0]

    def cstr(self, va, maxlen=200):
        o = self.off(va)
        if o is None:
            return None
        e = self.data.find(b'\0', o, o + maxlen)
        if e < 0:
            return None
        b = self.data[o:e]
        if not b or any(c < 32 or c >= 127 for c in b):
            return None
        return b.decode('ascii')

    def va_of_off(self, off):
        for n, vs, va, rs, rp in self.secs:
            if rp <= off < rp + rs:
                return self.base + va + (off - rp)
        return None

    def find_all(self, pat, start=0):
        i = start
        while True:
            i = self.data.find(pat, i)
            if i < 0:
                return
            yield i
            i += 1

    def refs_to(self, va):
        """va を 8 バイト値として持つ場所の VA 一覧(8 整列のみ)。"""
        r = []
        for o in self.find_all(struct.pack('<Q', va)):
            if o % 8 == 0:
                r.append(self.va_of_off(o))
        return r

    def string_vas(self, s):
        pat = b'\0' + s.encode() + b'\0'
        return [self.va_of_off(o + 1) for o in self.find_all(pat)]

    def disasm(self, va, n=120):
        o = self.off(va)
        return list(self.md.disasm(self.data[o:o + n * 8], va, n))

    def lea_targets(self, va, n=40):
        """関数先頭からの RIP 相対 LEA が指す先(データ)一覧。"""
        res = []
        for ins in self.disasm(va, n):
            if ins.mnemonic == 'lea' and 'rip' in ins.op_str:
                # lea reg, [rip + disp]
                import re
                m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', ins.op_str)
                if m:
                    disp = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                    res.append(ins.address + ins.size + disp)
            if ins.mnemonic == 'ret':
                break
        return res


# EPropertyGenFlags (5.6)
GEN = {0x00: 'Byte', 0x01: 'Int8', 0x02: 'Int16', 0x03: 'Int', 0x04: 'Int64', 0x05: 'UInt16', 0x06: 'UInt32',
       0x07: 'UInt64', 0x08: 'UnsizedInt', 0x09: 'UnsizedUInt', 0x0A: 'Float', 0x0B: 'Double', 0x0C: 'Bool',
       0x0D: 'SoftClass', 0x0E: 'WeakObject', 0x0F: 'LazyObject', 0x10: 'SoftObject', 0x11: 'Class', 0x12: 'Object',
       0x13: 'Interface', 0x14: 'Name', 0x15: 'Str', 0x16: 'Array', 0x17: 'Map', 0x18: 'Set', 0x19: 'Struct',
       0x1A: 'Delegate', 0x1B: 'InlineMulticastDelegate', 0x1C: 'SparseMulticastDelegate', 0x1D: 'Text',
       0x1E: 'Enum', 0x1F: 'FieldPath', 0x20: 'LargeWorldCoordinatesReal', 0x21: 'Optional', 0x22: 'VerseString',
       0x23: 'TObjectPtrClass?'}


class Param:
    __slots__ = ('va', 'name', 'gen', 'type', 'flags', 'pflags', 'dim', 'offset', 'extra', 'inner', 'key')

    def __repr__(self):
        return f'<{self.name}:{self.type} dim={self.dim} off={self.offset:#x}>'


def read_param(ex, va):
    p = Param()
    p.va = va
    nm = ex.u64(va)
    p.name = ex.cstr(nm)
    p.pflags = ex.u64(va, 16)
    g = ex.u32(va, 24)
    p.gen = g
    p.type = GEN.get(g & 0x3F, f'?{g & 0x3F:#x}')
    p.flags = g & ~0x3F
    p.dim = ex.u16(va, 48)
    p.offset = ex.u16(va, 50)
    p.extra = ex.u64(va, 56)
    p.inner = None
    p.key = None
    return p


def looks_like_param(ex, v):
    if not ex.isptr(v):
        return False
    try:
        nm = ex.u64(v)
        if not ex.isptr(nm):
            return False
        s = ex.cstr(nm)
        if not s or len(s) > 100:
            return False
        g = ex.u32(v, 24)
        if (g & 0x3F) > 0x22:
            return False
        of = ex.u32(v, 28)
        if of > 0xFFFF:
            return False
        dim = ex.u16(v, 48)
        return 1 <= dim <= 512
    except Exception:
        return False


def prop_array_bounds(ex, any_elem_slot_va):
    s = any_elem_slot_va
    while looks_like_param(ex, ex.u64(s - 8)):
        s -= 8
    e = any_elem_slot_va
    while looks_like_param(ex, ex.u64(e)):
        e += 8
    return s, e


def read_prop_array(ex, arr_va, count):
    """ConstructUProperties と同じく後ろから読み、Array/Set/Optional/Map/Enum の内側を直前要素から拾う。
    戻り値: 外側プロパティのリスト(宣言順)。"""
    params = [read_param(ex, ex.u64(arr_va + 8 * i)) for i in range(count)]

    def consume(i):
        p = params[i]
        j = i - 1
        t = p.type
        if t in ('Array', 'Set', 'Optional', 'Enum'):
            p.inner, j = consume(j)
        elif t == 'Map':
            p.inner, j = consume(j)   # value
            p.key, j = consume(j)
        return p, j

    out = []
    i = count - 1
    while i >= 0:
        p, i = consume(i)
        out.append(p)
    out.reverse()
    return out
