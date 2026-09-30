"""比較指標。

主指標 = NVIDIA FLIP(公式 Python 版 flip-evaluator)。LDR-FLIP は表示参照(トーンマップ後)、HDR-FLIP は
リニア HDR(露出範囲のスイープを FLIP 自身が行う)。補助 = SSIM / PSNR / ΔE2000 / 輝度ヒストグラム EMD /
平均・中央輝度の EV 差 / 色相ずれ。

全部「基準(ref)に対して test がどれだけ違うか」。値が大きいほど悪い指標と、小さいほど悪い指標(ssim・psnr)がある。
指標名(出力キー)は gate の しきい値キー(`<metric>_max` / `<metric>_min` / `<metric>_abs_max`)と同じ。

領域別: マスク(bool HxW)を渡すと、その領域の画素だけで集計する。FLIP・SSIM・ΔE は画像全体でマップを作ってから
領域内を平均する(空間フィルタは領域の外を見る。境界付近の画素は隣の領域の影響を受ける)。
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from . import color
from .tonemap import Aligned

ALL_METRICS = ["flip", "flip-hdr", "ssim", "psnr", "de2000", "hist", "lum", "hue"]
DEFAULT_METRICS = ["flip", "ssim", "psnr", "de2000", "hist", "lum", "hue"]   # flip-hdr は両方リニアのとき自動で足される(evaluate_pair)
_ALIAS = {"flip": "flip", "flip-ldr": "flip", "flip_ldr": "flip", "flip-hdr": "flip-hdr", "flip_hdr": "flip-hdr",
          "ssim": "ssim", "psnr": "psnr", "de2000": "de2000", "de": "de2000", "deltae": "de2000",
          "hist": "hist", "emd": "hist", "lum": "lum", "luminance": "lum", "hue": "hue"}

# 大きいほど悪い / 小さいほど悪い(gate の説明とレポートの並びに使う)
HIGHER_IS_BETTER = {"ssim", "psnr"}
LABELS = {
    "flip_ldr_mean": "LDR-FLIP 平均", "flip_ldr_median": "LDR-FLIP 中央値", "flip_ldr_p95": "LDR-FLIP p95",
    "flip_ldr_p99": "LDR-FLIP p99", "flip_ldr_max": "LDR-FLIP 最大",
    "flip_hdr_mean": "HDR-FLIP 平均", "flip_hdr_p95": "HDR-FLIP p95",
    "ssim": "SSIM(輝度)", "psnr": "PSNR [dB]",
    "de2000_mean": "ΔE2000 平均", "de2000_median": "ΔE2000 中央値", "de2000_p95": "ΔE2000 p95",
    "hist_emd_ev": "輝度ヒストグラム EMD [EV]", "lum_mean_ev": "平均輝度差 [EV]", "lum_median_ev": "中央輝度差 [EV]",
    "hue_shift_deg": "色相ずれ(符号付き平均) [deg]", "hue_abs_median_deg": "色相ずれ |中央値| [deg]",
    "exposure_ev_applied": "整列で当てた露出 [EV]",
}
PSNR_CAP = 100.0


def normalize_metrics(names) -> list[str]:
    if names is None:
        return list(DEFAULT_METRICS)
    if isinstance(names, str):
        names = [n for n in names.split(",") if n.strip()]
    out: list[str] = []
    for n in names:
        n = n.strip().lower()
        if n == "all":
            return list(ALL_METRICS)
        if n not in _ALIAS:
            raise ValueError(f"指標が不明: {n}(候補: {', '.join(ALL_METRICS)} / all)")
        k = _ALIAS[n]
        if k not in out:
            out.append(k)
    return out


@dataclass
class Maps:
    maps: dict[str, np.ndarray] = field(default_factory=dict)      # 名前 -> HxW
    notes: list[str] = field(default_factory=list)                 # 実行できなかった指標の理由
    flip_ldr_params: dict = field(default_factory=dict)
    flip_hdr_params: dict = field(default_factory=dict)
    ref_lin_disp: np.ndarray | None = None                         # 表示画像をリニアへ戻したもの(ヒストグラム/輝度/色差用)
    test_lin_disp: np.ndarray | None = None
    ref_canon: np.ndarray | None = None                            # sRGB エンコードへ正規化した表示画像
    test_canon: np.ndarray | None = None


def _canon(display: np.ndarray, eotf: str) -> np.ndarray:
    if eotf == "srgb":
        return np.asarray(display, dtype=np.float32)
    return color.linear_to_srgb(color.decode(display, eotf))


def _flip(ref: np.ndarray, test: np.ndarray, mode: str, params: dict | None) -> tuple[np.ndarray, dict]:
    try:
        import flip_evaluator as fe
    except ImportError as e:  # pragma: no cover
        raise RuntimeError("flip-evaluator が入っていない(pip install flip-evaluator。tools/parity/setup.ps1)") from e
    p = {k: v for k, v in (params or {}).items() if v is not None}
    arr_ref = np.ascontiguousarray(ref, dtype=np.float32)
    arr_test = np.ascontiguousarray(test, dtype=np.float32)
    if mode == "LDR":
        arr_ref, arr_test = np.clip(arr_ref, 0, 1), np.clip(arr_test, 0, 1)
    err, _mean, used = fe.evaluate(arr_ref, arr_test, mode, inputsRGB=(mode == "LDR"),
                                   applyMagma=False, computeMeanError=True, parameters=p)
    return np.asarray(err, dtype=np.float32).reshape(err.shape[0], err.shape[1]), dict(used)


def compute_maps(al: Aligned, metrics: list[str], flip_params: dict | None = None, ppd: float | None = None,
                 flip_hdr_params: dict | None = None) -> Maps:
    m = Maps()
    ref_c, test_c = _canon(al.ref_display, al.ref_eotf), _canon(al.test_display, al.test_eotf)
    m.ref_canon, m.test_canon = ref_c, test_c
    m.ref_lin_disp = color.srgb_to_linear(ref_c)
    m.test_lin_disp = color.srgb_to_linear(test_c)

    fp = dict(flip_params or {})
    if ppd:
        fp["ppd"] = ppd
    if "flip" in metrics:
        err, used = _flip(ref_c, test_c, "LDR", fp)
        m.maps["flip_ldr"], m.flip_ldr_params = err, used
    if "flip-hdr" in metrics:
        if al.ref_linear is None or al.test_linear is None:
            m.notes.append("flip-hdr: 両方がリニア HDR(PFM/EXR)のときだけ計算できる(PNG は 8bit の表示参照)。スキップした")
        else:
            hp = dict(fp)
            hp.update(flip_hdr_params or {})
            err, used = _flip(al.ref_linear, al.test_linear, "HDR", hp)
            m.maps["flip_hdr"], m.flip_hdr_params = err, used
    if "ssim" in metrics:
        from skimage.metrics import structural_similarity
        yr, yt = color.luma_display(ref_c), color.luma_display(test_c)
        _mean, smap = structural_similarity(yr, yt, data_range=1.0, gaussian_weights=True, sigma=1.5,
                                            use_sample_covariance=False, full=True)
        m.maps["ssim"] = smap.astype(np.float32)
    if "de2000" in metrics:
        m.maps["de2000"] = color.delta_e_2000(color.linear_to_lab(m.ref_lin_disp), color.linear_to_lab(m.test_lin_disp))
    return m


# ── 集計 ────────────────────────────────────────────────────────────────────

def _pick(a: np.ndarray, mask: np.ndarray | None) -> np.ndarray:
    return a[mask] if mask is not None else a.reshape(-1, *a.shape[2:])


def _stats(prefix: str, a: np.ndarray, out: dict, keys=("mean", "median", "p95", "p99", "max")) -> None:
    if a.size == 0:
        return
    for k in keys:
        if k == "mean":
            out[f"{prefix}_mean"] = float(np.mean(a))
        elif k == "median":
            out[f"{prefix}_median"] = float(np.median(a))
        elif k == "p95":
            out[f"{prefix}_p95"] = float(np.percentile(a, 95))
        elif k == "p99":
            out[f"{prefix}_p99"] = float(np.percentile(a, 99))
        elif k == "max":
            out[f"{prefix}_max"] = float(np.max(a))


def log_luma_hist(y: np.ndarray, lo: float = -12.0, hi: float = 2.0, bins: int = 256) -> np.ndarray:
    ev = np.log2(np.maximum(y, 2.0 ** lo))
    h, _ = np.histogram(ev, bins=bins, range=(lo, hi))
    return h.astype(np.float64) / max(1, h.sum())


def hist_emd_ev(y_ref: np.ndarray, y_test: np.ndarray, lo: float = -12.0, hi: float = 2.0, bins: int = 256) -> float:
    """log2 輝度ヒストグラムの EMD(1 次元 = 累積分布の差の面積)。単位 EV。同一分布なら 0。"""
    cr, ct = np.cumsum(log_luma_hist(y_ref, lo, hi, bins)), np.cumsum(log_luma_hist(y_test, lo, hi, bins))
    return float(np.sum(np.abs(cr - ct)) * (hi - lo) / bins)


def summarize(m: Maps, mask: np.ndarray | None = None, metrics: list[str] | None = None,
              exposure_ev_applied: float | None = None) -> dict:
    """指標の集計(mask があればその領域だけ)。キーは LABELS を参照。取れない値はキー自体を出さない。"""
    metrics = metrics or DEFAULT_METRICS
    out: dict = {}
    if mask is not None and not mask.any():
        return out
    if "flip_ldr" in m.maps:
        _stats("flip_ldr", _pick(m.maps["flip_ldr"], mask), out)
    if "flip_hdr" in m.maps:
        _stats("flip_hdr", _pick(m.maps["flip_hdr"], mask), out, keys=("mean", "median", "p95", "p99", "max"))
    if "ssim" in m.maps:
        s = m.maps["ssim"]
        pad = 5      # skimage: gaussian_weights + sigma1.5 -> 11x11 窓。境界 5 画素は捨てて平均する(skimage の mean と同じ)
        if mask is None:
            out["ssim"] = float(np.mean(s[pad:-pad, pad:-pad])) if min(s.shape) > 2 * pad else float(np.mean(s))
        else:
            mm = mask.copy()
            mm[:pad, :] = mm[-pad:, :] = False
            mm[:, :pad] = mm[:, -pad:] = False
            out["ssim"] = float(np.mean(s[mm])) if mm.any() else float(np.mean(s[mask]))
    if "psnr" in metrics and m.ref_canon is not None:
        d = (m.ref_canon - m.test_canon) ** 2
        mse = float(np.mean(_pick(d, mask)))
        out["psnr"] = PSNR_CAP if mse <= 0 else min(PSNR_CAP, 10.0 * math.log10(1.0 / mse))
    if "de2000" in m.maps:
        _stats("de2000", _pick(m.maps["de2000"], mask), out, keys=("mean", "median", "p95", "max"))
    if m.ref_lin_disp is not None:
        yr = _pick(color.luminance(m.ref_lin_disp), mask)
        yt = _pick(color.luminance(m.test_lin_disp), mask)
        if "hist" in metrics and yr.size:
            out["hist_emd_ev"] = hist_emd_ev(yr, yt)
        if "lum" in metrics and yr.size:
            eps = 1e-6
            out["lum_mean_ev"] = float(math.log2((np.mean(yt) + eps) / (np.mean(yr) + eps)))
            out["lum_median_ev"] = float(math.log2((np.median(yt) + eps) / (np.median(yr) + eps)))
        if "hue" in metrics:
            _hue(m, mask, out)
    if exposure_ev_applied is not None:
        out["exposure_ev_applied"] = float(exposure_ev_applied)
    return out


def _hue(m: Maps, mask: np.ndarray | None, out: dict, chroma_min: float = 8.0, min_pixels: int = 200) -> None:
    ref = _pick(m.ref_lin_disp, mask)
    test = _pick(m.test_lin_disp, mask)
    if ref.shape[0] > 400_000:                    # 巨大なときは間引く(色相の統計なので十分)
        step = ref.shape[0] // 400_000 + 1
        ref, test = ref[::step], test[::step]
    lr, lt = color.linear_to_lab(ref), color.linear_to_lab(test)
    ok = (color.chroma(lr) > chroma_min) & (color.chroma(lt) > chroma_min)
    if int(ok.sum()) < min_pixels:
        return
    d = color.circ_diff_deg(color.lch_hue_deg(lt[ok]), color.lch_hue_deg(lr[ok]))
    w = np.minimum(color.chroma(lr[ok]), color.chroma(lt[ok]))
    rad = np.radians(d)
    out["hue_shift_deg"] = float(np.degrees(np.arctan2(np.sum(w * np.sin(rad)), np.sum(w * np.cos(rad)))))
    out["hue_abs_median_deg"] = float(np.median(np.abs(d)))


def metric_direction(key: str) -> str:
    """'higher' = 大きいほど良い / 'lower' = 小さいほど良い / 'zero' = 0 に近いほど良い(符号付き)。"""
    if key in ("ssim", "psnr"):
        return "higher"
    if key in ("lum_mean_ev", "lum_median_ev", "hue_shift_deg", "exposure_ev_applied"):
        return "zero"
    return "lower"
