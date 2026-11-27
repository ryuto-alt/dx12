#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
時間方向の画質指標の数値計算（PI-01b / W1-NB4）。tools/bench/temporal_metrics.mjs が撮った PFM の連番を読んで JSON を出す。
エンジンは起動しない（PFM を読むだけ）。numpy だけに依存する（tools/parity の venv で足りる）。実行は必ず -I で:

  tools/parity/.venv/Scripts/python.exe -I tools/bench/temporal_metrics.py --manifest m.json [--out out.json]
  ... -I tools/bench/temporal_metrics.py --compare a.json b.json [--out diff.json]   （2 回の実行の指標の差）

manifest（temporal_metrics.mjs が書く）:
  { "exposure": 1.0,                       # 線形値に掛けてから L/(1+L) で 0..1 に畳む（PFM は露出前・トーンマップ前）
    "phases": {
      "flicker": { "files": [..pfm..], "region": [x0,y0,x1,y1] },                 # 静止カメラ・静止領域の N フレーム
      "ghost":   { "files": [..pfm..], "region": [..], "peakIndex": k, "bgCount": 4 },  # files[k] まで対象が見え、k+1 から消してある
      "ssim":    { "files": [..pfm..], "region": [..], "refCount": 4 }              # 最後の refCount 枚の平均を参照にして、各枚のマスク内 SSIM
    } }
  region は 0..1 の割合の矩形 [x0,y0,x1,y1]。省略なら全面。

指標（すべて region 内。2 回走らせた差は --compare で見る。決定論の受け入れは 1% 以内）:
  flicker.meanStd / p95Std / meanLevel : 各画素の「畳んだ輝度」の時間標準偏差の平均 / 95 パーセンタイル / 領域の平均の明るさ（参考）
  ghost.peakEnergy / energy[k] / ratio[k] / halfLifeFrames / meanRatio : 対象が消えた後の残光エネルギー
        E_k = Σ max(L_k − L_bg − thr, 0)（L は線形輝度、L_bg は最後の bgCount 枚の中央値、thr = 対象のピーク輝度の 1%）、ratio = E_k / E_peak（peak は peakIndex の枚）
  ssim.perFrame / min / mean / firstFrame : 各枚と参照の、マスク内の平均 SSIM（7x7 の一様窓、畳んだ輝度）
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

K1, K2 = 0.01, 0.03
GHOST_THRESHOLD = 0.01   # 残光として数える最小の明るさ（対象のピーク輝度に対する割合）
WIN = 7


def read_pfm(path) -> np.ndarray:
    """PFM → float32 HxWx3（上が先頭の並び。PFM は下から上に書かれているので反転する）。"""
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
        dt = "<f4" if scale < 0 else ">f4"
        data = np.frombuffer(f.read(w * h * ch * 4), dtype=dt)
    if data.size != w * h * ch:
        raise ValueError(f"PFM のサイズが合わない: {path}")
    a = np.flipud(data.reshape(h, w, ch) if ch == 3 else data.reshape(h, w)).astype(np.float32)
    if ch == 1:
        a = np.repeat(a[:, :, None], 3, axis=2)
    return np.nan_to_num(a, nan=0.0, posinf=65504.0, neginf=0.0)


def luma(rgb: np.ndarray) -> np.ndarray:
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]).astype(np.float64)


def squash(lin_luma: np.ndarray, exposure: float) -> np.ndarray:
    """線形輝度 → 0..1（露出を掛けて L/(1+L)）。SSIM とちらつきは表示に近い値で測る。"""
    v = lin_luma * float(exposure)
    return v / (1.0 + v)


def region_mask(shape, region) -> np.ndarray:
    h, w = shape[:2]
    m = np.zeros((h, w), dtype=bool)
    if not region:
        m[:] = True
        return m
    x0, y0, x1, y1 = region
    xs, xe = int(math.floor(x0 * w)), int(math.ceil(x1 * w))
    ys, ye = int(math.floor(y0 * h)), int(math.ceil(y1 * h))
    m[max(ys, 0):min(ye, h), max(xs, 0):min(xe, w)] = True
    if not m.any():
        raise ValueError(f"region が空: {region}")
    return m


