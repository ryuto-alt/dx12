# temporal_metrics.py / energy_check.py のテスト（エンジンも GPU も要らない。合成の PFM だけ）。
#   tools/parity/.venv/Scripts/python.exe -I -m pytest tools/bench/test_temporal_metrics.py -q
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import energy_check as ec  # noqa: E402
import temporal_metrics as tm  # noqa: E402

H, W = 48, 64


def write_pfm(path, a):
    a = np.asarray(a, dtype="<f4")
    if a.ndim == 2:
        a = np.stack([a] * 3, axis=-1)
    with open(path, "wb") as f:
        f.write(b"PF\n")
        f.write(f"{a.shape[1]} {a.shape[0]}\n".encode())
        f.write(b"-1.0\n")
        f.write(np.flipud(a).tobytes())
    return str(path)


def seq(tmp_path, tag, frames):
    return [write_pfm(tmp_path / f"{tag}_{i:03d}.pfm", f) for i, f in enumerate(frames)]


def flat(v):
    return np.full((H, W), v, dtype=np.float32)


def test_read_pfm_is_top_first(tmp_path):
    a = np.zeros((H, W, 3), np.float32)
    a[0, :, 0] = 5.0           # 画像の最上段が赤
    p = write_pfm(tmp_path / "a.pfm", a)
    r = tm.read_pfm(p)
    assert r[0, 0, 0] == 5.0 and r[-1, 0, 0] == 0.0


def test_flicker_static_is_zero_and_flicker_is_detected(tmp_path):
    still = seq(tmp_path, "s", [flat(0.5)] * 8)
    assert tm.flicker_metrics(still, None)["meanStd"] == pytest.approx(0.0, abs=1e-9)
    frames = []
    for i in range(8):
        f = flat(0.5)
        f[:, ::2] += 0.2 * (1 if i % 2 else -1)       # 1 画素おきに明滅
        frames.append(f)
    fl = tm.flicker_metrics(seq(tmp_path, "f", frames), None)
    assert fl["meanStd"] > 0.02
    # 明滅していない領域だけを測れば 0
    quiet = tm.flicker_metrics(seq(tmp_path, "f", frames), None)
    region_left = tm.flicker_metrics(seq(tmp_path, "f", [np.where(np.arange(W) < 8, f[:, :], 0.5) for f in frames]), [0.5, 0, 1, 1])
    assert region_left["meanStd"] == pytest.approx(0.0, abs=1e-9) and quiet["meanStd"] > 0.0


def orb(level):
    f = flat(0.1)
    f[20:28, 28:36] += level
    return f


def test_ghost_none_and_decaying(tmp_path):
    clean = [orb(10.0)] * 6 + [orb(0.0)] * 10
    g = tm.ghost_metrics(seq(tmp_path, "a", clean), [0.3, 0.3, 0.7, 0.7], 5)
    assert g["meanRatio"] < 1e-6 and g["halfLifeFrames"] == 1
    decay = [orb(10.0)] * 6 + [orb(10.0 * 0.6 ** (k + 1)) for k in range(16)]
    g = tm.ghost_metrics(seq(tmp_path, "b", decay), [0.3, 0.3, 0.7, 0.7], 5)
    assert 0.55 < g["ratio"][0] < 0.65 and g["halfLifeFrames"] == 2      # 0.6, 0.36 → 2 枚目で 0.5 未満
    assert g["meanRatio"] > 0.05


def test_ghost_ignores_noise_below_one_percent_of_peak(tmp_path):
    rng = np.random.default_rng(0)
    frames = [orb(10.0)] * 6 + [orb(0.0) + rng.normal(0, 0.01, (H, W)).astype(np.float32) for _ in range(10)]
    g = tm.ghost_metrics(seq(tmp_path, "n", frames), None, 5)
    assert g["meanRatio"] < 1e-3


def test_ghost_rejects_region_without_object(tmp_path):
    frames = [orb(10.0)] * 4 + [orb(0.0)] * 6
    with pytest.raises(ValueError):
        tm.ghost_metrics(seq(tmp_path, "r", frames), [0.0, 0.0, 0.2, 0.2], 3)


