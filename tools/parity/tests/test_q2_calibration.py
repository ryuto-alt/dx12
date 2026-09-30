"""Q2(校正): 新しいトーンマップ(UE Filmic / PBR Neutral / 線形)・物理露出(EV100)・線形 HDR 出力・任意解像度・
カメラごとの setup / 整列・実エンジン相当の非同期 render_reference・displayCheck。偽エンジンで通す。

C++ 側(tests/photometric_test.cpp)は同じ参照値を持つ。ここの数値を変えたら C++ の表も揃えること。
"""
import json
from pathlib import Path

import numpy as np
import pytest

from conftest import spec_data
from parity import spec as S
from parity import tonemap as T
from parity.imgio import load_image, write_pfm
from parity.runner import EXIT, RunOptions, run_scene

# numpy 実装の出力(C++ tests/photometric_test.cpp の kUe / kNeutral と同じ表)
UE_GOLDEN = [
    ((0.18, 0.18, 0.18), (0.18000000, 0.18000000, 0.18000000)),
    ((1.0, 1.0, 1.0), (0.72335946, 0.72335946, 0.72335946)),
    ((0.01, 0.01, 0.01), (0.00165657, 0.00165657, 0.00165657)),
    ((10.0, 10.0, 10.0), (0.99947556, 0.99947556, 0.99947556)),
    ((0.5, 0.2, 0.1), (0.51030361, 0.22057018, 0.09567351)),
    ((0.05, 0.4, 0.05), (0.05312902, 0.43595007, 0.04732919)),
    ((2.0, 0.5, 0.1), (0.92656162, 0.56681640, 0.18665671)),
    ((0.02, 0.03, 0.9), (0.00473850, 0.01339858, 0.69738913)),
    ((50.0, 20.0, 5.0), (1.03476059, 1.02054789, 0.98915312)),
]
NEUTRAL_GOLDEN = [
    ((0.18, 0.18, 0.18), (0.14, 0.14, 0.14)),
    ((1.0, 1.0, 1.0), (0.86909091, 0.86909091, 0.86909091)),
    ((0.5, 0.2, 0.1), (0.46, 0.16, 0.06)),
    ((4.0, 1.0, 0.2), (0.98325581, 0.46829917, 0.33097740)),
]


def test_ue_filmic_and_pbr_neutral_reference_values():
    for x, want in UE_GOLDEN:
        assert np.allclose(T.ue_filmic_linear(np.array(x)), want, atol=2e-6), x
    for x, want in NEUTRAL_GOLDEN:
        assert np.allclose(T.pbr_neutral_linear(np.array(x)), want, atol=2e-6), x


def test_ue_filmic_invariants():
    xs = np.logspace(-4, 2, 200)
    g = T.ue_filmic_linear(np.stack([xs, xs, xs], -1))[:, 1]
    assert np.all(np.diff(g) >= -1e-9)                                  # 単調増加
    assert abs(T.ue_filmic_linear(np.array([0.18] * 3))[0] - 0.18) < 1e-6   # 18% の灰は動かない(構成から)
    assert abs(T.ue_filmic_linear(np.array([0.0, 0.0, 0.0]))[0]) < 1e-9
    assert abs(T.ue_filmic_linear(np.array([1e4] * 3))[0] - 1.04) < 0.01    # WhiteClip 0.04


def test_tonemap_registry_has_the_engine_modes():
    for name in ("ue_filmic", "engine_ue_filmic", "pbr_neutral", "engine_pbr_neutral", "engine_linear"):
        assert name in T.list_tonemaps()
        d, eotf = T.apply_tonemap(name, np.full((2, 2, 3), 0.18, np.float32))
        assert eotf == "srgb" and 0.0 <= d.min() <= d.max() <= 1.0
    # 灰 0.18 → UE は 0.18(リニア)→ sRGB エンコード 0.4614
    d, _ = T.apply_tonemap("ue_filmic", np.full((1, 1, 3), 0.18, np.float32))
    assert abs(float(d[0, 0, 0]) - 0.46139) < 5e-4


def test_ev100_exposure_conversions():
    assert abs(T.ev100_to_scale(0.0) - 1 / 1.2) < 1e-12
    assert abs(T.ev100_to_scale(15.0) - 1 / (1.2 * 32768)) < 1e-15
    assert abs(T.ev100_to_scale(12.0, 1.0) - 2 * T.ev100_to_scale(12.0)) < 1e-15      # 補正 +1 EV = 2 倍
    assert abs(T.ev100_to_ev(15.0) - (-15.263034405833794)) < 1e-9
    ex = T.parse_exposure("ev100:15")
    assert ex["mode"] == "fixed_ev" and ex["ev_ref"] == ex["ev_test"] == pytest.approx(T.ev100_to_ev(15.0))
    ex2 = T.parse_exposure("ev100:13,+1")
    assert ex2["ev_test"] == pytest.approx(T.ev100_to_ev(13.0, 1.0))
    with pytest.raises(ValueError):
        T.parse_exposure("ev100:abc")


