#!/usr/bin/env python3
"""Graphite Gauge の製図ペンのアイコン（16px グリッド、線幅 1.25px、square cap）を、
SVG の <symbol> 一覧（assets/editor/icons/gg_icons.txt）から折れ線へ展開して生成物を作る。

入力  : assets/editor/icons/gg_icons.txt   （GG 仕様 panels/src/icons_*.txt の写し。--sync で元から取り直す）
出力  : assets/editor/icons/gg_icons.json  （id -> 折れ線。検査と他ツール用）
        assets/editor/icons/gg_icons.png   （目視確認用のアトラス。実行時には読まない）
        src/editor/GgIcons.gen.h           （実行時に使う constexpr の頂点表。ImDrawList::AddPolyline で描く）

使い方:
    python tools/theme/gen_gg_icons.py                 # 生成
    python tools/theme/gen_gg_icons.py --check         # 生成物が最新か検査（差分があれば 1 で終了）
    python tools/theme/gen_gg_icons.py --sync <GG の panels/src>   # 元の icons_base.txt / icons_extra.txt を写し直してから生成

ブランドのマーク（mark / mark-l）は色つきの別系統（BD の SplashBrandGeometry）なので対象外。
自作（GG 仕様は本リポジトリの作者の成果物）。外部のアイコンセットは使わない。
"""
import json, math, re, sys, hashlib, argparse
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "assets/editor/icons/gg_icons.txt"
OUT_JSON = ROOT / "assets/editor/icons/gg_icons.json"
OUT_PNG = ROOT / "assets/editor/icons/gg_icons.png"
OUT_H = ROOT / "src/editor/GgIcons.gen.h"

DEFAULT_W = 1.25
SKIP = {"mark", "mark-l"}
TOL = 0.04          # 折れ線の許容誤差（グリッド単位）

# ------------------------------------------------------------------ パス -> 折れ線
_tok = re.compile(r"([MmLlHhVvCcSsQqTtAaZz])|(-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)")


def _arc_points(p0, rx, ry, phi, fa, fs, p1):
    (x1, y1), (x2, y2) = p0, p1
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [p1]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if fa == fs:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0:
        dt -= 2 * math.pi
    if fs and dt < 0:
        dt += 2 * math.pi
    r = max(rx, ry)
    step = 2 * math.acos(max(0.0, 1 - TOL / r)) if r > TOL else 1.0
    n = max(4, int(math.ceil(abs(dt) / step)))
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x = cp * rx * math.cos(t) - sp * ry * math.sin(t) + cx
        y = sp * rx * math.cos(t) + cp * ry * math.sin(t) + cy
        pts.append((x, y))
    pts[-1] = p1
    return pts


def _bez(p0, c1, c2, p1):
    d = max(abs(p0[0] - 2 * c1[0] + c2[0]) + abs(p0[1] - 2 * c1[1] + c2[1]),
            abs(c1[0] - 2 * c2[0] + p1[0]) + abs(c1[1] - 2 * c2[1] + p1[1]))
    n = max(4, int(math.ceil(math.sqrt(d * 0.75 / TOL / 4)) * 2))
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * p1[0],
                    u ** 3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * p1[1]))
    out[-1] = p1
    return out


