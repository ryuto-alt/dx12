import math
import os
import tempfile
import urllib.request
from pathlib import Path

import numpy as np
import pytest
from scipy.ndimage import gaussian_filter

from parity import color
from parity import metrics as M
from parity.imgio import Img, load_image, save_png
from parity.tonemap import align_pair
from conftest import gradient


def disp(a):
    return Img(np.asarray(a, np.float32), "display", "srgb")


def run_metrics(ref, test, mets=None, tonemap="engine_aces", exposure=None, mask=None):
    al = align_pair(ref, test, tonemap, exposure)
    mets = M.normalize_metrics(mets)
    maps = M.compute_maps(al, mets)
    return M.summarize(maps, mask, mets), maps


# ── 同一 / 既知のずれ ──────────────────────────────────────────────────────

def test_identical_images_are_perfect():
    a = disp(gradient())
    m, _ = run_metrics(a, a)
    assert m["flip_ldr_mean"] == 0 and m["flip_ldr_max"] == 0
    assert m["ssim"] == pytest.approx(1.0, abs=1e-6)
    assert m["psnr"] == M.PSNR_CAP
    assert m["de2000_max"] == 0 and m["hist_emd_ev"] == 0
    assert m["lum_mean_ev"] == pytest.approx(0, abs=1e-5) and m["lum_median_ev"] == pytest.approx(0, abs=1e-5)


def test_known_exposure_shift_gives_expected_ev():
    """リニア空間で 2^0.5 倍(+0.5EV)明るくした画像の輝度差はちょうど +0.5 EV。"""
    lin = color.srgb_to_linear(gradient())
    a = disp(color.linear_to_srgb(lin * 0.5))
    b = disp(color.linear_to_srgb(lin * 0.5 * 2 ** 0.5))
    m, _ = run_metrics(a, b)
    assert m["lum_mean_ev"] == pytest.approx(0.5, abs=0.01)
    assert m["lum_median_ev"] == pytest.approx(0.5, abs=0.02)
    assert m["hist_emd_ev"] == pytest.approx(0.5, abs=0.06)     # 分布の平行移動の EMD = 移動量
    assert abs(m["hue_shift_deg"]) < 1.0                        # 露出だけなら色相はほぼ変わらない


def test_psnr_matches_analytic():
    a = np.full((64, 64, 3), 0.5, np.float32)
    b = a + 0.1
    m, _ = run_metrics(disp(a), disp(b))
    assert m["psnr"] == pytest.approx(20.0, abs=0.05)           # MSE = 0.01 -> 20 dB(8bit 量子化が入らない float なので厳密)


def test_delta_e_of_flat_colors_matches_direct_computation():
    a = np.full((32, 32, 3), [0.8, 0.3, 0.3], np.float32)
    b = np.full((32, 32, 3), [0.3, 0.8, 0.3], np.float32)
    m, _ = run_metrics(disp(a), disp(b), ["de2000"])
    d = color.delta_e_2000(color.linear_to_lab(color.srgb_to_linear(a[:1, :1])), color.linear_to_lab(color.srgb_to_linear(b[:1, :1])))
    assert m["de2000_mean"] == pytest.approx(float(d[0, 0]), rel=1e-4) and m["de2000_mean"] > 30


def test_hue_shift_detected_and_signed():
    """色相を回した画像で、符号つきの色相ずれが回転量に近い。"""
    import colorsys
    h, w = 40, 40
    rng = np.random.default_rng(0)
    hsv_h, hsv_s, hsv_v = rng.random((h, w)), 0.6 + 0.3 * rng.random((h, w)), 0.5 + 0.4 * rng.random((h, w))

    def to_rgb(hh):
        out = np.zeros((h, w, 3), np.float32)
        for i in range(h):
            for j in range(w):
                out[i, j] = colorsys.hsv_to_rgb(hh[i, j] % 1.0, hsv_s[i, j], hsv_v[i, j])
        return out
    a, b = to_rgb(hsv_h), to_rgb(hsv_h + 15 / 360.0)
    m, _ = run_metrics(disp(a), disp(b), ["hue"])
    assert m["hue_abs_median_deg"] > 8
    m2, _ = run_metrics(disp(b), disp(a), ["hue"])
    assert m["hue_shift_deg"] * m2["hue_shift_deg"] < 0          # 逆にすれば符号が反転する


# ── SSIM は独立実装(scipy のガウス窓)と一致 ───────────────────────────────

def ssim_independent(x, y, sigma=1.5, c1=0.01 ** 2, c2=0.03 ** 2, pad=5):
    f = lambda z: gaussian_filter(z, sigma, mode="reflect", truncate=3.5)  # noqa: E731
    mx, my = f(x), f(y)
    sxx, syy, sxy = f(x * x) - mx * mx, f(y * y) - my * my, f(x * y) - mx * my
    s = ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))
    return float(s[pad:-pad, pad:-pad].mean())


def test_ssim_equals_skimage_and_independent_implementation():
    from skimage.metrics import structural_similarity
    rng = np.random.default_rng(3)
    a = gradient(120, 200)
    b = np.clip(a + rng.normal(0, 0.05, a.shape), 0, 1).astype(np.float32)
    m, _ = run_metrics(disp(a), disp(b), ["ssim"])
    ya, yb = color.luma_display(a), color.luma_display(b)
    sk = structural_similarity(ya, yb, data_range=1.0, gaussian_weights=True, sigma=1.5, use_sample_covariance=False)
    assert m["ssim"] == pytest.approx(sk, abs=1e-6)              # scikit-image と一致
    assert m["ssim"] == pytest.approx(ssim_independent(ya, yb), abs=2e-3)   # 独立実装(scipy)ともほぼ一致
    assert 0.3 < m["ssim"] < 0.99


