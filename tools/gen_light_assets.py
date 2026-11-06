#!/usr/bin/env python3
"""懐中電灯 / ダウンライト用のクッキー (Light Function) と IES を手続きで作る。

出力: assets/engine/light/
  flashlight_a.png      ... 中心ホットスポット + 反射鏡のリング + 微かな汚れ（標準）
  flashlight_b.png      ... ファセット（分割反射鏡）の縞 + 二重リング。LED 風
  flashlight_dirty.png  ... 古い懐中電灯。強い汚れ・暗い斑・黄ばんだ縁・引っかき傷
  flashlight.ies        ... 狭いビームの縦角プロファイル（ホットスポット + 広いスピル）
  downlight.ies         ... 天井灯（cos^n、90° 以内）

クッキーの規約（shaders/forward/Lighting.hlsli SpotCookieIesFactor）:
  画像の中心 = スポット軸、画像の端 (r=1) = 外側コーンの縁。r>1 の四隅はコーンの外なので使われない。
  値はリニアの乗算率（最大 1.0）。PNG は sRGB 保存（SRV が _SRGB でリニアへ戻す）。
自作（CC0 相当）。乱数は固定シードで再現可能。

使い方:  python tools/gen_light_assets.py
"""
import os
import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "engine", "light")
N = 256


def grid():
    y, x = np.mgrid[0:N, 0:N].astype(np.float32)
    u = (x + 0.5) / N * 2.0 - 1.0
    v = (y + 0.5) / N * 2.0 - 1.0
    r = np.sqrt(u * u + v * v)
    a = np.arctan2(v, u)
    return u, v, r, a


def fbm(seed, octaves=5, base=4):
    """タイル不要の value noise を重ねた汚れ用のノイズ (0..1)。"""
    rng = np.random.default_rng(seed)
    acc = np.zeros((N, N), np.float32)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        g = base * (2 ** o)
        small = rng.random((g + 1, g + 1)).astype(np.float32)
        img = Image.fromarray((small * 255).astype(np.uint8)).resize((N, N), Image.BICUBIC)
        acc += np.asarray(img, np.float32) / 255.0 * amp
        tot += amp
        amp *= 0.5
    return acc / tot


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def save(name, lin):
    lin = np.clip(lin, 0.0, 1.0)
    # リニア -> sRGB
    s = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
    rgb = np.repeat((s * 255 + 0.5).astype(np.uint8)[:, :, None], 3, axis=2)
    Image.fromarray(rgb, "RGB").save(os.path.join(OUT, name))


def save_color(name, lin_rgb):
    lin = np.clip(lin_rgb, 0.0, 1.0)
    s = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
    Image.fromarray((s * 255 + 0.5).astype(np.uint8), "RGB").save(os.path.join(OUT, name))


def flashlight_a():
    u, v, r, a = grid()
    base = 0.50 + 0.50 * np.exp(-(r / 0.30) ** 2)            # 中心ホットスポット
    base *= 1.0 - 0.28 * smooth(0.30, 0.85, r)               # 周辺へ緩やかに落ちる
    ring = 0.55 * np.exp(-((r - 0.80) / 0.045) ** 2)         # 反射鏡の縁のリング
    ring += 0.22 * np.exp(-((r - 0.58) / 0.030) ** 2)        # 内側の弱いリング
    dark = 0.35 * np.exp(-((r - 0.70) / 0.035) ** 2)         # リングの間の暗い溝
    lin = base + ring - dark
    dirt = fbm(11)
    lin *= 0.86 + 0.14 * dirt                                # 微かな汚れ
    lin *= 1.0 - smooth(0.93, 1.0, r)                        # コーンの縁でゼロ
    save("flashlight_a.png", lin / max(lin.max(), 1e-6))


