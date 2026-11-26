"""ライティング評価(LS-00b)の影の指標。純粋な numpy / scipy。エンジンにも画像ファイルにも依存しない(単体テストは合成画像で)。

入力は gi_metrics と同じ**線形 float RGB(Rec.709・露出 1.0・トーンマップ前)**。領域マスクは lighting_scene.build_masks が
解析的な光線投射で作る(手描き無し)。ここの関数は「ラスタ画像 + PT 画像 + 領域マスク」から数値を返すだけ。

★影の項(shadow term)の定義:
    s = Y / lit_level      Y = 輝度(Rec.709)、lit_level = 領域内の輝度の 90 パーセンタイル(= その領域の「日向の明るさ」)
  画像ごとに自分の lit_level で割る(ラスタと PT の GI や露出の差を影の指標に持ち込まない)。
  影の画素 = s < SHADOW_S(0.5)。半影(penumbra)の画素 = PENUM_LO(0.1) < s < PENUM_HI(0.9)。

指標:
  coverage      PT で影になっている画素のうち、ラスタも影にしている割合(ratio = ラスタの影画素数 / PT の影画素数、iou も)。
                LS-02b「塔の影の画素が PT 比 95% 以上」に使う。
  acne          PT で日向(s ≥ LIT_S 0.9)の画素のうち、ラスタが影(s < ACNE_S 0.75)にしている割合。LS-02a「アクネ 0.1% 未満」。
  edge_width    影の縁の幅(px) = 半影の画素数 / 影の境界の長さ(画素)。縁から band_px 以内だけ数える(床の模様を拾わない)。
                edge_width_error = |幅(ラスタ) - 幅(PT)| / 幅(PT)。LS-02a「縁の幅の誤差が半減」。
  temporal      静止 N フレームの 8bit 表示での画素ごとの最大差(frames_8bit の max - min)。LS-F「1/255 以下」。
  texel_density 影のテクセル密度 [テクセル/px] = 1 画素が覆う距離 / 1 テクセルの大きさ。0.7 未満の距離帯が LS-13(VSM)の着手条件。
                エンジンの ComputeCascades(Application の ApplicationRender.cpp)と同じ式(分割 lambda・スライス錐台の外接球・半径を 1/16 切り上げ)。
"""
from __future__ import annotations

import math

import numpy as np

from .gi_metrics import luminance

SHADOW_S = 0.5
PENUM_LO = 0.1
PENUM_HI = 0.9
LIT_S = 0.9
ACNE_S = 0.75
LIT_PERCENTILE = 90.0
MIN_SHADOW_PIXELS = 200       # これ未満の影は「判定不能」
MIN_LIT_PIXELS = 400


# ── 影の項 ──────────────────────────────────────────────────────────────────

def lit_level(y: np.ndarray, mask: np.ndarray, pct: float = LIT_PERCENTILE) -> float:
    v = y[mask]
    if v.size == 0:
        return 0.0
    return float(np.percentile(v, pct))


def shadow_term(y: np.ndarray, level: float) -> np.ndarray:
    """s = Y / lit_level(0..1.5 に丸める)。lit_level が 0 なら全部 1(= 影の判定が無意味)。"""
    if level <= 1e-9:
        return np.ones_like(y)
    return np.clip(y / level, 0.0, 1.5)


def terms(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray):
    """(s_raster, s_pt, level_raster, level_pt)。"""
    yr, yp = luminance(raster), luminance(pt)
    lr, lp = lit_level(yr, mask), lit_level(yp, mask)
    return shadow_term(yr, lr), shadow_term(yp, lp), lr, lp


# ── coverage / acne ─────────────────────────────────────────────────────────