def test_ssim_identity_and_difference(tmp_path):
    rng = np.random.default_rng(1)
    base = rng.uniform(0.2, 2.0, (H, W)).astype(np.float32)
    ref = [base] * 4
    other = np.roll(base, 3, axis=1)                         # 3 画素ずらした絵（カット直後の別の絵）
    s = tm.ssim_metrics(seq(tmp_path, "s", [other, base, base, base, base, base]), None, ref_count=4)
    assert s["perFrame"][0] < 0.8 and s["perFrame"][1] > 0.999 and s["firstFrame"] < s["mean"]
    # マスク外の違いは見ない
    half = base.copy(); half[:, : W // 2] = other[:, : W // 2]
    s2 = tm.ssim_metrics(seq(tmp_path, "m", [half, base, base, base, base, base]), [0.75, 0, 1, 1], ref_count=4)
    assert s2["perFrame"][0] > 0.99


def test_box_filter_matches_bruteforce():
    rng = np.random.default_rng(2)
    a = rng.uniform(0, 1, (11, 13))
    got = tm._box(a, 7)
    p = np.pad(a, 3, mode="constant"); c = np.pad(np.ones_like(a), 3, mode="constant")
    for (y, x) in [(0, 0), (5, 6), (10, 12), (0, 12)]:
        assert got[y, x] == pytest.approx(p[y:y + 7, x:x + 7].sum() / c[y:y + 7, x:x + 7].sum())


def test_compare_two_runs():
    a = {"flicker": {"meanStd": 0.0064, "p95Std": 0.034, "frames": 32}, "ssim": {"mean": 0.996, "min": 0.99}}
    b = {"flicker": {"meanStd": 0.00641, "p95Std": 0.0341, "frames": 32}, "ssim": {"mean": 0.9961, "min": 0.9905}}
    assert tm.compare(a, b)["within1pct"]
    c = {"flicker": {"meanStd": 0.0070, "p95Std": 0.034, "frames": 32}, "ssim": {"mean": 0.996, "min": 0.99}}
    r = tm.compare(a, c)
    assert not r["within1pct"] and r["worst"] == "flicker.meanStd"


def test_run_manifest_phases(tmp_path):
    man = {"exposure": 1.0, "phases": {
        "flicker": {"files": seq(tmp_path, "f", [flat(0.5)] * 5), "region": [0, 0, 1, 1]},
        "ghost": {"files": seq(tmp_path, "g", [orb(10.0)] * 3 + [orb(0.0)] * 5), "region": None, "peakIndex": 2, "bgCount": 3},
        "ssim": {"files": seq(tmp_path, "s", [flat(0.5)] * 6), "region": None, "refCount": 2}}}
    out = tm.run_manifest(man)
    assert set(out) >= {"flicker", "ghost", "ssim"} and out["ssim"]["min"] == pytest.approx(1.0, abs=1e-6)


# ─── energy_check ───────────────────────────────────────────────────────
def point(sigma):
    yy, xx = np.mgrid[0:H, 0:W]
    g = np.exp(-(((xx - W / 2) ** 2) + ((yy - H / 2) ** 2)) / (2 * sigma ** 2))
    return (0.05 + 50.0 * g / g.sum()).astype(np.float32)


def test_energy_ratio_and_nonfinite(tmp_path):
    a = write_pfm(tmp_path / "a.pfm", point(1.0))
    b = write_pfm(tmp_path / "b.pfm", point(1.0) * 1.05)
    r = ec.analyze(a, b)
    assert r["total"]["ratio"] == pytest.approx(1.05, abs=1e-4) and r["nonFinite"] == {"a": 0, "b": 0}
    bad = point(1.0); bad[3, 3] = np.nan
    r = ec.analyze(write_pfm(tmp_path / "bad.pfm", bad), b)
    assert r["nonFinite"]["a"] == 1


def test_energy_profile_widens_with_blur(tmp_path):
    a = write_pfm(tmp_path / "a.pfm", point(0.8))
    b = write_pfm(tmp_path / "b.pfm", point(3.0))
    r = ec.analyze(a, b, center=[0.5, 0.5], rmax=0.3, bins=32)
    p = r["profile"]
    assert p["halfRadius"]["b"] > 2 * p["halfRadius"]["a"]
    assert p["l1"] > 0.3 and sum(p["a"]) == pytest.approx(1.0, abs=1e-6)
    same = ec.analyze(a, a, center=[0.5, 0.5], rmax=0.3)["profile"]
    assert same["l1"] == pytest.approx(0.0, abs=1e-9)


def test_energy_size_mismatch(tmp_path):
    a = write_pfm(tmp_path / "a.pfm", flat(1.0))
    b = write_pfm(tmp_path / "b.pfm", np.ones((H + 1, W), np.float32))
    with pytest.raises(ValueError):
        ec.analyze(a, b)
