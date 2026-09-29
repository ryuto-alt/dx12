"""UE 5.6 shipping exe の静的リフレクション表から、指定クラスとその依存 struct/enum だけの .usmap を書く。
ゲームは起動しない(exe のファイルを読むだけ)。"""
import struct, sys, argparse
from ueref import *

USMAP_TYPE = {
    'Byte': 'ByteProperty', 'Int8': 'Int8Property', 'Int16': 'Int16Property', 'Int': 'IntProperty',
    'Int64': 'Int64Property', 'UInt16': 'UInt16Property', 'UInt32': 'UInt32Property', 'UInt64': 'UInt64Property',
    'UnsizedInt': 'IntProperty', 'UnsizedUInt': 'UInt32Property', 'Float': 'FloatProperty', 'Double': 'DoubleProperty',
    'LargeWorldCoordinatesReal': 'DoubleProperty', 'Bool': 'BoolProperty', 'SoftClass': 'SoftObjectProperty',
    'WeakObject': 'WeakObjectProperty', 'LazyObject': 'LazyObjectProperty', 'SoftObject': 'SoftObjectProperty',
    'Class': 'ObjectProperty', 'Object': 'ObjectProperty', 'Interface': 'InterfaceProperty', 'Name': 'NameProperty',
    'Str': 'StrProperty', 'Array': 'ArrayProperty', 'Map': 'MapProperty', 'Set': 'SetProperty',
    'Struct': 'StructProperty', 'Delegate': 'DelegateProperty', 'InlineMulticastDelegate': 'MulticastInlineDelegateProperty',
    'SparseMulticastDelegate': 'MulticastDelegateProperty', 'Text': 'TextProperty', 'Enum': 'EnumProperty',
    'FieldPath': 'FieldPathProperty', 'Optional': 'OptionalProperty',
}
ETYPE = ['ByteProperty', 'BoolProperty', 'IntProperty', 'FloatProperty', 'ObjectProperty', 'NameProperty',
         'DelegateProperty', 'DoubleProperty', 'ArrayProperty', 'StructProperty', 'StrProperty', 'TextProperty',
         'InterfaceProperty', 'MulticastDelegateProperty', 'WeakObjectProperty', 'LazyObjectProperty',
         'AssetObjectProperty', 'SoftObjectProperty', 'UInt64Property', 'UInt32Property', 'UInt16Property',
         'Int64Property', 'Int16Property', 'Int8Property', 'MapProperty', 'SetProperty', 'EnumProperty',
         'FieldPathProperty', 'OptionalProperty', 'Utf8StrProperty', 'AnsiStrProperty', 'ClassProperty',
         'MulticastInlineDelegateProperty', 'SoftClassProperty']