def _box(a: np.ndarray, k: int) -> np.ndarray:
    """k x k の平均（縁は有効な範囲だけで割る）。cumsum で O(HW)。"""
    pad = k // 2
    h, w = a.shape
    s = np.pad(a, ((pad + 1, pad), (pad + 1, pad)), mode="constant")
    c = s.cumsum(0).cumsum(1)
    tot = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    ones = np.pad(np.ones_like(a), ((pad + 1, pad), (pad + 1, pad)), mode="constant")
    co = ones.cumsum(0).cumsum(1)
    cnt = co[k:, k:] - co[:-k, k:] - co[k:, :-k] + co[:-k, :-k]
    return tot[:h, :w] / cnt[:h, :w]


def ssim_map(a: np.ndarray, b: np.ndarray, data_range: float = 1.0) -> np.ndarray:
    """同じ大きさの 2 枚（0..data_range）の SSIM マップ。7x7 の一様窓。"""
    c1, c2 = (K1 * data_range) ** 2, (K2 * data_range) ** 2
    mu_a, mu_b = _box(a, WIN), _box(b, WIN)
    saa = _box(a * a, WIN) - mu_a * mu_a
    sbb = _box(b * b, WIN) - mu_b * mu_b
    sab = _box(a * b, WIN) - mu_a * mu_b
    return ((2 * mu_a * mu_b + c1) * (2 * sab + c2)) / ((mu_a * mu_a + mu_b * mu_b + c1) * (saa + sbb + c2))


def _load(files):
    frames = [luma(read_pfm(f)) for f in files]
    shape = frames[0].shape
    for f, fr in zip(files, frames):
        if fr.shape != shape:
            raise ValueError(f"フレームの大きさが揃っていない: {f}")
    return np.stack(frames)


def flicker_metrics(files, region, exposure=1.0) -> dict:
    if len(files) < 3:
        raise ValueError("flicker は 3 枚以上要る")
    lin = _load(files)
    m = region_mask(lin.shape[1:], region)
    t = squash(lin, exposure)[:, m]
    std = t.std(axis=0)
    mean = t.mean(axis=0)
    return {
        "frames": len(files), "pixels": int(m.sum()),
        "meanStd": float(std.mean()), "p95Std": float(np.percentile(std, 95)),
        "meanLevel": float(mean.mean()),
    }


def ghost_metrics(files, region, peak_index, bg_count=4) -> dict:
    n = len(files)
    if not (0 <= peak_index < n - 2):
        raise ValueError(f"peakIndex={peak_index} は不正（枚数 {n}。消えた後が 2 枚以上要る）")
    lin = _load(files)
    m = region_mask(lin.shape[1:], region)
    bg_n = max(1, min(bg_count, n - peak_index - 1))
    bg = np.median(lin[-bg_n:][:, m], axis=0)
    # 対象の最大の明るさの 1% 未満の残りは数えない（TAA のジッタ由来の雑音を「整流」して足すと、残光ゼロでも比が 0.4% ほど出るため）
    thr = GHOST_THRESHOLD * float(np.max(lin[peak_index][m] - bg))
    energy = np.maximum(lin[:, m] - bg[None, :] - thr, 0.0).sum(axis=1)
    peak = float(energy[peak_index])
    if peak <= 0:
        raise ValueError("ピークの枚に対象のエネルギーが無い（region が対象を含んでいない）")
    after = energy[peak_index + 1:]
    ratio = after / peak
    half = next((i + 1 for i, r in enumerate(ratio) if r < 0.5), None)
    return {
        "frames": n, "peakIndex": peak_index, "pixels": int(m.sum()), "peakEnergy": peak,
        "threshold": float(thr), "energy": [float(x) for x in after], "ratio": [float(x) for x in ratio],
        "halfLifeFrames": half, "meanRatio": float(ratio.mean()),
        "bgFrames": bg_n,
    }


