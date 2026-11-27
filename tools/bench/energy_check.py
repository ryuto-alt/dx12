#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
エネルギー保存と放射方向の強度プロファイル（PI-01b / W1-NB4）。ボケ（被写界深度）・ブルーム・回折の「明るさが増減していないか」「形が正しいか」を
線形 HDR の PFM 2 枚（A = 効果 OFF、B = 効果 ON。同じシーン・同じカメラ・同じ露出）から数値で出す。エンジンは起動しない。実行は -I で:

  tools/parity/.venv/Scripts/python.exe -I tools/bench/energy_check.py A.pfm B.pfm
        [--region x0,y0,x1,y1] [--center cx,cy] [--rmax 0.2] [--bins 32] [--exposure 1.0] [--out out.json]

出力（JSON）:
  total.{a,b,ratio}      : 全面（または region）の線形輝度の総和と B/A。エネルギー保存の指標（PI-05: 1.00±0.01）
  channels.ratio         : R/G/B 別の B/A（色が偏っていないか）
  nonFinite.{a,b}        : NaN / Inf の画素数（読み込み時に 0 へ直す前の数）
  profile（--center があるとき）:
     center / rmax / bins : 中心（画像の割合 0..1）・最大半径（画像の短辺に対する割合）・ビンの数
     a / b               : 各ビンの「背景（外周リングの中央値）を引いた輝度」の総和を、全ビンの総和で割ったもの（形の比較用。合計は 1）
     halfRadius.{a,b}    : 総和の 50% が収まる半径（短辺に対する割合）。ボケで広がると B が大きくなる
     peakRadius.{a,b}    : ビンあたりの平均輝度（面積で割った値）が最大の半径。中心が明るい点光源ならほぼ 0、リング状のボケなら縁
     edgeRatio.{a,b}     : 外側 20% のビンのエネルギー ÷ 総和（リング / 折り返しの強さ。PI-05 の「折り返しが 5% 以下」）
     l1                  : Σ|a−b|（形の違い。0 = 同じ形、2 = 重ならない）
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np


def read_pfm(path):
    """PFM → (float32 HxWx3, 非有限の画素数)。上が先頭の並び。"""
    with open(path, "rb") as f:
        hdr = f.readline().strip()
        if hdr not in (b"PF", b"Pf"):
            raise ValueError(f"PFM ではない: {path}")
        ch = 3 if hdr == b"PF" else 1
        dims = f.readline().split()
        while len(dims) < 2:
            dims += f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        scale = float(f.readline().strip())
        data = np.frombuffer(f.read(w * h * ch * 4), dtype="<f4" if scale < 0 else ">f4")
    if data.size != w * h * ch:
        raise ValueError(f"PFM のサイズが合わない: {path}")
    a = np.flipud(data.reshape(h, w, ch) if ch == 3 else data.reshape(h, w)).astype(np.float64)
    if ch == 1:
        a = np.repeat(a[:, :, None], 3, axis=2)
    bad = int((~np.isfinite(a)).any(axis=2).sum())
    return np.nan_to_num(a, nan=0.0, posinf=65504.0, neginf=0.0), bad


def luma(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def _rect(shape, region):
    h, w = shape[:2]
    m = np.zeros((h, w), dtype=bool)
    if not region:
        m[:] = True
        return m
    x0, y0, x1, y1 = region
    m[int(np.floor(y0 * h)):int(np.ceil(y1 * h)), int(np.floor(x0 * w)):int(np.ceil(x1 * w))] = True
    if not m.any():
        raise ValueError(f"region が空: {region}")
    return m


def radial_profile(lum, center, rmax, bins):
    """中心（割合）からの距離（短辺に対する割合）でビンに分け、各ビンの「背景を引いた輝度」の総和を返す。(bins,) と各ビンの面積（画素数）。
    背景 = 外周のリング（0.8·rmax〜rmax）の輝度の中央値。空や床の上の点光源でも、点のボケだけの形が取れる（空の面積が r² で増える影響を除く）。"""
    h, w = lum.shape
    cx, cy = center[0] * w, center[1] * h
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy) / min(w, h)
    ring = (r >= 0.8 * rmax) & (r < rmax)
    bg = float(np.median(lum[ring])) if ring.any() else 0.0
    ex = np.maximum(lum - bg, 0.0)
    idx = np.minimum((r / rmax * bins).astype(int), bins)  # bins = 範囲外
    inside = idx < bins
    s = np.bincount(idx[inside], weights=ex[inside], minlength=bins)[:bins]
    area = np.bincount(idx[inside], minlength=bins)[:bins]
    return s, area