def parse_path(d):
    """戻り値: [(points, closed)]"""
    toks = [(m.group(1), m.group(2)) for m in _tok.finditer(d)]
    i = 0
    subs = []
    cur = []
    closed = False
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    cmd = None
    lastc = None

    def num():
        nonlocal i
        v = float(toks[i][1])
        i += 1
        return v

    def flush():
        nonlocal cur, closed
        if len(cur) >= 1:
            subs.append((cur, closed))
        cur = []
        closed = False

    while i < len(toks):
        if toks[i][0]:
            cmd = toks[i][0]
            i += 1
            if cmd in "Zz":
                if cur:
                    closed = True
                    flush()
                pos = start
                lastc = None
                continue
        rel = cmd.islower()
        C = cmd.upper()
        ox, oy = pos if rel else (0.0, 0.0)
        if C == "M":
            flush()
            pos = (ox + num(), oy + num())
            start = pos
            cur = [pos]
            cmd = "l" if rel else "L"
            lastc = None
        elif C == "L":
            pos = (ox + num(), oy + num())
            if not cur:
                cur = [start]
            cur.append(pos)
            lastc = None
        elif C == "H":
            pos = ((pos[0] if rel else 0.0) + num(), pos[1])
            if not cur:
                cur = [start]
            cur.append(pos)
            lastc = None
        elif C == "V":
            pos = (pos[0], (pos[1] if rel else 0.0) + num())
            if not cur:
                cur = [start]
            cur.append(pos)
            lastc = None
        elif C == "C":
            c1 = (ox + num(), oy + num())
            c2 = (ox + num(), oy + num())
            p = (ox + num(), oy + num())
            if not cur:
                cur = [start]
            cur += _bez(pos, c1, c2, p)
            lastc = c2
            pos = p
        elif C == "S":
            c1 = (2 * pos[0] - lastc[0], 2 * pos[1] - lastc[1]) if lastc else pos
            c2 = (ox + num(), oy + num())
            p = (ox + num(), oy + num())
            if not cur:
                cur = [start]
            cur += _bez(pos, c1, c2, p)
            lastc = c2
            pos = p
        elif C == "A":
            rx, ry, phi, fa, fs = num(), num(), num(), int(num()), int(num())
            p = (ox + num(), oy + num())
            if not cur:
                cur = [start]
            cur += _arc_points(pos, rx, ry, phi, fa, fs, p)
            pos = p
            lastc = None
        else:
            raise ValueError("未対応のパスコマンド: " + cmd)
    flush()
    return subs


def ellipse_pts(cx, cy, rx, ry):
    r = max(rx, ry)
    n = max(12, int(math.ceil(2 * math.pi / (2 * math.acos(max(0.0, 1 - TOL / r))))))
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def rect_pts(x, y, w, h, rx):
    if rx <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    d = (f"M{x+rx} {y}H{x+w-rx}A{rx} {rx} 0 0 1 {x+w} {y+rx}V{y+h-rx}A{rx} {rx} 0 0 1 {x+w-rx} {y+h}"
         f"H{x+rx}A{rx} {rx} 0 0 1 {x} {y+h-rx}V{y+rx}A{rx} {rx} 0 0 1 {x+rx} {y}")
    pts = parse_path(d)[0][0]
    if pts[-1] == pts[0]:
        pts = pts[:-1]
    return pts


def dash(pts, closed, pattern):
    """折れ線を破線にして、開いた折れ線の列を返す（始点は図形の始点、SVG と同じ）"""
    if closed:
        pts = pts + [pts[0]]
    out = []
    idx = 0
    remain = pattern[0]
    on = True
    cur = [pts[0]]
    for a, b in zip(pts, pts[1:]):
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        if seg == 0:
            continue
        pos = 0.0
        while seg - pos > remain:
            pos += remain
            t = pos / seg
            q = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            if on:
                cur.append(q)
                out.append(cur)
                cur = []
            else:
                cur = [q]
            on = not on
            idx = (idx + 1) % len(pattern)
            remain = pattern[idx]
        remain -= seg - pos
        if on:
            cur.append(b)
    if on and len(cur) > 1:
        out.append(cur)
    return out