def ssim_metrics(files, region, ref_count=4, exposure=1.0) -> dict:
    n = len(files)
    ref_count = max(1, min(ref_count, n - 1))
    lin = _load(files)
    m = region_mask(lin.shape[1:], region)
    t = squash(lin, exposure)
    ref = t[-ref_count:].mean(axis=0)
    per = [float(ssim_map(t[i], ref)[m].mean()) for i in range(n - ref_count)]
    return {"frames": n, "refFrames": ref_count, "pixels": int(m.sum()), "perFrame": per,
            "min": float(min(per)), "mean": float(np.mean(per)), "firstFrame": per[0]}


def run_manifest(man: dict) -> dict:
    exp = float(man.get("exposure", 1.0))
    out = {"version": 1, "exposure": exp}
    ph = man.get("phases", {})
    if "flicker" in ph:
        p = ph["flicker"]
        out["flicker"] = flicker_metrics(p["files"], p.get("region"), exp)
    if "ghost" in ph:
        p = ph["ghost"]
        out["ghost"] = ghost_metrics(p["files"], p.get("region"), int(p["peakIndex"]), int(p.get("bgCount", 4)))
    if "ssim" in ph:
        p = ph["ssim"]
        out["ssim"] = ssim_metrics(p["files"], p.get("region"), int(p.get("refCount", 4)), exp)
    return out


def _scalars(d: dict, prefix="") -> dict:
    """比較に使うスカラー（配列は平均の値だけ。perFrame / energy / ratio の配列は個別に見ない）。"""
    r = {}
    for k, v in d.items():
        if isinstance(v, dict):
            r.update(_scalars(v, f"{prefix}{k}."))
        elif isinstance(v, (int, float)) and not isinstance(v, bool) and k not in ("frames", "pixels", "version", "peakIndex", "bgFrames", "refFrames"):
            r[f"{prefix}{k}"] = float(v)
    return r


# 相対差の分母の下限（値が小さい指標は、小さな絶対差を拡大して見せない）。キーの末尾で引く。
#   ratio 系 = 残光が 1% 未満は「残光なし」で、その中の揺れは決定論の乱れではない / ssim 系 = 0..1 の値なので絶対差そのもの
_FLOORS = {"meanRatio": 0.01, "meanStd": 1e-3, "p95Std": 1e-3, "min": 1.0, "mean": 1.0, "firstFrame": 1.0,
           "halfLifeFrames": 4.0}


def compare(a: dict, b: dict) -> dict:
    """2 回の実行の指標の差。相対差 = |a−b| / max(|a|, |b|, 下限)。受け入れ（決定論）は maxRelDiff ≤ 0.01。"""
    sa, sb = _scalars(a), _scalars(b)
    rel = {}
    for k in sorted(set(sa) & set(sb)):
        floor = _FLOORS.get(k.rsplit(".", 1)[-1], 1e-9)
        if k.startswith("ssim."):
            floor = 1.0
        rel[k] = abs(sa[k] - sb[k]) / max(abs(sa[k]), abs(sb[k]), floor)
    worst = max(rel, key=rel.get) if rel else None
    return {"relDiff": rel, "maxRelDiff": rel[worst] if worst else 0.0, "worst": worst,
            "within1pct": all(v <= 0.01 for v in rel.values())}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="時間方向の画質指標（PFM の連番 → JSON）")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--manifest", help="撮った PFM の一覧と領域（temporal_metrics.mjs が書く）")
    g.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"), help="2 回の結果の差")
    ap.add_argument("--out", help="JSON の出力先（省略で標準出力）")
    args = ap.parse_args(argv)
    if args.manifest:
        res = run_manifest(json.loads(Path(args.manifest).read_text(encoding="utf-8")))
    else:
        res = compare(*(json.loads(Path(p).read_text(encoding="utf-8")) for p in args.compare))
    text = json.dumps(res, ensure_ascii=False, indent=2)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.buffer.write((text + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
