"""GI のブロック状ムラ(プローブ格子の滲み)の数値化。

平らな領域(壁・天井・床)の中で、ラスタ/PT の対数比 log2(raster/PT) の局所的なばらつきを測る。
  blotch_std   = マスク内の log2 比(両画像を box ブラーして PT のノイズを落としたもの)の標準偏差(EV)。全体のずれ(平均)は引く
  grad_p95     = ラスタ画像の log2 輝度の勾配の 95 パーセンタイル(EV/px。ブロック境目の急な段差を見る。PT 側の同じ値を併記)
小さいほど PT に近くなめらか。
"""
from __future__ import annotations

import numpy as np
from scipy.ndimage import uniform_filter

from . import gi_metrics as GM


def _logy(img: np.ndarray, blur: int) -> np.ndarray:
    y = GM.luminance(img)
    if blur > 1:
        y = uniform_filter(y, size=blur, mode="nearest")
    return np.log2(y + GM.EV_EPS)


def blotch(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray, blur: int = 9) -> dict:
    lr, lp = _logy(raster, blur), _logy(pt, blur)
    # 境界の混じりを避けるため 2px 縮める(gi_metrics と同じ流儀)
    from scipy.ndimage import binary_erosion
    m = binary_erosion(mask, iterations=4)
    if m.sum() < 400:
        return {"px": int(m.sum()), "blotch_std": None}
    d = (lr - lp)[m]
    gy, gx = np.gradient(lr)
    gp_y, gp_x = np.gradient(lp)
    gr = np.hypot(gx, gy)[m]
    gpt = np.hypot(gp_x, gp_y)[m]
    return {"px": int(m.sum()), "blotch_std": float(np.std(d)), "bias_ev": float(np.mean(d)),
            "grad_p95": float(np.percentile(gr, 95)), "grad_p95_pt": float(np.percentile(gpt, 95))}
