#!/usr/bin/env python3
"""PE の .rsrc（リソース）を一覧にして比べる（W05-17）。標準ライブラリだけで動く。

使い方:
  python tools/bench/pe_resources.py dump <exe> [--json]        リソースの一覧（種別/名前/言語/サイズ/SHA256）
  python tools/bench/pe_resources.py diff <before.exe> <after.exe> [--ignore-type TYPE ...]
                                                                 差分があれば表示して終了コード 1
  python tools/bench/pe_resources.py has <exe> <type> <name>     そのリソースがあれば 0、無ければ 1

種別・名前は名前（文字列）か番号。RT_* の標準番号は名前で表示する。
"""
import hashlib
import struct
import sys
import json

RT = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING", 9: "ACCELERATOR",
      10: "RCDATA", 11: "MESSAGETABLE", 12: "GROUP_CURSOR", 14: "GROUP_ICON", 16: "VERSION",
      24: "MANIFEST"}


def _u16(d, o): return struct.unpack_from("<H", d, o)[0]
def _u32(d, o): return struct.unpack_from("<I", d, o)[0]


def read_resources(path):
    d = open(path, "rb").read()
    pe = _u32(d, 0x3C)
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"not a PE file: {path}")
    nsec = _u16(d, pe + 6)
    optsz = _u16(d, pe + 20)
    opt = pe + 24
    magic = _u16(d, opt)
    ddir = opt + (112 if magic == 0x20B else 96)
    rva, size = _u32(d, ddir + 2 * 8), _u32(d, ddir + 2 * 8 + 4)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        o = so + i * 40
        vsz, vaddr, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((vaddr, max(vsz, rsz), rptr))

    def rva2off(r):
        for va, sz, ptr in secs:
            if va <= r < va + sz:
                return r - va + ptr
        raise ValueError("bad rva")

    out = []
    if rva == 0:
        return out
    base = rva2off(rva)

    def name_of(off_field, level_base):
        v = off_field
        if v & 0x80000000:
            o = level_base + (v & 0x7FFFFFFF)
            n = _u16(d, o)
            return d[o + 2:o + 2 + n * 2].decode("utf-16le")
        return v

    def walk(off, level, path_):
        nn, ni = _u16(d, off + 12), _u16(d, off + 14)
        for i in range(nn + ni):
            nm, ofs = struct.unpack_from("<II", d, off + 16 + i * 8)
            key = name_of(nm, base)
            if ofs & 0x80000000:
                walk(base + (ofs & 0x7FFFFFFF), level + 1, path_ + [key])
            else:
                de = base + ofs
                drva, dsz = _u32(d, de), _u32(d, de + 4)
                data = d[rva2off(drva):rva2off(drva) + dsz]
                t, n, l = path_ + [key]
                out.append({"type": RT.get(t, t) if isinstance(t, int) else t,
                            "name": n, "lang": l, "size": dsz,
                            "sha256": hashlib.sha256(data).hexdigest()})
    walk(base, 0, [])
    out.sort(key=lambda r: (str(r["type"]), str(r["name"]), r["lang"]))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    cmd = argv[1]
    if cmd == "dump":
        rs = read_resources(argv[2])
        if "--json" in argv:
            print(json.dumps(rs, indent=1))
        else:
            for r in rs:
                print(f'{r["type"]}\t{r["name"]}\t{r["lang"]}\t{r["size"]}\t{r["sha256"][:16]}')
            print(f"# {len(rs)} resources")
        return 0
    if cmd == "diff":
        ign = set()
        a = argv[2:]
        files = []
        i = 0
        while i < len(a):
            if a[i] == "--ignore-type":
                ign.add(a[i + 1]); i += 2
            else:
                files.append(a[i]); i += 1
        ra = [r for r in read_resources(files[0]) if str(r["type"]) not in ign]
        rb = [r for r in read_resources(files[1]) if str(r["type"]) not in ign]
        key = lambda r: json.dumps(r, sort_keys=True)
        sa, sb = {key(r) for r in ra}, {key(r) for r in rb}
        for k in sorted(sa - sb): print("- " + k)
        for k in sorted(sb - sa): print("+ " + k)
        if sa == sb:
            print(f"IDENTICAL ({len(ra)} resources)")
            return 0
        return 1
    if cmd == "has":
        rs = read_resources(argv[2])
        ok = any(str(r["type"]) == argv[3] and str(r["name"]) == argv[4] for r in rs)
        print("FOUND" if ok else "MISSING")
        return 0 if ok else 1
    print(__doc__); return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