def test_ssim_of_constant_shift_equals_luminance_term():
    a = np.full((64, 64), 0.3, np.float32)
    b = np.full((64, 64), 0.5, np.float32)
    from skimage.metrics import structural_similarity
    s = structural_similarity(a, b, data_range=1.0, gaussian_weights=True, sigma=1.5, use_sample_covariance=False)
    c1 = 0.01 ** 2
    assert s == pytest.approx((2 * 0.3 * 0.5 + c1) / (0.3 ** 2 + 0.5 ** 2 + c1), abs=1e-5)


# ── FLIP ──────────────────────────────────────────────────────────────────

def test_flip_grows_with_noise_and_shift():
    rng = np.random.default_rng(1)
    a = gradient(120, 200)
    vals = []
    for s in (0.005, 0.02, 0.06):
        b = np.clip(a + rng.normal(0, s, a.shape), 0, 1).astype(np.float32)
        m, _ = run_metrics(disp(a), disp(b), ["flip"])
        vals.append(m["flip_ldr_mean"])
    assert vals[0] < vals[1] < vals[2]
    m, _ = run_metrics(disp(a), disp(np.clip(a + 0.15, 0, 1)), ["flip"])
    assert m["flip_ldr_mean"] > vals[0]


def test_flip_hdr_runs_on_linear_pair_and_orders_by_error():
    lin = (gradient(80, 120) * 4).astype(np.float32)
    a = Img(lin, "linear")
    m0, _ = run_metrics(a, a, ["flip-hdr"])
    m1, _ = run_metrics(a, Img(lin * 1.5, "linear"), ["flip-hdr"])
    assert m0["flip_hdr_mean"] == 0
    assert m1["flip_hdr_mean"] > 0.01


def test_flip_hdr_skipped_with_note_for_png_inputs():
    a = disp(gradient())
    al = align_pair(a, a, "engine_aces", None)
    maps = M.compute_maps(al, ["flip-hdr"])
    assert "flip_hdr" not in maps.maps and any("flip-hdr" in n for n in maps.notes)


FLIP_BASE = "https://raw.githubusercontent.com/NVlabs/flip/main/images/"


def _official(name):
    d = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()) / "UnoEngine" / "parity" / "cache" / "flip"
    d.mkdir(parents=True, exist_ok=True)
    p = d / name
    if not p.exists() or p.stat().st_size < 1000:
        try:
            with urllib.request.urlopen(FLIP_BASE + name, timeout=30) as r:
                p.write_bytes(r.read())
        except Exception as e:
            pytest.skip(f"公式サンプルを取得できない(オフライン): {e}")
    return p


def test_flip_ldr_matches_official_sample():
    """NVlabs/flip 付属の reference.png / test.png。公式の(パス指定)呼び出しと、本基盤の読み込み + 指標経路が一致する。"""
    import flip_evaluator as fe
    ref, test = _official("reference.png"), _official("test.png")
    _, official, _ = fe.evaluate(str(ref), str(test), "LDR")
    m, _ = run_metrics(load_image(ref), load_image(test), ["flip"])
    assert m["flip_ldr_mean"] == pytest.approx(official, rel=1e-4)
    assert official == pytest.approx(0.1597, abs=5e-4)            # 公式 Python 版 1.7 の値(記録)


def test_flip_hdr_matches_official_sample():
    import flip_evaluator as fe
    pytest.importorskip("OpenEXR")
    ref, test = _official("reference.exr"), _official("test.exr")
    try:
        _, official, params = fe.evaluate(str(ref), str(test), "HDR")
    except Exception as e:
        pytest.skip(f"公式ローダで EXR を読めない: {e}")
    a, b = load_image(ref), load_image(test)
    al = align_pair(a, b, "engine_aces", None)
    maps = M.compute_maps(al, ["flip-hdr"])
    ours = float(np.mean(maps.maps["flip_hdr"]))
    assert ours == pytest.approx(official, rel=2e-3)


# ── 領域別 ─────────────────────────────────────────────────────────────────

def test_region_metrics_only_count_the_masked_pixels():
    a = gradient(80, 160)
    b = a.copy()
    b[:, 80:] = np.clip(b[:, 80:] * 0.5, 0, 1)                    # 右半分だけ壊す
    left = np.zeros((80, 160), bool)
    left[:, :70] = True
    right = np.zeros((80, 160), bool)
    right[:, 90:] = True
    full, maps = run_metrics(disp(a), disp(b), ["flip", "lum", "psnr"])
    ml = M.summarize(maps, left, ["flip", "lum", "psnr"])
    mr = M.summarize(maps, right, ["flip", "lum", "psnr"])
    assert ml["flip_ldr_mean"] < 0.02 < 0.1 < mr["flip_ldr_mean"]
    assert abs(ml["lum_mean_ev"]) < 0.05 and mr["lum_mean_ev"] < -0.5
    assert ml["flip_ldr_mean"] < full["flip_ldr_mean"] < mr["flip_ldr_mean"]
    assert M.summarize(maps, np.zeros((80, 160), bool), ["flip"]) == {}


def test_normalize_metrics_rejects_unknown():
    with pytest.raises(ValueError):
        M.normalize_metrics("flip,bogus")
    assert M.normalize_metrics("flip_ldr,SSIM") == ["flip", "ssim"]
