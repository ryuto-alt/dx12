"""GroveLab カートのコース・カート・アイテム箱を Blender で layout.json から手続き生成する。

使い方（Blender の Python で）:
    exec(open(r".../hardware/lab/art/grove_kart/build_grove_kart.py", encoding="utf-8").read())
    build_all()            # 全オブジェクトを作り直す（palette 画像も作る）
    show_only(["course_road"])   # 書き出す前に対象だけ表示（kart_* は原点に重なるため）
書き出しは必ずエンジンのツールで:
    dx12_blender_export {objects:["course_road"], destPath:"models/kart/course_road.glb"}
    course_ground / course_props / course_trees_0..3 / kart_red,blue,green,yellow / itembox も同様。

座標: モデルのワールド座標 = エンジンのワールド座標（原点に identity で置く）。
  エンジン (x 右, y 上, z 奥) -> Blender (x, z, y)  … B() で変換。MIRROR=False なら (x, -z, y)。
全メッシュは「パレット 1 枚（256x256 の 16x16 色見本）」の見本に UV を落とすだけ（エンジンはテクスチャ無し=真っ白のため）。
"""
import bpy, math, json, os, random
import numpy as np
from mathutils import Matrix, Vector

# このファイルの場所から決める（exec で読んだときは __file__ が無いので既定の場所）
ART = (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals()
       else r"C:\Users\ryuto\Documents\dx12\hardware\lab\art\grove_kart")
LAYOUT = os.path.join(ART, "..", "..", "GroveLab", "assets", "kart", "layout.json")
MIRROR = False

# ---------------------------------------------------------------- パレット
PAL_DEF = """
asphalt 555a68
asphalt2 4d525f
white f6f6f0
black 25262e
lineyellow ffe36a
red ee3b34
redd b52621
blue 3478e8
blued 214fb0
green 3fc95e
greend 26953f
yellow ffd83c
yellowd d9a51c
orange ff8d2b
purple 8c52dc
pink ff82b4
cyan 40d6e6
skin ffc9a0
darkgrey 3d404c
grey 9aa1b0
lightgrey d3d7de
silver c9cfda
brown 8c5b34
brownd 6a4225
wood bf8b55
visor 2a3a66
grass1 74d34d
grass2 66c846
grass3 83dc5c
grassd 52b23c
forestfloor 3f9a3a
leafg1 52d45e
leafg2 3fc05c
leafg3 7ae872
leafo ff9d30
leafr ea573b
leafp ff9ccb
pine1 2db35e
pine2 3fca78
sand f2d88e
sand2 e8c67a
shore eadb9f
lakefloor 2c85ae
water1 3ccaf2
water2 2cabe3
waterd 1d82c4
rock a8b8ec
rockd 8a9ad8
snow f5f9ff
mntgreen 62c874
mntgreend 4db865
standwall c6cbd6
seat1 3b6fd8
seat2 d8453b
tunnelwall e4dcc8
tunnelroof b9b19c
lamp fff3a0
gold ffc531
mint 7be8c0
sky 8fd3ff
LEAF 52d45e
LEAF2 3fc05c
""".strip().splitlines()
PAL_NAMES = [l.split()[0] for l in PAL_DEF]
PAL_HEX = [l.split()[1] for l in PAL_DEF]
PAL = {n: i for i, n in enumerate(PAL_NAMES)}
CELLS = 16
IMG = 256


def hex2rgb(h):
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def build_palette():
    img = bpy.data.images.get("grove_palette")
    if img:
        bpy.data.images.remove(img)
    img = bpy.data.images.new("grove_palette", IMG, IMG, alpha=False)
    px = np.ones((IMG, IMG, 4), dtype=np.float32)
    cell = IMG // CELLS
    for i in range(CELLS * CELLS):
        col = hex2rgb(PAL_HEX[i]) if i < len(PAL_HEX) else (0.9, 0.2, 0.9)
        row, cx = divmod(i, CELLS)           # row 0 = 画像の上
        y0 = IMG - (row + 1) * cell           # Blender の画像は下が 0
        px[y0:y0 + cell, cx * cell:(cx + 1) * cell, :3] = col
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(ART, "grove_kart_palette.png")
    img.file_format = 'PNG'
    img.save()
    img.pack()
    return img


def swatch_uv(ci):
    row, cx = divmod(ci, CELLS)
    return ((cx + 0.5) / CELLS, 1.0 - (row + 0.5) / CELLS)


_MATS = {}


def get_mat(kind):
    if kind in _MATS and _MATS[kind].name in bpy.data.materials:
        return _MATS[kind]
    img = bpy.data.images["grove_palette"]
    mat = bpy.data.materials.new("pal_" + kind)
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = 'Linear'
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    if kind == "main":
        bsdf.inputs["Roughness"].default_value = 0.85
        bsdf.inputs["Metallic"].default_value = 0.0
    elif kind == "water":
        bsdf.inputs["Roughness"].default_value = 0.12
        bsdf.inputs["Metallic"].default_value = 0.35
    elif kind == "mnt":      # 遠景の山: 影側が真っ黒にならないよう弱く自発光させる
        bsdf.inputs["Roughness"].default_value = 0.95
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 0.35
    elif kind == "emit":
        bsdf.inputs["Roughness"].default_value = 0.35
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    _MATS[kind] = mat
    return mat


# ---------------------------------------------------------------- メッシュ蓄積
def B(p):
    x, y, z = p
    return (x, z if MIRROR else -z, y)


I4 = Matrix.Identity(4)


class M:
    def __init__(self, name, mats=("main",)):
        self.name, self.mats = name, list(mats)
        self.v, self.f = [], []
        self.remap = {}

    def vert(self, p):
        self.v.append((float(p[0]), float(p[1]), float(p[2])))
        return len(self.v) - 1

    def face(self, pts, col, hint=None, mat=0):
        ids = [self.vert(p) for p in pts]
        if isinstance(col, str):
            col = PAL[self.remap.get(col, col)]
        if hint is not None:
            q = [B(self.v[i]) for i in ids]
            nx = ny = nz = 0.0
            for a in range(len(q)):
                p0, p1 = q[a], q[(a + 1) % len(q)]
                nx += (p0[1] - p1[1]) * (p0[2] + p1[2])
                ny += (p0[2] - p1[2]) * (p0[0] + p1[0])
                nz += (p0[0] - p1[0]) * (p0[1] + p1[1])
            h = B(hint)
            if nx * h[0] + ny * h[1] + nz * h[2] < 0:
                ids = ids[::-1]
        self.f.append((ids, col, mat))

    def append(self, other, pos=(0, 0, 0), yaw=0.0, sc=1.0, remap=None):
        """other（原点基準のテンプレ）を回転 yaw・拡大 sc・移動 pos して取り込む（面の向きはそのまま）"""
        xf = Matrix.Translation(pos) @ Matrix.Rotation(yaw, 4, 'Y') @ Matrix.Scale(sc, 4)
        base = len(self.v)
        for v in other.v:
            w = xf @ Vector(v)
            self.v.append((w.x, w.y, w.z))
        for ids, col, mat in other.f:
            c = col
            if remap:
                c = PAL[remap.get(PAL_NAMES[col], PAL_NAMES[col])]
            self.f.append(([i + base for i in ids], c, mat))

    def tris(self):
        return sum(len(f[0]) - 2 for f in self.f)

    def build(self):
        old = bpy.data.objects.get(self.name)
        if old:
            bpy.data.objects.remove(old, do_unlink=True)
        me = bpy.data.meshes.new(self.name)
        me.from_pydata([B(v) for v in self.v], [], [f[0] for f in self.f])
        uv = me.uv_layers.new(name="UVMap")
        flat = []
        for ids, col, mat in self.f:
            u, v = swatch_uv(col)
            flat.extend((u, v) * len(ids))
        uv.data.foreach_set("uv", flat)
        me.polygons.foreach_set("material_index", [f[2] for f in self.f])
        me.polygons.foreach_set("use_smooth", [False] * len(self.f))
        me.update()
        for k in self.mats:
            me.materials.append(get_mat(k))
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob


# ---------------------------------------------------------------- プリミティブ（エンジン座標・面の向きは hint で自動）
def _rot(xf):
    return xf.to_3x3()


def _T(xf, p):
    w = xf @ Vector(p)
    return (w.x, w.y, w.z)


def box(m, size, col, xf=None, skip=(), center=(0, 0, 0)):
    xf = xf or I4
    sx, sy, sz = size[0] / 2, size[1] / 2, size[2] / 2
    cx, cy, cz = center
    R = _rot(xf)

    def P(x, y, z):
        return _T(xf, (cx + x, cy + y, cz + z))
    defs = {
        "px": ((1, 0, 0), [(sx, -sy, -sz), (sx, sy, -sz), (sx, sy, sz), (sx, -sy, sz)]),
        "nx": ((-1, 0, 0), [(-sx, -sy, -sz), (-sx, -sy, sz), (-sx, sy, sz), (-sx, sy, -sz)]),
        "py": ((0, 1, 0), [(-sx, sy, -sz), (-sx, sy, sz), (sx, sy, sz), (sx, sy, -sz)]),
        "ny": ((0, -1, 0), [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, -sy, sz), (-sx, -sy, sz)]),
        "pz": ((0, 0, 1), [(-sx, -sy, sz), (sx, -sy, sz), (sx, sy, sz), (-sx, sy, sz)]),
        "nz": ((0, 0, -1), [(-sx, -sy, -sz), (-sx, sy, -sz), (sx, sy, -sz), (sx, -sy, -sz)]),
    }
    for name, (n, pts) in defs.items():
        if name in skip:
            continue
        c = col.get(name, col.get("all")) if isinstance(col, dict) else col
        h = R @ Vector(n)
        m.face([P(*p) for p in pts], c, (h.x, h.y, h.z))


def circle(r, n, phase=0.0):
    return [(r * math.cos(2 * math.pi * a / n + phase), r * math.sin(2 * math.pi * a / n + phase)) for a in range(n)]


def loft(m, rings, cols, xf=None, cap0=None, cap1=None, mat=0):
    """rings = [(y, [(x,z)...]), ...]（局所 Y 軸まわりに積む）。点が 1 つの輪は頂点（先端）"""
    xf = xf or I4
    R = _rot(xf)
    for k in range(len(rings) - 1):
        y0, r0 = rings[k]
        y1, r1 = rings[k + 1]
        allp = r0 + r1
        cx = sum(p[0] for p in allp) / len(allp)
        cz = sum(p[1] for p in allp) / len(allp)

        def col_of(a):
            return cols(k, a) if callable(cols) else (cols[k] if isinstance(cols, (list, tuple)) else cols)
        if len(r0) == 1 or len(r1) == 1:
            if len(r0) == 1:
                ap, ya, base, yb = r0[0], y0, r1, y1
            else:
                ap, ya, base, yb = r1[0], y1, r0, y0
            nb = len(base)
            for a in range(nb):
                p, q = base[a], base[(a + 1) % nb]
                tri = [(ap[0], ya, ap[1]), (p[0], yb, p[1]), (q[0], yb, q[1])]
                hx = (tri[0][0] + tri[1][0] + tri[2][0]) / 3 - cx
                hz = (tri[0][2] + tri[1][2] + tri[2][2]) / 3 - cz
                h = R @ Vector((hx, 0.001, hz))
                m.face([_T(xf, t) for t in tri], col_of(a), (h.x, h.y, h.z), mat)
        else:
            n = len(r0)
            for a in range(n):
                b = (a + 1) % n
                q = [(r0[a][0], y0, r0[a][1]), (r0[b][0], y0, r0[b][1]), (r1[b][0], y1, r1[b][1]), (r1[a][0], y1, r1[a][1])]
                hx = sum(t[0] for t in q) / 4 - cx
                hz = sum(t[2] for t in q) / 4 - cz
                h = R @ Vector((hx, 0.001, hz))
                m.face([_T(xf, t) for t in q], col_of(a), (h.x, h.y, h.z), mat)
    if cap0 is not None:
        y, r = rings[0]
        h = R @ Vector((0, -1, 0))
        m.face([_T(xf, (p[0], y, p[1])) for p in r], cap0, (h.x, h.y, h.z), mat)
    if cap1 is not None and len(rings[-1][1]) > 2:
        y, r = rings[-1]
        h = R @ Vector((0, 1, 0))
        m.face([_T(xf, (p[0], y, p[1])) for p in r], cap1, (h.x, h.y, h.z), mat)


def cylinder(m, r, h, seg, col, xf=None, y0=0.0, cap0=None, cap1="same", mat=0, phase=0.0):
    c = circle(r, seg, phase)
    if cap1 == "same":
        cap1 = col if not callable(col) else None
    loft(m, [(y0, c), (y0 + h, c)], col, xf, cap0, cap1, mat)


def cone(m, r, h, seg, col, xf=None, y0=0.0, cap0=None):
    loft(m, [(y0, circle(r, seg)), (y0 + h, [(0.0, 0.0)])], col, xf, cap0, None)


def frustum(m, r0, r1, h, seg, col, xf=None, y0=0.0, cap0=None, cap1=None):
    loft(m, [(y0, circle(r0, seg)), (y0 + h, circle(r1, seg))], col, xf, cap0, cap1)


def hull(m, secs, col, xf=None, caps=True):
    """secs = [(z, halfwidth, halfheight, ycenter), ...] 前後に伸びる断面つなぎ（車体用）"""
    xf = xf or I4
    R = _rot(xf)
    def corners(s):
        z, hw, hh, yc = s
        return [(-hw, yc - hh, z), (hw, yc - hh, z), (hw, yc + hh, z), (-hw, yc + hh, z)]
    for k in range(len(secs) - 1):
        a, b = corners(secs[k]), corners(secs[k + 1])
        yc = (secs[k][3] + secs[k + 1][3]) / 2
        for i in range(4):
            j = (i + 1) % 4
            q = [a[i], a[j], b[j], b[i]]
            cx = sum(t[0] for t in q) / 4
            cy = sum(t[1] for t in q) / 4 - yc
            hn = R @ Vector((cx, cy, 0))
            c = col[i] if isinstance(col, (list, tuple)) else col
            m.face([_T(xf, t) for t in q], c, (hn.x, hn.y, hn.z))
    if caps:
        h = R @ Vector((0, 0, -1))
        m.face([_T(xf, t) for t in corners(secs[0])], col[0] if isinstance(col, (list, tuple)) else col, (h.x, h.y, h.z))
        h = R @ Vector((0, 0, 1))
        m.face([_T(xf, t) for t in corners(secs[-1])], col[0] if isinstance(col, (list, tuple)) else col, (h.x, h.y, h.z))


_ICO = {}