def _half_radius(prof, rmax):
    c = np.cumsum(prof)
    if c[-1] <= 0:
        return None
    i = int(np.searchsorted(c, 0.5 * c[-1]))
    return float((i + 1) / len(prof) * rmax)


def analyze(a_path, b_path, region=None, center=None, rmax=0.2, bins=32, exposure=1.0):
    a_rgb, a_bad = read_pfm(a_path)
    b_rgb, b_bad = read_pfm(b_path)
    if a_rgb.shape != b_rgb.shape:
        raise ValueError(f"大きさが違う: {a_rgb.shape} と {b_rgb.shape}")
    a_rgb, b_rgb = a_rgb * exposure, b_rgb * exposure
    m = _rect(a_rgb.shape, region)
    la, lb = luma(a_rgb), luma(b_rgb)
    ta, tb = float(la[m].sum()), float(lb[m].sum())
    out = {
        "version": 1,
        "size": [int(a_rgb.shape[1]), int(a_rgb.shape[0])],
        "region": region,
        "nonFinite": {"a": a_bad, "b": b_bad},
        "total": {"a": ta, "b": tb, "ratio": tb / ta if ta > 0 else None},
        "channels": {"ratio": [float(b_rgb[..., c][m].sum() / a_rgb[..., c][m].sum()) if a_rgb[..., c][m].sum() > 0 else None for c in range(3)]},
    }
    if center is not None:
        pa, area = radial_profile(la, center, rmax, bins)
        pb, _ = radial_profile(lb, center, rmax, bins)
        sa, sb = float(pa.sum()), float(pb.sum())
        na = pa / sa if sa > 0 else pa
        nb = pb / sb if sb > 0 else pb
        dens_a = np.where(area > 0, pa / np.maximum(area, 1), 0.0)
        dens_b = np.where(area > 0, pb / np.maximum(area, 1), 0.0)
        edge = max(1, int(round(bins * 0.2)))
        out["profile"] = {
            "center": list(center), "rmax": rmax, "bins": bins,
            "a": [float(x) for x in na], "b": [float(x) for x in nb],
            "halfRadius": {"a": _half_radius(pa, rmax), "b": _half_radius(pb, rmax)},
            "peakRadius": {"a": float((int(np.argmax(dens_a)) + 0.5) / bins * rmax), "b": float((int(np.argmax(dens_b)) + 0.5) / bins * rmax)},
            "edgeRatio": {"a": float(pa[-edge:].sum() / pa.sum()) if pa.sum() > 0 else None,
                          "b": float(pb[-edge:].sum() / pb.sum()) if pb.sum() > 0 else None},
            "l1": float(np.abs(na - nb).sum()),
        }
    return out


def _floats(s, n):
    v = [float(x) for x in s.split(",")]
    if len(v) != n:
        raise argparse.ArgumentTypeError(f"{n} 個のカンマ区切りの数値が要る: {s}")
    return v


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="線形 HDR 2 枚のエネルギー比と放射方向プロファイル")
    ap.add_argument("a", help="A（効果 OFF）の PFM")
    ap.add_argument("b", help="B（効果 ON）の PFM")
    ap.add_argument("--region", type=lambda s: _floats(s, 4))
    ap.add_argument("--center", type=lambda s: _floats(s, 2))
    ap.add_argument("--rmax", type=float, default=0.2)
    ap.add_argument("--bins", type=int, default=32)
    ap.add_argument("--exposure", type=float, default=1.0)
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    res = analyze(args.a, args.b, args.region, args.center, args.rmax, args.bins, args.exposure)
    text = json.dumps(res, ensure_ascii=False, indent=2)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.buffer.write((text + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
