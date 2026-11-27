# -*- coding: utf-8 -*-
"""BA-13m: 起動演出「標準」と「リッチ（BA-13）」の静止画モック（Python / numpy / Pillow のみ。窓は出さない）。

Calibrate のキーフレーム 3 枚（1,543 / 1,628 / 2,148 ms）を、同じ状態から 2 通りに描く。
  standard : GG 仕様の起動画面（フラット。黄は指針・ハブ・校正三角・工程の現在線だけ）
  rich     : 標準の上に「質感の上乗せ層」を足したもの。光らせない（グロー・ブルームなし）。
             1. 異方性のヘアラインのベゼル（ハイライトはハムに同期して移動する）
             2. カバーガラス（薄い反射、縁のフレネル、屈折のゆがみ、埃と細い傷）
             3. 指針の影（高さで半影が広がる。動いている間は浮き、ロックで着座して締まる）
             4. 文字盤の刻印（法線から計算した縁取りのハイライトと影）
             5. 層ごとの微小な視差（最大 1.5 px 以下）
  黄の画素は標準と同じ要素だけ（検査: --check）。

出力: %USERPROFILE%\\dx12-scorecard\\w1\\ba13_mock\\ （--out で変更）。リポジトリには画像を入れない。
形状は GG ブランド幾何 v3.1（geo.py）の寸法を写している。時刻は Calibrate のキュー表
（assets/editor/sounds/calibrate/events.json のロック 1,628 ms と C23 = 1,543 ms、登録のピン 2,148 ms）。

使い方:
  python tools/ba13_mock/ba13_mock.py            # 6 枚 + 比較シート + 拡大の切り出しを出す
  python tools/ba13_mock/ba13_mock.py --check    # 出力と不変条件を検査する
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ------------------------------------------------------------------ トークン（GG final tokens.json の値）
def hexc(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], dtype=np.float32)

BG0, BG1, BG2, BG3, BG4 = (hexc(x) for x in ("#131312", "#1B1A19", "#222120", "#2A2927", "#34322F"))
GROOVE = hexc("#0A0A09")
BORDER, BORDER_STRONG = hexc("#2C2B28"), hexc("#46443F")
T1, T2, T3, T4 = (hexc(x) for x in ("#E6E2DA", "#CFCAC1", "#B4AFA6", "#A39E94"))
ACCENT = hexc("#F0C23B")
GRID_MINOR_A = 0x0E / 255.0

# ------------------------------------------------------------------ 寸法（段階 L、1600x900 DIP）
W, H = 1600, 900
DCX, DCY = 520.0, 450.0
K = 2.1
R_BEZEL, R_TICK_OUT, R_LABEL, R_ARC, R_DISC = 268.0, 250.0, 214.0, 190.0, 132.0
SS = 2                      # 浮動小数の合成は 2 倍解像度
OS = 2                      # マスクの描画はさらに 2 倍（合計 4 倍）で描いて縮める
PARALLAX_MAX_PX = 1.5       # 視差の上限（BA-13: 1.5px 以下）
VIEW = np.array([0.85, 0.53]) / math.hypot(0.85, 0.53)
PARALLAX = {"plate": 0.0, "mark": 0.5, "needle": 1.0, "glass": 1.5}
LIGHT_XY = np.array([-0.70, -0.70])            # 光は左上から

# Mark（geo.py）
UX0, UX1, UW, UTOP, UCY = 22.0, 78.0, 12.0, 30.0, 60.0
HUB_R, PIN_R, NW0, NW1 = 5.5, 1.8, 4.4, 2.4

STEPS = [("01", "コア", 260), ("02", "デバイス", 440), ("03", "シェーダー", 960), ("04", "アセット", 710), ("05", "エディタ UI", 470)]
FIGURE_NO = "UN-0.9.3"      # 図番 UN-x.y.z（オーナー決定 4）。x.y.z = アプリの版

KEYFRAMES = {
    # t_ms: 状態。lobe = ベゼルの異方性ハイライトの向き（ハムに同期して回る。1,648〜1,848 のスイープ後は右上で止まる）
    1543: dict(name="1543", title="C23（360° 到達、行き過ぎの直前）", ghosts=True, lock=False, stamp=False, status="校正中",
               lobe=305.0, height=1.00, aligned=0.0, lock_label=False, seq="05 / 05"),
    1628: dict(name="1628", title="ロック（指針が静止）", ghosts=False, lock=True, stamp=False, status="校正中",
               lobe=317.0, height=0.30, aligned=0.0, lock_label=True, seq="05 / 05"),
    2148: dict(name="2148", title="登録のピン（図番の刻印）", ghosts=False, lock=True, stamp=True, status="校正済",
               lobe=38.0, height=0.30, aligned=1.0, lock_label=False, seq="05 / 05"),
}

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


# ------------------------------------------------------------------ フォント
def _first(paths):
    for p in paths:
        if os.path.exists(p):
            return p
    return None

FONT_JA = _first([os.path.join(REPO, "assets", "editor", "fonts", "NotoSansJP-Regular.ttf"),
                  r"C:\Windows\Fonts\YuGothM.ttc", r"C:\Windows\Fonts\meiryo.ttc"])
FONT_MONO = _first([r"C:\Windows\Fonts\consola.ttf", r"C:\Windows\Fonts\cour.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"])
FONT_MONO_B = _first([r"C:\Windows\Fonts\consolab.ttf", FONT_MONO])
_fc = {}

def font(kind, px):
    key = (kind, px)
    if key not in _fc:
        path = {"ja": FONT_JA, "mono": FONT_MONO, "monob": FONT_MONO_B}[kind]
        _fc[key] = ImageFont.truetype(path, int(round(px))) if path else ImageFont.load_default()
    return _fc[key]


# ------------------------------------------------------------------ 数値の部品
def gblur(a, sigma):
    """ガウスのぼかし（箱 3 回。浮動小数のまま、軸ごと）。"""
    if sigma < 0.3:
        return a
    n = 3
    wl = int(math.floor(math.sqrt(12.0 * sigma * sigma / n + 1.0)))
    wl -= (wl % 2 == 0)
    wu = wl + 2
    m = round((12.0 * sigma * sigma - n * wl * wl - 4 * n * wl - 3 * n) / (-4.0 * wl - 4.0))
    out = a.astype(np.float32)
    for i in range(n):
        w = wl if i < m else wu
        out = _box(out, w, 0)
        out = _box(out, w, 1)
    return out

def _box(a, w, axis):
    r = w // 2
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r + 1, r)
    p = np.pad(a, pad, mode="edge")
    c = np.cumsum(p, axis=axis, dtype=np.float64)
    hi = np.take(c, range(w, w + a.shape[axis]), axis=axis)
    lo = np.take(c, range(0, a.shape[axis]), axis=axis)
    return ((hi - lo) / w).astype(np.float32)

def sample_bilinear(img, sx, sy):
    h, w = img.shape[:2]
    sx = np.clip(sx, 0, w - 1.001)
    sy = np.clip(sy, 0, h - 1.001)
    x0, y0 = sx.astype(np.int32), sy.astype(np.int32)
    fx, fy = (sx - x0)[..., None], (sy - y0)[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy

def over(dst, color, alpha):
    a = alpha[..., None] if alpha.ndim == dst.ndim - 1 else alpha
    return dst * (1 - a) + np.asarray(color, dtype=np.float32) * a

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# ------------------------------------------------------------------ マスクの描画（4 倍で描いて 2 倍へ縮める）
class Region:
    def __init__(self, x0, y0, w, h):
        self.x0, self.y0, self.w, self.h = x0, y0, w, h
        self.sw, self.sh = int(w * SS), int(h * SS)

    def new(self, mode="L"):
        return Image.new(mode, (self.sw * OS, self.sh * OS), 0 if mode == "L" else (0, 0, 0, 0))

    def P(self, x, y, off=(0.0, 0.0)):
        return ((x + off[0] - self.x0) * SS * OS, (y + off[1] - self.y0) * SS * OS)

    def mask(self, fn, off=(0.0, 0.0)):
        img = self.new("L")
        d = ImageDraw.Draw(img)
        fn(d, lambda x, y: self.P(x, y, off), SS * OS)
        return np.asarray(img.reduce(OS), dtype=np.float32) / 255.0

DIAL = Region(DCX - 310, DCY - 310, 620, 620)
FULL = Region(0, 0, W, H)


def pt(deg, r, off=(0.0, 0.0)):
    a = math.radians(deg)
    return DCX + math.sin(a) * r + off[0], DCY - math.cos(a) * r + off[1]

def circle(d, P, S, cx, cy, r, w=1.0, fill=False):
    x0, y0 = P(cx - r, cy - r)
    x1, y1 = P(cx + r, cy + r)
    if fill:
        d.ellipse([x0, y0, x1, y1], fill=255)
    else:
        d.ellipse([x0, y0, x1, y1], outline=255, width=max(1, int(round(w * S))))

def line(d, P, S, a, b, w=1.0):
    d.line([P(*a), P(*b)], fill=255, width=max(1, int(round(w * S))))


# ------------------------------------------------------------------ 幾何（geo.py v3.1 を写す）
def u_outline(x0, x1, s, top, cy, seg=48):
    R = (x1 - x0) / 2
    r = R - s
    cx = (x0 + x1) / 2
    pts = [(x0, top), (x0, cy)]
    for i in range(1, seg):
        a = math.pi - math.pi * i / seg
        pts.append((cx + R * math.cos(a), cy + R * math.sin(a)))
    pts += [(x1, cy), (x1, top), (x1 - s, top), (x1 - s, cy)]
    for i in range(1, seg):
        a = math.pi * i / seg
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    pts += [(cx - r, cy), (x0 + s, top)]
    return pts

def u_dial_pts(off=(0.0, 0.0)):
    cxm = (UX0 + UX1) / 2
    return [(DCX + (x - cxm) * K + off[0], DCY + (y - UCY) * K + off[1]) for x, y in u_outline(UX0, UX1, UW, UTOP, UCY)]

def needle_poly(deg, length, off=(0.0, 0.0), s0=0.0, s1=1.0):
    """指針の多角形。s0..s1 は根元から先端までの割合（影の帯分割用）。"""
    w0, w1 = NW0 * K * 0.66, NW1 * K * 0.95
    base = PIN_R * K + 1.2
    def at(s):
        d = base + (length - base) * s
        w = w0 + (w1 - w0) * s
        return d, w
    out = []
    for sgn, s in ((-1, s0), (-1, s1), (1, s1), (1, s0)):
        d, w = at(s)
        out.append((sgn * w / 2, -d))
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    return [(DCX + off[0] + x * ca - y * sa, DCY + off[1] + x * sa + y * ca) for x, y in out]

def wordmark_polys(cap_px, ox, oy):
    """UNO ENGINE（キャップ高 cap_px）の多角形の一覧 [(outer, inner|None)]。geo.py のグリフ定義の写し。"""
    CAP, OV, BOLD, LIGHT = 100.0, 1.4, 16.0, 7.0
    Wd = dict(U=70, N=74, O=82, E=54, Nl=70, G=80)
    sc = cap_px / CAP
    polys = []
    def add(pts, inner=None):
        polys.append(([(ox + (x + 0) * sc, oy + (y + 0) * sc) for x, y in pts],
                      None if inner is None else [(ox + x * sc, oy + y * sc) for x, y in inner]))
    def g_N(x, sw, w):
        diag = sw * 0.94
        x0c, x1c = x + sw / 2, x + w - sw / 2
        slope = (x1c - x0c) / CAP
        sin_t = CAP / math.hypot(x1c - x0c, CAP)
        hw = (diag / 2) / sin_t
        y_r = ((x + w - sw) - (x0c + hw)) / slope
        y_l = ((x + sw) - (x0c - hw)) / slope
        return [(x, CAP), (x, 0), (x0c + hw, 0), (x + w - sw, y_r), (x + w - sw, 0), (x + w, 0),
                (x + w, CAP), (x1c - hw, CAP), (x + sw, y_l), (x + sw, CAP)]
    def g_E(x, sw, w):
        mw = w * 0.88
        return [(x, 0), (x + w, 0), (x + w, sw), (x + sw, sw), (x + sw, CAP / 2 - sw / 2), (x + mw, CAP / 2 - sw / 2),
                (x + mw, CAP / 2 + sw / 2), (x + sw, CAP / 2 + sw / 2), (x + sw, CAP - sw), (x + w, CAP - sw), (x + w, CAP), (x, CAP)]
    def g_O(x, sw, w, seg=64):
        cx, cy = x + w / 2, CAP / 2
        Rx, Ry = w / 2, CAP / 2 + OV
        rx, ry = Rx - sw * 1.06, Ry - sw * 0.92
        o = [(cx + Rx * math.cos(2 * math.pi * i / seg), cy + Ry * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
        i_ = [(cx + rx * math.cos(2 * math.pi * i / seg), cy + ry * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
        return o, i_
    def stroke_to_fill(pts, h):
        n = len(pts)
        def nrm(a, b):
            dx, dy = b[0] - a[0], b[1] - a[1]
            L = math.hypot(dx, dy)
            return (-dy / L, dx / L)
        left, right = [], []
        for i in range(n):
            if i == 0:
                nx, ny = nrm(pts[0], pts[1]); k = 1.0
            elif i == n - 1:
                nx, ny = nrm(pts[-2], pts[-1]); k = 1.0
            else:
                n0 = nrm(pts[i - 1], pts[i]); n1 = nrm(pts[i], pts[i + 1])
                mx, my = n0[0] + n1[0], n0[1] + n1[1]
                L = math.hypot(mx, my)
                nx, ny = mx / L, my / L
                k = 1.0 / max(0.2, (nx * n0[0] + ny * n0[1]))
            left.append((pts[i][0] + nx * h * k, pts[i][1] + ny * h * k))
            right.append((pts[i][0] - nx * h * k, pts[i][1] - ny * h * k))
        return left + right[::-1]
    def g_G(x, sw, w, seg=72):
        cx, cy = x + w / 2, CAP / 2
        rx = w / 2 - sw * 0.53
        ry = CAP / 2 + OV - sw / 2
        bar_y = CAP * 0.56
        a_end = 360 + math.degrees(math.asin((cy - bar_y) / ry))
        a0 = 38.0
        cl = []
        for i in range(seg + 1):
            a = math.radians(a0 + (a_end - a0) * i / seg)
            cl.append((cx + rx * math.cos(a), cy - ry * math.sin(a)))
        end = cl[-1]
        cl = [p for p in cl[:-1] if math.hypot(p[0] - end[0], p[1] - end[1]) >= sw] + [end]
        cl.append((cx + w * 0.03, bar_y))
        return stroke_to_fill(cl, sw / 2)
    x = 0.0
    add(u_outline(x, x + Wd["U"], BOLD, 0, CAP + OV - Wd["U"] / 2, seg=40)); x += Wd["U"] + 30
    add(g_N(x, BOLD, Wd["N"])); x += Wd["N"] + 27
    o, i_ = g_O(x, BOLD, Wd["O"]); add(o, i_); x += Wd["O"] + 88
    add(g_E(x, LIGHT, Wd["E"])); x += Wd["E"] + 34
    add(g_N(x, LIGHT, Wd["Nl"])); x += Wd["Nl"] + 31
    add(g_G(x, LIGHT, Wd["G"])); x += Wd["G"] + 33
    add([(x, 0), (x + LIGHT, 0), (x + LIGHT, CAP), (x, CAP)]); x += LIGHT + 34
    add(g_N(x, LIGHT, Wd["Nl"])); x += Wd["Nl"] + 34
    add(g_E(x, LIGHT, Wd["E"])); x += Wd["E"]
    return polys


# ------------------------------------------------------------------ 文字（余白つき）
def text_width(txt, kind, px, spacing=0.0):
    f = font(kind, px * SS * OS)
    return sum(f.getlength(c) for c in txt) / (SS * OS) + spacing * px * max(0, len(txt) - 1)

def draw_text(d, P, S, x, y, txt, kind, px, spacing=0.0, anchor="l", fill=255):
    """y は行の上端（CSS の top に近い）。spacing は em。"""
    f = font(kind, px * S)
    wtot = text_width(txt, kind, px, spacing)
    if anchor == "r":
        x -= wtot
    elif anchor == "c":
        x -= wtot / 2
    cx = x
    for c in txt:
        d.text(P(cx, y), c, font=f, fill=fill, anchor="la")
        cx += f.getlength(c) / S + spacing * px


# ------------------------------------------------------------------ 背景と UI（標準・リッチ共通）
def render_background():
    ys, xs = np.mgrid[0:H * SS, 0:W * SS].astype(np.float32)
    xs, ys = (xs + 0.5) / SS, (ys + 0.5) / SS
    t2 = np.sqrt(((xs - 800) / 1500) ** 2 + ((ys - 420) / 900) ** 2)
    base = BG0 + (GROOVE - BG0) * np.clip(t2 / 0.78, 0, 1)[..., None]
    t1 = np.sqrt(((xs - DCX) / 640) ** 2 + ((ys - DCY) / 560) ** 2)
    a1 = np.clip(1 - t1 / 0.72, 0, 1)
    img = over(base, BG2, a1)
    # 方眼（inset 28、40px、不透明度 0.55）
    grid = np.zeros((H * SS, W * SS), np.float32)
    for k in range(0, 40):
        x = 28 + 40 * k
        if x < W - 28:
            grid[28 * SS:(H - 28) * SS, x * SS:x * SS + SS] = 1
    for k in range(0, 23):
        y = 28 + 40 * k
        if y < H - 28:
            grid[y * SS:y * SS + SS, 28 * SS:(W - 28) * SS] = 1
    img = over(img, T1, grid * GRID_MINOR_A * 0.55)
    frame = FULL.mask(lambda d, P, S: d.rectangle([*P(28, 28), *P(W - 28 - 0.01, H - 28 - 0.01)], outline=255, width=int(S)))
    img = over(img, BORDER, frame)
    return img

def render_ui(img, kf):
    """表題欄・版情報・右の列・トンボ。標準もリッチも同じ。戻り値: 図番セルの矩形（刻印用）。"""
    layers = []   # (mask, color)
    def tomboes(d, P, S):
        L = 24
        for (x, y, sx, sy) in [(20, 20, 1, 1), (W - 20, 20, -1, 1), (20, H - 20, 1, -1), (W - 20, H - 20, -1, -1)]:
            d.line([P(x + sx * L, y), P(x, y), P(x, y + sy * L)], fill=255, width=int(2 * S), joint="curve")
        for (x0, y0, x1, y1) in [(W / 2, 20, W / 2, 34), (W / 2, H - 34, W / 2, H - 20), (20, H / 2, 34, H / 2), (W - 34, H / 2, W - 20, H / 2)]:
            pass
    layers.append((FULL.mask(tomboes), ACCENT, 1.0))
    def tomboes_minor(d, P, S):
        for (x0, y0, x1, y1) in [(W / 2, 20, W / 2, 34), (W / 2, H - 34, W / 2, H - 20), (20, H / 2, 34, H / 2), (W - 34, H / 2, W - 20, H / 2)]:
            d.line([P(x0, y0), P(x1, y1)], fill=255, width=int(S))
    layers.append((FULL.mask(tomboes_minor), T4, 0.55))

    def text_layer(items):
        def fn(d, P, S):
            for it in items:
                draw_text(d, P, S, *it[0], **it[1])
        return FULL.mask(fn)

    # 上端
    top = [((52, 47), dict(txt="起動シーケンス", kind="ja", px=12, spacing=0.04)),]
    # draw_text の引数順: x,y,txt,... → 辞書で渡す
    def T(x, y, txt, kind, px, spacing=0.0, anchor="l"):
        return ((x, y), dict(txt=txt, kind=kind, px=px, spacing=spacing, anchor=anchor))
    def draw_items(items):
        def fn(d, P, S):
            for (x, y), kw in items:
                draw_text(d, P, S, x, y, kw["txt"], kw["kind"], kw["px"], kw["spacing"], kw["anchor"])
        return FULL.mask(fn)
    img = over(img, T3, draw_items([T(52, 46, "起動シーケンス", "ja", 12, .04)]))
    img = over(img, T2, draw_items([T(150, 46, kf["seq"], "monob", 12, .08)]))
    sep = FULL.mask(lambda d, P, S: d.line([P(221, 54), P(239, 54)], fill=255, width=int(S)))
    img = over(img, BORDER_STRONG, sep)
    img = over(img, T3, draw_items([T(250, 46, "Sunken Courtyard", "ja", 12, .04)]))

    # 右の列
    eb = FULL.mask(lambda d, P, S: d.line([P(900, 251), P(928, 251)], fill=255, width=int(S)))
    img = over(img, T3, eb)
    img = over(img, T3, draw_items([T(942, 243, "DX12 リアルタイムエンジン · エディタ", "ja", 13, .12)]))
    wm = wordmark_polys(60.0, 901.0, 281.2)
    def fn_wm(d, P, S):
        for outer, inner in wm:
            d.polygon([P(x, y) for x, y in outer], fill=255)
            if inner:
                d.polygon([P(x, y) for x, y in inner], fill=0)
    img = over(img, T1, FULL.mask(fn_wm))
    img = over(img, T2, draw_items([T(900, 371, "精度で、世界をつくる。", "ja", 17, .34)]))

    # 進捗（ロード完了: 100.0 %）
    img = over(img, T1, draw_items([T(900, 478, "100.0", "mono", 58, -.02)]))
    wpct = text_width("100.0", "mono", 58, -.02)
    img = over(img, T3, draw_items([T(900 + wpct + 6, 506, "%", "mono", 22)]))
    img = over(img, T1, draw_items([T(1500, 506, "校正完了", "ja", 14, 0, "r")]))
    img = over(img, T3, draw_items([T(1500, 530, "すべての工程が完了しました", "ja", 12, 0, "r")]))
    # 工程 5 本（すべて完了）
    xs = [900 + i * 122 for i in range(5)]
    seg = FULL.mask(lambda d, P, S: [d.line([P(x, 586.5), P(x + 112, 586.5)], fill=255, width=int(S)) for x in xs])
    img = over(img, BORDER_STRONG * 0.5 + T4 * 0.5, seg)
    its = []
    for i, (no, name, ms) in enumerate(STEPS):
        x = xs[i]
        its += [T(x, 596, no, "mono", 11.5), T(x + 112, 596, f"{ms / 1000:.2f} s", "mono", 11.5, 0, "r"), T(x, 614, name, "ja", 13)]
    img = over(img, T3, draw_items(its))

    # 左下の版情報
    ver = [T(52, 794, "v0.9.3", "monob", 12, .04), T(52 + text_width("v0.9.3", "monob", 12, .04) + 14, 794, "build 2611.26 · x64 · Development", "mono", 12, .04),
           T(52, 815, "RTX 5060 · 8 GB · Driver 581.42", "mono", 12, .04), T(52, 836, "前回 3.78 s · 推定残り 0.00 s", "ja", 12, .04)]
    img = over(img, T3, draw_items(ver))
    # 表題欄
    cells = [("図番", FIGURE_NO, 1203, 78, "mono"), ("尺度", "1 : 1", 1281, 62, "mono"), ("公差", "±0.01", 1343, 64, "mono"),
             ("工程", kf["seq"], 1407, 77, "mono"), ("状態", kf["status"], 1484, 64, "ja")]
    # セルの幅を内容から決める
    xx = 1203
    cellx = []
    for lab, val, _, _, kind in cells:
        w = max(text_width(lab, "ja", 11), text_width(val, kind if kind == "ja" else "monob", 12)) + 26
        cellx.append((xx, w))
        xx += w
    right = W - 52
    shift = right - xx
    cellx = [(x + shift, w) for x, w in cellx]
    box = FULL.mask(lambda d, P, S: d.rectangle([*P(cellx[0][0], 801), *P(right, 853)], outline=255, width=int(S)))
    img = over(img, BORDER_STRONG, box)
    div = FULL.mask(lambda d, P, S: [d.line([P(x, 801), P(x, 853)], fill=255, width=int(S)) for x, _ in cellx[1:]])
    img = over(img, BORDER, div)
    its1, its2 = [], []
    for (lab, val, *_rest, kind), (x, w) in zip(cells, cellx):
        its1.append(T(x + 13, 809, lab, "ja", 11))
        its2.append(T(x + 13, 829, val, "ja" if kind == "ja" else "monob", 12))
    img = over(img, T3, draw_items(its1))
    img = over(img, T1, draw_items(its2))
    for m, c, a in layers:
        img = over(img, c, m * a)
    return img, cellx[0], (801, 853)


# ------------------------------------------------------------------ 文字盤の要素（マスク）
def dial_masks(kf, off_plate, off_mark, off_needle):
    R = DIAL
    m = {}
    m["bezel_outer"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_BEZEL - 1.0, 2.0), off_plate)
    m["bezel_inner"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_BEZEL - 7.0, 1.0), off_plate)
    m["disc_fill"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_DISC, fill=True), off_plate)
    m["disc_line"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_DISC, 1.0), off_plate)
    m["arc_ring"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_ARC, 1.0), off_plate)
    m["progress"] = R.mask(lambda d, P, S: circle(d, P, S, DCX, DCY, R_ARC, 1.5), off_plate)   # 100%: 全周
    def ticks(sel):
        def fn(d, P, S):
            for i in range(100):
                major, mid = i % 10 == 0, i % 5 == 0
                if sel != ("major" if major else ("mid" if mid else "minor")):
                    continue
                ln = 18 if major else (12 if mid else 7)
                deg = i * 3.6
                d.line([P(*pt(deg, R_TICK_OUT)), P(*pt(deg, R_TICK_OUT - ln))], fill=255, width=int((2 if major else 1) * S))
        return fn
    for sel in ("major", "mid", "minor"):
        m["tick_" + sel] = R.mask(ticks(sel), off_plate)
    def labels(d, P, S):
        for i in range(10):
            x, y = pt(i * 36, R_LABEL)
            if i == 0:
                draw_text(d, P, S, x - 7, y - 6.5, "100", "monob", 11, 0, "r")
                d.line([P(x, y - 6), P(x, y + 6)], fill=int(255 * .8), width=int(S))
                draw_text(d, P, S, x + 7, y - 6.5, "000", "monob", 11, 0, "l")
            else:
                draw_text(d, P, S, x, y - 6.5, f"{i * 10:03d}", "monob", 11, 0, "c")
    m["labels"] = R.mask(labels, off_plate)
    def cross(d, P, S):
        pat = [12, 3, 2, 3]
        def dashed(a, b):
            ax, ay = a; bx, by = b
            L = math.hypot(bx - ax, by - ay)
            ux, uy = (bx - ax) / L, (by - ay) / L
            s, k = 0.0, 0
            while s < L:
                e = min(L, s + pat[k % 4])
                if k % 2 == 0:
                    d.line([P(ax + ux * s, ay + uy * s), P(ax + ux * e, ay + uy * e)], fill=255, width=int(S))
                s = e + 0.0
                k += 1
        R_ = R_BEZEL
        dashed((DCX - R_ - 18, DCY), (DCX - R_ + 26, DCY))
        dashed((DCX + R_ - 26, DCY), (DCX + R_ + 18, DCY))
        dashed((DCX, DCY + R_ - 26), (DCX, DCY + R_ + 18))
    m["cross"] = R.mask(cross, off_plate)
    m["triangle"] = R.mask(lambda d, P, S: d.polygon([P(DCX - 7, DCY - R_BEZEL - 14), P(DCX + 7, DCY - R_BEZEL - 14), P(DCX, DCY - R_BEZEL - 3)], fill=255), off_plate)
    m["umark"] = R.mask(lambda d, P, S: d.polygon([P(x, y) for x, y in u_dial_pts(off_mark)], fill=255))
    L = R_TICK_OUT - 6
    m["needle"] = R.mask(lambda d, P, S: d.polygon([P(x, y) for x, y in needle_poly(0.0, L, off_needle)], fill=255))
    m["needle_halo"] = R.mask(lambda d, P, S: (d.polygon([P(x, y) for x, y in needle_poly(0.0, L, off_needle)], fill=255, outline=255, width=int(3 * S))))
    def hub(d, P, S):
        circle(d, P, S, DCX + off_mark[0], DCY + off_mark[1], HUB_R * K, fill=True)
    m["hub"] = R.mask(hub)
    m["hub_halo"] = R.mask(lambda d, P, S: circle(d, P, S, DCX + off_mark[0], DCY + off_mark[1], HUB_R * K + 1.5, fill=True))
    m["pin_hole"] = R.mask(lambda d, P, S: circle(d, P, S, DCX + off_mark[0], DCY + off_mark[1], PIN_R * K, fill=True))
    ghosts = []
    if kf["ghosts"]:
        for dd, a in ((2.5, .24), (5.0, .10)):
            ghosts.append((R.mask(lambda d, P, S, dd=dd: d.line([P(DCX + off_needle[0], DCY + off_needle[1]), P(*pt(-dd, L, off_needle))], fill=255, width=int(S))), a))
    m["ghosts"] = ghosts
    # ロック表示（小さな書き込み）と刻印
    def lock_label(d, P, S):
        draw_text(d, P, S, DCX + 16, DCY - R_BEZEL - 19, "LOCK", "monob", 10, .12, "l")
    m["lock_label"] = R.mask(lock_label, off_plate)
    def stamp(d, P, S):
        cy = DCY + 163
        draw_text(d, P, S, DCX + 10, cy - 7, FIGURE_NO, "monob", 12, .10, "c")
        w = text_width(FIGURE_NO, "monob", 12, .10) + 26
        d.rectangle([*P(DCX - w / 2 + 10, cy - 11), *P(DCX + w / 2 + 10, cy + 11)], outline=255, width=int(1.5 * S))
        x0 = DCX - w / 2 - 12
        circle(d, P, S, x0 + 2, cy, 6.0, 1.2)
        d.line([P(x0 - 6, cy), P(x0 + 10, cy)], fill=255, width=int(S))
        d.line([P(x0 + 2, cy - 8), P(x0 + 2, cy + 8)], fill=255, width=int(S))
    m["stamp"] = R.mask(stamp, off_plate)
    return m


# ------------------------------------------------------------------ 標準の文字盤
def compose_standard_dial(img, m, kf):
    reg = DIAL
    sx, sy = int(reg.x0 * SS), int(reg.y0 * SS)
    crop = img[sy:sy + reg.sh, sx:sx + reg.sw]
    al = 1.0
    ba = 1.0
    crop = over(crop, BORDER_STRONG, m["bezel_outer"])
    crop = over(crop, BORDER, m["bezel_inner"])
    crop = over(crop, BG0, m["disc_fill"])
    crop = over(crop, BORDER, m["disc_line"])
    crop = over(crop, BORDER, m["arc_ring"])
    crop = over(crop, T3, m["progress"] * 0.85)
    a = kf["aligned"] * 0.18
    crop = over(crop, T2, m["tick_major"])
    crop = over(crop, T3, m["tick_mid"] * (0.75 + a))
    crop = over(crop, T4, m["tick_minor"] * (0.45 + a))
    crop = over(crop, T3, m["labels"])
    crop = over(crop, T4, m["cross"] * 0.6)
    crop = over(crop, ACCENT, m["triangle"])
    if kf["lock_label"]:
        crop = over(crop, T2, m["lock_label"])
    if kf["stamp"]:
        crop = over(crop, T2, m["stamp"] * 0.9)
    crop = over(crop, T1, m["umark"])
    for g, a_ in m["ghosts"]:
        crop = over(crop, ACCENT, g * a_)
    crop = over(crop, BG0, m["needle_halo"])
    crop = over(crop, ACCENT, m["needle"])
    crop = over(crop, BG0, m["hub_halo"])
    hub_ring = np.clip(m["hub"] - m["pin_hole"], 0, 1)
    crop = over(crop, ACCENT, hub_ring)
    img[sy:sy + reg.sh, sx:sx + reg.sw] = crop
    return img


# ------------------------------------------------------------------ リッチ: 質感の上乗せ層
def _hash_noise_1d(n, seed, smooth=1):
    rng = np.random.default_rng(seed)
    a = rng.random(n).astype(np.float32)
    for _ in range(smooth):
        a = (a + np.roll(a, 1) + np.roll(a, -1)) / 3
    a = (a - a.min()) / (a.max() - a.min() + 1e-6)
    return a

def bevel_light(mask, sigma, engraved=True):
    """マスクを高さ場とみて、縁の明暗を返す（lit, shade）。engraved なら刻印（右下の壁が光る）。"""
    h = gblur(mask, sigma)
    gy, gx = np.gradient(h)
    s = (gx * LIGHT_XY[0] + gy * LIGHT_XY[1])
    peak = max(1e-6, float(np.abs(s).max()))
    s = s / peak
    if engraved:
        s = -s
    return np.clip(-s, 0, 1), np.clip(s, 0, 1)

def compose_rich_dial(img, m, kf, m_shadow):
    reg = DIAL
    sx, sy = int(reg.x0 * SS), int(reg.y0 * SS)
    ys, xs = np.mgrid[0:reg.sh, 0:reg.sw].astype(np.float32)
    px_ = xs / SS + reg.x0 + 0.5 / SS
    py_ = ys / SS + reg.y0 + 0.5 / SS
    dx, dy = px_ - DCX, py_ - DCY
    r = np.sqrt(dx * dx + dy * dy)
    phi = (np.degrees(np.arctan2(dx, -dy)) + 360.0) % 360.0        # 真上 0°、時計回り
    crop = img[sy:sy + reg.sh, sx:sx + reg.sw].copy()

    # ---- 1. 台座（プレート）: 外周に向かって少し暗くなる旋盤目の面
    plate_r = R_BEZEL - 7.0
    inside = (r < plate_r).astype(np.float32)
    inside = gblur(inside, SS * 0.4)
    grain = _hash_noise_1d(4096, 11, smooth=1)[np.clip((r * 6).astype(np.int32), 0, 4095)]
    plate = BG2 + (BG1 - BG2) * smoothstep(60, plate_r, r)[..., None]
    plate = plate * (0.965 + 0.07 * grain[..., None])
    # 旋盤目に載る薄い異方性の面ハイライト（広く、弱い）
    lob = _lobe(phi, kf["lobe"], 38.0)
    plate = plate + (T2 * 0.035) * (lob * smoothstep(40, plate_r, r))[..., None]
    crop = over(crop, plate, inside)

    # ---- 2. 中央のカップ（凹み）: 縁に内側の影、上側が暗く下側が明るい
    cup = m["disc_fill"]
    cup_b = gblur(cup, SS * 2.2)
    gy, gx = np.gradient(cup_b)
    sh = np.clip(-(gx * LIGHT_XY[0] + gy * LIGHT_XY[1]) * (SS * 2.2) * 2.2, -1, 1)   # 縁の内側: +が光側
    crop = over(crop, BG0, cup)
    crop = over(crop, np.zeros(3, np.float32), np.clip(sh, 0, 1) * 0.0 + cup * np.clip(-sh, 0, 1) * 0.6)
    crop = over(crop, T1, cup * np.clip(sh, 0, 1) * 0.10)
    crop = over(crop, BORDER, m["disc_line"])

    # ---- 3. 刻印: 目盛り・数字・リング（刻み線の色は標準と同じ。縁の明暗を足す）
    a = kf["aligned"] * 0.18
    ink = np.zeros(m["tick_major"].shape, np.float32)
    ink = np.maximum(ink, m["tick_major"])
    ink = np.maximum(ink, m["tick_mid"] * 0.8)
    ink = np.maximum(ink, m["tick_minor"] * 0.5)
    ink = np.maximum(ink, m["labels"] * 0.85)
    ink = np.maximum(ink, m["cross"] * 0.6)
    ink = np.maximum(ink, m["arc_ring"] * 0.7)
    if kf["stamp"]:
        ink = np.maximum(ink, m["stamp"])
    # 刻印の溝は下地より少し暗くし、その上に標準と同じ色を薄く塗る
    groove = gblur(ink, SS * 0.55)
    crop = over(crop, np.zeros(3, np.float32), groove * 0.55)
    crop = over(crop, T2, m["tick_major"] * 0.92)
    crop = over(crop, T3, m["tick_mid"] * (0.70 + a))
    crop = over(crop, T4, m["tick_minor"] * (0.42 + a))
    crop = over(crop, T3, m["labels"] * 0.92)
    crop = over(crop, T4, m["cross"] * 0.55)
    crop = over(crop, BORDER_STRONG, m["arc_ring"] * 0.9)
    crop = over(crop, T3, m["progress"] * 0.80)
    if kf["stamp"]:
        crop = over(crop, T1, m["stamp"] * 0.88)
    lit, shd = bevel_light(ink, SS * 0.7, engraved=True)
    crop = over(crop, T1, lit * 0.32 * np.clip(ink * 3, 0, 1) + lit * 0.10)
    crop = over(crop, np.zeros(3, np.float32), shd * 0.45)
    if kf["lock_label"]:
        crop = over(crop, T2, m["lock_label"])

    # ---- 4. ベゼルのヘアライン（円周方向に研磨。異方性ハイライトが lobe の向きに出る）
    band = np.clip(smoothstep(R_BEZEL - 7.2, R_BEZEL - 6.6, r) * (1 - smoothstep(R_BEZEL + 0.6, R_BEZEL + 1.2, r)), 0, 1)
    hair = _hash_noise_1d(8192, 5, smooth=0)[np.clip(((r - (R_BEZEL - 8)) * 7.0 * SS / 2).astype(np.int32) % 8192, 0, 8191)]
    hair2 = _hash_noise_1d(8192, 6, smooth=1)[np.clip(((r - (R_BEZEL - 8)) * 2.3 * SS / 2).astype(np.int32) % 8192, 0, 8191)]
    streak = 0.55 * hair + 0.45 * hair2
    lobe = _lobe(phi, kf["lobe"], 20.0) + 0.55 * _lobe(phi, kf["lobe"] + 180.0, 20.0)
    lobe += 0.22 * (_lobe(phi, kf["lobe"] + 90.0, 26.0) + _lobe(phi, kf["lobe"] - 90.0, 26.0))
    across = np.clip((r - (R_BEZEL - 7.0)) / 7.0, 0, 1)                   # 帯の内→外
    prof = 0.65 + 0.35 * np.sin(np.pi * across)                            # 中央が少し明るい（丸み）
    lum = 0.16 + 0.10 * (streak - 0.5) + 0.62 * lobe * (0.45 + 0.55 * streak) * prof
    metal = BG3 + (T2 * 0.92 - BG3) * np.clip(lum, 0, 1)[..., None]
    crop = over(crop, metal, band)
    crop = over(crop, BORDER_STRONG, m["bezel_outer"] * 0.55)             # 外縁の暗線
    crop = over(crop, np.zeros(3, np.float32), m["bezel_inner"] * 0.8)    # 内縁の溝
    crop = over(crop, T1, np.clip(m["bezel_outer"] * (0.20 + 0.45 * _lobe(phi, kf["lobe"], 40.0)), 0, 1) * band * 0.0 +
                m["bezel_outer"] * (0.12 + 0.50 * _lobe(phi, kf["lobe"], 40.0)))   # 外縁のきらめき

    # ---- 5. 中央の U（層 mark。縁に上側の面ハイライト、脚の影）
    u = m["umark"]
    u_sh = gblur(np.roll(np.roll(u, int(round(1.2 * SS)), axis=0), int(round(1.0 * SS)), axis=1), SS * 1.1)
    crop = over(crop, np.zeros(3, np.float32), u_sh * 0.55 * (1 - u))
    crop = over(crop, T1, u)
    ulit, ushd = bevel_light(u, SS * 0.8, engraved=False)
    crop = over(crop, np.ones(3, np.float32), ulit * 0.30 * u)
    crop = over(crop, np.zeros(3, np.float32), ushd * 0.20 * u)

    # ---- 6. 指針の影（高さで半影が広がる）と指針
    crop = over(crop, np.zeros(3, np.float32), np.clip(m_shadow, 0, 1) * 0.85 * (1 - m["needle"]))
    for g, a_ in m["ghosts"]:
        crop = over(crop, ACCENT, g * a_)
    crop = over(crop, BG0, m["needle_halo"] * 0.9)
    crop = over(crop, ACCENT, m["needle"])
    nlit, nshd = bevel_light(m["needle"], SS * 0.7, engraved=False)
    crop = over(crop, ACCENT * 0.55, nshd * 0.55 * m["needle"])      # 影側の縁を落とす（黄のまま、光は足さない）
    crop = over(crop, BG0, m["hub_halo"])
    hub_ring = np.clip(m["hub"] - m["pin_hole"], 0, 1)
    crop = over(crop, ACCENT, hub_ring)
    crop = over(crop, BG0, m["pin_hole"])
    hlit, hshd = bevel_light(m["hub"], SS * 0.7, engraved=False)
    crop = over(crop, ACCENT * 0.6, hshd * 0.5 * hub_ring)

    # ---- 7. カバーガラス（屈折 → 反射 → 埃）
    glass_r = R_BEZEL - 7.0
    # 屈折: 縁に向かって内容が外へわずかにずれる（最大 1.4 px）
    disp = 1.4 * np.clip(r / glass_r, 0, 1.2) ** 4
    ux, uy = dx / np.maximum(r, 1e-3), dy / np.maximum(r, 1e-3)
    sxm = xs - ux * disp * SS
    sym = ys - uy * disp * SS
    warped = sample_bilinear(crop, sxm, sym)
    in_glass = gblur((r < glass_r - 0.3).astype(np.float32), SS * 0.4)[..., None]
    crop = crop * (1 - in_glass) + warped * in_glass
    # 反射: 左上から入る広い斜めの帯（1.5px の視差つき）。弱く、無彩色寄り
    goff = VIEW * PARALLAX["glass"]
    gx_, gy_ = px_ - goff[0], py_ - goff[1]
    n = np.array([0.82, 0.57])
    sd = (gx_ - DCX) * n[0] + (gy_ - DCY) * n[1]                       # 帯の法線方向の距離
    band1 = smoothstep(-170, -120, sd) * (1 - smoothstep(-60, -20, sd))   # 幅広
    band2 = smoothstep(-95, -88, sd) * (1 - smoothstep(-80, -72, sd))     # 細い線
    refl = (0.030 * band1 + 0.040 * band2) * (r < glass_r)
    refl = refl * (0.7 + 0.3 * smoothstep(0, glass_r, r))
    crop = over(crop, T1, refl)
    # 縁のフレネル: ガラスの内縁だけ細く明るい
    fres = smoothstep(glass_r - 5.5, glass_r - 0.8, r) * (1 - smoothstep(glass_r - 0.8, glass_r, r))
    crop = over(crop, T1, fres * (0.05 + 0.10 * _lobe(phi, kf["lobe"], 60.0)))
    # 埃と細い傷: ハイライトに近いほど見える（光を拾う）
    rng = np.random.default_rng(1543)
    dust = np.zeros((reg.sh, reg.sw), np.float32)
    for _ in range(46):
        ang = rng.uniform(0, 2 * math.pi)
        rad = glass_r * math.sqrt(rng.uniform(0.02, 0.96))
        cx_ = DCX + rad * math.cos(ang) - reg.x0
        cy_ = DCY + rad * math.sin(ang) - reg.y0
        rr = rng.uniform(0.35, 0.95) * SS
        x0, x1 = int(max(0, (cx_ * SS) - 4)), int(min(reg.sw, cx_ * SS + 5))
        y0, y1 = int(max(0, (cy_ * SS) - 4)), int(min(reg.sh, cy_ * SS + 5))
        yy, xx_ = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        d2 = (xx_ - cx_ * SS) ** 2 + (yy - cy_ * SS) ** 2
        dust[y0:y1, x0:x1] = np.maximum(dust[y0:y1, x0:x1], np.exp(-d2 / (2 * rr * rr)) * rng.uniform(0.35, 1.0))
    for k in range(5):                       # 細い傷（円弧の一部）
        ang0 = rng.uniform(0, 360)
        rad = glass_r * rng.uniform(0.35, 0.9)
        span = rng.uniform(8, 26)
        arc = np.zeros((reg.sh, reg.sw), np.float32)
        dphi = np.abs(((phi - ang0 + 180) % 360) - 180)
        arc = (np.abs(r - rad) < 0.28) * (dphi < span) * (1 - dphi / span)
        dust = np.maximum(dust, arc.astype(np.float32) * 0.6)
    vis = 0.04 + 0.30 * _lobe(phi, kf["lobe"] + 20.0, 70.0) + 0.14 * np.clip(refl * 25, 0, 1)
    crop = over(crop, T1, dust * np.clip(vis, 0, 0.5) * (r < glass_r))

    # ---- 8. 内側の陰（ガラスの縁にかかる影。立体感の下地）
    ao = smoothstep(glass_r - 26, glass_r, r) * (r < glass_r + 0.5)
    crop = over(crop, np.zeros(3, np.float32), ao * 0.22)

    # ベゼルの外側の落ち影（台座が背景から浮いて見える）
    outside = np.clip((r - R_BEZEL) / 9.0, 0, 1)
    crop = over(crop, np.zeros(3, np.float32), (1 - outside) * (r >= R_BEZEL) * (r < R_BEZEL + 9) * 0.28)
    # 校正三角と刻み・トンボは標準と同じ黄で最前面（視差: plate）
    crop = over(crop, ACCENT, m["triangle"])
    img[sy:sy + reg.sh, sx:sx + reg.sw] = crop
    return img


def _lobe(phi, center, width):
    d = np.abs(((phi - center + 180.0) % 360.0) - 180.0)
    return np.exp(-(d / width) ** 2).astype(np.float32)


def needle_shadow(kf, off_needle):
    """高さで半影が変わる指針の影。根元は低く（シャープ）、先端は高く（広くぼける）。高さは動作中 1.0、ロックで 0.3。"""
    R = DIAL
    L = R_TICK_OUT - 6
    total = np.zeros((R.sh, R.sw), np.float32)
    nseg = 8
    for i in range(nseg):
        s0, s1 = i / nseg, (i + 1) / nseg + 0.02
        sm = (s0 + s1) / 2
        height = (2.0 + 9.0 * sm) * kf["height"] + 0.8
        off = (off_needle[0] + 0.55 * height, off_needle[1] + 0.62 * height)
        seg = R.mask(lambda d, P, S: d.polygon([P(x, y) for x, y in needle_poly(0.0, L, off, s0, min(s1, 1.0))], fill=255))
        sigma = (0.5 + 0.42 * height) * SS
        total = np.maximum(total, gblur(seg, sigma) * (1.0 - 0.015 * height))
    return total


# ------------------------------------------------------------------ 全体
def render(kf_id, rich):
    kf = KEYFRAMES[kf_id]
    pe = (0.0, 0.0)
    if rich:
        off_mark = tuple(VIEW * PARALLAX["mark"])
        off_needle = tuple(VIEW * PARALLAX["needle"])
    else:
        off_mark = off_needle = (0.0, 0.0)
    img = render_background()
    m = dial_masks(kf, pe, off_mark, off_needle)
    # 針は常に真上（0° = 360°）。ゴーストは進行方向の後ろ（反時計側）
    if rich:
        shadow = needle_shadow(kf, off_needle)
        img = compose_rich_dial(img, m, kf, shadow)
    else:
        img = compose_standard_dial(img, m, kf)
    img, fig_cell, (cy0, cy1) = render_ui(img, kf)
    # 2x の浮動小数 → 1x
    out = img.reshape(H, SS, W, SS, 3).mean(axis=(1, 3))
    # 軽いディザ（バンディング避け。決定論）
    rng = np.random.default_rng(7)
    out = np.clip(out + (rng.random(out.shape, dtype=np.float32) - 0.5) / 255.0, 0, 1)
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGB"), img


def render_2x_crop(img2x, box):
    """2x の浮動小数から DIP の矩形を切り出す（拡大の比較用）。"""
    x0, y0, x1, y1 = box
    c = img2x[int(y0 * SS):int(y1 * SS), int(x0 * SS):int(x1 * SS)]
    return Image.fromarray((np.clip(c, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def label(img, text, sub=None):
    d = ImageDraw.Draw(img)
    f = font("ja", 18)
    d.text((14, 10), text, font=f, fill=(230, 226, 218))
    if sub:
        d.text((14, 34), sub, font=font("mono", 13), fill=(180, 175, 166))


def build(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = {}
    crops = {}
    sheet_tiles = []
    for kid in KEYFRAMES:
        pair = {}
        for rich in (False, True):
            im, im2x = render(kid, rich)
            name = f"kf_{kid:04d}_{'rich' if rich else 'standard'}.png"
            im.save(os.path.join(out_dir, name), optimize=True)
            files[name] = im
            pair[rich] = (im, im2x)
            # 拡大: ベゼルの左上（ヘアラインとガラス）と指針の根元〜先端（影の半影）。3 倍
            crops[(kid, rich, "bezel")] = render_2x_crop(im2x, (DCX - 250, DCY - 262, DCX - 90, DCY - 140))
            crops[(kid, rich, "needle")] = render_2x_crop(im2x, (DCX - 30, DCY - 262, DCX + 90, DCY - 100))
        # 左右比較
        cmp_img = Image.new("RGB", (W * 2 + 12, H), (6, 6, 6))
        cmp_img.paste(pair[False][0], (0, 0))
        cmp_img.paste(pair[True][0], (W + 12, 0))
        label(cmp_img.crop((0, 0, W, 60)), "")
        cmp_img.save(os.path.join(out_dir, f"compare_{kid:04d}.png"), optimize=True)
        sheet_tiles.append((kid, pair[False][0], pair[True][0]))
    # 拡大の比較シート（3 キーフレーム × 2 部位、標準 | リッチ）
    tw = 640
    rows = []
    for kid in KEYFRAMES:
        for part in ("bezel", "needle"):
            a = crops[(kid, False, part)]
            b = crops[(kid, True, part)]
            sc = tw / a.width
            a = a.resize((tw, int(a.height * sc)), Image.LANCZOS)
            b = b.resize((tw, int(b.height * sc)), Image.LANCZOS)
            rows.append((kid, part, a, b))
    hh = sum(r[2].height + 8 for r in rows) + 8
    sheet = Image.new("RGB", (tw * 2 + 36, hh + 0), (6, 6, 6))
    y = 8
    for kid, part, a, b in rows:
        sheet.paste(a, (12, y))
        sheet.paste(b, (tw + 24, y))
        d = ImageDraw.Draw(sheet)
        d.text((16, y + 4), f"{kid} ms  {part}  標準", font=font("ja", 14), fill=(230, 226, 218))
        d.text((tw + 28, y + 4), f"{kid} ms  {part}  リッチ", font=font("ja", 14), fill=(230, 226, 218))
        y += a.height + 8
    sheet.save(os.path.join(out_dir, "detail_compare.png"), optimize=True)
    # 全体のコンタクトシート（50%）
    cw, ch = W // 2, H // 2
    cs = Image.new("RGB", (cw * 2 + 36, ch * 3 + 44), (6, 6, 6))
    for i, (kid, a, b) in enumerate(sheet_tiles):
        yy = 12 + i * (ch + 10)
        cs.paste(a.resize((cw, ch), Image.LANCZOS), (12, yy))
        cs.paste(b.resize((cw, ch), Image.LANCZOS), (cw + 24, yy))
        d = ImageDraw.Draw(cs)
        d.text((18, yy + 4), f"標準  {kid} ms", font=font("ja", 14), fill=(230, 226, 218))
        d.text((cw + 30, yy + 4), f"リッチ  {kid} ms", font=font("ja", 14), fill=(230, 226, 218))
    cs.save(os.path.join(out_dir, "contact_sheet.png"), optimize=True)
    return files


# ------------------------------------------------------------------ 検査
def yellow_mask(a):
    r, g, b = a[..., 0].astype(np.int32), a[..., 1].astype(np.int32), a[..., 2].astype(np.int32)
    return (r > 110) & (g > 80) & (r - b > 60) & (r >= g)

def check(out_dir):
    ok = True
    def fail(msg):
        nonlocal ok
        ok = False
        print("FAIL:", msg)
    mags = [math.hypot(*(VIEW * v)) for v in PARALLAX.values()]
    if max(mags) > PARALLAX_MAX_PX + 1e-9:
        fail(f"視差が上限を超える: {max(mags)}")
    for kid in KEYFRAMES:
        paths = {r: os.path.join(out_dir, f"kf_{kid:04d}_{r}.png") for r in ("standard", "rich")}
        ims = {}
        for r, p in paths.items():
            if not os.path.exists(p):
                fail(f"無い: {p}")
                continue
            im = Image.open(p).convert("RGB")
            if im.size != (W, H):
                fail(f"サイズが違う: {p} {im.size}")
            ims[r] = np.asarray(im)
        if len(ims) < 2:
            continue
        diff = np.abs(ims["standard"].astype(np.int32) - ims["rich"].astype(np.int32)).mean()
        ys, ysr = yellow_mask(ims["standard"]).sum(), yellow_mask(ims["rich"]).sum()
        # 差分は文字盤の中だけ（右の列と表題欄は同一）
        right = np.abs(ims["standard"][:, 880:].astype(np.int32) - ims["rich"][:, 880:].astype(np.int32)).max()
        print(f"{kid}: 平均差 {diff:.2f}  黄の画素 標準 {ys} / リッチ {ysr}  右の列の最大差 {right}")
        if diff < 0.8:
            fail(f"{kid}: リッチが標準とほぼ同じ（差が小さい）")
        if ysr > ys * 1.06 + 50:
            fail(f"{kid}: リッチの黄の画素が標準より多い（グローの疑い）")
        if right != 0:
            fail(f"{kid}: 右の列が標準と違う（上乗せは文字盤だけのはず）")
        if ims["rich"].mean() > ims["standard"].mean() * 1.25:
            fail(f"{kid}: リッチが明るすぎる（光らせない）")
    for extra in ("contact_sheet.png", "detail_compare.png"):
        if not os.path.exists(os.path.join(out_dir, extra)):
            fail(f"無い: {extra}")
    print("OK" if ok else "NG")
    return ok


def default_out():
    return os.path.join(os.environ.get("USERPROFILE") or os.path.expanduser("~"), "dx12-scorecard", "w1", "ba13_mock")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=default_out())
    ap.add_argument("--check", action="store_true", help="出力済みの画像を検査する（描画はしない）")
    a = ap.parse_args(argv)
    if a.check:
        return 0 if check(a.out) else 1
    build(a.out)
    print("wrote", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