def ico(detail):
    if detail in _ICO:
        return _ICO[detail]
    t = (1 + 5 ** 0.5) / 2
    vs = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t), (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    vs = [tuple(np.array(v) / np.linalg.norm(v)) for v in vs]
    fs = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
          (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    for _ in range(detail):
        cache, nf = {}, []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                p = np.array(vs[a]) + np.array(vs[b])
                vs.append(tuple(p / np.linalg.norm(p)))
                cache[key] = len(vs) - 1
            return cache[key]
        for a, b, c in fs:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        fs = nf
    _ICO[detail] = (vs, fs)
    return _ICO[detail]


def blob(m, r, col, xf=None, detail=1, sc=(1, 1, 1)):
    xf = xf or I4
    R = _rot(xf)
    vs, fs = ico(detail)
    for a, b, c in fs:
        pts = [(vs[i][0] * r * sc[0], vs[i][1] * r * sc[1], vs[i][2] * r * sc[2]) for i in (a, b, c)]
        cen = np.mean([vs[i] for i in (a, b, c)], axis=0)
        h = R @ Vector(tuple(cen))
        cc = col(tuple(cen)) if callable(col) else col
        m.face([_T(xf, p) for p in pts], cc, (h.x, h.y, h.z))


def extrude(m, pts, y0, y1, col_top, col_side=None, mat=0):
    """凸多角形 pts=[(x,z)...] を y0..y1 に押し出す（底面なし）"""
    col_side = col_side or col_top
    cx = sum(p[0] for p in pts) / len(pts)
    cz = sum(p[1] for p in pts) / len(pts)
    m.face([(p[0], y1, p[1]) for p in pts], col_top, (0, 1, 0), mat)
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        mx, mz = (a[0] + b[0]) / 2 - cx, (a[1] + b[1]) / 2 - cz
        m.face([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], col_side, (mx, 0, mz), mat)


def xfm(pos=(0, 0, 0), yaw=0.0, pitch=0.0, roll=0.0):
    return Matrix.Translation(pos) @ Matrix.Rotation(yaw, 4, 'Y') @ Matrix.Rotation(pitch, 4, 'X') @ Matrix.Rotation(roll, 4, 'Z')


# ---------------------------------------------------------------- コース情報
def ss(x, a, b):
    t = np.clip((np.asarray(x, float) - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


class Course:
    def __init__(self):
        d = json.load(open(LAYOUT, encoding="utf-8"))
        self.d = d
        self.P = np.array(d["centerline"], float)
        self.N = len(self.P)
        T = np.roll(self.P, -1, 0) - np.roll(self.P, 1, 0)
        T /= np.linalg.norm(T, axis=1)[:, None]
        self.T = T
        self.R = np.stack([T[:, 1], -T[:, 0]], 1)         # 右向き
        self.W = d["width"]
        self.HW = self.W / 2
        self.cx = float((self.P[:, 0].min() + self.P[:, 0].max()) / 2)
        self.cz = float((self.P[:, 1].min() + self.P[:, 1].max()) / 2)
        self.zone = {k: tuple(int(round(x)) for x in v["s"]) for k, v in d["zones"].items()}

    def pt(self, i, lat, y):
        i = int(math.floor(i)) % self.N if not isinstance(i, int) else i % self.N
        return (self.P[i, 0] + self.R[i, 0] * lat, y, self.P[i, 1] + self.R[i, 1] * lat)

    def ptf(self, s, lat, y):
        """小数の s（間は補間）"""
        a = int(math.floor(s))
        f = s - a
        p0, p1 = self.pt(a, lat, y), self.pt(a + 1, lat, y)
        return (p0[0] + (p1[0] - p0[0]) * f, y, p0[2] + (p1[2] - p0[2]) * f)

    def yaw(self, i):
        i %= self.N
        return math.atan2(self.T[i, 0], self.T[i, 1])

    def fwd(self, i):
        i %= self.N
        return (self.T[i, 0], self.T[i, 1])

    def rng(self, a, b, step=1):
        a, b = int(a), int(b)
        if b < a:
            b += self.N
        return list(range(a, b + 1, step))

    def in_zone(self, j, name, margin=0):
        a, b = self.zone[name]
        L = (b - a) % self.N
        return ((np.asarray(j) - (a - margin)) % self.N) <= (L + 2 * margin)

    def turn_sum(self, name):
        a, b = self.zone[name]
        t = 0.0
        for i in self.rng(a, b):
            p, q, r = self.P[(i - 1) % self.N], self.P[i % self.N], self.P[(i + 1) % self.N]
            t += (q[0] - p[0]) * (r[1] - q[1]) - (q[1] - p[1]) * (r[0] - q[0])
        return t

    def outside(self, name):
        """カーブの外側の符号（lat の +右 / -左）。右カーブ(turn<0)なら外は左(-1)"""
        return -1 if self.turn_sum(name) < 0 else 1

    def near(self, X, Z):
        X = np.asarray(X, float)
        shp = X.shape
        xf, zf = X.ravel(), np.asarray(Z, float).ravel()
        D = np.empty(len(xf)); J = np.empty(len(xf), int); LAT = np.empty(len(xf))
        for a in range(0, len(xf), 1500):
            dx = xf[a:a + 1500, None] - self.P[None, :, 0]
            dz = zf[a:a + 1500, None] - self.P[None, :, 1]
            d2 = dx * dx + dz * dz
            j = d2.argmin(1)
            rows = np.arange(len(j))
            D[a:a + 1500] = np.sqrt(d2[rows, j])
            J[a:a + 1500] = j
            LAT[a:a + 1500] = dx[rows, j] * self.R[j, 0] + dz[rows, j] * self.R[j, 1]
        return D.reshape(shp), J.reshape(shp), LAT.reshape(shp)

    # ---- 地面の高さ（木や小物の置き場所にも使う）
    def lake_factor(self, d, j):
        bz = self.zone["lakeBridge"]
        a, b = bz
        j = np.asarray(j, float)
        bf = ss(j, a - 8, a + 8) * (1 - ss(j, b - 8, b + 8))
        prof = 1 - ss(d, 13, 34)
        return bf * prof

    def hills(self, X, Z, d):
        X = np.asarray(X, float); Z = np.asarray(Z, float)
        n = (np.sin(X * 0.011 + 1.3) * np.cos(Z * 0.013 + 0.4) + 0.6 * np.sin(X * 0.027 + Z * 0.019 + 2.0)
             + 0.35 * np.cos(X * 0.05 - Z * 0.041))
        far = 1 + np.sqrt((X - self.cx) ** 2 + (Z - self.cz) ** 2) / 180.0
        n = (n + 1.95) * 0.5
        return 6.5 * n * far * ss(d - self.HW, 26, 60)

    def ground_h(self, X, Z):
        d, j, lat = self.near(X, Z)
        lake = self.lake_factor(d, j)
        base = -0.04 - 0.08 * (1 - ss(d, 6.5, 9.0))     # 道の下だけ少し低く（遠景の Z ファイト対策）
        return base + self.hills(X, Z, d) - 3.6 * lake, d, j, lat, lake


# ---------------------------------------------------------------- 掃引（道に沿った断面押し出し）
def sweep(m, C, idxs, prof, cols, closed=False, mat=0, caps=False):
    """prof = [(lat, y), ...]  右が + の (横, 高さ)。左→右の上面が上向き、時計回りの閉断面が外向き。
    cols: list（辺ごと）か callable(k, j)->色名|None。idxs は整数 s のリスト（隣同士を結ぶ）"""
    ne = len(prof) if closed else len(prof) - 1
    for k in range(len(idxs) - 1):
        i0, i1 = idxs[k], idxs[k + 1]
        for j in range(ne):
            (l0, y0), (l1, y1) = prof[j], prof[(j + 1) % len(prof)]
            if abs(l1 - l0) < 1e-9 and abs(y1 - y0) < 1e-9:
                continue
            c = cols(k, j) if callable(cols) else cols[j]
            if c is None:
                continue
            q = [C.pt(i0, l0, y0), C.pt(i0, l1, y1), C.pt(i1, l1, y1), C.pt(i1, l0, y0)]
            r = C.R[i0 % C.N]
            dl, dy = l1 - l0, y1 - y0
            hint = (r[0] * (-dy), dl, r[1] * (-dy))
            m.face(q, c, hint, mat)
    if caps and closed:
        for idx, sgn in ((idxs[0], -1), (idxs[-1], 1)):
            f = C.fwd(idx)
            m.face([C.pt(idx, l, y) for l, y in prof], cols[0] if not callable(cols) else cols(0, 0), (f[0] * sgn, 0, f[1] * sgn), mat)


def box_at(m, C, s, lat, yc, size, col, dyaw=0.0, skip=()):
    """s（小数可）, lat の位置に、道の向きに合わせて箱を置く。size=(横, 高さ, 奥行き) 中心の高さ yc"""
    p = C.ptf(s, lat, yc)
    xf = xfm(p, C.yaw(int(math.floor(s))) + dyaw)
    box(m, size, col, xf, skip)


def local_xf(C, s, lat, y, dyaw=0.0):
    p = C.ptf(s, lat, y)
    return xfm(p, C.yaw(int(math.floor(s))) + dyaw)


# ---------------------------------------------------------------- 道
def build_road(C):
    m = M("course_road")
    N = C.N
    idxs = list(range(1, N + 1, 2)) + [N + 1]
    nseg = len(idxs) - 1
    Y = 0.015
    prof = [(-6, Y), (-5.7, Y), (-5.4, Y), (-0.15, Y), (0.15, Y), (5.4, Y), (5.7, Y), (6, Y)]

    def road_cols(k, j):
        if k == nseg - 1:           # スタート線の区間は市松で別に作る
            return None
        tone = "asphalt" if (k // 6) % 2 == 0 else "asphalt2"
        if j in (1, 5):
            return "white"
        if j == 3:
            return "lineyellow" if k % 3 == 0 else tone
        return tone
    sweep(m, C, idxs, prof, road_cols)
    # 市松のスタート線（s=-1..1）
    for r, (sa, sb) in enumerate(((N - 1, N), (N, N + 1))):
        for c in range(-6, 6):
            col = "white" if (c + r) % 2 == 0 else "black"
            m.face([C.pt(sa, c, Y), C.pt(sa, c + 1, Y), C.pt(sb, c + 1, Y), C.pt(sb, c, Y)], col, (0, 1, 0))
    # 縁石（赤白）
    ci = list(range(0, N + 1, 2))

    def curb_cols(k, j):
        return None if j == 3 else ("red" if k % 2 == 0 else "white")
    sweep(m, C, ci, [(-7, 0.0), (-7, 0.045), (-6, 0.045), (-6, 0.0)], curb_cols, closed=True)
    sweep(m, C, ci, [(6, 0.0), (6, 0.045), (7, 0.045), (7, 0.0)], curb_cols, closed=True)
    # 大回りの外側の大きな縁石
    a, b = C.zone["turn1"]
    o = C.outside("turn1")
    wide = C.rng(a - 4, b + 4, 2)
    pr = [(7.0, 0.03), (9.6, 0.03)] if o > 0 else [(-9.6, 0.03), (-7.0, 0.03)]
    sweep(m, C, wide, pr, lambda k, j: "red" if (k // 2) % 2 == 0 else "white")
    # スタート前のグリッド（16m）: 4 列 x 左右 2 台、白い枠
    G0, G1 = 0.015, 0.03
    for row in range(4):
        sc = N - 4 - row * 4
        for lat in (-2.9, 2.9):
            lc = sc
            hw, depth, t = 1.2, 3.0, 0.14
            # 前線・左線・右線（凸の細い長方形）
            def rect(s0, s1, l0, l1):
                return [C.ptf(s0, l0, 0)[0::2], C.ptf(s0, l1, 0)[0::2], C.ptf(s1, l1, 0)[0::2], C.ptf(s1, l0, 0)[0::2]]
            extrude(m, rect(lc + depth, lc + depth + t, lat - hw, lat + hw), G0, G1, "white")
            extrude(m, rect(lc, lc + depth + t, lat - hw, lat - hw + t), G0, G1, "white")
            extrude(m, rect(lc, lc + depth + t, lat + hw - t, lat + hw), G0, G1, "white")
    # ヘアピン前の矢印（右カーブなら右へ）
    hp = C.zone["hairpin"][0]
    sgn = -1 if C.outside("hairpin") > 0 else 1
    ang = math.radians(32) * (1 if sgn > 0 else -1)
    for sc in (hp - 28, hp - 20, hp - 12):
        for lat0 in (-2.6, 2.6):
            def to_w(u, w):
                uu = u * math.cos(ang) + w * math.sin(ang)
                ww = -u * math.sin(ang) + w * math.cos(ang)
                p = C.ptf(sc, lat0, 0)
                f = C.fwd(sc)
                r = (f[1], -f[0])
                return (p[0] + r[0] * uu + f[0] * ww, p[2] + r[1] * uu + f[1] * ww)
            shaft = [to_w(-0.28, 0), to_w(0.28, 0), to_w(0.28, 2.4), to_w(-0.28, 2.4)]
            head = [to_w(-0.95, 2.4), to_w(0.95, 2.4), to_w(0, 4.0)]
            extrude(m, shaft, G0, G1, "white")
            extrude(m, head, G0, G1, "white")
    return m


# ---------------------------------------------------------------- 地面・湖・山
def build_ground(C):
    m = M("course_ground", mats=("main", "water", "mnt"))
    cx, cz = round(C.cx), round(C.cz)
    xs = np.concatenate([np.arange(cx - 300, cx - 160, 20), np.arange(cx - 160, cx + 160, 5), np.arange(cx + 160, cx + 301, 20)]).astype(float)
    zs = np.concatenate([np.arange(cz - 300, cz - 160, 20), np.arange(cz - 160, cz + 160, 5), np.arange(cz + 160, cz + 301, 20)]).astype(float)
    XX, ZZ = np.meshgrid(xs, zs, indexing="ij")
    H, d, j, lat, lake = C.ground_h(XX, ZZ)
    # セル中心
    xc = (xs[:-1] + xs[1:]) / 2
    zc = (zs[:-1] + zs[1:]) / 2
    XC, ZC = np.meshgrid(xc, zc, indexing="ij")
    _, dc, jc, _, _ = C.ground_h(XC, ZC)
    forest = C.in_zone(jc, "forest", 12) & (dc < 70)
    rnd = random.Random(5)
    for a in range(len(xs) - 1):
        for b in range(len(zs) - 1):
            hh = [H[a, b], H[a + 1, b], H[a + 1, b + 1], H[a, b + 1]]
            hm = sum(hh) / 4
            if hm < -0.6 + 0.0:
                col = "lakefloor"
            elif hm < -0.2:
                col = "shore"
            elif forest[a, b]:
                col = "forestfloor" if (a + b) % 2 == 0 else "grassd"
            else:
                par = (int(xc[a] // 10) + int(zc[b] // 10)) % 2
                if hm > 4.0:
                    col = "grass3" if par == 0 else "grass1"
                else:
                    col = "grass1" if par == 0 else "grass2"
                if dc[a, b] > 90 and rnd.random() < 0.12:
                    col = "grass3"
            pts = [(xs[a], hh[0], zs[b]), (xs[a + 1], hh[1], zs[b]), (xs[a + 1], hh[2], zs[b + 1]), (xs[a], hh[3], zs[b + 1])]
            m.face(pts, col, (0, 1, 0))
    # 縁の壁（下へ）
    e = 12.0
    x0, x1, z0, z1 = xs[0], xs[-1], zs[0], zs[-1]
    for pts, hn in (([(x0, 0, z0), (x1, 0, z0)], (0, 0, -1)), ([(x0, 0, z1), (x1, 0, z1)], (0, 0, 1))):
        (a, _, c), (b, _, c2) = pts
        m.face([(a, 0, c), (b, 0, c2), (b, -e, c2), (a, -e, c)], "mntgreend", hn)
    for pts, hn in (([(x0, 0, z0), (x0, 0, z1)], (-1, 0, 0)), ([(x1, 0, z0), (x1, 0, z1)], (1, 0, 0))):
        (a, _, c), (_, _, c2) = pts
        m.face([(a, 0, c), (a, 0, c2), (a, -e, c2), (a, -e, c)], "mntgreend", hn)
    # 砂地（ヘアピンと最終コーナーの外側）
    for zn in ("hairpin", "finalCorner"):
        a, b = C.zone[zn]
        o = C.outside(zn)
        idx = C.rng(a - 8, b + 8, 2)
        pr = [(7.2, 0.01), (20.0, 0.01)] if o > 0 else [(-20.0, 0.01), (-7.2, 0.01)]
        sweep(m, C, idx, pr, lambda k, jj: "sand" if (k // 2) % 2 == 0 else "sand2")
    # 水面（湖）: ground が -0.6 より低いセルの範囲だけ
    lakecells = []
    for a in range(len(xs) - 1):
        for b in range(len(zs) - 1):
            if max(lake[a, b], lake[a + 1, b], lake[a + 1, b + 1], lake[a, b + 1]) > 0.1:
                lakecells.append((a, b))
    if lakecells:
        A = np.array(lakecells)
        a0, a1, b0, b1 = A[:, 0].min(), A[:, 0].max() + 1, A[:, 1].min(), A[:, 1].max() + 1
        step = 10.0
        wx = np.arange(xs[a0] - 5, xs[a1] + 5 + 1, step)
        wz = np.arange(zs[b0] - 5, zs[b1] + 5 + 1, step)
        for ia in range(len(wx) - 1):
            for ib in range(len(wz) - 1):
                col = "water1" if (ia + ib) % 2 == 0 else "water2"
                m.face([(wx[ia], -0.6, wz[ib]), (wx[ia + 1], -0.6, wz[ib]), (wx[ia + 1], -0.6, wz[ib + 1]), (wx[ia], -0.6, wz[ib + 1])],
                       col, (0, 1, 0), 1)
    # 遠くの山
    rng = random.Random(21)
    half = 292.0
    n = 46
    per = 8 * half
    for q in range(n):
        t = (q + rng.random() * 0.6) / n * per
        side, u = divmod(t, 2 * half)
        u -= half
        pos = [(u, -half), (half, u), (-u, half), (-half, -u)][int(side) % 4]
        mx, mz = cx + pos[0] + rng.uniform(-20, 20), cz + pos[1] + rng.uniform(-20, 20)
        R = rng.uniform(55, 100)
        Hh = rng.uniform(50, 125)
        seg = 8
        ph = rng.random() * 6.28

        def ring(r):
            return [(r * (1 + rng.uniform(-0.14, 0.14)) * math.cos(2 * math.pi * a / seg + ph),
                     r * (1 + rng.uniform(-0.14, 0.14)) * math.sin(2 * math.pi * a / seg + ph)) for a in range(seg)]
        rings = [(-8, ring(R)), (0.34 * Hh, ring(R * 0.62)), (0.72 * Hh, ring(R * 0.3)), (Hh, [(rng.uniform(-3, 3), rng.uniform(-3, 3))])]

        def mc(k, a):
            if k == 0:
                return "mntgreen" if a % 2 == 0 else "mntgreend"
            if k == 1:
                return "rock" if a % 2 == 0 else "rockd"
            return "snow"
        loft(m, rings, mc, xfm((mx, 0, mz)), mat=2)
    return m


# ---------------------------------------------------------------- 木
def tpl_round(rng):
    t = M("tpl")
    cylinder(t, 0.32, 1.9, 5, "brown", cap0=None, cap1="brown")
    blob(t, 1.75, "LEAF", xfm((0, 3.0, 0)), 1, (1.0, 0.9, 1.0))
    if rng.random() < 0.6:
        blob(t, 1.15, "LEAF2", xfm((0.9, 2.5, 0.5)), 1, (1, 0.9, 1))
    return t


def tpl_pine(rng):
    t = M("tpl")
    cylinder(t, 0.28, 1.5, 5, "brownd")
    for k, (r, y) in enumerate(((2.0, 1.2), (1.55, 2.5), (1.1, 3.7))):
        cone(t, r, 2.2, 7, "LEAF" if k != 1 else "LEAF2", y0=y, cap0="LEAF" if k == 0 else None)
    return t


def tpl_tall(rng):
    t = M("tpl")
    cylinder(t, 0.3, 1.4, 5, "brown")
    blob(t, 1.3, "LEAF", xfm((0, 3.6, 0)), 1, (0.85, 1.9, 0.85))
    return t


def build_trees(C):
    rng = random.Random(17)
    P = []

    def ok(x, z, d, j, lat, minclr):
        if d - C.HW < minclr:
            return False
        for zn in ("hairpin", "finalCorner"):
            if C.in_zone(j, zn, 10) and d < 28:
                return False
        if C.in_zone(j, "tunnel", 10) and d < 26:
            return False
        if C.in_zone(j, "grandstand", 12) and lat < 0 and d < 60:
            return False
        if C.in_zone(j, "turn1", 6) and C.outside("turn1") * lat > 0 and d < 26:
            return False
        h, _, _, _, lk = C.ground_h(np.array([x]), np.array([z]))
        if lk[0] > 0.01 or h[0] < -0.2:
            return False
        for (px, pz) in P:
            if (px - x) ** 2 + (pz - z) ** 2 < 3.4 ** 2:
                return False
        for ex, ez, er in AVOID:
            if (ex - x) ** 2 + (ez - z) ** 2 < er * er:
                return False
        return True
    fa, fb = C.zone["forest"]
    tries = 0
    while len(P) < 110 and tries < 6000:
        tries += 1
        s = rng.uniform(fa - 6, fb + 6)
        side = rng.choice((-1, 1))
        lt = side * (C.HW + 8.5 + rng.random() ** 1.6 * 30)
        p = C.ptf(s, lt, 0)
        d, j, lat = C.near([p[0]], [p[2]])
        if ok(p[0], p[2], d[0], j[0], lat[0], 8.0):
            P.append((p[0], p[2]))
    nforest = len(P)
    tries = 0
    while len(P) < nforest + 100 and tries < 8000:
        tries += 1
        x = rng.uniform(C.cx - 230, C.cx + 230)
        z = rng.uniform(C.cz - 230, C.cz + 230)
        d, j, lat = C.near([x], [z])
        if d[0] > 110 and rng.random() < 0.8:
            continue
        if ok(x, z, d[0], j[0], lat[0], 12.0):
            P.append((x, z))
    tpls = [tpl_round(rng), tpl_pine(rng), tpl_tall(rng)]
    objs = [M(f"course_trees_{q}", mats=("mnt",)) for q in range(4)]   # 葉は影側が真っ黒にならないよう弱い自発光
    arr = np.array(P)
    H = C.ground_h(arr[:, 0], arr[:, 1])[0]
    leaf_cols = [("leafg1", "leafg3"), ("leafg2", "leafg1"), ("leafg3", "leafg2"), ("leafo", "leafr"), ("leafp", "leafg3")]
    pine_cols = [("pine1", "pine2"), ("pine2", "pine1")]
    for (x, z), h in zip(P, H):
        kind = rng.choices([0, 1, 2], weights=[4, 4, 1.4])[0]
        if kind == 1:
            a, b = rng.choice(pine_cols)
        else:
            a, b = rng.choices(leaf_cols, weights=[4, 4, 3, 0.7, 0.5])[0]
        q = (1 if x >= C.cx else 0) + (2 if z >= C.cz else 0)
        objs[q].append(tpls[kind], (x, h - 0.1, z), rng.uniform(0, 6.28), rng.uniform(0.8, 1.45), {"LEAF": a, "LEAF2": b})
    return objs, len(P), nforest


AVOID = []


# ---------------------------------------------------------------- 小物（ゲート・観客席・トンネル・橋・タイヤ…）
def build_props(C):
    m = M("course_props", mats=("mnt",))   # 影側が真っ黒にならないよう弱い自発光
    N = C.N
    rnd = random.Random(3)
    sg = C.d["start"]["gate"]
    po, bh, bt = sg["poleOffset"], sg["barHeight"], sg["barThick"]
    # ---- スタートゲート（s=0）。バーの前面は +0.3m（信号球が手前 -0.5m に来る）
    fz = 1.0
    for sd in (-1, 1):
        box_at(m, C, 0, sd * po, 4.1, (1.2, 8.2, bt), {"all": "red", "py": "redd"}, skip=("ny",))
        box_at(m, C, 0, sd * po, 0.3, (1.7, 0.6, bt + 0.6), "darkgrey", skip=("ny",))
        box_at(m, C, 0 + 0.0, sd * po, 5.0, (1.24, 0.5, bt + 0.04), "white", skip=("ny",))
    xfb = local_xf(C, 0, 0, bh)
    # 本体バー: 中心 fwd=+1.0
    pb = C.ptf(0, 0, bh)
    f = C.fwd(0)
    barc = (pb[0] + f[0] * fz, bh, pb[2] + f[1] * fz)
    box(m, (2 * po + 1.2, bt, bt), {"all": "darkgrey", "pz": "darkgrey", "py": "grey"}, xfm(barc, C.yaw(0)), skip=("ny",))
    # バー前面の市松（面から 3cm 手前）
    ncell = int((2 * po) / 0.7)
    cw = 2 * po / ncell
    for r in range(2):
        for c in range(ncell):
            lat0 = -po + c * cw
            y0 = bh - 0.7 + r * 0.7
            col = "white" if (c + r) % 2 == 0 else "black"
            q = [C.ptf(0.27, lat0, y0), C.ptf(0.27, lat0 + cw, y0), C.ptf(0.27, lat0 + cw, y0 + 0.7), C.ptf(0.27, lat0, y0 + 0.7)]
            m.face(q, col, (-f[0], 0, -f[1]))
    # 上の看板
    box(m, (9.0, 1.7, 0.4), {"all": "red", "pz": "yellow", "nz": "yellow"}, xfm((barc[0], bh + bt / 2 + 0.85, barc[2]), C.yaw(0)), skip=("ny",))
    for q in (-1, 0, 1):
        box(m, (0.9, 0.9, 0.46), "white", xfm(C.ptf(0, 0, bh + bt / 2 + 0.85) if False else (barc[0] + C.R[0, 0] * q * 2.4, bh + bt / 2 + 0.85, barc[2] + C.R[0, 1] * q * 2.4), C.yaw(0)))
    for sd in (-1, 1):   # ポール先の旗
        box_at(m, C, 0 + fz, sd * po, 8.7, (0.12, 1.2, 0.12), "grey")
        box_at(m, C, 0 + fz + 0.9, sd * po, 8.8, (0.1, 0.9, 1.6), "yellow" if sd > 0 else "cyan")

    # ---- 観客席（左）。断面を掃引
    ga, gb = C.zone["grandstand"]
    sa, sb = ga + 3, gb - 2
    idx = C.rng(sa, sb, 2)
    prof = [(-23, 0), (-23, 5), (-20.8, 5), (-20.8, 4), (-18.6, 4), (-18.6, 3), (-16.4, 3), (-16.4, 2), (-14.2, 2), (-14.2, 1), (-12, 1), (-12, 0)]
    # 時計回りで「外向き」: 上へ→右へ→下へ…（左外側にある階段）。底(最後の辺)は作らない
    def stand_cols(k, j):
        if j == len(prof) - 1:
            return None
        (l0, y0), (l1, y1) = prof[j], prof[j + 1]
        if abs(y1 - y0) < 1e-9:
            return "seat1" if (k // 2) % 2 == 0 else "seat2"
        return "standwall"
    sweep(m, C, idx, prof, stand_cols)
    # 観客
    crowd = ["red", "blue", "yellow", "green", "orange", "pink", "purple", "white", "cyan", "yellowd"]
    for tier in range(5):
        lat = -(13.1 + 2.2 * tier)
        y = tier + 1.0
        for s in range(sa + 1, (sb if sb > sa else sb + N)):
            if rnd.random() < 0.12:
                continue
            ss_ = s + rnd.uniform(-0.15, 0.15)
            pos = C.ptf(ss_, lat, y)
            R0 = C.R[int(s) % N]
            yaw = math.atan2(R0[0], R0[1])            # 道（右向き）を向く
            col = rnd.choice(crowd)
            hh = rnd.uniform(0.55, 0.75)
            box(m, (0.55, hh, 0.38), col, xfm((pos[0], y + hh / 2, pos[2]), yaw), skip=("ny",))
            box(m, (0.34, 0.34, 0.34), {"all": "skin", "pz": "skin"}, xfm((pos[0], y + hh + 0.17, pos[2]), yaw), skip=("ny",))
            if rnd.random() < 0.3:     # 手を挙げる
                for sd in (-1, 1):
                    if rnd.random() < 0.6:
                        e = C.R[int(s) % N]
                        fwd = C.fwd(int(s))
                        box(m, (0.12, 0.5, 0.12), "skin", xfm((pos[0] + fwd[0] * sd * 0.3, y + hh + 0.1, pos[2] + fwd[1] * sd * 0.3), yaw), skip=("ny",))
    # 屋根と柱
    rprof = [(-24.5, 7.0), (-24.5, 7.6), (-10.8, 7.6), (-10.8, 6.5), (-11.2, 6.5), (-11.2, 7.0)]
    sweep(m, C, idx, rprof, lambda k, j: ("red" if (k // 2) % 2 == 0 else "white"), closed=True, caps=True)
    for s in range(sa, sb if sb > sa else sb + N, 8):
        for lat in (-23.4, -11.6):
            box_at(m, C, s, lat, 3.5, (0.4, 7.0, 0.4), "grey", skip=("ny",))
    # ピットウォール
    sweep(m, C, C.rng(sa - 2, sb + 2, 2), [(-9.4, 0.0), (-9.4, 1.0), (-8.6, 1.0), (-8.6, 0.0)],
          lambda k, j: None if j == 3 else ("blue" if (k // 2) % 2 == 0 else "white"), closed=True)
    # 右側の白い柵
    fs = C.rng(sa, sb, 1)
    for s in fs[::3]:
        box_at(m, C, s, 10.2, 0.55, (0.2, 1.1, 0.2), "white", skip=("ny",))
    fi = C.rng(sa, sb, 2)
    for yy in (0.4, 0.85):
        sweep(m, C, fi, [(10.1, yy - 0.06), (10.1, yy + 0.06), (10.3, yy + 0.06), (10.3, yy - 0.06)],
              lambda k, j: None if j == 3 else "wood", closed=True)

    # ---- 大回りの外側: 旗と看板
    a1, b1 = C.zone["turn1"]
    o = C.outside("turn1")
    for n, s in enumerate(range(a1 + 2, b1, 8)):
        lat = o * 9.5
        box_at(m, C, s, lat, 3.0, (0.18, 6.0, 0.18), "silver", skip=("ny",))
        box_at(m, C, s, lat - o * 0.9, 5.6, (1.8, 1.0, 0.05), ["red", "yellow", "blue", "white"][n % 4])
    for n, s in enumerate(range(a1 + 6, b1, 24)):
        lat = o * 13.0
        for q in (-3.0, 3.0):
            box_at(m, C, s + q * 0.0, lat + o * q * 0.0, 1.7, (0.3, 3.4, 0.3), "darkgrey", skip=("ny",)) if False else None
        p = C.ptf(s, lat, 3.4)
        R0 = C.R[s % N]
        # 看板は道に直角（道の方を向く）ではなく、道に平行な板（S 方向に長い）
        xf = xfm(p, C.yaw(s))
        box(m, (0.3, 2.4, 7.0), {"all": "white", "nx" if o > 0 else "px": ["red", "blue", "orange"][n % 3]}, xf)
        for q in (-2.6, 2.6):
            box(m, (0.3, 3.4, 0.3), "darkgrey", xfm((p[0] + C.fwd(s)[0] * q, 1.7, p[2] + C.fwd(s)[1] * q), C.yaw(s)), skip=("ny",))
        # 看板のしま
        box(m, (0.34, 0.6, 7.04), {"all": "yellow"}, xf.copy() @ Matrix.Translation((0, 0.5, 0)))

    # ---- トンネル
    ta, tb = C.zone["tunnel"]
    ti = C.rng(ta, tb, 2)
    if ti[-1] != tb:
        ti.append(tb)
    sweep(m, C, ti, [(-9.2, 0.0), (-9.2, 8.4), (-7.6, 8.4), (-7.6, 0.0)], lambda k, j: None if j == 3 else "tunnelwall", closed=True)
    sweep(m, C, ti, [(7.6, 0.0), (7.6, 8.4), (9.2, 8.4), (9.2, 0.0)], lambda k, j: None if j == 3 else "tunnelwall", closed=True)
    sweep(m, C, ti, [(-9.2, 7.2), (-9.2, 8.4), (9.2, 8.4), (9.2, 7.2)], lambda k, j: "tunnelroof" if j != 1 or True else None, closed=True)
    # 丘
    mp = [(-16, -0.05), (-14.5, 3), (-12, 6.5), (-9, 9.0), (-4, 10.2), (4, 10.2), (9, 9.0), (12, 6.5), (14.5, 3), (16, -0.05)]
    sweep(m, C, ti, mp, lambda k, j: ["grassd", "grass1", "grass3", "grass3", "grass3", "grass3", "grass1", "grassd", "grassd"][j], closed=False)
    # 入口・出口の枠
    for s in (ta - 0.2, tb + 0.2):
        for sd in (-1, 1):
            box_at(m, C, s, sd * 9.4, 4.7, (3.0, 9.4, 1.6), {"all": "orange", "pz": "yellow", "nz": "yellow"}, skip=("ny",))
        box_at(m, C, s, 0, 8.4, (21.8, 2.2, 1.6), {"all": "yellow", "pz": "orange", "nz": "orange"}, skip=("ny",))
    for s in range(ta + 4, tb, 6):       # ランプ
        box_at(m, C, s, 0, 6.95, (1.8, 0.22, 0.6), "lamp")

    # ---- 橋: 床板・橋脚・欄干
    la, lb = C.zone["lakeBridge"]
    bi = C.rng(la - 2, lb + 2, 2)
    sweep(m, C, bi, [(-8.4, 0.0), (-8.4, -0.6), (8.4, -0.6), (8.4, 0.0)], lambda k, j: None if j == 3 else ("lightgrey" if j != 0 else "grey"), closed=True)
    for s in range(la + 4, lb, 10):
        for sd in (-1, 1):
            box_at(m, C, s, sd * 5.0, -2.6, (1.6, 4.4, 1.6), "grey", skip=("ny",))
    for sd in (-1, 1):
        l_in, l_out = (7.55, 7.85) if sd > 0 else (-7.85, -7.55)
        sweep(m, C, bi, [(l_in, 0.95), (l_in, 1.15), (l_out, 1.15), (l_out, 0.95)], lambda k, j: None if j == 3 else ("red" if (k // 4) % 2 == 0 else "white"), closed=True)
        for s in range(la - 2, lb + 3, 3):
            box_at(m, C, s, (l_in + l_out) / 2, 0.55, (0.3, 1.1, 0.3), "white", skip=("ny",))

    # ---- タイヤの壁（ヘアピン・最終コーナーの外側、道の端から約 9m 外）
    tire = M("tire")
    for zn in ("hairpin", "finalCorner"):
        a, b = C.zone[zn]
        o = C.outside(zn)
        for row, (off, shift) in enumerate(((C.HW + 8.7, 0.0), (C.HW + 9.7, 0.5))):
            for s in C.rng(a - 4, b + 4):
                sf = s + shift
                p = C.ptf(sf, o * off, -0.04)
                yaw = C.yaw(s)
                cols = [("red", "black", "red"), ("white", "black", "white"), ("blue", "black", "blue"), ("yellow", "black", "yellow")][(s // 2) % 4]
                for k, cc in enumerate(cols):
                    cylinder(m, 0.5, 0.34, 8, cc, xfm((p[0], p[1] + k * 0.34, p[2]), yaw), cap0=None, cap1=cc if k == 2 else None)

    # ---- 風車（遠景）
    wx, wz = C.cx + 175, C.cz - 70
    wh = float(C.ground_h(np.array([wx]), np.array([wz]))[0][0])
    AVOID.append((wx, wz, 22.0))
    frustum(m, 4.2, 2.6, 13.0, 8, ["white", "pink", "white", "pink", "white", "pink", "white", "pink"] if False else (lambda k, a: "white" if a % 2 == 0 else "lightgrey"),
            xfm((wx, wh, wz)), cap0="grey")
    cone(m, 3.4, 3.6, 8, lambda k, a: "red" if a % 2 == 0 else "redd", xfm((wx, wh + 13.0, wz)), y0=0.0, cap0="redd")
    hub = (wx, wh + 14.2, wz + 3.2)
    for ang in (0.3, 0.3 + math.pi / 2):
        xf = Matrix.Translation(hub) @ Matrix.Rotation(ang, 4, 'Z')
        box(m, (1.0, 22.0, 0.35), "wood", xf)
        for sgn in (-1, 1):
            xf2 = Matrix.Translation(hub) @ Matrix.Rotation(ang, 4, 'Z') @ Matrix.Translation((0.9, sgn * 6.5, 0.0))
            box(m, (1.8, 8.0, 0.1), "white", xf2)
    box(m, (1.5, 1.5, 1.2), "darkgrey", Matrix.Translation(hub))
    # ---- 熱気球（遠景）
    bx, bz = C.cx - 140, C.cz + 30
    by = float(C.ground_h(np.array([bx]), np.array([bz]))[0][0]) + 58
    AVOID.append((bx, bz, 20.0))
    stripe = ["red", "yellow", "blue", "white", "orange", "green"]
    blob(m, 9.0, lambda c: stripe[int(((math.atan2(c[2], c[0]) + math.pi) / (2 * math.pi)) * 12) % len(stripe)], Matrix.Translation((bx, by, bz)), 1, (1, 1.2, 1))
    box(m, (2.4, 1.8, 2.4), "wood", Matrix.Translation((bx, by - 15.5, bz)))
    for sx_, sz_ in ((-1, -1), (-1, 1), (1, -1), (1, 1)):
        box(m, (0.12, 5.4, 0.12), "brownd", Matrix.Translation((bx + sx_ * 1.0, by - 11.8, bz + sz_ * 1.0)))
    return m


# ---------------------------------------------------------------- カート
KART_COL = {"red": ("red", "redd"), "blue": ("blue", "blued"), "green": ("green", "greend"), "yellow": ("yellow", "yellowd")}


def build_kart(color):
    m = M("kart_" + color)
    a, b = KART_COL[color]
    m.remap = {"KART": a, "KARTD": b}
    # 車体
    hull(m, [(-1.0, 0.52, 0.2, 0.42), (0.45, 0.55, 0.22, 0.44)], "KART", caps=True)
    hull(m, [(0.45, 0.55, 0.22, 0.44), (1.22, 0.36, 0.13, 0.36)], ["KART", "KART", "KART", "KART"])
    for sd in (-1, 1):
        box(m, (0.2, 0.22, 1.1), "KARTD", Matrix.Translation((sd * 0.62, 0.34, -0.25)))
    box(m, (1.35, 0.2, 0.22), {"all": "white", "pz": "white"}, Matrix.Translation((0, 0.3, 1.25)))
    box(m, (1.2, 0.2, 0.2), "darkgrey", Matrix.Translation((0, 0.34, -1.15)))
    # ホイール
    for (x, y, z, r, w) in ((0.68, 0.33, 0.85, 0.33, 0.32), (-0.68, 0.33, 0.85, 0.33, 0.32),
                            (0.66, 0.40, -0.82, 0.40, 0.38), (-0.66, 0.40, -0.82, 0.40, 0.38)):
        xf = Matrix.Translation((x, y, z)) @ Matrix.Rotation(math.pi / 2, 4, 'Z')
        cylinder(m, r, w, 10, "black", xf, y0=-w / 2, cap0="black", cap1="black")
        cylinder(m, r * 0.5, w + 0.05, 8, "silver", xf, y0=-w / 2 - 0.025, cap0="silver", cap1="silver")
    # エンジンと排気
    box(m, (0.7, 0.4, 0.45), {"all": "grey", "py": "silver"}, Matrix.Translation((0, 0.72, -0.9)), skip=("ny",))
    for sd in (-1, 1):
        xf = Matrix.Translation((sd * 0.2, 0.62, -1.05)) @ Matrix.Rotation(-math.pi / 2, 4, 'X')
        cylinder(m, 0.08, 0.35, 6, "silver", xf, y0=-0.05, cap0=None, cap1="black")
    # リアウィング
    box(m, (1.3, 0.06, 0.34), "KARTD", Matrix.Translation((0, 1.0, -1.15)))
    for sd in (-1, 1):
        box(m, (0.06, 0.35, 0.2), "grey", Matrix.Translation((sd * 0.5, 0.82, -1.1)), skip=("ny",))
    # シート
    box(m, (0.62, 0.12, 0.6), "darkgrey", Matrix.Translation((0, 0.66, -0.2)))
    box(m, (0.62, 0.5, 0.12), "darkgrey", Matrix.Translation((0, 0.92, -0.5)))
    # ドライバー
    box(m, (0.5, 0.52, 0.36), "KARTD", Matrix.Translation((0, 0.96, -0.2)), skip=("ny",))
    blob(m, 0.25, "skin", Matrix.Translation((0, 1.26, -0.12)), 1)
    blob(m, 0.31, "KART", Matrix.Translation((0, 1.3, -0.17)), 1)
    box(m, (0.4, 0.12, 0.1), "visor", Matrix.Translation((0, 1.27, 0.1)))
    for sd in (-1, 1):
        xf = Matrix.Translation((sd * 0.3, 1.0, 0.12)) @ Matrix.Rotation(sd * 0.15, 4, 'Y') @ Matrix.Rotation(math.radians(-20), 4, 'X')
        box(m, (0.13, 0.13, 0.62), "KARTD", xf)
        box(m, (0.15, 0.15, 0.12), "white", Matrix.Translation((sd * 0.2, 0.96, 0.4)))
    # ハンドル
    box(m, (0.06, 0.06, 0.45), "grey", Matrix.Translation((0, 0.85, 0.28)) @ Matrix.Rotation(math.radians(-25), 4, 'X'))
    xf = Matrix.Translation((0, 0.98, 0.42)) @ Matrix.Rotation(math.radians(-53), 4, 'X')
    cylinder(m, 0.2, 0.05, 8, "black", xf, y0=-0.025, cap0="black", cap1="black")
    return m


def build_itembox():
    m = M("itembox", mats=("emit",))
    cols = ["red", "orange", "yellow", "green", "cyan", "purple"]
    h = 0.65
    box(m, (1.3, 1.3, 1.3), {"px": "red", "nx": "orange", "py": "yellow", "ny": "purple", "pz": "cyan", "nz": "green"})
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                box(m, (0.3, 0.3, 0.3), "gold", Matrix.Translation((sx * 0.57, sy * 0.57, sz * 0.57)))
    glyph = [".XXX.", "X...X", "....X", "...X.", "..X..", ".....", "..X.."]
    faces = [((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)), ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
             ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1))]
    t = 0.12
    for n, u, v in faces:
        for ry, row in enumerate(glyph):
            for cx_, ch in enumerate(row):
                if ch != "X":
                    continue
                uu = (cx_ - 2) * t
                vv = (3 - ry) * t
                def P(du, dv):
                    return tuple(n[k] * (h + 0.025) + u[k] * (uu + du) + v[k] * (vv + dv) for k in range(3))
                m.face([P(-t / 2, -t / 2), P(t / 2, -t / 2), P(t / 2, t / 2), P(-t / 2, t / 2)], "white", n)
    return m


# ---------------------------------------------------------------- まとめ
ALL = ["course_road", "course_ground", "course_props", "course_trees_0", "course_trees_1", "course_trees_2", "course_trees_3",
       "kart_red", "kart_blue", "kart_green", "kart_yellow", "itembox"]


def show_only(names):
    for o in bpy.data.objects:
        if o.name in ALL:
            o.hide_viewport = o.name not in names
            o.hide_render = o.name not in names
            o.hide_set(o.name not in names)


def build_all(parts=None):
    os.makedirs(ART, exist_ok=True)
    for ob in list(bpy.data.objects):
        if ob.name in ("Cube",):
            bpy.data.objects.remove(ob, do_unlink=True)
    build_palette()
    _MATS.clear()
    for k in list(bpy.data.materials):
        if k.name.startswith("pal_"):
            bpy.data.materials.remove(k)
    C = Course()
    AVOID.clear()
    stats = {}
    want = lambda n: parts is None or n in parts
    if want("props"):
        mp = build_props(C)
        mp.build(); stats["course_props"] = mp.tris()
    if want("road"):
        mr = build_road(C); mr.build(); stats["course_road"] = mr.tris()
    if want("ground"):
        mg = build_ground(C); mg.build(); stats["course_ground"] = mg.tris()
    if want("trees"):
        objs, n, nf = build_trees(C)
        for o in objs:
            o.build(); stats[o.name] = o.tris()
        stats["trees"] = n
    if want("karts"):
        for c in KART_COL:
            k = build_kart(c); k.build(); stats["kart_" + c] = k.tris()
    if want("itembox"):
        ib = build_itembox(); ib.build(); stats["itembox"] = ib.tris()
    return stats