# ------------------------------------------------------------------ SVG の読み込み
def load_icons(text):
    root = ET.fromstring("<defs xmlns='http://www.w3.org/2000/svg'>" + text + "</defs>")
    ns = "{http://www.w3.org/2000/svg}"
    icons = []
    for sym in root.iter(ns + "symbol"):
        iid = sym.get("id")
        if iid in SKIP:
            continue
        vb = [float(v) for v in sym.get("viewBox").split()]
        paths = []
        for el in sym:
            tag = el.tag.replace(ns, "")
            a = el.attrib
            if a.get("stroke") not in (None, "none", "currentColor") or "stroke-opacity" in a:
                raise ValueError(f"{iid}: 色つきのストロークは対象外")
            fill = a.get("fill") == "currentColor"
            stroke = a.get("stroke") != "none"
            w = float(a.get("stroke-width", DEFAULT_W))
            da = [float(v) for v in re.split(r"[ ,]+", a["stroke-dasharray"].strip())] if "stroke-dasharray" in a else None
            if tag == "path":
                shapes = parse_path(a["d"])
            elif tag == "circle":
                shapes = [(ellipse_pts(float(a["cx"]), float(a["cy"]), float(a["r"]), float(a["r"])), True)]
            elif tag == "ellipse":
                shapes = [(ellipse_pts(float(a["cx"]), float(a["cy"]), float(a["rx"]), float(a["ry"])), True)]
            elif tag == "rect":
                x, y, rw, rh = (float(a[k]) for k in ("x", "y", "width", "height"))
                shapes = [(rect_pts(x, y, rw, rh, float(a.get("rx", 0))), True)]
            else:
                raise ValueError(f"{iid}: 未対応の要素 {tag}")
            for pts, closed in shapes:
                if len(pts) < 1:
                    continue
                if closed and len(pts) > 1 and pts[0] == pts[-1]:
                    pts = pts[:-1]
                if fill:
                    paths.append(dict(w=0.0, closed=True, fill=True, pts=pts))
                if not stroke:
                    continue
                if da:
                    for seg in dash(pts, closed, da):
                        paths.append(dict(w=w, closed=False, fill=False, pts=seg))
                else:
                    if len(pts) == 1:   # 点（"h.1" 等）は極短い線。square cap で四角い点になる
                        pts = [pts[0], (pts[0][0] + 0.001, pts[0][1])]
                    paths.append(dict(w=w, closed=closed, fill=False, pts=pts))
        icons.append(dict(id=iid, vb=vb[2], paths=paths))
    return icons


def r3(v):
    return round(v, 3)


def fl(v):
    """C++ の float リテラル（"8" -> "8.0f"）"""
    t = repr(round(float(v), 3))
    if "e" in t or "E" in t:
        t = "%.6f" % v
    if "." not in t:
        t += ".0"
    return t + "f"


def build_json(icons):
    return {"version": 1, "grid": 16, "strokeWidth": DEFAULT_W, "cap": "square", "join": "miter",
            "icons": [{"id": ic["id"], "vb": ic["vb"],
                       "paths": [{"w": p["w"], "closed": p["closed"], "fill": p["fill"],
                                  "pts": [[r3(x), r3(y)] for x, y in p["pts"]]} for p in ic["paths"]]}
                      for ic in icons]}


def build_header(icons, src_sha):
    L = []
    L.append("#pragma once")
    L.append("// ★自動生成: tools/theme/gen_gg_icons.py（入力 assets/editor/icons/gg_icons.txt、SHA256 = %s）。手で直さない。" % src_sha)
    L.append("// Graphite Gauge の製図ペンのアイコン。16px グリッドの折れ線（線幅 1.25、square cap、miter join）。")
    L.append("// 描くのは editor/GgIconDraw.h。id は GG 仕様の symbol id（\"i-mesh\" 等）。")
    L.append("")
    L.append("#include <cstdint>")
    L.append("")
    L.append("namespace dx12e::gg_icons")
    L.append("{")
    L.append("struct Path { float w; bool closed; bool fill; std::uint16_t first; std::uint16_t count; };   // first/count は kPts の添字（点の個数）")
    L.append("struct Icon { const char* id; float vb; std::uint16_t firstPath; std::uint16_t pathCount; };")
    L.append("")
    pts = []
    paths = []
    table = []
    for ic in icons:
        fp = len(paths)
        for p in ic["paths"]:
            paths.append((p["w"], p["closed"], p["fill"], len(pts), len(p["pts"])))
            pts += [(r3(x), r3(y)) for x, y in p["pts"]]
        table.append((ic["id"], ic["vb"], fp, len(ic["paths"])))
    assert len(pts) < 65535 and len(paths) < 65535
    L.append("inline constexpr float kPts[][2] = {")
    row = []
    for (x, y) in pts:
        row.append("{%s,%s}" % (fl(x), fl(y)))
        if len(row) == 8:
            L.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        L.append("    " + ", ".join(row) + ",")
    L.append("};")
    L.append("inline constexpr Path kPaths[] = {")
    for w, c, f, fi, cn in paths:
        L.append("    { %s, %s, %s, %d, %d }," % (fl(w), "true" if c else "false", "true" if f else "false", fi, cn))
    L.append("};")
    L.append("// id の昇順（二分探索できる）")
    L.append("inline constexpr Icon kIcons[] = {")
    for id_, vb, fp, pc in sorted(table):
        L.append('    { "%s", %s, %d, %d },' % (id_, fl(vb), fp, pc))
    L.append("};")
    L.append("inline constexpr int kIconCount = %d;" % len(table))
    L.append("} // namespace dx12e::gg_icons")
    return "\n".join(L) + "\n"


