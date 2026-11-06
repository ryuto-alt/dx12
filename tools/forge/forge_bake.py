"""forge_bake.py - 小物を「物ごとの専用 UV + 縁/くぼみ/上面のマスクで素材を重ねた」2K〜4K テクスチャへ焼く(Blender 5.2, Cycles)。

  blender -b -noaudio --python forge_bake.py -- --in mesh.fbx --recipe recipe.json --out out.glb
         [--res auto|1024|2048|4096] [--preview preview.png] [--front +x]

詳細は README.md。レシピの書式もそちら。
"""
import bpy
import bmesh
import json
import math
import os
import re
import sys
import time
import traceback
import numpy as np
from mathutils import Vector

T0 = time.time()
PHASES = {}


def log(*a):
    print("[forge %6.1fs]" % (time.time() - T0), *a, flush=True)


def phase(name, t):
    PHASES[name] = round(time.time() - t, 1)
    log("phase", name, PHASES[name], "s")


# ---------------------------------------------------------------- 引数
def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(res="auto", preview=None, front="+x", samples_mask=96, density=1024.0)
    i = 0
    while i < len(argv):
        k = argv[i]
        if k in ("--in", "--recipe", "--out", "--res", "--preview", "--front", "--samples-mask", "--density"):
            key = k[2:].replace("-", "_")
            o[key] = argv[i + 1]
            i += 2
        else:
            i += 1
    o["samples_mask"] = int(o["samples_mask"])
    o["density"] = float(o["density"])
    for k in ("in", "recipe", "out"):
        if k not in o:
            raise SystemExit("missing --" + k)
    return o


CC0 = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "UnoEngine", "cc0")


# ---------------------------------------------------------------- ノード小道具
def col4(c):
    c = tuple(c)
    return c + (1.0,) if len(c) == 3 else c


class NB:
    def __init__(self, nt):
        self.nt = nt

    def node(self, typ, **props):
        n = self.nt.nodes.new(typ)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def put(self, n, idx, v):
        s = n.inputs[idx]
        if v is None:
            return
        if isinstance(v, (int, float)):
            if s.type == "RGBA":
                s.default_value = (v, v, v, 1.0)
            elif s.type == "VECTOR":
                s.default_value = (v, v, v)
            else:
                s.default_value = float(v)
        elif isinstance(v, (tuple, list)):
            s.default_value = col4(v) if s.type == "RGBA" else tuple(v)
        else:
            self.nt.links.new(v, s)

    def math(self, op, a, b=None, c=None, cl=False):
        n = self.node("ShaderNodeMath", operation=op, use_clamp=cl)
        self.put(n, 0, a)
        self.put(n, 1, b)
        self.put(n, 2, c)
        return n.outputs[0]

    def mixf(self, f, a, b):
        n = self.node("ShaderNodeMix", data_type="FLOAT")
        self.put(n, 0, f)
        self.put(n, 2, a)
        self.put(n, 3, b)
        return n.outputs[0]

    def mixc(self, f, a, b, blend="MIX"):
        n = self.node("ShaderNodeMix", data_type="RGBA", blend_type=blend)
        n.clamp_factor = True
        self.put(n, 0, f)
        self.put(n, 6, a)
        self.put(n, 7, b)
        return n.outputs[2]

    def sr(self, x, lo, hi):  # smoothstep
        n = self.node("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP", clamp=True)
        self.put(n, 0, x)
        self.put(n, 1, lo)
        self.put(n, 2, hi)
        return n.outputs[0]

    def noise(self, vec, scale, detail=5.0, rough=0.55):
        n = self.node("ShaderNodeTexNoise", noise_dimensions="3D")
        self.nt.links.new(vec, n.inputs["Vector"])
        n.inputs["Scale"].default_value = scale
        n.inputs["Detail"].default_value = detail
        n.inputs["Roughness"].default_value = rough
        return n.outputs["Fac"]

    def sep(self, col):
        n = self.node("ShaderNodeSeparateColor")
        self.nt.links.new(col, n.inputs[0])
        return n.outputs[0], n.outputs[1], n.outputs[2]

    def comb(self, r, g, b):
        n = self.node("ShaderNodeCombineColor")
        self.put(n, 0, r)
        self.put(n, 1, g)
        self.put(n, 2, b)
        return n.outputs[0]

    def vmul(self, v, t):
        n = self.node("ShaderNodeVectorMath", operation="MULTIPLY")
        self.nt.links.new(v, n.inputs[0])
        n.inputs[1].default_value = t
        return n.outputs[0]

    def mapping(self, vec, scale=(1, 1, 1), rot=(0, 0, 0), loc=(0, 0, 0)):
        n = self.node("ShaderNodeMapping", vector_type="POINT")
        self.nt.links.new(vec, n.inputs["Vector"])
        n.inputs["Location"].default_value = loc
        n.inputs["Rotation"].default_value = rot
        n.inputs["Scale"].default_value = scale
        return n.outputs[0]


def load_img(path, data=False):
    im = bpy.data.images.load(path, check_existing=True)
    im.colorspace_settings.name = "Non-Color" if data else "sRGB"
    return im


# ---------------------------------------------------------------- スキャン素材
def scan_files(tid, packed_dir):
    """素材 id → 画像パス。2K キャッシュ優先、無ければ packed(1K)。"""
    d = os.path.join(CC0, tid)

    def find(key):
        for ext in (".png", ".jpg"):
            p = os.path.join(d, "%s_%s%s" % (tid, key, ext))
            if os.path.exists(p):
                return p
        return None

    c = find("Color")
    if c:
        return dict(color=c, normal=find("NormalGL"), rough=find("Roughness"), metal=find("Metalness"), orm=None, src="cc0-2K")
    if packed_dir:
        p = lambda s: os.path.join(packed_dir, "%s_%s.png" % (tid, s))
        if os.path.exists(p("C")):
            return dict(color=p("C"), normal=p("N"), rough=None, metal=None, orm=p("ORM"), src="packed-1K")
    return None


