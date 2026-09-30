"""色空間の変換と色差。

  リニア RGB(Rec.709 原色 / D65) <-> sRGB エンコード <-> XYZ <-> CIELAB / LCh
  ΔE2000 は scikit-image の実装(CIE 2000 の式どおり)を使う。
"""
from __future__ import annotations

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)   # Rec.709 の輝度係数(リニア RGB 用)

_RGB2XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                     [0.2126729, 0.7151522, 0.0721750],
                     [0.0193339, 0.1191920, 0.9503041]], dtype=np.float64)
_D65 = np.array([0.95047, 1.0, 1.08883])


# ── 伝達関数 ────────────────────────────────────────────────────────────────

def srgb_to_linear(v: np.ndarray) -> np.ndarray:
    v = np.asarray(v, dtype=np.float32)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4).astype(np.float32)


def linear_to_srgb(v: np.ndarray) -> np.ndarray:
    v = np.clip(np.asarray(v, dtype=np.float32), 0.0, None)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1.0 / 2.4) - 0.055).astype(np.float32)


def gamma22_to_linear(v: np.ndarray) -> np.ndarray:
    return np.power(np.clip(np.asarray(v, dtype=np.float32), 0.0, None), 2.2).astype(np.float32)


def linear_to_gamma22(v: np.ndarray) -> np.ndarray:
    return np.power(np.clip(np.asarray(v, dtype=np.float32), 0.0, None), 1.0 / 2.2).astype(np.float32)


def decode(display: np.ndarray, eotf: str) -> np.ndarray:
    if eotf == "srgb":
        return srgb_to_linear(display)
    if eotf == "gamma22":
        return gamma22_to_linear(display)
    raise ValueError(f"eotf が不明: {eotf}")


def encode(linear: np.ndarray, eotf: str) -> np.ndarray:
    if eotf == "srgb":
        return linear_to_srgb(linear)
    if eotf == "gamma22":
        return linear_to_gamma22(linear)
    raise ValueError(f"eotf が不明: {eotf}")


# ── 輝度 ────────────────────────────────────────────────────────────────────

def luminance(linear_rgb: np.ndarray) -> np.ndarray:
    """リニア RGB -> 相対輝度 Y(HxW)。"""
    return (np.asarray(linear_rgb, dtype=np.float32) @ LUMA).astype(np.float32)


def luma_display(display_rgb: np.ndarray) -> np.ndarray:
    """エンコード済み(ガンマ空間)RGB の輝度 = SSIM 用の luma(リニアには戻さない)。"""
    return (np.asarray(display_rgb, dtype=np.float32) @ LUMA).astype(np.float32)


# ── CIELAB ──────────────────────────────────────────────────────────────────

def linear_to_lab(linear_rgb: np.ndarray) -> np.ndarray:
    """リニア RGB(0..1 想定)-> CIELAB(D65)。float64。"""
    rgb = np.clip(np.asarray(linear_rgb, dtype=np.float64), 0.0, None)
    xyz = rgb @ _RGB2XYZ.T / _D65
    eps, kappa = 216.0 / 24389.0, 24389.0 / 27.0
    f = np.where(xyz > eps, np.cbrt(xyz), (kappa * xyz + 16.0) / 116.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)


def delta_e_2000(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    from skimage.color import deltaE_ciede2000
    return deltaE_ciede2000(lab1, lab2).astype(np.float32)


def lch_hue_deg(lab: np.ndarray) -> np.ndarray:
    return np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360.0


def chroma(lab: np.ndarray) -> np.ndarray:
    return np.hypot(lab[..., 1], lab[..., 2])


def circ_diff_deg(h_test: np.ndarray, h_ref: np.ndarray) -> np.ndarray:
    """色相角の符号付き差(-180..180]。test が ref より正方向へどれだけ回っているか。"""
    d = (h_test - h_ref + 180.0) % 360.0 - 180.0
    return np.where(d == -180.0, 180.0, d)
