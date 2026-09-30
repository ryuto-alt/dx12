import json
import math

import numpy as np
import pytest

from parity import color
from parity import gate as G
from parity import heatmap as H
from parity import noisefloor as NF
from parity import spec as S
from parity import tonemap as T
from parity.imgio import Img
from parity.paths import TOOL_DIR
from conftest import gradient, spec_data


# ── トーンマップ・露出 ─────────────────────────────────────────────────────

@pytest.mark.parametrize("name", T.list_tonemaps())
def test_tonemaps_are_monotone_bounded_and_zero_at_zero(name):
    x = np.linspace(0, 16, 400, dtype=np.float32)
    rgb = np.stack([x, x, x], -1)
    d, _ = T.apply_tonemap(name, rgb)
    y = d[:, 0]
    assert d.min() >= 0 and d.max() <= 1.0 + 1e-6
    assert np.all(np.diff(y) >= -1e-5)
    assert y[0] < 0.05


def test_engine_aces_matches_the_shader_formula():
    """PostProcess.hlsl: saturate(x(2.51x+0.03)/(x(2.43x+0.59)+0.14)) の後 pow(1/2.2)。"""
    x = 0.18
    y = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    d, eotf = T.apply_tonemap("engine_aces", np.full((1, 1, 3), x, np.float32))
    assert eotf == "gamma22"
    assert float(d[0, 0, 0]) == pytest.approx(y ** (1 / 2.2), abs=1e-6)


def test_ue_filmic_alias_and_unknown():
    a, _ = T.apply_tonemap("ue_filmic_approx", np.full((1, 1, 3), 0.5, np.float32))
    b, _ = T.apply_tonemap("aces_hill", np.full((1, 1, 3), 0.5, np.float32))
    assert np.array_equal(a, b)
    with pytest.raises(ValueError):
        T.get_tonemap("bogus")


def test_parse_exposure():
    assert T.parse_exposure("auto")["mode"] == "auto_median"
    assert T.parse_exposure("fixed:-1.5")["ev_test"] == -1.5
    assert T.parse_exposure(None)["mode"] == "none"
    with pytest.raises(ValueError):
        T.parse_exposure("bogus")


def test_auto_exposure_recovers_a_known_ev_linear_pair():
    lin = (gradient(60, 100) * 3).astype(np.float32)
    ref, test = Img(lin, "linear"), Img(lin * 2 ** -1.5, "linear")
    al = T.align_pair(ref, test, "engine_aces", "auto")
    assert al.info["ev_test_applied"] == pytest.approx(1.5, abs=0.02)
    assert al.info["exposure_ev_applied"] == pytest.approx(1.5, abs=0.02)


def test_auto_exposure_recovers_ev_between_linear_ref_and_display_test():
    lin = (gradient(60, 100) * 2).astype(np.float32)
    disp_img, eotf = T.apply_tonemap("engine_aces", lin * 2 ** 1.0)   # 表示側は +1EV で撮られた
    test = Img(disp_img, "display", eotf)
    al = T.align_pair(Img(lin, "linear"), test, "engine_aces", "auto")
    assert al.info["ev_ref_applied"] == pytest.approx(1.0, abs=0.03)


def test_rank_tonemaps_finds_the_operator_that_made_the_reference():
    lin = (gradient(80, 120) * 6).astype(np.float32)
    ref_disp, eotf = T.apply_tonemap("aces_hill", lin * 2 ** 0.5)
    rank = T.rank_tonemaps(lin, Img(ref_disp, "display", eotf), ["engine_aces", "aces_hill", "linear_clip", "reinhard"])
    assert rank[0]["tonemap"] == "aces_hill" and rank[0]["emd"] < 0.02
    assert rank[0]["ev"] == pytest.approx(0.5, abs=0.15)
    assert rank[-1]["emd"] > rank[0]["emd"]


# ── ノイズ床 ──────────────────────────────────────────────────────────────

def noisy(sigma, seed, base=None):
    base = gradient(90, 160) if base is None else base
    rng = np.random.default_rng(seed)
    return Img(np.clip(base + rng.normal(0, sigma, base.shape), 0, 1).astype(np.float32), "display", "srgb")


def test_noise_floor_grows_with_sigma_and_ssim_falls():
    lo = NF.measure([noisy(0.005, 1), noisy(0.005, 2)])["floors"]
    hi = NF.measure([noisy(0.04, 1), noisy(0.04, 2)])["floors"]
    assert lo["flip_ldr_mean"] < hi["flip_ldr_mean"]
    assert lo["ssim"] > hi["ssim"]
    assert lo["psnr"] > hi["psnr"]


