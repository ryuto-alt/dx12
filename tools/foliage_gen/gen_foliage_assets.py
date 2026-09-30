#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
植生テスト用アセット生成(F1: 植生 + 風)。著作権のある素材は一切使わず、コードだけで
低ポリの木・低木・草・岩(.glb)と葉/樹皮/草/遠景樹形/岩のテクスチャ(.png)を作る。

    python gen_foliage_assets.py [--out <assets ディレクトリ>] [--preview <png のパス>]

既定の出力先は使い捨てプロジェクトの assets(リポジトリ内へは出さない):
    <out>/models/foliage/*.glb     <out>/textures/foliage/*.png

■ 頂点色 COLOR_0(VEC4 float)はアルベドではなく「風 + AO」のデータ
    R = 葉のはばたき(フラッター)の振幅重み(幹=0 / 葉カードは付け根 0 → 先端・縁 1 / 草は先端ほど 1)
    G = 葉ごとの位相(カード 1 枚ごとにランダム 0..1。同じカードの頂点は同じ値)
    B = 曲げ重み(幹も葉も草も 1 / 岩は 0)
    A = 焼き込み AO(1 = 遮蔽なし。樹冠の内側・根元ほど 0.5..0.7)
  エンジンの ModelLoader は mColors[0] を r,g,b,a のまま Vertex.color へ入れる(無ければ白)。
  PS は albedo に input.color を掛けるので、エンジン側の植生 VS は頂点色を PS へ渡さず
  (または 1 に置き換えて)使うこと。

■ メッシュの原点は根元(地面 y=0、xz は中心)。単位 m、Y up。
■ 材質: 幹/岩 = OPAQUE、葉/草/遠景カード = alphaMode MASK(alphaCutoff 0.5)、doubleSided。
■ UV は glTF 規約(左上原点)のまま。ModelLoader が FlipUVs を掛けるが、assimp の glTF
  インポータが V を反転して返すので相殺され、D3D の左上原点に揃う。
"""
import argparse
import json
import math
import os
import random
import struct
import sys
import zlib

import numpy as np

DEFAULT_OUT = (r"C:\Users\ryuto\AppData\Local\Temp\claude\C--Windows-System32"
               r"\9c1abfab-1ef3-4472-84d8-27f0717dbf64\scratchpad\f1proj\assets")


# ============================================================================
# PNG 書き出し(zlib + struct)
# ============================================================================
def png_bytes(rgba):
    """rgba: (H, W, 4) uint8 → PNG バイト列。"""
    h, w, _ = rgba.shape
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


# ============================================================================
# テクスチャ生成
# ============================================================================
def value_noise(h, w, cell, rng, octaves=1):
    """滑らかな値ノイズ(0..1)。numpy だけ。cell = 最粗のセル数(高さ方向)。"""
    out = np.zeros((h, w), np.float64)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        ch = max(2, int(cell * (2 ** o)))
        cw = max(2, int(cell * (2 ** o) * w / h))
        g = rng.random((ch + 2, cw + 2))
        ys = np.linspace(0, ch, h, endpoint=False)
        xs = np.linspace(0, cw, w, endpoint=False)
        y0 = ys.astype(int); x0 = xs.astype(int)
        fy = (ys - y0)[:, None]; fx = (xs - x0)[None, :]
        fy = fy * fy * (3 - 2 * fy); fx = fx * fx * (3 - 2 * fx)
        a = g[y0][:, x0]; b = g[y0][:, x0 + 1]
        c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
        out += amp * ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy)
        tot += amp
        amp *= 0.5
    return out / tot


def dilate_color(rgb, alpha, iters=16):
    """アルファ 0 の画素へ近傍の不透明色を広げる(ミップで縁が白/黒ににじまないように)。"""
    rgb = rgb.astype(np.float64).copy()
    known = alpha > 0
    for _ in range(iters):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float64)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                c = np.roll(np.roll(rgb, dy, 0), dx, 1)
                acc += c * k[..., None]
                cnt += k
        fill = (~known) & (cnt > 0)
        rgb[fill] = acc[fill] / cnt[fill][:, None]
        known = known | fill
    rgb[~known] = (60, 110, 40)
    return rgb


def finish_rgba(rgb, alpha_mask):
    """alpha_mask(bool) → 0/255 のはっきりした縁 + 透明部のダイレーション。"""
    a = np.where(alpha_mask, 255, 0).astype(np.uint8)
    rgb = dilate_color(rgb, a)
    out = np.zeros(a.shape + (4,), np.uint8)
    out[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    out[..., 3] = a
    return out


def leaf_atlas(rng_seed=11):
    """512x512。2x2 セル: 0=楕円 1=幅広(ハート寄り) 2=針状の束 3=先の尖った葉。
    各セルで葉の付け根はセル下辺中央(カードの根元側)。"""
    S, C = 512, 256
    rng = np.random.default_rng(rng_seed)
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float64)
    mask = np.zeros((S, S), bool)
    rgb = np.zeros((S, S, 3), np.float64)
    noise = value_noise(S, S, 10, rng, 4)
    fine = value_noise(S, S, 48, rng, 2)
    for cell in range(4):
        cy, cx = divmod(cell, 2)
        y0, x0 = cy * C, cx * C
        u = (xx - x0 - C / 2) / (C / 2)          # -1..1(横)
        v = 1.0 - (yy - y0) / C                  # 0(下端=付け根)..1(上端=先)
        inside = (np.abs(u) <= 1) & (v >= 0) & (v <= 1)
        if cell == 0:      # 楕円
            m = ((u / 0.82) ** 2 + ((v - 0.5) / 0.48) ** 2) <= 1.0
        elif cell == 1:    # 幅広
            m = ((u / 0.88) ** 2 + ((v - 0.5) / 0.47) ** 2) <= 1.0
            m &= ~((np.abs(u) < 0.1) & (v > 0.86))      # 先に小さな切れ込み
        elif cell == 2:    # 針状の束: 細い線を扇状に 5 本
            m = np.zeros_like(inside)
            for k in range(5):
                ang = (k - 2) * 0.28
                dx = u - (v * math.tan(ang))
                w = 0.13 * (1.0 - 0.5 * v)
                m |= (np.abs(dx) < w) & (v > 0.02) & (v < 0.96 - abs(k - 2) * 0.05)
        else:              # 先の尖った葉(レンズ形)
            half = 0.85 * np.sin(np.pi * np.clip(v, 0, 1)) ** 0.7
            m = (np.abs(u) < half) & (v > 0.02) & (v < 0.98)
        m &= inside
        # 色: 緑の個体差 + 葉脈(中央線と斜脈)+ 縁を少し暗く
        base = np.array([[58, 118, 38], [74, 138, 44], [46, 104, 42], [88, 146, 52]][cell], np.float64)
        var = 0.78 + 0.44 * noise
        col = base[None, None, :] * var[..., None] * (0.92 + 0.16 * fine[..., None])
        vein = (np.abs(u) < 0.03) & (v < 0.95)
        vein |= (np.abs(np.abs(u) - 0.45 * v) < 0.025) & (v > 0.1) & (v < 0.8)
        col[vein] *= 1.25
        edge = np.clip(1.0 - (np.abs(u) / 0.9), 0, 1)
        col *= (0.80 + 0.20 * np.clip(edge * 2, 0, 1))[..., None]
        mask |= m
        rgb = np.where(m[..., None], col, rgb)
    return finish_rgba(rgb, mask), mask.mean()


def bark_texture(seed=21):
    S = 256
    rng = np.random.default_rng(seed)
    # 縦に長い筋: x 方向に細かく y 方向に粗いノイズ
    n1 = value_noise(S, S, 3, rng, 1)
    ridge = np.zeros((S, S))
    g = rng.random((S // 4, 24))
    ridge = np.repeat(np.repeat(g, 4, 0), S // 24 + 1, 1)[:S, :S]
    ridge = (ridge + np.roll(ridge, 1, 1) + np.roll(ridge, -1, 1)) / 3
    n2 = value_noise(S, S, 12, rng, 3)
    t = 0.5 * ridge + 0.3 * n2 + 0.2 * n1
    base = np.array([96, 68, 44], np.float64)
    rgb = base[None, None, :] * (0.55 + 0.9 * t)[..., None]
    rgb[..., 2] *= 0.92
    out = np.zeros((S, S, 4), np.uint8)
    out[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def grass_blade_texture(seed=31):
    """128x512 縦長。下端が付け根。"""
    W, H = 128, 512
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    u = (xx + 0.5 - W / 2) / (W / 2)
    t = 1.0 - (yy + 0.5) / H                      # 0=付け根 1=先
    half = 0.9 * (1.0 - t ** 1.6) * (0.6 + 0.4 * np.clip(t * 6, 0, 1)) + 0.02
    mask = (np.abs(u) < half) & (t > 0.0)
    col0 = np.array([38, 86, 26], np.float64)
    col1 = np.array([150, 180, 70], np.float64)
    tt = np.clip(t, 0, 1)[..., None]
    rgb = col0 * (1 - tt) + col1 * tt
    noise = value_noise(H, W, 16, rng, 2)
    rgb *= (0.85 + 0.3 * noise)[..., None]
    rgb[np.abs(u) < 0.06] *= 1.18                  # 中央の脈
    return finish_rgba(rgb, mask), mask.mean()


def tree_far_texture(seed=41):
    """256x512。樹形の遠景シルエット(緑の塊 + 幹)。画像の下端 = 根元。"""
    W, H = 256, 512
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    u = (xx + 0.5 - W / 2) / (W / 2)               # -1..1
    t = 1.0 - (yy + 0.5) / H                        # 0=根元 1=てっぺん
    mask = np.zeros((H, W), bool)
    # 樹冠: 円の和(中心 t≈0.66、半径 ≈0.34)
    circles = [(0.0, 0.64, 0.30), (-0.22, 0.56, 0.21), (0.22, 0.58, 0.21),
               (-0.10, 0.80, 0.19), (0.12, 0.79, 0.19), (0.0, 0.93, 0.11),
               (-0.34, 0.50, 0.12), (0.34, 0.51, 0.12)]
    for cx, cy, r in circles:
        du = (u - 0.0) * (W / H) - cx          # 画素が等方になる正規化座標(横 = u * W/H)
        dt = (t - cy)
        mask |= (du * du + dt * dt) <= r * r
    # 樹冠の縁を葉の塊らしく波打たせる
    n = value_noise(H, W, 20, rng, 3)
    mask &= (n > 0.18)
    # 幹
    trunk = (np.abs(u) < (0.08 * (1.0 - 0.5 * t / 0.6))) & (t < 0.55)
    mask |= trunk
    crown = mask & ~trunk
    shade = 0.55 + 0.6 * np.clip((t - 0.3) / 0.7, 0, 1) * (0.6 + 0.4 * n)
    green = np.array([62, 120, 40], np.float64)[None, None, :] * shade[..., None] * (0.8 + 0.4 * value_noise(H, W, 40, rng, 2))[..., None]
    brown = np.array([88, 62, 40], np.float64)[None, None, :] * np.ones((H, W, 1))
    rgb = np.where(crown[..., None], green, brown)
    return finish_rgba(rgb, mask), mask.mean()


def grass_far_texture(seed=61):
    """草の遠景用: 縦グラデ(根元 暗い → 先端 明るい)の不透明 64x64。"""
    S = 64
    rng = np.random.default_rng(seed)
    t = np.linspace(1.0, 0.0, S)[:, None] * np.ones((1, S))       # 画像の上 = 先端(v=0)= 1
    n = value_noise(S, S, 4, rng, 2)
    v = 0.55 + 0.6 * t
    v = v * (0.9 + 0.2 * n)
    out = np.zeros((S, S, 4), np.uint8)
    out[..., 0] = np.clip(58 * v, 0, 255).astype(np.uint8)
    out[..., 1] = np.clip(118 * v, 0, 255).astype(np.uint8)
    out[..., 2] = np.clip(36 * v, 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def rock_texture(seed=51):
    S = 256
    rng = np.random.default_rng(seed)
    n = value_noise(S, S, 6, rng, 5)
    speck = rng.random((S, S)) * 0.12
    v = 0.35 + 0.55 * n + speck
    rgb = np.stack([v * 140, v * 138, v * 132], -1)
    out = np.zeros((S, S, 4), np.uint8)
    out[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


# ============================================================================
# メッシュ組み立て
# ============================================================================
class Prim:
    """1 プリミティブ(= 1 サブメッシュ = 1 材質)。"""

    def __init__(self, material):
        self.material = material
        self.pos, self.nrm, self.col, self.uv, self.idx = [], [], [], [], []

    def vert(self, p, n, c, uv):
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
        self.pos.append(tuple(p))
        self.nrm.append((n[0] / ln, n[1] / ln, n[2] / ln))
        self.col.append(tuple(c))
        self.uv.append(tuple(uv))
        return len(self.pos) - 1

    def tri(self, a, b, c):
        self.idx += [a, b, c]

    def tris(self):
        return len(self.idx) // 3


def vadd(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def vsub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def vmul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def vdot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def vcross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def vlen(a): return math.sqrt(vdot(a, a))
def vnorm(a):
    l = vlen(a) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)


def add_leaf_card(prim, center, up_dir, side_dir, size, cell, phase, ao, normal):
    """葉カード(頂点 4・三角形 2)。center=カード中心 / up_dir=付け根→先端の向き。
    付け根辺(下)の頂点は R=0、先端辺(上)は R=1。UV は atlas の cell(0..3)を使う。"""
    cy, cx = divmod(cell, 2)
    u0, u1 = cx * 0.5, cx * 0.5 + 0.5
    v0, v1 = cy * 0.5, cy * 0.5 + 0.5           # v0=セル上端 v1=セル下端(画像座標)
    hu = vmul(up_dir, size * 0.5)
    hs = vmul(side_dir, size * 0.5)
    corners = [
        (vsub(vsub(center, hu), hs), (u0, v1), 0.0),   # 左下(付け根)
        (vadd(vsub(center, hu), hs), (u1, v1), 0.0),   # 右下(付け根)
        (vadd(vadd(center, hu), hs), (u1, v0), 1.0),   # 右上(先端)
        (vsub(vadd(center, hu), hs), (u0, v0), 1.0),   # 左上(先端)
    ]
    ids = [prim.vert(p, normal, (r, phase, 1.0, ao), uv) for p, uv, r in corners]
    prim.tri(ids[0], ids[1], ids[2])
    prim.tri(ids[0], ids[2], ids[3])
    # 両面: 裏面用に頂点を複製せず、カリング無し(doubleSided)で描く


def add_tube(prim, p0, p1, r0, r1, sides, rings, ao0, ao1, uv_v0=0.0, uv_v1=1.0, cap=False):
    """先細りの円柱(p0→p1)。幹 / 枝。頂点色: R=0 G=0 B=1 A=AO。"""
    axis = vnorm(vsub(p1, p0))
    ref = (1.0, 0.0, 0.0) if abs(axis[0]) < 0.9 else (0.0, 0.0, 1.0)
    e1 = vnorm(vcross(axis, ref))
    e2 = vcross(axis, e1)
    ring_ids = []
    for k in range(rings + 1):
        t = k / rings
        c = vadd(p0, vmul(vsub(p1, p0), t))
        r = r0 + (r1 - r0) * t
        ao = ao0 + (ao1 - ao0) * t
        ids = []
        for s in range(sides + 1):
            a = 2 * math.pi * s / sides
            d = vadd(vmul(e1, math.cos(a)), vmul(e2, math.sin(a)))
            ids.append(prim.vert(vadd(c, vmul(d, r)), d, (0.0, 0.0, 1.0, ao),
                                 (s / sides, uv_v0 + (uv_v1 - uv_v0) * t)))
        ring_ids.append(ids)
    for k in range(rings):
        for s in range(sides):
            a, b = ring_ids[k][s], ring_ids[k][s + 1]
            c, d = ring_ids[k + 1][s + 1], ring_ids[k + 1][s]
            prim.tri(a, b, c)
            prim.tri(a, c, d)
    if cap:   # 先端の蓋(枝の先)
        cid = prim.vert(p1, axis, (0.0, 0.0, 1.0, ao1), (0.5, uv_v1))
        for s in range(sides):
            prim.tri(ring_ids[rings][s], ring_ids[rings][s + 1], cid)


def sphere_shell_point(rng, R, rmin):
    """球殻内の一様乱数点(rmin..R の半径)。"""
    while True:
        v = (rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1))
        l = vlen(v)
        if 0.05 < l <= 1.0:
            r = rmin + (R - rmin) * (l ** (1.0 / 3.0))
            return vmul(vnorm(v), r)


def make_canopy_cards(prim, rng, center, R, count, size, rmin_frac, ao_lo=0.55, squash=1.0, min_y=None):
    """樹冠(または低木)を球状の葉カードで埋める。法線は中心から外向き。"""
    for _ in range(count):
        off = sphere_shell_point(rng, R, R * rmin_frac)
        off = (off[0], off[1] * squash, off[2])
        p = vadd(center, off)
        if min_y is not None:
            p = (p[0], max(p[1], min_y + 0.8 * size), p[2])
        out = vnorm(off)
        # カードの向き: 外向き法線の周りにランダムに回す。付け根は中心側、先端は外側寄り
        rnd = (rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1))
        side = vnorm(vcross(out, rnd)) if vlen(vcross(out, rnd)) > 1e-3 else (1.0, 0.0, 0.0)
        up = vnorm(vadd(vmul(vcross(side, out), 1.0), vmul(out, 0.5)))
        cell = rng.randrange(4)
        phase = rng.random()
        rr = vlen(off) / R
        ao = ao_lo + (1.0 - ao_lo) * min(1.0, max(0.0, (rr - 0.3) / 0.7))
        s = size * rng.uniform(0.8, 1.2)
        # 法線は外向きに上向き成分を少し足して樹冠の陰影を丸く
        nrm = vnorm(vadd(out, (0.0, 0.25, 0.0)))
        add_leaf_card(prim, p, up, side, s, cell, phase, ao, nrm)


def build_tree(lod, seed):
    rng = random.Random(seed)
    trunk = Prim("bark")
    leaves = Prim("leaf")
    # 幹(地面 y=0 から)
    if lod == 0:
        sides, rings, n_leaf, lsize = 8, 3, 210, 0.95
    else:
        sides, rings, n_leaf, lsize = 5, 2, 60, 1.75
    H = 3.4
    add_tube(trunk, (0, 0, 0), (0, H, 0), 0.24, 0.09, sides, rings, 0.62, 1.0, 0.0, 1.6)
    # 枝(2〜3 本)
    branches = [(2.3, 0.0, 0.85), (2.7, 2.1, 0.75), (2.9, 4.2, 0.8)] if lod == 0 else [(2.4, 0.5, 0.9), (2.8, 3.6, 0.8)]
    canopy_c = (0.0, 4.6, 0.0)
    R = 2.4
    for hy, ang, ln in branches:
        base = (0.0, hy, 0.0)
        d = (math.cos(ang) * 0.75, 0.62, math.sin(ang) * 0.75)
        tip = vadd(base, vmul(vnorm(d), 1.4 * ln * 1.6))
        add_tube(trunk, base, tip, 0.075, 0.03, 6 if lod == 0 else 4, 1, 0.9, 1.0, 0.2, 0.9, cap=True)
    # 樹冠: 葉カード。R=2.4 中心 y=4.6 → てっぺん ≈ 7.0m
    make_canopy_cards(leaves, rng, canopy_c, R, n_leaf, lsize, 0.35, ao_lo=0.5, squash=0.95)
    return [trunk, leaves]


def build_tree_far():
    """遠景クロスカード: 幅 5.0 x 高さ 7.0 の縦板を 2 枚(90 度の十字)。MASK・tree_far.png。
    2 枚にしたのは塗りつぶし(オーバードロー)を 1/3 減らすため（3 枚だと遠景の森の主パスが重い）。"""
    p = Prim("far")
    W, Hh = 5.0, 7.0
    for k in range(2):
        a = math.pi * k / 2.0
        d = (math.cos(a), 0.0, math.sin(a))
        phase = (k + 0.5) / 2.0
        pts = [(-1, 0.0, (0.0, 1.0)), (1, 0.0, (1.0, 1.0)), (1, Hh, (1.0, 0.0)), (-1, Hh, (0.0, 0.0))]
        ids = []
        for sx, y, uv in pts:
            pos = (d[0] * sx * W * 0.5, y, d[2] * sx * W * 0.5)
            # 法線: 樹冠中心(y=4.4)から外向き(丸い陰影)
            nrm = vnorm(vsub(pos, (0.0, 3.6, 0.0)))
            # R: 高さ 0.35 以上でなだらかに 1 へ(上部ほどはばたく)
            r = min(1.0, max(0.0, (y / Hh - 0.3) / 0.6))
            ao = 0.75 + 0.25 * (y / Hh)
            ids.append(p.vert(pos, nrm, (r, phase, 1.0, ao), uv))
        p.tri(ids[0], ids[1], ids[2])
        p.tri(ids[0], ids[2], ids[3])
    return [p]


def build_bush(lod, seed):
    rng = random.Random(seed)
    leaves = Prim("leaf")
    n, size = (60, 0.55) if lod == 0 else (20, 0.95)
    make_canopy_cards(leaves, rng, (0.0, 0.62, 0.0), 0.55, n, size, 0.3, ao_lo=0.5, squash=0.95, min_y=0.0)
    return [leaves]


def build_grass(lod, seed):
    rng = random.Random(seed)
    g = Prim("grass")
    blades = (9, 4)[lod]
    segs = (3, 2)[lod]
    for b in range(blades):
        ang = 2 * math.pi * (b + rng.uniform(-0.25, 0.25)) / blades
        d = (math.cos(ang), 0.0, math.sin(ang))          # 放射方向(湾曲する向き)
        tang = (-d[2], 0.0, d[0])
        hgt = rng.uniform(0.36, 0.55) * (1.0, 1.05)[lod]
        width = 0.075 * (1.0, 1.7)[lod]
        lean = rng.uniform(0.10, 0.22) * hgt * 2.2       # 先端が倒れる水平量
        base = vmul(d, rng.uniform(0.02, 0.10))
        phase = rng.random()
        rows = []
        for r in range(segs + 1):
            t = r / segs
            c = vadd(base, vadd(vmul(d, lean * t * t), (0.0, hgt * (1.0 - 0.12 * t * t) * t, 0.0)))
            # 曲線の接線から法線(草の「面」の向き)= tang × 接線
            t2 = (r + 0.5) / segs
            tan = vnorm((d[0] * lean * 2 * t, hgt, d[2] * lean * 2 * t))
            face = vnorm(vcross(tang, tan))
            # 草の法線は上向きに寄せて柔らかい陰影に
            nrm = vnorm(vadd(vmul(face, 0.55), (0.0, 0.85, 0.0)))
            ao = 0.55 + 0.45 * t
            wv = width * 0.5
            l = vsub(c, vmul(tang, wv))
            rr = vadd(c, vmul(tang, wv))
            v = 1.0 - t
            i0 = g.vert(l, nrm, (t, phase, 1.0, ao), (0.0, v))
            i1 = g.vert(rr, nrm, (t, phase, 1.0, ao), (1.0, v))
            rows.append((i0, i1))
        for r in range(segs):
            a, b2 = rows[r]
            c, d2 = rows[r + 1]
            g.tri(a, b2, d2)
            g.tri(a, d2, c)
    return [g]


def build_grass_far(seed):
    """草の遠景 LOD: 先細りの三角形 3 枚(三角形 3・頂点 9)。**不透明**(アルファテスト無し)= 深度 / 本体とも PS の
    テクスチャ抜きが要らず、小さな三角形の大量描画が安い。材質 grass_far(縦グラデの緑 1 枚)。"""
    rng = random.Random(seed)
    g = Prim("grass_far")
    for b in range(3):
        ang = 2 * math.pi * (b + rng.uniform(-0.2, 0.2)) / 3.0
        d = (math.cos(ang), 0.0, math.sin(ang))
        tang = (-d[2], 0.0, d[0])
        hgt = rng.uniform(0.40, 0.58)
        width = 0.16
        lean = rng.uniform(0.05, 0.14)
        phase = rng.random()
        base = vmul(d, rng.uniform(0.02, 0.09))
        tip = vadd(base, vadd(vmul(d, lean), (0.0, hgt, 0.0)))
        face = vnorm(vcross(tang, vnorm(vsub(tip, base))))
        nrm = vnorm(vadd(vmul(face, 0.55), (0.0, 0.85, 0.0)))
        i0 = g.vert(vsub(base, vmul(tang, width * 0.5)), nrm, (0.0, phase, 1.0, 0.55), (0.0, 1.0))
        i1 = g.vert(vadd(base, vmul(tang, width * 0.5)), nrm, (0.0, phase, 1.0, 0.55), (1.0, 1.0))
        i2 = g.vert(tip, nrm, (1.0, phase, 1.0, 1.0), (0.5, 0.0))
        g.tri(i0, i1, i2)
    return [g]


def build_rock(seed):
    rng = np.random.default_rng(seed)
    slices, stacks = 14, 8
    grid = []
    # 経度 x 緯度の格子点に乱数変位をつけた低ポリ球。下半分は潰して平らな底にする
    disp = {}
    def P(i, j):
        if j == 0: key = ("top",)
        elif j == stacks: key = ("bot",)
        else: key = (i % slices, j)
        if key not in disp:
            disp[key] = 1.0 + rng.uniform(-0.18, 0.18)
        th = math.pi * j / stacks
        ph = 2 * math.pi * (i % slices) / slices
        r = disp[key]
        x = math.sin(th) * math.cos(ph) * r
        y = math.cos(th) * r
        z = math.sin(th) * math.sin(ph) * r
        return (x * 0.55, y * 0.34, z * 0.48)
    p = Prim("rock")
    faces = []
    for j in range(stacks):
        for i in range(slices):
            a, b, c, d = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
            uvs = [(i / slices, j / stacks), ((i + 1) / slices, j / stacks),
                   ((i + 1) / slices, (j + 1) / stacks), (i / slices, (j + 1) / stacks)]
            if j == 0:
                faces.append(((a, c, d), (uvs[0], uvs[2], uvs[3])))
            elif j == stacks - 1:
                faces.append(((a, b, c), (uvs[0], uvs[1], uvs[2])))
            else:
                faces.append(((a, b, c), (uvs[0], uvs[1], uvs[2])))
                faces.append(((a, c, d), (uvs[0], uvs[2], uvs[3])))
    # 原点を根元(底 y=0)へ: 最小 y を 0 にする
    miny = min(v[1] for f, _ in faces for v in f)
    for (tri, uvs) in faces:
        pts = [(v[0], v[1] - miny, v[2]) for v in tri]
        n = vnorm(vcross(vsub(pts[1], pts[0]), vsub(pts[2], pts[0])))
        # 面法線が外向き(中心 (0,0.3,0) から)になるよう向きを揃える
        cen = vmul(vadd(vadd(pts[0], pts[1]), pts[2]), 1 / 3.0)
        if vdot(n, vsub(cen, (0.0, 0.3, 0.0))) < 0:
            pts = [pts[0], pts[2], pts[1]]; uvs = (uvs[0], uvs[2], uvs[1]); n = vmul(n, -1)
        ids = []
        for pt, uv in zip(pts, uvs):
            ao = 0.7 + 0.3 * min(1.0, pt[1] / 0.5)
            ids.append(p.vert(pt, n, (0.0, 0.0, 0.0, ao), uv))
        p.tri(*ids)
    return [p]


# ============================================================================
# GLB 書き出し
# ============================================================================
MATERIALS = {
    # name: (texture, alphaMode, cutoff, roughness)
    "bark":  ("bark.png",       "OPAQUE", None, 0.92),
    "leaf":  ("leaf_atlas.png", "MASK",   0.5,  0.75),
    "far":   ("tree_far.png",   "MASK",   0.5,  0.85),
    "grass": ("grass_blade.png", "MASK",  0.5,  0.8),
    "grass_far": ("grass_far.png", "OPAQUE", None, 0.8),
    "rock":  ("rock.png",       "OPAQUE", None, 0.95),
}


def write_glb(path, prims, tex_bytes, name, plain=False):
    binbuf = bytearray()
    views, accessors = [], []

    def add_view(data, target=None):
        while len(binbuf) % 4:
            binbuf.append(0)
        off = len(binbuf)
        binbuf.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    def add_acc(arr, ctype, comp, target, with_minmax=False):
        data = arr.tobytes()
        bv = add_view(data, target)
        a = {"bufferView": bv, "componentType": comp, "count": int(arr.shape[0]), "type": ctype}
        if with_minmax:
            a["min"] = [float(x) for x in arr.min(0)]
            a["max"] = [float(x) for x in arr.max(0)]
        accessors.append(a)
        return len(accessors) - 1

    images, textures, materials, mat_index, prim_json = [], [], [], {}, []
    img_index = {}
    for p in prims:
        if p.material not in mat_index:
            tex, mode, cutoff, rough = MATERIALS[p.material]
            if tex not in img_index:
                bv = add_view(tex_bytes[tex])
                images.append({"bufferView": bv, "mimeType": "image/png", "name": tex})
                textures.append({"source": len(images) - 1, "sampler": 0})
                img_index[tex] = len(textures) - 1
            m = {"name": p.material,
                 "pbrMetallicRoughness": {"baseColorTexture": {"index": img_index[tex]},
                                          "baseColorFactor": [1, 1, 1, 1],
                                          "metallicFactor": 0.0, "roughnessFactor": rough},
                 "alphaMode": mode, "doubleSided": True}
            if cutoff is not None:
                m["alphaCutoff"] = cutoff
            materials.append(m)
            mat_index[p.material] = len(materials) - 1
        pos = np.array(p.pos, np.float32)
        nrm = np.array(p.nrm, np.float32)
        col = np.array(p.col, np.float32)
        uv = np.array(p.uv, np.float32)
        idx = np.array(p.idx, np.uint32)
        prim_json.append({
            "attributes": {
                "POSITION": add_acc(pos, "VEC3", 5126, 34962, True),
                "NORMAL": add_acc(nrm, "VEC3", 5126, 34962),
                **({} if plain else {"COLOR_0": add_acc(col, "VEC4", 5126, 34962)}),
                "TEXCOORD_0": add_acc(uv, "VEC2", 5126, 34962),
            },
            "indices": add_acc(idx.reshape(-1, 1), "SCALAR", 5125, 34963),
            "material": mat_index[p.material], "mode": 4})
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_foliage_assets.py (自作・著作権フリー)"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": prim_json}],
        "materials": materials, "textures": textures, "images": images,
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(binbuf)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    while len(binbuf) % 4:
        binbuf.append(0)
    total = 12 + 8 + len(js) + 8 + len(binbuf)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON")); f.write(js)
        f.write(struct.pack("<I4s", len(binbuf), b"BIN\x00")); f.write(bytes(binbuf))


# ============================================================================
# 検査(再読込)
# ============================================================================
def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, total = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF" and ver == 2 and total == len(data), "glb ヘッダ不正"
    off = 12
    js = binb = None
    while off < len(data):
        ln, tag = struct.unpack_from("<I4s", data, off)
        body = data[off + 8: off + 8 + ln]
        if tag == b"JSON": js = json.loads(body)
        elif tag.startswith(b"BIN"): binb = body
        off += 8 + ln
    return js, binb


def inspect(path):
    js, binb = read_glb(path)
    rows = []
    allmin = np.array([1e9] * 3); allmax = np.array([-1e9] * 3)
    total_tris = 0
    for prim in js["meshes"][0]["primitives"]:
        acc = js["accessors"]
        pa = acc[prim["attributes"]["POSITION"]]
        ca = acc[prim["attributes"]["COLOR_0"]]
        assert ca["type"] == "VEC4" and ca["componentType"] == 5126
        ia = acc[prim["indices"]]
        def load(a, n):
            bv = js["bufferViews"][a["bufferView"]]
            dt = {5126: np.float32, 5125: np.uint32}[a["componentType"]]
            return np.frombuffer(binb, dt, a["count"] * n, bv["byteOffset"]).reshape(a["count"], n)
        pos = load(pa, 3); col = load(ca, 4); idx = load(ia, 1).ravel()
        assert idx.max() < pa["count"] and len(idx) % 3 == 0
        assert np.allclose(pos.min(0), pa["min"], atol=1e-5) and np.allclose(pos.max(0), pa["max"], atol=1e-5)
        allmin = np.minimum(allmin, pos.min(0)); allmax = np.maximum(allmax, pos.max(0))
        mat = js["materials"][prim["material"]]
        total_tris += len(idx) // 3
        rows.append((mat["name"], mat["alphaMode"], len(idx) // 3, pa["count"],
                     col.min(0).round(2).tolist(), col.max(0).round(2).tolist()))
    return total_tris, allmin, allmax, rows


# ============================================================================
def main():
    ap = argparse.ArgumentParser(description="植生テスト用アセット生成")
    ap.add_argument("--out", default=DEFAULT_OUT, help="assets ディレクトリ(models/foliage と textures/foliage を作る)")
    ap.add_argument("--preview", default="", help="簡易プレビュー PNG の出力パス(matplotlib があるときだけ)")
    args = ap.parse_args()

    mdir = os.path.join(args.out, "models", "foliage")
    tdir = os.path.join(args.out, "textures", "foliage")
    os.makedirs(mdir, exist_ok=True); os.makedirs(tdir, exist_ok=True)

    # --- テクスチャ ---
    leaf, cov_leaf = leaf_atlas()
    bark = bark_texture()
    grass, cov_grass = grass_blade_texture()
    far, cov_far = tree_far_texture()
    rock = rock_texture()
    gfar = grass_far_texture()
    texs = {"leaf_atlas.png": leaf, "bark.png": bark, "grass_blade.png": grass,
            "tree_far.png": far, "rock.png": rock, "grass_far.png": gfar}
    tex_bytes = {}
    for name, img in texs.items():
        b = png_bytes(img)
        tex_bytes[name] = b
        with open(os.path.join(tdir, name), "wb") as f:
            f.write(b)
    print("[texture] alpha 被覆率: leaf_atlas=%.2f grass_blade=%.2f tree_far=%.2f" % (cov_leaf, cov_grass, cov_far))

    # --- メッシュ ---
    models = {
        "tree_lod0.glb": (build_tree(0, 101), "tree_lod0"),
        "tree_lod1.glb": (build_tree(1, 101), "tree_lod1"),
        "tree_lod2.glb": (build_tree_far(), "tree_lod2"),
        "bush_lod0.glb": (build_bush(0, 202), "bush_lod0"),
        "bush_lod1.glb": (build_bush(1, 202), "bush_lod1"),
        "grass_lod0.glb": (build_grass(0, 303), "grass_lod0"),
        "grass_lod1.glb": (build_grass(1, 303), "grass_lod1"),
        "grass_lod2.glb": (build_grass_far(303), "grass_lod2"),
        "rock_lod0.glb": (build_rock(404), "rock_lod0"),
    }
    for fn, (prims, nm) in models.items():
        write_glb(os.path.join(mdir, fn), prims, tex_bytes, nm)
    # 頂点色（風 + AO のデータ）を持たない版。**通常のメッシュとして描く比較用**（パストレーサー / 通常の MeshRenderer は
    # 頂点色をアルベドに掛けるので、風データ入りの glb をそのまま使うと色が壊れる）。
    pdir = os.path.join(args.out, "models", "foliage_plain")
    os.makedirs(pdir, exist_ok=True)
    for fn in ("tree_lod0.glb", "bush_lod0.glb", "rock_lod0.glb"):
        prims, nm = models[fn]
        write_glb(os.path.join(pdir, fn), prims, tex_bytes, nm + "_plain", plain=True)

    # --- 検査 ---
    print("\n%-16s %6s  %-24s %-26s" % ("file", "tris", "AABB min", "AABB max"))
    for fn in models:
        tris, mn, mx, rows = inspect(os.path.join(mdir, fn))
        print("%-16s %6d  (%5.2f,%5.2f,%5.2f)  (%5.2f,%5.2f,%5.2f)  submesh=%d" %
              (fn, tris, *mn, *mx, len(rows)))
        for r in rows:
            print("    %-6s %-6s tris=%-5d verts=%-5d colorMin(RGBA)=%s colorMax=%s" % r)

    if args.preview:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, axs = plt.subplots(2, 4, figsize=(16, 8))
            for ax, fn in zip(axs.ravel(), models):
                js, binb = read_glb(os.path.join(mdir, fn))
                for prim in js["meshes"][0]["primitives"]:
                    pa = js["accessors"][prim["attributes"]["POSITION"]]
                    ca = js["accessors"][prim["attributes"]["COLOR_0"]]
                    bv = js["bufferViews"][pa["bufferView"]]
                    pos = np.frombuffer(binb, np.float32, pa["count"] * 3, bv["byteOffset"]).reshape(-1, 3)
                    ax.scatter(pos[:, 0], pos[:, 1], s=1,
                               c="saddlebrown" if js["materials"][prim["material"]]["name"] in ("bark", "rock") else "green")
                ax.set_aspect("equal"); ax.set_title(fn)
            fig.savefig(args.preview, dpi=80)
            print("preview:", args.preview)
        except Exception as e:   # matplotlib 無しでも生成自体は成功扱い
            print("preview skipped:", e)
    print("\nOK: %s" % args.out)


if __name__ == "__main__":
    sys.exit(main())
