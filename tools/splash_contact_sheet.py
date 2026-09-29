#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
--splash-preview が書いた PNG 連番（frames.csv 付き）から、連続フレームを並べたコンタクトシート PNG を作る。

  python tools/splash_contact_sheet.py <previewDir> <out.png> [--cols 5] [--phase intro,ready,transition]
                                       [--every 1] [--width 380] [--crop card]

  --phase  出すフェーズ（frames.csv の phase 列。loading_p10 等は 'loading' で前方一致）。既定: すべて
  --every  N 枚に 1 枚だけ使う
  --width  1 コマの幅(px)。既定 380
  --crop   card = 窓の余白（影の逃げ）を残す / tight = カード周辺だけに切る
  各コマの左上に「時刻(ms) フェーズ %」を重ねる。
"""
import argparse
import csv
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--cols", type=int, default=5)
    ap.add_argument("--phase", default="")
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--width", type=int, default=380)
    ap.add_argument("--crop", default="card")
    ap.add_argument("--from-ms", type=int, default=-1)
    ap.add_argument("--to-ms", type=int, default=10**9)
    a = ap.parse_args()

    rows = list(csv.DictReader((a.dir / "frames.csv").open(encoding="utf-8")))
    want = [p for p in a.phase.split(",") if p]
    sel = []
    for r in rows:
        ms = int(round(float(r["t"]) * 1000))
        if ms < a.from_ms or ms > a.to_ms:
            continue
        if want and not any(r["phase"].startswith(p) for p in want):
            continue
        sel.append(r)
    sel = sel[:: max(1, a.every)]
    if not sel:
        print("該当フレームがありません")
        return 1

    try:
        font = ImageFont.truetype("C:/Windows/Fonts/YuGothM.ttc", 13)
    except Exception:
        font = ImageFont.load_default()

    ims = []
    for r in sel:
        im = Image.open(a.dir / r["file"]).convert("RGB")
        if a.crop == "tight":
            w, h = im.size
            m = int(w * 40 / 816)
            im = im.crop((m, m, w - m, h - m))
        s = a.width / im.width
        im = im.resize((a.width, int(im.height * s)), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        label = f"{int(round(float(r['t'])*1000))}ms {r['phase']} {r['percent']}%"
        d.rectangle((0, 0, 7 * len(label) + 10, 20), fill=(0, 0, 0))
        d.text((4, 2), label, fill=(255, 255, 255), font=font)
        ims.append(im)

    cols = max(1, min(a.cols, len(ims)))
    rws = (len(ims) + cols - 1) // cols
    cw, ch = ims[0].size
    sheet = Image.new("RGB", (cols * cw, rws * ch), (20, 20, 22))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * cw, (i // cols) * ch))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.out)
    print(f"{len(ims)} コマ -> {a.out} ({sheet.width}x{sheet.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