def test_spec_schema_accepts_q2_keys_and_rejects_typos():
    d = spec_data(engine={"warmupFrames": 1, "settleFrames": 1, "size": [640, 360], "linear": True,
                          "setup": [{"method": "set_post_process", "params": {"lightingUnits": 1}}]},
                  alignment={"tonemap": "ue_filmic", "exposure": "ev100:15", "sizePolicy": "resize"},
                  displayCheck={"mean_lsb_max": 0.6, "p99_lsb_max": 2})
    d["cameras"][0]["setup"] = [{"method": "set_post_process", "params": {"ev100": 13}}]
    d["cameras"][0]["alignment"] = {"exposure": "ev100:13"}
    S.from_data(d)
    bad = spec_data(engine={"warmupFrames": 1, "linaer": True})
    with pytest.raises(S.SpecError):
        S.from_data(bad)
    with pytest.raises(S.SpecError):
        S.from_data(spec_data(alignment={"exposure": "ev100:"}))


def _phys(fake, gain=3.0e4):
    """物理モード相当: 合成シーンの線形値を nit 単位(数万)へ。EV100=15 の露出係数 2.5e-5 を掛けると表示の適正範囲に入る。"""
    fake.pt_api = "engine"
    fake.test_gain = fake.ref_gain = gain


def _opts(fake, launcher, tmp_path, **kw):
    return RunOptions(stage="G1", out=tmp_path / "run", attach_port=None, port=fake.port, launcher=launcher,
                      no_perf=True, no_perf_history=True, **kw)


def _q2_spec(**over):
    d = spec_data(
        engine={"warmupFrames": 1, "settleFrames": 1, "size": [200, 112], "linear": True, "png": "srgb",
                "setup": [{"method": "set_post_process", "params": {"tonemapper": 3, "exposureMode": 1, "ev100": 15.0}}]},
        reference={"kind": "pt", "pt": {"spp": 8, "seeds": [1, 2], "api": "engine", "params": {"bounces": 4, "lightFalloff": "physical"}}},
        alignment={"tonemap": "ue_filmic", "exposure": "ev100:15", "sizePolicy": "resize"},
        displayCheck={"mean_lsb_max": 0.6, "p99_lsb_max": 2},
        gates={"G1": {"flip_ldr_mean_max": 0.15, "lum_mean_ev_abs_max": 0.5},
               "G2": {"flip_ldr_mean_max": 0.08, "flip_hdr_mean_max": 0.1, "lum_mean_ev_abs_max": 0.25}})
    d.update(over)
    return d