def test_noise_floor_needs_two_images_and_pairs_take_worst():
    with pytest.raises(ValueError):
        NF.measure([noisy(0.01, 1)])
    r = NF.measure([noisy(0.01, 1), noisy(0.01, 2), noisy(0.04, 3)], pairs="all")
    assert len(r["pairs"]) == 3
    assert r["floors"]["flip_ldr_mean"] == pytest.approx(max(p["flip_ldr_mean"] for p in r["pairs"]))


def test_single_correction_is_smaller_by_sqrt2():
    a, b = noisy(0.03, 1), noisy(0.03, 2)
    pair = NF.measure([a, b])["floors"]
    single = NF.measure([a, b], correction="single")["floors"]
    assert single["flip_ldr_mean"] == pytest.approx(pair["flip_ldr_mean"] / math.sqrt(2))
    assert single["ssim"] > pair["ssim"]


def test_threshold_from_floor_accepts_noise_and_rejects_a_real_shift():
    """しきい値 = max(固定, 床 x 1.5)。ノイズだけの差は通り、露出ずれ(実差)は落ちる。"""
    base = gradient(90, 160)
    floors = NF.measure([noisy(0.03, 1, base), noisy(0.03, 2, base)])["floors"]
    fixed = {"flip_ldr_mean_max": 0.001, "ssim_min": 0.999}          # 固定値だけだとノイズでも落ちる厳しさ
    ref = noisy(0.03, 10, base)
    from parity.evaluate import evaluate_pair
    import tempfile, pathlib
    with tempfile.TemporaryDirectory() as td:
        only_fixed = evaluate_pair(ref, noisy(0.03, 11, base), pathlib.Path(td), gate_name="G1", gate_full=fixed, write_images=False)
        with_floor = evaluate_pair(ref, noisy(0.03, 11, base), pathlib.Path(td), gate_name="G1", gate_full=fixed, floors=floors, write_images=False)
        shifted = Img(np.clip(color.linear_to_srgb(color.srgb_to_linear(base) * 2 ** 0.6), 0, 1).astype(np.float32), "display", "srgb")
        real = evaluate_pair(ref, shifted, pathlib.Path(td), gate_name="G1", gate_full=fixed, floors=floors, write_images=False)
    assert only_fixed["verdict"]["status"] == "fail"
    assert with_floor["verdict"]["status"] == "pass"
    assert any(c["floor_applied"] for c in with_floor["verdict"]["checks"])
    assert real["verdict"]["status"] == "fail"


def test_effective_limit_rules():
    fl = {"flip_ldr_mean": 0.1, "ssim": 0.9, "psnr": 30.0, "lum_mean_ev": -0.2}
    assert NF.effective_limit("flip_ldr_mean", "max", 0.05, fl, 1.5) == (pytest.approx(0.15), True)
    assert NF.effective_limit("flip_ldr_mean", "max", 0.5, fl, 1.5) == (0.5, False)
    lim, relaxed = NF.effective_limit("ssim", "min", 0.99, fl, 1.5)
    assert lim == pytest.approx(0.85) and relaxed
    lim, _ = NF.effective_limit("psnr", "min", 60.0, fl, 1.5)
    assert lim == pytest.approx(30 - 20 * math.log10(1.5))
    assert NF.effective_limit("lum_mean_ev", "abs_max", 0.1, fl, 1.5)[0] == pytest.approx(0.3)
    assert NF.effective_limit("flip_ldr_mean", "max", 0.05, None, 1.5) == (0.05, False)


# ── ゲート・仕様 ──────────────────────────────────────────────────────────

def test_parse_key():
    assert G.parse_key("flip_ldr_mean_max") == ("flip_ldr_mean", "max")
    assert G.parse_key("ssim_min") == ("ssim", "min")
    assert G.parse_key("lum_mean_ev_abs_max") == ("lum_mean_ev", "abs_max")
    assert G.parse_key("flip_ldr_max_max") == ("flip_ldr_max", "max")
    for bad in ("flip_ldr_mean", "bogus_max", "flip_ldr_max"):
        with pytest.raises(G.GateError):
            G.parse_key(bad)