# ---------------------------------------------------------------- 層の材質
def build_layer_material(name, P, ctx):
    """スロット 1 つぶんの層材質。出力ソケット(color, rough, metal, normal)と bake 用ノードを返す。"""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    B = NB(nt)
    tc = B.node("ShaderNodeNewGeometry")
    pos = tc.outputs["Position"]
    geo_n = tc.outputs["Normal"]
    uv0 = B.node("ShaderNodeUVMap", uv_map="UV0").outputs[0]
    uvf = B.node("ShaderNodeUVMap", uv_map="ForgeUV").outputs[0]

    tint = P.get("tint", (0.5, 0.5, 0.5))
    flat = P.get("flat", 0.0)
    rflat = P.get("rflat", 0.55)
    rmul = P.get("rough", 1.0)
    metal_ov = P.get("metal", None)
    tile = P.get("tile", 1.0)
    scale = 1.0 / max(tile, 1e-3)
    wear = P.get("wear", 0.0)
    dirt = P.get("dirt", 0.6)
    nstr = P.get("nstr", 1.0)
    heat_amt = P.get("heat", 1.0)
    rust_amt = P.get("rust", 0.0)
    wearcol = P.get("wearcol", (0.4, 0.4, 0.4))
    wearmetal = P.get("wearmetal", 1.0)
    iron = bool(P.get("iron", False))

    # --- スキャン素材(2 回サンプルを混ぜてタイルを隠す。2 回目は回して別スケール)
    sc = scan_files(P.get("tex", ""), P.get("packed_dir"))
    mm_noise = B.noise(pos, 1.3, 3.0)
    mm = B.sr(mm_noise, 0.35, 0.65)
    mapA = B.mapping(uv0, (scale, scale, 1))
    mapB = B.mapping(uv0, (scale * 0.43, scale * 0.43, 1), (0, 0, 0.55), (0.31, 0.57, 0))
    mapN = B.mapping(uv0, (scale * 0.43, scale * 0.43, 1), (0, 0, 0), (0.31, 0.57, 0))

    def tex(path, vec, data):
        n = B.node("ShaderNodeTexImage", interpolation="Linear", extension="REPEAT")
        n.image = load_img(path, data)
        nt.links.new(vec, n.inputs["Vector"])
        return n.outputs["Color"]

    if sc:
        ca, cb = tex(sc["color"], mapA, False), tex(sc["color"], mapB, False)
        base = B.mixc(mm, ca, cb)
        if sc["orm"]:
            oa, ob = tex(sc["orm"], mapA, True), tex(sc["orm"], mapB, True)
            ra, rb = B.sep(oa)[1], B.sep(ob)[1]
            ma, mb = B.sep(oa)[2], B.sep(ob)[2]
        else:
            ra = B.sep(tex(sc["rough"], mapA, True))[0] if sc["rough"] else 0.6
            rb = B.sep(tex(sc["rough"], mapB, True))[0] if sc["rough"] else 0.6
            ma = B.sep(tex(sc["metal"], mapA, True))[0] if sc["metal"] else 0.0
            mb = B.sep(tex(sc["metal"], mapB, True))[0] if sc["metal"] else 0.0
        r0 = B.mixf(mm, ra, rb) if not isinstance(ra, float) else 0.6
        m0 = B.mixf(mm, ma, mb) if not isinstance(ma, float) else 0.0
        if sc["normal"]:
            na, nb = tex(sc["normal"], mapA, True), tex(sc["normal"], mapN, True)
            nmix = B.mixc(mm, na, nb)
        else:
            nmix = None
    else:
        base, r0, m0, nmix = (0.5, 0.5, 0.5), 0.6, 0.0, None

    base = B.mixc(flat, base, (0.5, 0.5, 0.5)) if flat > 0 else base
    color = B.mixc(1.0, base, tint, "MULTIPLY")
    rough = B.math("MULTIPLY", r0, rmul, cl=True) if not isinstance(r0, float) else r0 * rmul
    if flat > 0 or True:
        rough = B.mixf(flat, rough, rflat) if flat > 0 else rough
    metal = float(metal_ov) if metal_ov is not None else m0

    base_col = color                                   # 層を重ねる前(= UE の式の色)

    # --- マスク
    mk = B.node("ShaderNodeTexImage", interpolation="Closest", extension="EXTEND")
    mk.image = ctx["mask_img"]
    nt.links.new(uvf, mk.inputs["Vector"])
    E, C, Cbig = B.sep(mk.outputs["Color"])
    tk = B.node("ShaderNodeTexImage", interpolation="Closest", extension="EXTEND")
    tk.image = ctx["touch_img"]
    nt.links.new(uvf, tk.inputs["Vector"])
    T = B.sep(tk.outputs["Color"])[0]                  # 手の触れる所(取っ手・ボタンの近く)
    nrm_sep = B.node("ShaderNodeSeparateXYZ")
    nt.links.new(geo_n, nrm_sep.inputs[0])
    U = nrm_sep.outputs[2]
    H = B.math("SUBTRACT", _sepz(B, pos), ctx["zmin"])
    Hn = B.math("DIVIDE", H, max(ctx["height"], 0.05), cl=True)
    attr = B.node("ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="Col")
    vcR, vcG, vcB = B.sep(attr.outputs["Color"])
    vis = B.node("ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="Vis").outputs["Fac"]
    N1 = B.noise(pos, 9.0, 6.0, 0.6)
    N2 = B.noise(pos, 55.0, 4.0, 0.55)
    Ns = B.noise(B.vmul(pos, (1, 1, 0.07)), 9.0, 5.0, 0.55)
    vert = B.math("SUBTRACT", 1.0, B.sr(B.math("ABSOLUTE", U), 0.35, 0.8))   # 縦の面ほど 1
    chip = 0.0
    hv = 0.0
    rf = 0.0

    # 粗さのむら(指でこすった所・拭いた所): 低周波だが振れ幅は小さく
    rough = B.math("ADD", rough, B.math("MULTIPLY", B.math("SUBTRACT", N1, 0.5), 0.3), cl=True)

    # --- 縁のハイライト(全素材: 角が光を拾う)
    hl = B.math("MULTIPLY", B.sr(E, 0.3, 0.9), B.math("ADD", 0.45, B.math("MULTIPLY", N2, 0.55)))
    hl_amt = 0.22 if wear > 0.05 else 0.5
    hlm = B.math("MULTIPLY", hl, hl_amt)
    color = B.mixc(hlm, color, (1.9, 1.9, 1.9), "MULTIPLY")
    rough = B.mixf(B.math("MULTIPLY", hlm, 0.5), rough, B.math("MULTIPLY", rough, 0.7))

    # --- 塗装剥げ: 縁 x 頂点色R x 崩し + 手の触れる所の擦れ > 閾値。剥げの縁に暗い細線
    if wear > 0.02:
        m = B.math("MULTIPLY", E, B.math("ADD", 0.5, B.math("MULTIPLY", vcR, 1.0)))
        g = B.math("ADD", 0.45, B.math("MULTIPLY", B.math("ADD", B.math("MULTIPLY", N2, 0.6), B.math("MULTIPLY", N1, 0.4)), 1.1))
        v = B.math("ADD", B.math("MULTIPLY", m, g), B.math("MULTIPLY", B.math("MULTIPLY", T, B.math("ADD", 0.3, N2)), 0.55))
        a = max(0.12, 0.50 - 0.28 * wear)
        chip = B.sr(v, a, a + 0.05)
        outer = B.sr(v, a - 0.07, a - 0.005)
        ring = B.math("MAXIMUM", B.math("SUBTRACT", outer, chip), 0.0)
        color = B.mixc(B.math("MULTIPLY", ring, 0.6), color, (0.35, 0.31, 0.28), "MULTIPLY")
        wc = B.mixc(B.math("MULTIPLY", N2, 0.6), wearcol, (wearcol[0] * 0.6, wearcol[1] * 0.6, wearcol[2] * 0.6))
        color = B.mixc(chip, color, wc)
        rough = B.mixf(chip, rough, 0.40)
        metal = B.mixf(chip, metal, float(wearmetal))

    # --- 垢: くぼみ / 下ほど / 大域 AO / 縦の垂れ筋 / 手垢(触れる所は暗い油膜で艶が出る)
    low = B.math("SUBTRACT", 1.0, B.math("MULTIPLY", Hn, 3.2, cl=True))   # 下から物の高さの約 3 割まで
    low = B.math("MULTIPLY", low, low)
    gm = B.math("MULTIPLY", B.math("MULTIPLY", C, dirt), B.math("ADD", 0.5, B.math("MULTIPLY", N1, 0.9)))
    gl = B.math("MULTIPLY", B.math("MULTIPLY", low, dirt * 0.9), B.math("ADD", 0.3, B.math("MULTIPLY", Ns, 1.0)))
    gb = B.math("MULTIPLY", B.math("MULTIPLY", Cbig, dirt), 0.45)
    drip = B.math("MULTIPLY", B.math("MULTIPLY", B.sr(Ns, 0.57, 0.74), vert), B.math("MULTIPLY", dirt, 0.55))
    hand = B.math("MULTIPLY", B.math("MULTIPLY", T, B.math("ADD", 0.35, N2)), 0.45)
    grime = B.math("ADD", B.math("ADD", B.math("MULTIPLY", gm, 1.4), B.math("MULTIPLY", gl, 0.85)), B.math("ADD", gb, drip), cl=True)
    grime = B.math("ADD", grime, hand, cl=True)
    color = B.mixc(grime, color, (0.36, 0.30, 0.23), "MULTIPLY")
    rough = B.mixf(B.math("MULTIPLY", grime, 0.7), rough, 0.9)
    rough = B.mixf(B.math("MULTIPLY", hand, 0.9), rough, B.math("MULTIPLY", rough, 0.55))   # 手垢は艶
    metal = B.math("MULTIPLY", metal, B.math("SUBTRACT", 1.0, B.math("MULTIPLY", grime, 0.4))) if not isinstance(metal, float) or metal > 0 else metal

    # --- 上面の埃
    dm = B.math("MULTIPLY", B.math("MULTIPLY", B.sr(U, 0.5, 0.95), B.math("SUBTRACT", 1.0, C)),
                B.math("MULTIPLY", B.math("ADD", 0.35, B.math("MULTIPLY", N1, 0.9)), min(1.0, dirt * 0.9)))
    dm = B.math("MULTIPLY", dm, 0.8, cl=True)
    color = B.mixc(dm, color, (0.21, 0.2, 0.18))
    rough = B.mixf(dm, rough, 0.95)
    metal = B.math("MULTIPLY", metal, B.math("SUBTRACT", 1.0, dm)) if not isinstance(metal, float) or metal > 0 else metal

    # --- 錆の垂れ(鉄系だけ。縦に伸ばしたノイズ x 頂点色B)
    if iron and rust_amt > 0.01:
        rm = B.math("MULTIPLY", B.math("ADD", B.math("ADD", B.math("MULTIPLY", vcB, 1.4), B.math("MULTIPLY", C, 0.3)),
                                       B.math("MULTIPLY", E, 0.25), cl=True), rust_amt)
        rv = B.math("ADD", B.math("MULTIPLY", Ns, 0.7), B.math("MULTIPLY", N2, 0.3))
        rf = B.sr(B.math("MULTIPLY", rm, B.math("ADD", 0.35, B.math("MULTIPLY", rv, 1.1))), 0.16, 0.34)
        rc = B.mixc(N1, (0.13, 0.05, 0.018), (0.055, 0.02, 0.008))
        rf = B.math("MULTIPLY", rf, B.math("ADD", 0.55, B.math("MULTIPLY", N2, 0.45)))
        color = B.mixc(rf, color, rc)
        rough = B.mixf(rf, rough, 0.88)
        metal = B.mixf(rf, metal, 0.25)

    # --- 熱焼け(頂点色G)
    if heat_amt > 0.01:
        hv = B.math("MULTIPLY", B.math("MULTIPLY", vcG, heat_amt), B.math("ADD", 0.7, B.math("MULTIPLY", N1, 0.6)), cl=True)
        hh = B.sr(vcG, 0.45, 0.9)
        hc = B.mixc(hh, (0.19, 0.085, 0.032), (0.012, 0.016, 0.045))
        color = B.mixc(B.math("MULTIPLY", hv, 0.92), color, hc)
        rough = B.mixf(B.math("MULTIPLY", hv, 0.7), rough, 0.62)

    # --- 法線: スキャン + Bevel の丸め
    nm = B.node("ShaderNodeNormalMap", space="TANGENT", uv_map="UV0")
    nm.inputs["Strength"].default_value = float(nstr)
    if nmix is not None:
        nt.links.new(nmix, nm.inputs["Color"])
    bev = B.node("ShaderNodeBevel", samples=8)
    bev.inputs["Radius"].default_value = ctx["bevel_r"]
    nt.links.new(nm.outputs["Normal"], bev.inputs["Normal"])

    # --- ORM: AO は控えめに 0.4〜1.0。見えない面(vis=0)は 1
    occl = B.math("MAXIMUM", B.math("MULTIPLY", C, 0.9), B.math("MULTIPLY", Cbig, 0.6), cl=True)
    ao = B.mixf(vis, 1.0, B.math("SUBTRACT", 1.0, B.math("MULTIPLY", occl, 0.6)))
    orm = B.comb(ao, rough, metal)

    # --- 検査用(--stats): スロット番号 / 見える面 / 上向き、層の量
    sid = (ctx["slot_index"].get(name, 0) + 1) * 0.01
    ids = B.comb(sid, vis, B.math("ADD", B.math("MULTIPLY", U, 0.5), 0.5))
    dbg1 = B.comb(grime, dm, rf)
    dbg2 = B.comb(chip, hv, T)

    # --- 出力(パスごとにつなぎ替える)
    emis = B.node("ShaderNodeEmission")
    prin = B.node("ShaderNodeBsdfPrincipled")
    nt.links.new(bev.outputs["Normal"], prin.inputs["Normal"])
    out = B.node("ShaderNodeOutputMaterial")
    imgn = B.node("ShaderNodeTexImage")
    imgn.select = True
    nt.nodes.active = imgn
    return dict(mat=mat, nt=nt, color=color, orm=orm, emis=emis, prin=prin, out=out, imgn=imgn,
                base=base_col, ids=ids, dbg1=dbg1, dbg2=dbg2, pos=pos, ao=ao)


def _sepz(B, vec):
    n = B.node("ShaderNodeSeparateXYZ")
    B.nt.links.new(vec, n.inputs[0])
    return n.outputs[2]


def build_mask_material(ctx):
    mat = bpy.data.materials.new("forge_mask")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    B = NB(nt)
    g = B.node("ShaderNodeNewGeometry")
    bev = B.node("ShaderNodeBevel", samples=12)
    bev.inputs["Radius"].default_value = ctx["bevel_r"]
    dot = B.node("ShaderNodeVectorMath", operation="DOT_PRODUCT")
    nt.links.new(bev.outputs["Normal"], dot.inputs[0])
    nt.links.new(g.outputs["Normal"], dot.inputs[1])
    e = B.sr(B.math("SUBTRACT", 1.0, dot.outputs["Value"]), 0.002, 0.06)

    def ao(dist):
        n = B.node("ShaderNodeAmbientOcclusion", samples=8, inside=False, only_local=True)
        n.inputs["Distance"].default_value = dist
        return n.outputs["AO"]

    cx = B.sr(ao(0.02), 0.35, 0.75)                      # 縁の候補は開けた所だけ(凹角を外す)
    E = B.math("MULTIPLY", e, cx)
    cav = B.math("SUBTRACT", 1.0, ao(0.06))
    C = B.math("MULTIPLY", cav, 1.35, cl=True)
    Cb = B.math("MULTIPLY", B.math("SUBTRACT", 1.0, ao(ctx["ao_big"])), 1.3, cl=True)
    rgb = B.comb(E, C, Cb)
    emis = B.node("ShaderNodeEmission")
    nt.links.new(rgb, emis.inputs["Color"])
    out = B.node("ShaderNodeOutputMaterial")
    nt.links.new(emis.outputs[0], out.inputs["Surface"])
    imgn = B.node("ShaderNodeTexImage")
    imgn.select = True
    nt.nodes.active = imgn
    return dict(mat=mat, imgn=imgn)


def dummy_material(img):
    mat = bpy.data.materials.new("forge_dummy")
    mat.use_nodes = True
    nt = mat.node_tree
    n = nt.nodes.new("ShaderNodeTexImage")
    n.image = img
    n.select = True
    nt.nodes.active = n
    return mat


# ---------------------------------------------------------------- GPU
def setup_cycles():
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    dev = "CPU"
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        for typ in ("OPTIX", "CUDA"):
            try:
                prefs.compute_device_type = typ
                prefs.refresh_devices() if hasattr(prefs, "refresh_devices") else prefs.get_devices()
                gpus = [d for d in prefs.devices if d.type != "CPU"]
                if gpus:
                    for d in prefs.devices:
                        d.use = d.type != "CPU"
                    dev = typ
                    break
            except Exception:  # noqa: BLE001
                continue
    except Exception as e:  # noqa: BLE001
        log("cycles prefs failed", e)
    sc.cycles.device = "GPU" if dev != "CPU" else "CPU"
    sc.cycles.use_denoising = False
    sc.view_settings.view_transform = "Standard"
    sc.view_settings.look = "None"
    log("device:", dev)
    return dev


def do_bake(ob, bake_type, samples, margin=8, normal=False):
    sc = bpy.context.scene
    sc.cycles.samples = samples
    sc.cycles.use_adaptive_sampling = False
    kw = dict(type=bake_type, margin=margin, margin_type="EXTEND", use_clear=True, target="IMAGE_TEXTURES", uv_layer="ForgeUV")
    if normal:
        kw["normal_space"] = "TANGENT"
    bpy.ops.object.bake(**kw)


# ---------------------------------------------------------------- メイン
def main():
    A = parse_args()
    recipe = json.load(open(A["recipe"], encoding="utf-8"))
    out = os.path.abspath(A["out"])
    outdir = os.path.dirname(out)
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(out))[0]
    work = os.path.join(outdir, "_forge_" + stem)
    os.makedirs(work, exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    dev = setup_cycles()
    t = time.time()
    ext = os.path.splitext(A["in"])[1].lower()
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=A["in"])
    else:
        bpy.ops.import_scene.gltf(filepath=A["in"])
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    for o in bpy.data.objects:   # 空(ソケット)は親を外して位置を保つ
        if o is not ob and o.parent is not None:
            m = o.matrix_world.copy()
            o.parent = None
            o.matrix_world = m
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    me = ob.data
    # UV 名
    me.uv_layers[0].name = "UV0"
    for extra in list(me.uv_layers)[1:]:
        me.uv_layers.remove(extra)
    # 三角化(モディファイアで適用 = 法線等を保つ)
    md = ob.modifiers.new("tri", "TRIANGULATE")
    md.quad_method = "BEAUTY"
    md.min_vertices = 4
    bpy.ops.object.modifier_apply(modifier="tri")
    me = ob.data
    ntri = len(me.polygons)
    log("imported", ob.name, "tris", ntri, "slots", len(ob.material_slots))
    phase("import", t)

    # --- スロット分類
    slots = []
    for i, s in enumerate(ob.material_slots):
        nm = re.sub(r"\.\d{3}$", "", s.material.name) if s.material else "none"
        spec = recipe["slots"].get(nm)
        if spec is None:
            spec = {"kind": "bake"}
            log("slot not in recipe, default:", nm)
        slots.append((nm, spec))
    bake_idx = [i for i, (_, sp) in enumerate(slots) if sp.get("kind", "bake") == "bake"]
    keep_idx = [i for i, (_, sp) in enumerate(slots) if sp.get("kind") == "image"]

    npoly = len(me.polygons)
    mi = np.zeros(npoly, dtype=np.int32)
    me.polygons.foreach_get("material_index", mi)
    isb = np.isin(mi, bake_idx)
    area = np.zeros(npoly, dtype=np.float32)
    me.polygons.foreach_get("area", area)
    area_b = float(area[isb].sum())
    co = np.zeros(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    bmin, bmax = co.min(0), co.max(0)
    diag = float(np.linalg.norm(bmax - bmin))

    # 解像度
    if A["res"] == "auto":
        need = area_b * A["density"] ** 2 / 0.62
        r = 1024
        while r * r < need and r < 4096:
            r *= 2
        res = r
    else:
        res = int(A["res"])
    bevel_r = min(0.008, max(0.003, diag * 0.004))
    log("area_b %.2f m2 diag %.2f res %d bevel %.1f mm" % (area_b, diag, res, bevel_r * 1000))

    if not bake_idx:
        raise SystemExit("焼く面が無い")

    # --- 焼き用 UV
    t = time.time()
    fuv = me.uv_layers.new(name="ForgeUV", do_init=True)
    me.uv_layers.active = fuv
    fuv.active_render = True
    sel = isb.astype(np.bool_)
    me.polygons.foreach_set("select", sel)
    me.update()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.context.scene.tool_settings.use_uv_select_sync = True
    bpy.ops.mesh.select_mode(type="FACE")
    bm = bmesh.from_edit_mesh(me)
    for f in bm.faces:
        f.select = bool(sel[f.index])
    bmesh.update_edit_mesh(me)
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.0, correct_aspect=True, scale_to_bounds=False)
    margin = 8.0 / res
    try:
        bpy.ops.uv.pack_islands(udim_source="CLOSEST_UDIM", rotate=True, margin_method="ADD", margin=margin, shape_method="AABB")
    except Exception as e:  # noqa: BLE001
        log("pack_islands fallback", e)
        bpy.ops.uv.pack_islands(rotate=True, margin=margin)
    bpy.ops.object.mode_set(mode="OBJECT")
    me = ob.data
    # テクセル密度
    uv = np.zeros(len(me.loops) * 2, dtype=np.float32)
    me.uv_layers["ForgeUV"].data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2).reshape(-1, 3, 2)  # 三角形前提
    uva = 0.5 * np.abs((uv[:, 1, 0] - uv[:, 0, 0]) * (uv[:, 2, 1] - uv[:, 0, 1]) - (uv[:, 2, 0] - uv[:, 0, 0]) * (uv[:, 1, 1] - uv[:, 0, 1]))
    density = math.sqrt(float(uva[isb].sum()) * res * res / max(area_b, 1e-9))
    umin, umax = uv[isb].reshape(-1, 2).min(0), uv[isb].reshape(-1, 2).max(0)
    log("texel density %.0f px/m, uv bbox %s %s, coverage %.2f" % (density, umin, umax, float(uva[isb].sum())))
    phase("uv", t)

    # --- マスクを焼く
    ctx = dict(bevel_r=bevel_r, zmin=float(bmin[2]))
    mask_img = bpy.data.images.new("forge_mask", res, res, alpha=False, float_buffer=True)
    mask_img.colorspace_settings.name = "Non-Color"
    ctx["mask_img"] = mask_img
    dummy_img = bpy.data.images.new("forge_dummy", 8, 8)
    dmat = dummy_material(dummy_img)
    mm = build_mask_material(ctx)
    mm["imgn"].image = mask_img
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    for i in range(len(ob.material_slots)):
        ob.material_slots[i].material = mm["mat"] if i in bake_idx else dmat
    t = time.time()
    do_bake(ob, "EMIT", A["samples_mask"], margin=16)
    phase("bake_mask", t)

    try:
        mask_img.filepath_raw = os.path.join(work, stem + '_mask.png')
        mask_img.file_format = 'PNG'
        mask_img.save()
    except Exception as e:  # noqa: BLE001
        log('mask save', e)
    # --- 層材質
    t = time.time()
    lm = {}
    for i in bake_idx:
        nm, sp = slots[i]
        if nm not in lm:
            lm[nm] = build_layer_material("L_" + nm, sp, ctx)
        ob.material_slots[i].material = lm[nm]["mat"]
    phase("build_layers", t)

    def run_pass(kind, img, samples, normal=False):
        for L in lm.values():
            nt = L["nt"]
            for lk in list(L["out"].inputs["Surface"].links):
                nt.links.remove(lk)
            L["imgn"].image = img
            if kind == "color":
                nt.links.new(L["color"], L["emis"].inputs["Color"])
                nt.links.new(L["emis"].outputs[0], L["out"].inputs["Surface"])
            elif kind == "orm":
                nt.links.new(L["orm"], L["emis"].inputs["Color"])
                nt.links.new(L["emis"].outputs[0], L["out"].inputs["Surface"])
            else:
                nt.links.new(L["prin"].outputs[0], L["out"].inputs["Surface"])
        dummy_img.colorspace_settings.name = "sRGB"
        tt = time.time()
        do_bake(ob, "NORMAL" if normal else "EMIT", samples, margin=8, normal=normal)
        phase("bake_" + kind, tt)

    img_c = bpy.data.images.new("forge_color", res, res, alpha=False)
    img_c.colorspace_settings.name = "sRGB"
    img_o = bpy.data.images.new("forge_orm", res, res, alpha=False)
    img_o.colorspace_settings.name = "Non-Color"
    img_n = bpy.data.images.new("forge_normal", res, res, alpha=False)
    img_n.colorspace_settings.name = "Non-Color"
    run_pass("color", img_c, 8)
    run_pass("orm", img_o, 8)
    run_pass("normal", img_n, 48, normal=True)

    # --- 保存
    paths = {}
    for k, im in (("color", img_c), ("orm", img_o), ("normal", img_n)):
        p = os.path.join(work, "%s_%s.png" % (stem, k))
        im.filepath_raw = p
        im.file_format = "PNG"
        im.save()
        paths[k] = p
    log("saved textures", paths)

    # --- 最終材質(書き出し用)
    for o in bpy.data.objects:
        if o.type == "MESH" and o is not ob:
            pass
    final_mats = build_final_materials(ob, me, slots, bake_idx, keep_idx, paths, stem, work)
    # UV: UV0 を捨て ForgeUV だけ残す
    me.uv_layers.remove(me.uv_layers["UV0"])
    me.uv_layers[0].name = "UVMap"
    # 頂点カラーは捨てる(dx12 は色に掛けてしまう)
    for a in list(me.color_attributes):
        me.color_attributes.remove(a)
    me.update()

    # --- 書き出し
    t = time.time()
    export_glb(out, final_mats)
    phase("export", t)

    # --- プレビュー
    if A["preview"]:
        t = time.time()
        try:
            render_preview(ob, A["preview"], A["front"], work)
        except Exception:  # noqa: BLE001
            traceback.print_exc()
        phase("preview", t)

    info = dict(name=stem, seconds=round(time.time() - T0, 1), phases=PHASES, resolution=res, tris=ntri,
                texel_density_px_per_m=round(density, 1), surface_area_m2=round(area_b, 3), bevel_mm=round(bevel_r * 1000, 1),
                device=dev, baked_slots=[slots[i][0] for i in bake_idx], kept_slots=[slots[i][0] for i in keep_idx],
                glb_mb=round(os.path.getsize(out) / 1e6, 2), diag_m=round(diag, 3))
    with open(out + ".forge.json", "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=1)
    log("done", json.dumps(info, ensure_ascii=False))


# ---------------------------------------------------------------- 最終材質・書き出し
def final_material(name, paths, rough_const=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    B = NB(nt)
    prin = B.node("ShaderNodeBsdfPrincipled")
    out = B.node("ShaderNodeOutputMaterial")
    nt.links.new(prin.outputs[0], out.inputs["Surface"])
    c = B.node("ShaderNodeTexImage")
    c.image = load_img(paths["color"], False)
    nt.links.new(c.outputs["Color"], prin.inputs["Base Color"])
    o = B.node("ShaderNodeTexImage")
    o.image = load_img(paths["orm"], True)
    R, G, Bl = B.sep(o.outputs["Color"])
    nt.links.new(G, prin.inputs["Roughness"])
    nt.links.new(Bl, prin.inputs["Metallic"])
    grp = bpy.data.node_groups.get("glTF Material Output")
    if grp is None:
        grp = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        grp.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
    gn = B.node("ShaderNodeGroup")
    gn.node_tree = grp
    nt.links.new(R, gn.inputs[0])
    if paths.get("normal"):
        n = B.node("ShaderNodeTexImage")
        n.image = load_img(paths["normal"], True)
        nm = B.node("ShaderNodeNormalMap", space="TANGENT", uv_map="UVMap")
        nt.links.new(n.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], prin.inputs["Normal"])
    return mat


def gfx_material(name, image_path, alpha):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    B = NB(nt)
    prin = B.node("ShaderNodeBsdfPrincipled")
    out = B.node("ShaderNodeOutputMaterial")
    nt.links.new(prin.outputs[0], out.inputs["Surface"])
    c = B.node("ShaderNodeTexImage")
    c.image = load_img(image_path, False)
    nt.links.new(c.outputs["Color"], prin.inputs["Base Color"])
    if alpha:
        nt.links.new(c.outputs["Alpha"], prin.inputs["Alpha"])
        try:
            mat.blend_method = "CLIP"
        except Exception:  # noqa: BLE001
            pass
    prin.inputs["Roughness"].default_value = 0.8
    prin.inputs["Metallic"].default_value = 0.0
    return mat


def build_final_materials(ob, me, slots, bake_idx, keep_idx, paths, stem, work):
    for m0 in list(bpy.data.materials):   # 元の材質名と衝突して .001 が付かないように退避
        if not m0.name.startswith(('L_', 'forge_')):
            m0.name = '_old_' + m0.name
    # 小さい定数 ORM(R=AO 1, G=粗さ, B=金属 0)。dx12 は factor を読まず ORM に {1,1} を掛けるので、全材質に ORM を持たせる
    gorms = {}

    def gorm_path(rv):
        if rv not in gorms:
            im = bpy.data.images.new("forge_gfx_orm_%d" % int(rv * 100), 4, 4, alpha=False)
            im.colorspace_settings.name = "Non-Color"
            im.pixels = [1.0, rv, 0.0, 1.0] * 16
            p = os.path.join(work, "gfx_orm_%d.png" % int(rv * 100))
            im.filepath_raw = p
            im.file_format = "PNG"
            im.save()
            gorms[rv] = p
        return gorms[rv]

    mats = [final_material("forge_" + stem, paths)]
    alpha_names = []
    new_slot_of = {}
    for i in bake_idx:
        new_slot_of[i] = 0
    for i in keep_idx:
        nm, sp = slots[i]
        m = gfx_material(nm, sp["image"], sp.get("alpha") == "mask")
        # ORM を載せる
        nt = m.node_tree
        prin = [n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"][0]
        o = nt.nodes.new("ShaderNodeTexImage")
        o.image = load_img(gorm_path(float(sp.get("rough_const", 0.75))), True)
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(o.outputs["Color"], sep.inputs[0])
        nt.links.new(sep.outputs[1], prin.inputs["Roughness"])
        nt.links.new(sep.outputs[2], prin.inputs["Metallic"])
        new_slot_of[i] = len(mats)
        mats.append(m)
        if sp.get("alpha") == "mask":
            alpha_names.append(m.name)
    # スロット再構成
    npoly = len(me.polygons)
    mi = np.zeros(npoly, dtype=np.int32)
    me.polygons.foreach_get("material_index", mi)
    new = np.zeros(npoly, dtype=np.int32)
    for old, nw in new_slot_of.items():
        new[mi == old] = nw
    ob.data.materials.clear()
    for m in mats:
        ob.data.materials.append(m)
    me.polygons.foreach_set("material_index", new)
    me.update()
    return dict(mats=mats, alpha=alpha_names)


def export_glb(out, fm):
    bpy.ops.object.select_all(action="SELECT")
    kw = dict(filepath=out, export_format="GLB", export_yup=True, export_apply=True, export_texcoords=True,
              export_normals=True, export_tangents=True, export_materials="EXPORT", export_cameras=False,
              export_lights=False, use_selection=False, export_image_format="AUTO")
    try:
        bpy.ops.export_scene.gltf(export_vertex_color="NONE", **kw)
    except TypeError:
        bpy.ops.export_scene.gltf(**kw)
    postprocess_glb(out, fm["alpha"])


def postprocess_glb(path, alpha_names):
    import struct
    d = open(path, "rb").read()
    jl, jt = struct.unpack("<II", d[12:20])
    js = json.loads(d[20:20 + jl].decode("utf-8"))
    rest = d[20 + jl:]
    for m in js.get("materials", []):
        pbr = m.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorFactor"] = [1, 1, 1, 1]
        pbr["metallicFactor"] = 1.0
        pbr["roughnessFactor"] = 1.0
        m.pop("emissiveFactor", None)
        m["doubleSided"] = m.get("doubleSided", False)
        if m.get("name") in alpha_names:
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
        elif m.get("alphaMode") == "BLEND":
            m.pop("alphaMode")
        if "occlusionTexture" not in m and "metallicRoughnessTexture" in pbr:
            m["occlusionTexture"] = {"index": pbr["metallicRoughnessTexture"]["index"]}
    js["asset"]["generator"] = "forge_bake.py (Blender %s)" % bpy.app.version_string
    b = json.dumps(js, separators=(",", ":")).encode("utf-8")
    b += b" " * ((4 - len(b) % 4) % 4)
    total = 12 + 8 + len(b) + len(rest)
    open(path, "wb").write(d[:8] + struct.pack("<I", total) + struct.pack("<II", len(b), jt) + b + rest)


# ---------------------------------------------------------------- プレビュー
def render_preview(ob, png, front, work):
    sc = bpy.context.scene
    # 材質はすでに最終材質。ライトとカメラを置く
    pts = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    bmin = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    bmax = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    c = (bmin + bmax) / 2
    size = bmax - bmin
    D = size.length
    sgn = {"+x": (1, 0), "-x": (-1, 0), "+y": (0, 1), "-y": (0, -1)}[front]
    fwd = Vector((sgn[0], sgn[1], 0))
    right = Vector((-fwd.y, fwd.x, 0))

    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.05, 0.055, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
    sc.world = world
    # 床(影を受ける。物の底に)
    bpy.ops.mesh.primitive_plane_add(size=D * 6, location=(c.x, c.y, bmin.z - 0.001))
    fl = bpy.context.active_object
    fm = bpy.data.materials.new("floor")
    fm.use_nodes = True
    fm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.09, 0.09, 0.095, 1)
    fm.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.9
    fl.data.materials.append(fm)

    def light(loc, energy, size_, color=(1, 1, 1)):
        ld = bpy.data.lights.new("l", "AREA")
        ld.energy = energy
        ld.size = size_
        ld.color = color
        lo = bpy.data.objects.new("l", ld)
        sc.collection.objects.link(lo)
        lo.location = loc
        d = c - Vector(loc)
        lo.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
        return lo

    k = max(D, 0.5)
    light(c + fwd * k * 1.6 + right * k * 1.0 + Vector((0, 0, k * 1.6)), 110 * k * k, k * 1.2, (1.0, 0.93, 0.82))
    light(c - fwd * k * 1.2 - right * k * 1.6 + Vector((0, 0, k * 0.8)), 35 * k * k, k * 1.5, (0.7, 0.8, 1.0))
    light(c - fwd * k * 1.5 + right * k * 0.6 + Vector((0, 0, k * 1.8)), 50 * k * k, k, (1, 1, 1))

    cam_d = bpy.data.cameras.new("c")
    cam_d.lens = 50
    cam_d.clip_start = 0.01
    cam_d.clip_end = 100
    cam = bpy.data.objects.new("c", cam_d)
    sc.collection.objects.link(cam)
    sc.camera = cam
    fov = 2 * math.atan(18 / 50)
    R = D / 2
    dist = R / math.sin(fov / 2) * 1.02
    views = []
    az = math.radians(35)
    d1 = (fwd * math.cos(az) + right * math.sin(az)) * math.cos(math.radians(18)) + Vector((0, 0, math.sin(math.radians(18))))
    views.append((c + d1 * dist, c))
    d2 = Vector((fwd.x * 0.25, fwd.y * 0.25, 1.0)).normalized()
    views.append((c + d2 * dist, c))
    # 接写: 物の正面側の縁あたり(上 1/3、手前の角寄り)
    tgt = c + fwd * (abs(size.x * fwd.x) + abs(size.y * fwd.y)) * 0.5 + Vector((0, 0, size.z * 0.18)) + right * size.x * 0.12 * (0 if abs(fwd.x) < 0.5 else 1)
    cd = max(0.28, D * 0.22)
    d3 = (fwd * 0.85 + right * 0.45 + Vector((0, 0, 0.45))).normalized()
    views.append((tgt + d3 * cd, tgt))

    sc.render.resolution_x = sc.render.resolution_y = 768
    sc.render.resolution_percentage = 100
    sc.cycles.samples = 48
    sc.cycles.use_denoising = True
    try:
        sc.cycles.denoiser = "OPTIX"
    except Exception:  # noqa: BLE001
        pass
    sc.view_settings.view_transform = "Standard"
    tiles = []
    for i, (loc, tg) in enumerate(views):
        cam.location = loc
        cam.rotation_euler = (tg - loc).to_track_quat("-Z", "Y").to_euler()
        p = os.path.join(work, "pv_%d.png" % i)
        sc.render.filepath = p
        sc.render.image_settings.file_format = "PNG"
        try:
            bpy.ops.render.render(write_still=True)
        except Exception:  # noqa: BLE001
            sc.cycles.use_denoising = False
            bpy.ops.render.render(write_still=True)
        tiles.append(p)
    arrs = []
    for p in tiles:
        im = bpy.data.images.load(p)
        im.colorspace_settings.name = "Non-Color"
        w, h = im.size
        a = np.zeros(w * h * 4, dtype=np.float32)
        im.pixels.foreach_get(a)
        arrs.append(a.reshape(h, w, 4))
    big = np.concatenate(arrs, axis=1)
    h, w = big.shape[:2]
    oi = bpy.data.images.new("pv", w, h, alpha=False)
    oi.colorspace_settings.name = "Non-Color"
    oi.pixels.foreach_set(big.reshape(-1))
    oi.filepath_raw = os.path.abspath(png)
    oi.file_format = "PNG"
    oi.save()
    log("preview", png)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        sys.exit(1)
