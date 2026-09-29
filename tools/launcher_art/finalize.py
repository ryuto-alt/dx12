#!/usr/bin/env python3
"""render.mjs が撮った raw_*.png を、ランチャーが読む大きさへ整えて assets/editor/launcher/ へ置く。

  python tools/launcher_art/finalize.py --raw <render.mjs の --out> [--dst assets/editor/launcher]

  ・hero.png        1600x900  （背景ヒーロー。16:9）
  ・tmpl_<id>.png   800x500   （テンプレートのカード画像。16:10。中央を切り出し）
Pillow が必要（pip install pillow）。
"""
import argparse
import os
from PIL import Image

CARDS = {"fps": "tmpl_fps.png", "tps": "tmpl_tps.png", "2d": "tmpl_2d.png", "empty": "tmpl_empty.png"}
# 切り出しの追加指定: (左, 上, 右, 下) を 0..1 の割合で（元画像に対して）。既定は全体。
PRE_CROP = {"2d": (0.0, 0.0, 1.0, 0.93)}
# 16:10 へ切るときの水平位置（0=左 0.5=中央 1=右）
FOCUS_X = {"tps": 0.42}


def crop_ratio(im, ratio, fx=0.5, fy=0.5):
    w, h = im.size
    if w / h > ratio:
        nw = int(h * ratio)
        x = int((w - nw) * fx)
        return im.crop((x, 0, x + nw, h))
    nh = int(w / ratio)
    y = int((h - nh) * fy)
    return im.crop((0, y, w, y + nh))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True)
    ap.add_argument("--dst", default=os.path.join(os.path.dirname(__file__), "..", "..", "assets", "editor", "launcher"))
    a = ap.parse_args()
    os.makedirs(a.dst, exist_ok=True)

    hero = os.path.join(a.raw, "raw_hero.png")
    if os.path.exists(hero):
        im = Image.open(hero).convert("RGB")
        im = crop_ratio(im, 16 / 9).resize((1600, 900), Image.LANCZOS)
        im.save(os.path.join(a.dst, "hero.png"), optimize=True)
        print("hero.png")
    for k, name in CARDS.items():
        p = os.path.join(a.raw, "raw_%s.png" % k)
        if not os.path.exists(p):
            continue
        im = Image.open(p).convert("RGB")
        if k in PRE_CROP:
            w, h = im.size
            l, t, r, b = PRE_CROP[k]
            im = im.crop((int(w * l), int(h * t), int(w * r), int(h * b)))
        im = crop_ratio(im, 16 / 10, FOCUS_X.get(k, 0.5)).resize((800, 500), Image.LANCZOS)
        im.save(os.path.join(a.dst, name), optimize=True)
        print(name)


if __name__ == "__main__":
    main()