class Gen:
    def __init__(self, ex):
        self.ex = ex
        self.structs = {}     # name -> dict(name, super, props, count)
        self.enums = {}       # name -> [(value, name)]
        self.warn = []

    # --- struct / enum の解決 ---
    def struct_params_from_func(self, fn):
        ex = self.ex
        for t in ex.lea_targets(fn):
            if ex.off(t) is None:
                continue
            nm = ex.u64(t, 24)
            if not ex.isptr(nm):
                continue
            s = ex.cstr(nm)
            arr = ex.u64(t, 32)
            if s and ex.isptr(arr):
                cnt = ex.u16(t, 40)
                if cnt == 0 or looks_like_param(ex, ex.u64(arr)):
                    return t
        return None

    def enum_from_func(self, fn):
        ex = self.ex
        for t in ex.lea_targets(fn):
            if ex.off(t) is None:
                continue
            nm = ex.u64(t, 16)
            if not ex.isptr(nm):
                continue
            s = ex.cstr(nm)
            arr = ex.u64(t, 32)
            if s and s.startswith('E') and ex.isptr(arr):
                cnt = ex.u16(t, 44)
                ents = []
                for i in range(cnt):
                    en = ex.u64(arr, 16 * i)
                    ev = struct.unpack_from('<q', ex.data, ex.off(arr) + 16 * i + 8)[0]
                    ename = ex.cstr(en) if ex.isptr(en) else None
                    if ename is None:
                        return None
                    if '::' in ename:
                        ename = ename.split('::', 1)[1]
                    ents.append((ev, ename))
                return s, ents
        return None

    def resolve_struct(self, fn):
        ex = self.ex
        t = self.struct_params_from_func(fn)
        if t is None:
            self.warn.append(f'struct params not found for fn {fn:#x}')
            return None
        name = ex.cstr(ex.u64(t, 24))
        if name in self.structs:
            return name
        self.structs[name] = None  # 再帰防止
        sup = None
        sfn = ex.u64(t, 8)
        if sfn:
            sup = self.resolve_struct(sfn)
        cnt = ex.u16(t, 40)
        arr = ex.u64(t, 32)
        props = read_prop_array(ex, arr, cnt) if cnt else []
        self.structs[name] = self.build(name, sup, props)
        return name

    def resolve_enum(self, fn):
        r = self.enum_from_func(fn)
        if r is None:
            self.warn.append(f'enum not found for fn {fn:#x}')
            return None
        name, ents = r
        self.enums.setdefault(name, ents)
        return name

    def ptype(self, p):
        """usmap のプロパティ型(ネスト構造)を返す。"""
        ex = self.ex
        t = p.type
        if t == 'Byte' and p.extra and ex.isptr(p.extra) and ex.off(p.extra) is not None:
            en = self.resolve_enum(p.extra)
            if en:
                return ('EnumProperty', ('ByteProperty',), en)
            return ('ByteProperty',)
        if t == 'Enum':
            en = self.resolve_enum(p.extra) or 'EUnknown'
            return ('EnumProperty', self.ptype(p.inner), en)
        if t == 'Struct':
            sn = self.resolve_struct(p.extra)
            return ('StructProperty', sn or 'Unknown')
        if t in ('Array', 'Set', 'Optional'):
            return (USMAP_TYPE[t], self.ptype(p.inner))
        if t == 'Map':
            return ('MapProperty', self.ptype(p.key), self.ptype(p.inner))
        if t in USMAP_TYPE:
            return (USMAP_TYPE[t],)
        raise ValueError(f'unknown type {t} for {p.name}')

    def build(self, name, sup, props):
        entries = []
        idx = 0
        for p in props:
            entries.append((idx, p.dim, p.name, self.ptype(p)))
            idx += p.dim
        return {'name': name, 'super': sup, 'entries': entries, 'count': idx}

    def add_class(self, name, sup, array_va, count):
        props = read_prop_array(self.ex, array_va, count)
        self.structs[name] = self.build(name, sup, props)

    # --- 出力 ---
    def write(self, path, dump=None):
        names = []
        nidx = {}

        def N(s):
            if s is None:
                return -1
            if s not in nidx:
                nidx[s] = len(names)
                names.append(s)
            return nidx[s]

        body = bytearray()

        def wtype(t):
            b = bytearray()
            b.append(ETYPE.index(t[0]))
            if t[0] == 'EnumProperty':
                b += wtype(t[1]); b += struct.pack('<i', N(t[2]))
            elif t[0] == 'StructProperty':
                b += struct.pack('<i', N(t[1]))
            elif t[0] in ('ArrayProperty', 'SetProperty', 'OptionalProperty'):
                b += wtype(t[1])
            elif t[0] == 'MapProperty':
                b += wtype(t[1]); b += wtype(t[2])
            return b

        enum_b = bytearray(struct.pack('<I', len(self.enums)))
        for en, ents in sorted(self.enums.items()):
            enum_b += struct.pack('<i', N(en)) + struct.pack('<H', len(ents))
            for v, n in ents:
                enum_b += struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF) + struct.pack('<i', N(n))
        st_b = bytearray(struct.pack('<I', len(self.structs)))
        for sn, s in sorted(self.structs.items()):
            st_b += struct.pack('<i', N(sn)) + struct.pack('<i', N(s['super']))
            st_b += struct.pack('<HH', s['count'], len(s['entries']))
            for idx, dim, pn, t in s['entries']:
                st_b += struct.pack('<HB', idx, dim) + struct.pack('<i', N(pn)) + wtype(t)
        nm_b = bytearray(struct.pack('<I', len(names)))
        for n in names:
            e = n.encode('utf-8')
            nm_b += struct.pack('<H', len(e)) + e
        payload = bytes(nm_b + enum_b + st_b)
        # 版 = ExplicitEnumValues(4)、バージョン情報なし、無圧縮
        out = struct.pack('<HB', 0x30C4, 4) + struct.pack('<i', 0) + struct.pack('<B', 0) + struct.pack('<II', len(payload), len(payload)) + payload
        open(path, 'wb').write(out)
        if dump:
            with open(dump, 'w', encoding='utf-8') as f:
                for sn, s in sorted(self.structs.items()):
                    f.write(f"{sn} : {s['super']}  count={s['count']}\n")
                    for idx, dim, pn, t in s['entries']:
                        f.write(f"   [{idx}] x{dim} {pn} {t}\n")
                for en, ents in sorted(self.enums.items()):
                    f.write(f"enum {en}: {ents}\n")