def test_judge_pass_fail_unavailable_and_strict():
    vals = {"flip_ldr_mean": 0.2, "ssim": 0.95}
    r = G.judge(vals, {"flip_ldr_mean_max": 0.15, "ssim_min": 0.9})
    assert r["status"] == "fail" and len(r["reasons"]) == 1 and "0.2" in r["reasons"][0]
    r = G.judge(vals, {"flip_hdr_mean_max": 0.1, "ssim_min": 0.9})
    assert r["status"] == "pass" and r["warnings"]
    assert G.judge(vals, {"flip_hdr_mean_max": 0.1}, strict=True)["status"] == "fail"


def test_sanity_detects_black_white_nan_flat():
    assert G.sanity(np.zeros((8, 8, 3), np.float32))["problems"] == ["all_black"]
    assert "all_white" in G.sanity(np.ones((8, 8, 3), np.float32))["problems"]
    assert "nan_or_inf" in G.sanity(np.full((8, 8, 3), np.nan, np.float32))["problems"]
    assert G.sanity(np.full((8, 8, 3), 0.4, np.float32))["problems"] == ["flat"]
    assert G.sanity(gradient())["ok"]


def test_overall_and_resolve_gate():
    assert G.overall(["pass", "skipped"]) == "pass"
    assert G.overall(["skipped", "skipped"]) == "skipped"
    assert G.overall(["pass", "fail"]) == "fail"
    d = spec_data(milestones={"Q2": "G2"})
    assert G.resolve_gate(d, "Q2")[0] == "G2"
    assert G.resolve_gate(d, "regression")[0] == "REG"
    with pytest.raises(G.GateError):
        G.resolve_gate(d, "nope")


def test_sample_scenes_validate_and_have_expected_structure():
    files = S.list_specs()
    assert {f.stem for f in files} >= {"ps0_calibration", "ps1_indoor_corridor", "smoke_generated"}
    for f in files:
        sp = S.load_spec(f)
        assert sp.hash()
    ps0 = S.load_spec(TOOL_DIR / "scenes" / "ps0_calibration.json")
    assert ps0.data["parityScene"] == "PS-0" and len(ps0.cameras) == 4     # furnace は skip
    assert G.resolve_gate(ps0.data, "Q2")[0] == "G2"


def test_invalid_specs_are_rejected_with_reasons():
    bad = spec_data()
    del bad["cameras"]
    with pytest.raises(S.SpecError, match="cameras"):
        S.from_data(bad)
    dup = spec_data(cameras=[{"name": "a", "position": [0, 0, 0], "target": [0, 0, 1]}] * 2)
    with pytest.raises(S.SpecError, match="重複"):
        S.from_data(dup)
    with pytest.raises(S.SpecError, match="flip_mean"):
        S.from_data(spec_data(gates={"G1": {"flip_mean": 0.1}}))
    with pytest.raises(S.SpecError):
        S.from_data(spec_data(id="Bad Id"))
    with pytest.raises(S.SpecError, match="regions"):
        S.from_data(spec_data(cameras=[{"name": "a", "position": [0, 0, 0], "target": [0, 0, 1], "regions": ["sky"]}]))


def test_spec_hash_changes_with_content():
    a = S.from_data(spec_data())
    b = S.from_data(spec_data(gates={"G1": {"ssim_min": 0.5}}))
    assert a.hash() != b.hash() and a.hash() == S.from_data(spec_data()).hash()


# ── ヒートマップ ──────────────────────────────────────────────────────────

def test_heatmap_and_tiles(tmp_path):
    err = np.zeros((64, 96), np.float32)
    err[16:32, 48:64] = 0.9
    p = H.save_error_heatmap(tmp_path / "h.png", err, 1.0)
    from PIL import Image
    im = Image.open(p)
    assert im.size[0] == 96 and im.size[1] > 64                     # カラーバー分が付く
    w = H.worst_tiles(err, 16, 3)
    assert w[0]["x"] == 48 and w[0]["y"] == 16 and w[0]["mean"] == pytest.approx(0.9)
    m = H.magma(np.array([0.0, 1.0]))
    assert m[0].sum() < 0.1 and m[1].sum() > 2.4                    # 黒 -> 明るい黄
    sheet = H.contact_sheet([("a", gradient()), ("b", gradient())], tmp_path / "c.png")
    assert Image.open(sheet).size[0] >= 320
    assert H.signed_luma_diff(np.zeros((4, 4, 3)), np.ones((4, 4, 3)))[0, 0, 0] > 0.9   # 明るければ赤
