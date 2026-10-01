"""GI 評価ハーネス(S1)の指標・判定・マスクの単体テスト(合成画像のみ。エンジン不要)。"""
import math

import numpy as np
import pytest

from parity import gi_eval as GE
from parity import gi_metrics as GM
from parity import gi_scene as GS


def flat(h, w, rgb):
    return np.broadcast_to(np.array(rgb, np.float32), (h, w, 3)).copy()


def full(h=40, w=60):
    return np.ones((h, w), bool)


def test_luminance_rec709():
    assert GM.luminance(flat(2, 2, (1, 0, 0)))[0, 0] == pytest.approx(0.2126)
    assert GM.luminance(flat(2, 2, (1, 1, 1)))[0, 0] == pytest.approx(1.0, abs=1e-6)


def test_ev_mean_known_offset():
    pt = flat(40, 60, (0.2, 0.2, 0.2))
    ras = pt * 2 ** 1.5  # eps(2^-8)の影響で数 % 内側に寄る
    s = GM.region_stats(ras, pt, full())
    assert s["ev_mean"] == pytest.approx(1.5, abs=0.03)
    assert s["ev_logmean"] == pytest.approx(1.5, abs=0.03)
    assert GM.region_stats(pt, pt, full())["ev_mean"] == pytest.approx(0.0, abs=1e-9)


def test_ev_eps_keeps_black_finite():
    z = flat(40, 60, (0, 0, 0))
    s = GM.region_stats(z, z, full())
    assert s["ev_mean"] == 0.0 and s["black_rate_raster"] == 1.0 and s["black_rate_pt"] == 1.0
    lit = flat(40, 60, (0.1, 0.1, 0.1))
    assert math.isfinite(GM.region_stats(lit, z, full())["ev_mean"])


def test_region_mask_only_counts_inside():
    pt = flat(40, 60, (0.5, 0.5, 0.5))
    ras = pt.copy()
    m = np.zeros((40, 60), bool)
    m[:, :30] = True
    ras[:, :30] *= 4.0       # マスク内だけ +2EV
    assert GM.region_stats(ras, pt, m)["ev_mean"] == pytest.approx(2.0, abs=0.01)
    assert GM.region_stats(ras, pt, ~m)["ev_mean"] == pytest.approx(0.0, abs=1e-6)


def test_black_rate_and_rg():
    pt = flat(40, 60, (0.3, 0.1, 0.1))
    ras = pt.copy()
    ras[:20] = 0.0
    s = GM.region_stats(ras, pt, full())
    assert s["black_rate_raster"] == pytest.approx(0.5)
    assert s["black_rate_pt"] == 0.0
    assert s["rg_pt"] == pytest.approx(3.0)


def test_small_region_not_evaluable():
    m = np.zeros((40, 60), bool)
    m[0, :10] = True
    assert GM.region_stats(flat(40, 60, (1, 1, 1)), flat(40, 60, (1, 1, 1)), m)["evaluable"] is False
    assert GM.region_stats(flat(40, 60, (1, 1, 1)), flat(40, 60, (1, 1, 1)), np.zeros((40, 60), bool))["pixels"] == 0


def _st(ev_target, shape=(40, 60)):
    pt = flat(*shape, (0.2, 0.2, 0.2))
    return GM.region_stats(pt * 2 ** ev_target, pt, full(*shape))


def test_judge_a1():
    ok = GM.judge_a1({"main": {"walls": _st(0.3), "ceiling": _st(-0.4), "floor": _st(0.0)}}, ["walls", "ceiling", "floor"])
    assert ok["status"] == GM.PASS
    bad = GM.judge_a1({"main": {"walls": _st(0.3), "ceiling": _st(2.0), "floor": _st(0.0)}}, ["walls", "ceiling", "floor"])
    assert bad["status"] == GM.FAIL and "ceiling" in bad["reason"]
    assert GM.judge_a1({"main": {}}, ["walls"])["status"] == GM.UNJUDGEABLE


def test_judge_a2():
    pt = flat(40, 60, (0, 0, 0))
    dark = GM.region_stats(pt.copy(), pt, full())
    assert GM.judge_a2(dark)["status"] == GM.PASS
    leak = GM.region_stats(flat(40, 60, (0.05, 0.05, 0.05)), pt, full())
    r = GM.judge_a2(leak)
    assert r["status"] == GM.FAIL and r["black_rate_diff_pt"] == pytest.approx(-100.0)
    assert GM.judge_a2(None)["status"] == GM.UNJUDGEABLE


def test_judge_a3():
    pt = flat(40, 60, (0.3, 0.2, 0.2))      # R/G = 1.5
    same = GM.region_stats(pt * 0.5, pt, full())   # 同じ色味
    assert GM.judge_a3(same)["status"] == GM.PASS
    grey = GM.region_stats(flat(40, 60, (0.2, 0.2, 0.2)), pt, full())      # R/G = 1.0 (R>G でない)
    assert GM.judge_a3(grey)["status"] == GM.FAIL
    too_red = GM.region_stats(flat(40, 60, (0.5, 0.2, 0.2)), pt, full())    # 2.5 vs 1.5
    assert GM.judge_a3(too_red)["status"] == GM.FAIL
    nored = GM.region_stats(pt, flat(40, 60, (0.2, 0.2, 0.2)), full())      # PT が R>G でない
    assert GM.judge_a3(nored)["status"] == GM.UNJUDGEABLE