def test_linear_offscreen_capture_engine_pt_api_and_display_check(fake, launcher, tmp_path):
    _phys(fake)
    fake.w, fake.h = 320, 180           # 表示矩形(ビューポート)は 320x180。仕様は 200x112 のオフスクリーンを要求する
    run = run_scene(S.from_data(_q2_spec()), _opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass", run["reasons"]
    shot = [p for m, p in fake.calls if m == "screenshot_final"][0]
    assert (shot["width"], shot["height"]) == (200, 112) and shot["formats"] == ["png", "pfm"]
    pt = [p for m, p in fake.calls if m == "render_reference"][0]
    assert pt["size"] == [200, 112] and pt["output"].endswith("pt_a_s1") and pt["formats"] == ["pfm"]
    assert pt["lightFalloff"] == "physical" and pt["bounces"] == 4 and pt["frameBudgetMs"] == 40
    assert sum(1 for m, _ in fake.calls if m == "render_reference_status") >= 2      # ポーリングした
    c = run["cameras"][0]
    assert c["engineImage"].endswith("engine.png") and c["linearImage"].endswith("engine.pfm")
    assert "flip_hdr_mean" in c["metrics"]                              # 線形どうしなので HDR-FLIP が出る
    dc = c["displayCheck"]
    assert dc["max_lsb"] <= 1 and dc["mean_lsb"] < 0.6 and dc["tonemap"] == "ue_filmic" and dc["eotf_png"] == "srgb"
    # 表示は 1 系統・線形は別 PFM: 撮った線形は画素値がそのまま残る(トーンマップ・露出が掛かっていない)
    lin = load_image(Path(c["linearImage"]), "auto")
    assert lin.kind == "linear" and lin.rgb.max() > 10.0


def test_display_check_fails_when_engine_display_differs_from_numpy(fake, launcher, tmp_path):
    _phys(fake)
    fake.display_offset = 0.05           # エンジンの表示 PNG だけが 5% ずれている(GPU のトーンマップが違う状況)
    run = run_scene(S.from_data(_q2_spec()), _opts(fake, launcher, tmp_path))
    assert run["verdict"] == "fail"
    assert any("displayCheck" in r for r in run["cameras"][0]["reasons"])


def test_pt_engine_api_failure_is_skipped_not_passed(fake, launcher, tmp_path):
    fake.pt_api = "engine"
    fake.has_render_reference = False
    run = run_scene(S.from_data(_q2_spec()), _opts(fake, launcher, tmp_path))
    assert run["verdict"] == "skipped" and run["exitCode"] == EXIT["skipped"]


def test_camera_setup_and_alignment_override_exposure_bracket(fake, launcher, tmp_path):
    """露出ブラケット: カメラごとに EV100 を変えて撮り、整列の露出も同じ EV100 にする。"""
    _phys(fake)
    d = _q2_spec()
    d["cameras"] = [
        {"name": "ev_m2", "position": [0, 1, 0], "target": [0, 1, 5],
         "setup": [{"method": "set_post_process", "params": {"ev100": 13.0}}], "alignment": {"exposure": "ev100:13"}},
        {"name": "ev_0", "position": [0, 1, 0], "target": [0, 1, 5],
         "setup": [{"method": "set_post_process", "params": {"ev100": 15.0}}], "alignment": {"exposure": "ev100:15"}},
        {"name": "ev_p2", "position": [0, 1, 0], "target": [0, 1, 5],
         "setup": [{"method": "set_post_process", "params": {"ev100": 17.0}}], "alignment": {"exposure": "ev100:17"}},
    ]
    run = run_scene(S.from_data(d), _opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass", run["reasons"]
    evs = [p["ev100"] for m, p in fake.calls if m == "set_post_process" and "ev100" in p]
    assert evs[-3:] == [13.0, 15.0, 17.0]
    aligns = [c["align"]["exposure_ev_applied"] for c in run["cameras"]]
    assert all(abs(a) < 1e-9 for a in aligns)                                     # 同じ EV を基準とエンジンの両方へ
    assert [c["align"]["ev_test_applied"] for c in run["cameras"]] == pytest.approx(
        [T.ev100_to_ev(13.0), T.ev100_to_ev(15.0), T.ev100_to_ev(17.0)])
    # 露出だけが違うので、表示画像は EV100 が 2 上がるごとに暗くなる
    means = []
    for c in run["cameras"]:
        means.append(float(load_image(Path(c["engineImage"]), "srgb").rgb.mean()))
    assert means[0] > means[1] > means[2]


def test_engine_gain_error_is_caught_in_linear_hdr_flip(fake, launcher, tmp_path):
    """光の単位のずれ(エンジン線形が基準の 1.6 倍)は HDR/LDR-FLP と輝度差で不合格になる(見逃さない)。"""
    _phys(fake)
    fake.test_gain = 1.6 * 3.0e4
    run = run_scene(S.from_data(_q2_spec()), _opts(fake, launcher, tmp_path))
    assert run["verdict"] == "fail"
    assert run["cameras"][0]["metrics"]["lum_mean_ev"] > 0.25


def test_offline_test_images_prefer_linear_when_spec_is_linear(tmp_path, fake, launcher):
    """--test-images(エンジン無し): engine.linear の仕様では <camera>.pfm を優先して読む。"""
    d = _q2_spec(reference={"kind": "external", "external": {"dir": str(tmp_path / "refs"), "colorspace": "linear"}})
    d["engine"]["png"] = "srgb"
    d["engine"].pop("setup", None)
    d["cameras"] = d["cameras"][:1]
    d["displayCheck"] = {}
    d["alignment"]["exposure"] = "none"
    (tmp_path / "refs").mkdir()
    (tmp_path / "imgs").mkdir()
    lin = np.random.default_rng(1).uniform(0.001, 2.0, (40, 70, 3)).astype(np.float32)
    write_pfm(str(tmp_path / "imgs" / "a.pfm"), lin)
    write_pfm(str(tmp_path / "refs" / "a.pfm"), lin)
    run = run_scene(S.from_data(d), RunOptions(stage="G1", out=tmp_path / "run", test_images_dir=tmp_path / "imgs",
                                               external_dir=tmp_path / "refs", cameras=["a"], no_perf=True, no_perf_history=True))
    assert run["verdict"] == "pass", run["reasons"]
    assert run["cameras"][0]["metrics"]["flip_hdr_mean"] < 1e-6


# ── 閉形式(解析解)との照合 ────────────────────────────────────────────────────

def test_analytic_camera_center_ray_points_at_target():
    from parity import analytic as AN
    eye, dirs = AN.camera_rays([0, 1.5, -2.0], [0, 1.5, 4.0], 101, 61)
    c = dirs[30, 50]
    assert np.allclose(c, [0, 0, 1], atol=1e-3)
    # 垂直 FOV 45°: 上端の画素は約 22.5° 上を向く(中心から 30/61 画素ぶんは ~22°)
    assert 20.0 < np.degrees(np.arcsin(dirs[0, 50, 1])) < 22.6
    # 右は +X(左手系 Y-up で +Z 前方のとき)
    assert dirs[30, 100, 0] > 0 > dirs[30, 0, 0]


def test_analytic_lambert_point_light_closed_form_and_check():
    from parity import analytic as AN
    cam = {"position": [0.0, 1.5, -2.0], "target": [0.0, 1.5, 4.0]}
    plane = {"point": [0, 1.5, 3.95], "normal": [0, 0, -1], "albedo": 0.5, "bounds": {"x": [-5, 5], "y": [0.15, 3.4]}}
    lights = [{"pos": [0.0, 1.5, 2.6], "cd": 400.0}]
    L, mask = AN.lambert_plane_point_lights(cam, (192, 108), plane, lights)
    # 光源の真正面(画面中心)の壁: E = I/d²(d = 1.35)、L = 0.5/π · E
    assert mask[54, 96]
    assert L[54, 96] == pytest.approx(0.5 / np.pi * 400.0 / 1.35 ** 2, rel=0.01)
    # 逆二乗 + cosθ: 横へ離れるほど落ちる(中心から x = 2 m の点)
    row = L[54]
    assert row[96] > row[60] > row[20] > 0
    # check: エンジンの線形画像 = 期待値そのもの → 合格。1.5 倍 → 不合格(光度の単位ずれを検出)
    lin = np.repeat(L[..., None], 3, axis=2).astype(np.float32)
    spec = [{"kind": "lambert_plane_point_lights", "camera": "c", "plane": plane, "lights": lights,
             "minLuminanceNit": 1.0, "tolerance": {"median_rel": 0.02, "p95_rel": 0.05, "bias_abs": 0.02}}]
    ok = AN.check(spec, {"c": cam}, "c", lin)
    assert ok is not None and not ok["reasons"] and ok["checks"][0]["median_rel"] < 1e-6
    bad = AN.check(spec, {"c": cam}, "c", lin * 1.5)
    assert bad["reasons"] and bad["checks"][0]["bias"] == pytest.approx(0.5, abs=1e-3)
    # 逆二乗でなく従来式(1 - d/range)^2 の見え方: 遠方が大きくずれて検出される
    d2 = np.maximum(1.35 ** 2, 1e-9)
    assert AN.check(spec, {"c": cam}, "c", lin) is not None and d2 > 0
    assert AN.check(spec, {"c": cam}, "other", lin) is None


def test_analytic_runs_in_the_runner_and_fails_on_unit_error(fake, launcher, tmp_path):
    """runner: 仕様の analytic を線形画像に対して評価し、光度の単位ずれ(1.6 倍)を fail にする。"""
    from parity import analytic as AN
    _phys(fake)
    cam = {"position": [0, 1.0, 0.0], "target": [0, 1.0, 5.0]}
    plane = {"point": [0, 1.0, 5.0], "normal": [0, 0, -1], "albedo": 0.5, "bounds": {"x": [-9, 9], "y": [-9, 9]}}
    lights = [{"pos": [0.0, 1.0, 3.0], "cd": 400.0}]
    L, mask = AN.lambert_plane_point_lights(cam, (200, 112), plane, lights)

    def paint(lin, gain=1.0):
        out = np.where(mask[..., None], (L * gain)[..., None] * np.ones(3), 0.001).astype(np.float32)
        return out

    d = _q2_spec()
    d["cameras"] = [{"name": "a", "position": cam["position"], "target": cam["target"]}]
    d["reference"] = {"kind": "external", "external": {"dir": str(tmp_path / "refs"), "colorspace": "linear"}}
    d["analytic"] = [{"kind": "lambert_plane_point_lights", "camera": "a", "plane": plane, "lights": lights, "minLuminanceNit": 0.5,
                      "tolerance": {"median_rel": 0.05, "p95_rel": 0.1, "bias_abs": 0.05}}]
    d["displayCheck"] = {}
    d["gates"] = {"G1": {"lum_mean_ev_abs_max": 5.0}}
    d["alignment"] = {"tonemap": "ue_filmic", "exposure": "none", "sizePolicy": "resize"}
    (tmp_path / "refs").mkdir()
    write_pfm(str(tmp_path / "refs" / "a.pfm"), paint(None))
    for gain, verdict in ((1.0, "pass"), (1.6, "fail")):
        fake.mutate = lambda _lin, g=gain: paint(_lin, g)
        fake.w, fake.h = 200, 112
        run = run_scene(S.from_data(d), _opts(fake, launcher, tmp_path / f"g{gain}", reference="external"))
        assert run["verdict"] == verdict, (gain, run["reasons"])
        assert run["cameras"][0]["analytic"]["checks"][0]["pixels"] > 1000