def build_png(icons):
    from PIL import Image, ImageDraw
    cols = 12
    cell = 40
    pad = 4
    S = 8
    rows = (len(icons) + cols - 1) // cols
    img = Image.new("RGBA", (cols * cell, rows * cell), (22, 24, 28, 255))
    for n, ic in enumerate(icons):
        cx, cy = (n % cols) * cell, (n // cols) * cell
        k = (cell - 2 * pad) / ic["vb"] * S
        big = Image.new("L", (cell * S, cell * S), 0)
        d = ImageDraw.Draw(big)

        def T(p):
            return (pad * S + p[0] * k, pad * S + p[1] * k)

        for p in ic["paths"]:
            pts = [T(q) for q in p["pts"]]
            if p["fill"]:
                d.polygon(pts, fill=255)
                continue
            w = max(1, round(p["w"] * k))
            if p["closed"]:
                pts = pts + [pts[0], pts[1]] if len(pts) > 1 else pts
            elif len(pts) >= 2:
                def ext(a, b):
                    dx, dy = a[0] - b[0], a[1] - b[1]
                    l = math.hypot(dx, dy) or 1
                    return (a[0] + dx / l * w / 2, a[1] + dy / l * w / 2)
                pts = [ext(pts[0], pts[1])] + pts[1:-1] + [ext(pts[-1], pts[-2])]
            d.line(pts, fill=255, width=w, joint="curve")
        small = big.resize((cell, cell), Image.LANCZOS)
        img.paste((230, 226, 218, 255), (cx, cy), small)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--sync", metavar="GG_PANELS_SRC")
    args = ap.parse_args()
    if args.sync:
        d = Path(args.sync)
        txt = ((d / "icons_base.txt").read_text(encoding="utf-8").rstrip("\n") + "\n"
               + (d / "icons_extra.txt").read_text(encoding="utf-8").rstrip("\n") + "\n")
        SRC.write_text(txt, encoding="utf-8", newline="\n")
    raw = SRC.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    icons = load_icons(raw.decode("utf-8"))
    ids = [i["id"] for i in icons]
    assert len(ids) == len(set(ids)), "id が重複"
    js = json.dumps(build_json(icons), ensure_ascii=False, separators=(",", ":")) + "\n"
    hdr = build_header(icons, sha)
    if args.check:
        bad = []
        if not OUT_JSON.exists() or OUT_JSON.read_text(encoding="utf-8") != js:
            bad.append(str(OUT_JSON))
        if not OUT_H.exists() or OUT_H.read_text(encoding="utf-8") != hdr:
            bad.append(str(OUT_H))
        if bad:
            print("生成物が古い:", *bad, sep="\n  ")
            return 1
        print("OK: %d 個のアイコン" % len(icons))
        return 0
    OUT_JSON.write_text(js, encoding="utf-8", newline="\n")
    OUT_H.write_text(hdr, encoding="utf-8", newline="\n")
    build_png(icons).save(OUT_PNG, optimize=True)
    print("生成: %d 個のアイコン" % len(icons))
    return 0


if __name__ == "__main__":
    sys.exit(main())