def test_judge_a4():
    pt = flat(40, 60, (0.1, 0.1, 0.1))
    leg = GM.region_stats(flat(40, 60, (0.8, 0.8, 0.8)), pt, full())
    darker = GM.region_stats(flat(40, 60, (0.3, 0.3, 0.3)), pt, full())     # -1.4EV
    close = GM.region_stats(flat(40, 60, (0.7, 0.7, 0.7)), pt, full())      # -0.19EV
    assert GM.judge_a4(darker, leg, False)["status"] == GM.PASS
    assert GM.judge_a4(close, leg, False)["status"] == GM.FAIL
    assert GM.judge_a4(leg, leg, True)["status"] == GM.NA


def test_signed_ev_map_colors():
    pt = flat(4, 4, (0.2, 0.2, 0.2))
    up = GE.signed_ev_map(pt * 8, pt)[0, 0]       # +3EV: 赤
    dn = GE.signed_ev_map(pt / 8, pt)[0, 0]       # -3EV: 青
    ok = GE.signed_ev_map(pt, pt)[0, 0]           # 0: 白
    assert up[0] == 1 and up[1] < 0.05 and dn[2] == 1 and dn[0] < 0.1 and np.allclose(ok, 1.0)


# ── シーン・マスク ──────────────────────────────────────────────────────────

def _mini_scene():
    d = {"id": "t", "spec": {"entities": [
        {"name": "Floor", "kind": "box", "at": [0, -0.5, 0], "size": [20, 1, 20], "color": "#808080"},
        {"name": "Ball", "kind": "sphere", "at": [0, 1.0, 5], "size": 2.0, "color": "#ff0000"},
        {"name": "Lamp", "kind": "light", "light": "point", "at": [0, 3, 0], "components": {"pointLight": {"intensity": 5, "range": 8}}},
    ]}, "cameras": [{"name": "c", "position": [0, 1.0, 0], "target": [0, 1.0, 5]}],
         "regions": {"floor": {"entities": ["Floor"]}, "ball": {"entities": ["Ball"]},
                     "floor_near_ball": {"entities": ["Floor"], "nearEntity": "Ball", "maxDist": 1.5}},
         "ddgi": {"min": [-3, 0, -3], "max": [3, 3, 3], "spacing": 1.0}, "sun": {}}
    return GS.GiScene("t", d, None)


def test_masks_from_ray_casting():
    s = _mini_scene()
    m = GS.build_masks(s, s.cameras[0], 160, 90, erode_px=0)
    # 画面中央は球、下半分は床、球と床は排他
    assert m["ball"][45, 80] and not m["floor"][45, 80]
    assert m["floor"][85, 80] and not m["ball"][85, 80]
    assert not (m["ball"] & m["floor"]).any()
    assert not m["floor"][0, 80]                      # 画面の上端は空(床は地平線より下だけ)
    # 球の見かけの半径: 距離 5・半径 1 → 画角 45° なら画面高の 1/2 に対し tan = 0.2/0.414 ≈ 0.48 → 直径は約 0.48 * 90 = 43px 前後
    h = int(m["ball"][:, 80].sum())
    assert 38 <= h <= 48
    assert 0 < m["floor_near_ball"].sum() < m["floor"].sum()
    me = GS.build_masks(s, s.cameras[0], 160, 90, erode_px=2)
    assert me["ball"].sum() < m["ball"].sum()


def test_ddgi_grid_covers_bounds():
    g = GS.ddgi_grid({"min": [-4.0, 0.0, -3.0], "max": [4.0, 3.2, 3.0], "spacing": 0.8})
    assert (g["probeCountX"], g["probeCountY"], g["probeCountZ"]) == (10, 4, 8)
    for ax, (lo, hi) in zip("XYZ", [(-4.0, 4.0), (0.0, 3.2), (-3.0, 3.0)]):
        o, n = g[f"origin{ax}"], g[f"probeCount{ax}"]
        assert lo <= o and o + (n - 1) * 0.8 <= hi + 1e-9


def test_engine_scene_conversion():
    sc = GS.to_engine_scene(_mini_scene())
    names = [e["name"] for e in sc["entities"]]
    assert "Sun" not in names and "Grid" not in names
    floor = sc["entities"][0]
    assert floor["primitive"] == "box" and floor["transform"]["scale"] == [20.0, 1.0, 20.0]
    assert floor["color"][0] == pytest.approx(0.2158, abs=1e-3)           # #80 -> リニア
    assert sc["entities"][1]["transform"]["scale"] == [2.0, 2.0, 2.0]     # 球の直径 = scale
    assert sc["entities"][2]["pointLight"]["intensity"] == 5
    assert sc["ssgi"]["enabled"] is False and sc["raytracing"]["ddgi"]["enabled"] is False


def test_shipped_scenes_load_and_have_regions():
    scenes = GS.discover_scenes(GE.SCENES_DIR)
    assert {s.id for s in scenes} == {"gr1", "gr2", "gr3", "gr4"}
    for s in scenes:
        names = [e["name"] for e in s.entities]
        assert "Sun" not in names and "Grid" not in names
        assert s.data["sun"]["ambient"] == 0
        for cam in s.cameras:
            m = GS.build_masks(s, cam, 320, 180)
            assert sum(int(v.sum()) for v in m.values()) > 0
    gr1 = next(s for s in scenes if s.id == "gr1")
    assert gr1.camera("main")["position"] == [3.3, 1.7, -2.7]