def find_class_array(ex, prop_names):
    """指定名のプロパティを「全部」持つ PropPointers 配列(クラス/構造体)を返す: [(start_va, count)]。"""
    per_name = []
    for pn in prop_names:
        seen = {}
        for sva in ex.string_vas(pn):
            for pva in ex.refs_to(sva):
                for slot in ex.refs_to(pva):
                    s, e = prop_array_bounds(ex, slot)
                    seen[s] = (e - s) // 8
        per_name.append(seen)
    common = set(per_name[0])
    for d in per_name[1:]:
        common &= set(d)
    return sorted((s, per_name[0][s]) for s in common)


DEFAULT_CLASSES = ['StaticMesh:StreamableRenderAsset:StaticMaterials,LODForCollision',
                   'StreamableRenderAsset:Object:NeverStream,NumCinematicMipLevels']

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='UE5.6 shipping exe から静的に .usmap を作る（ゲームは起動しない）。'
                                 'exe 内の UHT 生成リフレクション表(Statics::PropPointers)を読み、指定クラスと依存 struct/enum を書く。')
    ap.add_argument('exe', help='<Game>-Win64-Shipping.exe')
    ap.add_argument('out', help='出力 .usmap')
    ap.add_argument('--class', dest='classes', action='append', metavar='NAME:SUPER:PROP[,PROP..]',
                    help='読むクラス。PROP は「そのクラスだけが持つ」プロパティ名（配列の特定に使う）。既定: StaticMesh 系')
    ap.add_argument('--dump', help='人間可読のダンプ')
    a = ap.parse_args()
    ex = Exe(a.exe)
    g = Gen(ex)
    for spec in (a.classes or DEFAULT_CLASSES):
        cn, sup, pns = spec.split(':')
        if not pns:                       # UPROPERTY を持たないクラス（例: MaterialInstanceConstant）
            g.structs[cn] = {'name': cn, 'super': sup, 'entries': [], 'count': 0}
            continue
        arrs = find_class_array(ex, pns.split(','))
        print(cn, [(hex(s), c) for s, c in arrs])
        if len(arrs) != 1:
            sys.exit(f'{cn}: PROP hints must select exactly one array (got {len(arrs)}); add more distinguishing props')
        s, c = arrs[0]
        g.add_class(cn, sup, s, c)
    g.structs.setdefault('Object', {'name': 'Object', 'super': None, 'entries': [], 'count': 0})
    g.write(a.out, a.dump)
    print('structs', len(g.structs), 'enums', len(g.enums))
    for w in g.warn:
        print('WARN', w)