def coverage(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray) -> dict:
    sr, sp, lr, lp = terms(raster, pt, mask)
    br = (sr < SHADOW_S) & mask
    bp = (sp < SHADOW_S) & mask
    n_p, n_r = int(bp.sum()), int(br.sum())
    inter, union = int((br & bp).sum()), int((br | bp).sum())
    out = {"mask_pixels": int(mask.sum()), "pt_shadow_px": n_p, "raster_shadow_px": n_r,
           "evaluable": n_p >= MIN_SHADOW_PIXELS, "lit_level_raster": lr, "lit_level_pt": lp}
    out["ratio"] = (n_r / n_p) if n_p else None
    out["iou"] = (inter / union) if union else None
    out["missing_px"] = int((bp & ~br).sum())      # PT は影・ラスタは日向
    out["extra_px"] = int((br & ~bp).sum())        # PT は日向・ラスタは影
    out["captured"] = (inter / n_p) if n_p else None
    return out


def acne_rate(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray) -> dict:
    """PT が日向(s ≥ 0.9)の画素のうち、ラスタが影(s < 0.75)にしている割合(0..1)。"""
    sr, sp, _, _ = terms(raster, pt, mask)
    lit = (sp >= LIT_S) & mask
    n = int(lit.sum())
    if n < MIN_LIT_PIXELS:
        return {"evaluable": False, "lit_px": n, "acne_px": 0, "rate": None}
    bad = int(((sr < ACNE_S) & lit).sum())
    return {"evaluable": True, "lit_px": n, "acne_px": bad, "rate": bad / n}


# ── 縁の幅 ──────────────────────────────────────────────────────────────────

def _erode(m: np.ndarray, iters: int = 1) -> np.ndarray:
    from scipy.ndimage import binary_erosion
    return binary_erosion(m, iterations=iters, border_value=0)


def _dilate(m: np.ndarray, iters: int) -> np.ndarray:
    from scipy.ndimage import binary_dilation
    return binary_dilation(m, iterations=iters)


def edge_width(s: np.ndarray, mask: np.ndarray, band_px: int = 12, inner_px: int = 3) -> dict:
    """影の縁の幅[px] = 半影の画素数 / 影の境界の長さ。マスクの外周(inner_px)と、境界から band_px より遠い画素は数えない。"""
    shadow = (s < SHADOW_S) & mask
    if int(shadow.sum()) < MIN_SHADOW_PIXELS:
        return {"evaluable": False, "boundary_px": 0, "penumbra_px": 0, "width_px": None}
    inner = _erode(mask, inner_px)
    boundary = (shadow & ~_erode(shadow)) & inner
    nb = int(boundary.sum())
    if nb == 0:
        return {"evaluable": False, "boundary_px": 0, "penumbra_px": 0, "width_px": None}
    near = _dilate(boundary, band_px) & inner
    penum = (s > PENUM_LO) & (s < PENUM_HI) & near
    npen = int(penum.sum())
    return {"evaluable": True, "boundary_px": nb, "penumbra_px": npen, "width_px": npen / nb}


def edge_width_error(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray, band_px: int = 12) -> dict:
    sr, sp, _, _ = terms(raster, pt, mask)
    wr, wp = edge_width(sr, mask, band_px), edge_width(sp, mask, band_px)
    out = {"raster": wr, "pt": wp, "evaluable": bool(wr["evaluable"] and wp["evaluable"])}
    if out["evaluable"]:
        d = wr["width_px"] - wp["width_px"]
        out["width_diff_px"] = d
        out["error_rel"] = abs(d) / max(wp["width_px"], 1e-6)
    else:
        out["width_diff_px"] = None
        out["error_rel"] = None
    return out


def shadow_stats(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray) -> dict:
    """領域 1 つぶんの影の指標一式(coverage / acne / edge)。"""
    return {"coverage": coverage(raster, pt, mask), "acne": acne_rate(raster, pt, mask), "edge": edge_width_error(raster, pt, mask)}


# ── 時間的な安定 ────────────────────────────────────────────────────────────

def to_8bit(display: np.ndarray) -> np.ndarray:
    """表示参照 0..1 の画像を 8bit(0..255)の整数へ。"""
    return np.clip(np.floor(display * 255.0 + 0.5), 0, 255).astype(np.int16)