def flashlight_b():
    u, v, r, a = grid()
    seg = 16
    facet = 0.5 + 0.5 * np.cos(a * seg)                      # 分割反射鏡の放射状の縞
    base = 0.42 + 0.58 * np.exp(-(r / 0.26) ** 2)
    base *= 1.0 - 0.20 * smooth(0.25, 0.9, r)
    fac = 1.0 - 0.18 * facet * smooth(0.20, 0.55, r) * (1 - smooth(0.80, 0.95, r))
    ring = 0.42 * np.exp(-((r - 0.84) / 0.035) ** 2) + 0.30 * np.exp(-((r - 0.66) / 0.025) ** 2)
    gap = 0.30 * np.exp(-((r - 0.75) / 0.025) ** 2)
    lin = (base + ring - gap) * fac
    dirt = fbm(23, 4, 3)
    lin *= 0.90 + 0.10 * dirt
    lin *= 1.0 - smooth(0.94, 1.0, r)
    save("flashlight_b.png", lin / max(lin.max(), 1e-6))


def flashlight_dirty():
    u, v, r, a = grid()
    base = 0.30 + 0.70 * np.exp(-(r / 0.34) ** 2)
    base *= 1.0 - 0.45 * smooth(0.25, 0.95, r)
    ring = 0.38 * np.exp(-((r - 0.78) / 0.06) ** 2)
    lin = base + ring
    blotch = fbm(37, 5, 3)
    blotch = smooth(0.35, 0.75, blotch)                      # 低周波の暗い斑
    lin *= 1.0 - 0.55 * blotch
    fine = fbm(41, 6, 16)
    lin *= 0.78 + 0.22 * fine
    # 引っかき傷（細い暗い線）
    rng = np.random.default_rng(5)
    for _ in range(7):
        ang = rng.uniform(0, np.pi)
        off = rng.uniform(-0.7, 0.7)
        d = np.abs(u * np.sin(ang) - v * np.cos(ang) - off)
        lin *= 1.0 - 0.20 * np.exp(-(d / 0.006) ** 2) * (rng.uniform(0.5, 1.0))
    lin *= 1.0 - smooth(0.92, 1.0, r)
    lin = lin / max(lin.max(), 1e-6)
    # 古いガラスの黄ばみ（縁ほど暖色）
    tint = np.stack([np.ones_like(r), 1.0 - 0.10 * smooth(0.2, 0.95, r), 1.0 - 0.30 * smooth(0.2, 0.95, r)], axis=2)
    save_color("flashlight_dirty.png", lin[:, :, None] * tint)


def write_ies(name, header, vert, cd):
    lines = [
        "IESNA:LM-63-2002",
        "[TEST] procedural / tools/gen_light_assets.py",
        "[MANUFAC] dx12 engine (self-made, CC0)",
        "[LUMINAIRE] " + header,
        "[LAMP] LED",
        "TILT=NONE",
        f"1 -1 1 {len(vert)} 1 1 2 0 0 0",
        "1.0 1.0 10.0",
        " ".join(f"{x:g}" for x in vert),
        "0",
        " ".join(f"{x:.1f}" for x in cd),
        "",
    ]
    with open(os.path.join(OUT, name), "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


def ies():
    # 懐中電灯: 狭いホットスポット（半値幅 ~ 10°）+ 広いスピル + 縁の薄いリング。0..180° 全域。
    vert = list(range(0, 181, 2))
    cd = []
    for t in vert:
        hot = np.exp(-(t / 11.0) ** 2)
        spill = np.exp(-(t / 32.0) ** 2)
        ring = 0.10 * np.exp(-((t - 24.0) / 3.0) ** 2)
        side = 0.01 * (1.0 + np.cos(np.radians(t))) * 0.5
        cd.append(2000.0 * (0.62 * hot + 0.28 * spill + ring + side))
    write_ies("flashlight.ies", "Flashlight, narrow beam", vert, cd)
    # 天井灯（ダウンライト）: cos^n。真下が最大、90° で 0、それより上は出ない。
    vert2 = list(range(0, 91, 3))
    cd2 = [900.0 * max(np.cos(np.radians(t)), 0.0) ** 3.2 for t in vert2]
    write_ies("downlight.ies", "Ceiling downlight, cos^3.2", vert2, cd2)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    flashlight_a()
    flashlight_b()
    flashlight_dirty()
    ies()
    print("wrote", os.path.normpath(OUT))