def temporal_stats(frames_8bit: np.ndarray, mask: np.ndarray | None = None) -> dict:
    """frames_8bit: [N,H,W,3] の 8bit 表示。画素・チャンネルごとの (max - min) を領域内で集計する。"""
    f = np.asarray(frames_8bit, dtype=np.int16)
    if f.ndim != 4 or f.shape[0] < 2:
        return {"evaluable": False, "frames": int(f.shape[0]) if f.ndim == 4 else 0}
    rng = (f.max(axis=0) - f.min(axis=0)).max(axis=2)       # [H,W]
    sig = f.astype(np.float64).std(axis=0).mean(axis=2)      # [H,W] 画素ごとの時間の標準偏差(チャンネル平均)
    m = np.ones(rng.shape, bool) if mask is None else mask
    n = int(m.sum())
    if n == 0:
        return {"evaluable": False, "frames": int(f.shape[0])}
    r, sg = rng[m], sig[m]
    return {"evaluable": True, "frames": int(f.shape[0]), "pixels": n,
            "max_range": int(r.max()), "p99_range": float(np.percentile(r, 99)),
            "over_1_rate": float((r > 1).mean()), "sigma_mean": float(sg.mean()), "sigma_p99": float(np.percentile(sg, 99))}


# ── テクセル密度(エンジンの CSM の写し) ────────────────────────────────────

def csm_splits(near: float, far: float, n: int = 4, lam: float = 0.7) -> list[float]:
    ratio = far / max(near, 1e-4)
    out = []
    for i in range(n):
        p = (i + 1) / n
        out.append(lam * near * ratio ** p + (1.0 - lam) * (near + (far - near) * p))
    return out


def csm_radii(near: float, far: float, fov_y_deg: float, aspect: float, n: int = 4, lam: float = 0.7) -> list[float]:
    """各カスケードの外接球の半径[m](ComputeCascades と同じ: スライス錐台の 8 隅の平均を中心に、最遠の隅までの距離を 1/16 へ切り上げ)。"""
    th = math.tan(math.radians(fov_y_deg) / 2.0)
    splits = csm_splits(near, far, n, lam)
    radii, prev = [], near
    for f in splits:
        pts = []
        for z in (prev, f):
            hy, hx = z * th, z * th * aspect
            for sy in (-1, 1):
                for sx in (-1, 1):
                    pts.append((sx * hx, sy * hy, z))
        a = np.array(pts, dtype=np.float64)
        c = a.mean(axis=0)
        r = float(np.linalg.norm(a - c, axis=1).max())
        radii.append(math.ceil(r * 16.0) / 16.0)
        prev = f
    return radii


def texel_density(distance: float, near: float = 0.1, far: float = 400.0, fov_y_deg: float = 45.0, aspect: float = 16 / 9,
                  height_px: int = 720, res: int = 2048, n: int = 4, lam: float = 0.7) -> dict:
    """視点からの距離(視線方向の深さ) distance の地面の 1 画素あたりのテクセル数。1 画素が覆う長さ / 1 テクセルの長さ。
    distance が属するカスケードを分割で決める(最後のカスケードより遠ければ最後)。"""
    splits = csm_splits(near, far, n, lam)
    radii = csm_radii(near, far, fov_y_deg, aspect, n, lam)
    ci = next((i for i, s in enumerate(splits) if distance <= s), n - 1)
    texel_m = 2.0 * radii[ci] / res
    px_m = 2.0 * distance * math.tan(math.radians(fov_y_deg) / 2.0) / height_px      # 画面に垂直な面での 1 画素の幅
    return {"distance": distance, "cascade": ci, "splits": splits, "radius": radii[ci], "texel_m": texel_m,
            "pixel_m": px_m, "texels_per_px": px_m / texel_m}


def density_bands(distances: list[float], **kw) -> list[dict]:
    return [texel_density(d, **kw) for d in distances]
