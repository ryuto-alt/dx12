"""ライティング評価(LS-00b)の影の指標・シーン・評価ハーネスの単体テスト(合成画像のみ。エンジン不要)。"""
import json
import os
import math
from pathlib import Path

import numpy as np
import pytest

from parity import gi_scene as GS
from parity import lighting_eval as LE
from parity import lighting_scene as LS
from parity import shadow_metrics as SM
from parity.imgio import write_pfm

H, W = 120, 200


def full():
    return np.ones((H, W), bool)


def img(y):
    return np.repeat(np.asarray(y, np.float32)[..., None], 3, axis=2)


def ramp_shadow(x0=80, width=0, lit=1.0, dark=0.05):
    """左 = 影・右 = 日向の縦の縁。width > 0 なら縁を線形の半影にする。"""
    xs = np.arange(W, dtype=np.float32)
    t = np.clip((xs - x0) / max(width, 1e-6), 0, 1) if width > 0 else (xs >= x0).astype(np.float32)
    row = dark + (lit - dark) * t
    return img(np.broadcast_to(row, (H, W)).copy())


def test_shadow_term_normalises_by_lit_level():
    y = np.full((H, W), 0.4)
    y[:, :60] = 0.1
    lvl = SM.lit_level(y, full())
    assert lvl == pytest.approx(0.4)
    s = SM.shadow_term(y, lvl)
    assert s[0, 0] == pytest.approx(0.25) and s[0, -1] == pytest.approx(1.0)
    assert SM.shadow_term(y, 0.0).min() == 1.0


def test_coverage_identical_is_one():
    a = ramp_shadow()
    c = SM.coverage(a, a, full())
    assert c["ratio"] == pytest.approx(1.0) and c["iou"] == pytest.approx(1.0) and c["missing_px"] == 0 and c["evaluable"]


def test_coverage_detects_missing_shadow():
    pt = ramp_shadow(x0=80)
    ras = ramp_shadow(x0=40)            # ラスタの影が狭い(左 40 列だけ)
    c = SM.coverage(ras, pt, full())
    assert c["ratio"] == pytest.approx(40 / 80, abs=0.02)
    assert c["captured"] == pytest.approx(40 / 80, abs=0.02)
    assert c["missing_px"] == 40 * H and c["extra_px"] == 0


def test_coverage_not_evaluable_when_pt_has_no_shadow():
    flat = img(np.full((H, W), 0.5))
    c = SM.coverage(flat, flat, full())
    assert c["evaluable"] is False


def test_acne_counts_lit_pixels_turned_dark():
    pt = ramp_shadow(x0=80)
    ras = pt.copy()
    ras[10:20, 120:140] = 0.2           # 日向の 10x20 = 200 画素が暗い(s = 0.2 < 0.75)
    a = SM.acne_rate(ras, pt, full())
    lit_px = int(((SM.terms(ras, pt, full())[1]) >= SM.LIT_S).sum())
    assert a["evaluable"] and a["acne_px"] == 200 and a["rate"] == pytest.approx(200 / lit_px)
    assert SM.acne_rate(pt, pt, full())["rate"] == 0.0


def test_acne_ignores_already_dark_pt_pixels():
    pt = ramp_shadow(x0=80)
    ras = pt.copy()
    ras[:, :80] = 0.0                   # PT の影の中を更に暗くしても数えない
    assert SM.acne_rate(ras, pt, full())["rate"] == 0.0


@pytest.mark.parametrize("width", [4, 8, 16])
def test_edge_width_matches_ramp_width(width):
    s = SM.shadow_term(SM.luminance(ramp_shadow(x0=90, width=width)), 1.0)
    e = SM.edge_width(s, full(), band_px=20, inner_px=3)
    # s = 0.05 + 0.95 t(線形)。半影(0.1 < s < 0.9)の t の幅 = (0.9 - 0.1) / 0.95
    expect = width * (0.9 - 0.1) / 0.95
    assert e["evaluable"]
    assert e["width_px"] == pytest.approx(expect, rel=0.3, abs=1.0)


def test_edge_width_error_relative():
    pt = ramp_shadow(x0=90, width=8)
    ras = ramp_shadow(x0=90, width=4)
    r = SM.edge_width_error(ras, pt, full(), band_px=20)
    assert r["evaluable"] and r["width_diff_px"] < 0 and 0.2 < r["error_rel"] < 0.8
    same = SM.edge_width_error(pt, pt, full(), band_px=20)
    assert same["error_rel"] == pytest.approx(0.0)


def test_edge_width_hard_edge_is_about_zero():
    e = SM.edge_width(SM.shadow_term(SM.luminance(ramp_shadow()), 1.0), full())
    assert e["evaluable"] and e["width_px"] < 0.5


@pytest.mark.parametrize("dark", [0.0, 0.05, 0.2, 0.35])
def test_edge_width_hard_edge_with_ambient_fill_is_zero(dark):
    # 本影が環境光で持ち上がっていても、硬い縁は 0 px(環境光を半影と数えない)
    r = SM.edge_width_error(ramp_shadow(dark=dark), ramp_shadow(dark=dark), full(), band_px=12)
    assert r["evaluable"]
    assert r["raster"]["width_px"] < 0.5 and r["pt"]["width_px"] < 0.5


@pytest.mark.parametrize("dark", [0.05, 0.2, 0.35])
def test_edge_width_ramp_independent_of_umbra_fill(dark):
    r = SM.edge_width_error(ramp_shadow(x0=90, width=10, dark=dark), ramp_shadow(x0=90, width=10, dark=dark), full(), band_px=20)
    assert r["raster"]["width_px"] == pytest.approx(10 * 0.8, rel=0.25, abs=1.0)


def test_edge_width_error_ignores_fill_difference():
    # ラスタは本影が明るい(0.35)・PT は暗い(0.05)が、縁の幅は同じ → 誤差はほぼ 0
    pt = ramp_shadow(x0=90, width=8, dark=0.05)
    ras = ramp_shadow(x0=90, width=8, dark=0.35)
    r = SM.edge_width_error(ras, pt, full(), band_px=20)
    assert r["evaluable"] and r["error_rel"] < 0.15
    # 縁は硬いのに本影が明るいだけのラスタを、半影の広いものと誤判定しない
    hard = SM.edge_width_error(ramp_shadow(dark=0.35), ramp_shadow(x0=90, width=8, dark=0.05), full(), band_px=20)
    assert hard["raster"]["width_px"] < 0.5 and hard["error_rel"] > 0.7


def test_edge_width_no_contrast_is_not_evaluable():
    r = SM.edge_width_error(img(np.full((H, W), 0.5)), img(np.full((H, W), 0.5)), full())
    assert r["evaluable"] is False


def test_temporal_stats_static_and_flicker():
    base = np.full((H, W, 3), 128, np.int16)
    static = np.stack([base] * 8)
    t = SM.temporal_stats(static)
    assert t["max_range"] == 0 and t["over_1_rate"] == 0.0 and t["sigma_mean"] == 0.0
    fl = static.copy()
    fl[3, 10:20, 5:15, :] = 125         # 1 フレームだけ 3 段暗い画素
    t2 = SM.temporal_stats(fl)
    assert t2["max_range"] == 3 and t2["over_1_rate"] == pytest.approx(100 / (H * W))
    m = np.zeros((H, W), bool)
    m[100:, :] = True
    assert SM.temporal_stats(fl, m)["max_range"] == 0       # 領域外のちらつきは数えない
    assert SM.temporal_stats(static[:1])["evaluable"] is False


def test_to_8bit_rounds():
    assert SM.to_8bit(np.array([0.0, 1.0, 0.5, 2.0, -1.0])).tolist() == [0, 255, 128, 255, 0]


def test_csm_splits_match_engine_numbers():
    # Application.h のコメント「分割 ≒ 31/64/125/400m」(lambda 0.7・near 0.1・far 400)
    sp = SM.csm_splits(0.1, 400.0, 4, 0.7)
    assert sp[-1] == pytest.approx(400.0)
    assert [round(x) for x in sp[:3]] == [31, 64, 125] or abs(sp[0] - 31) < 2


def test_csm_radii_monotonic_and_rounded():
    r = SM.csm_radii(0.1, 400.0, 45.0, 16 / 9)
    assert all(b > a for a, b in zip(r, r[1:]))
    assert all(abs(x * 16 - round(x * 16)) < 1e-9 for x in r)


def test_texel_density_scales_with_res_and_cascade():
    a = SM.texel_density(5.0)
    b = SM.texel_density(20.0)
    assert a["cascade"] == 0 and b["cascade"] == 0
    assert b["texels_per_px"] > a["texels_per_px"]               # 同じカスケードでは遠いほど 1 画素が大きい = 密度は上がる
    c = SM.texel_density(5.0, res=4096)
    assert c["texels_per_px"] == pytest.approx(a["texels_per_px"] * 2)
    assert SM.texel_density(1000.0)["cascade"] == 3
    assert [d["distance"] for d in SM.density_bands([5, 10])] == [5, 10]


def test_texel_density_pixel_footprint():
    t = SM.texel_density(10.0, height_px=720, fov_y_deg=90.0)
    assert t["pixel_m"] == pytest.approx(2 * 10.0 * math.tan(math.radians(45)) / 720)


# ── シーン ──────────────────────────────────────────────────────────────────

def test_scene_files_load_and_match_generator():
    import importlib.util
    spec = importlib.util.spec_from_file_location("lighting_scenes_gen", Path(__file__).resolve().parents[1] / "gen" / "lighting_scenes.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    scenes = LS.load_scenes()
    assert [s.id for s in scenes] == ["LS-A", "LS-B", "LS-C", "LS-D", "LS-E", "LS-F"]
    for sid, fn in mod.SCENES.items():       # 生成器と書き出し済みの JSON が一致する(手で JSON を直さない)
        disk = json.loads((LS.SCENES_DIR / f"{sid}.json").read_text(encoding="utf-8"))
        assert disk == json.loads(json.dumps(fn()))


def test_scenes_have_the_documented_conditions():
    by = {s.id: s for s in LS.load_scenes()}
    assert by["LS-A"].data["conditions"] == ["high", "epic"]                 # High(RT OFF)と Epic(RT ON)の 2 条件
    assert by["LS-A"].data["models"][0]["path"].endswith("mannequin.glb")    # NB14 のスキンドのマネキン
    vs = by["LS-F"].variants()
    assert vs[1]["id"] == "evening10" and vs[1]["sun"]["elevation"] == 10
    assert by["LS-F"].data["temporal"]["frames"] == 8
    for s in by.values():
        for c in s.data["conditions"]:
            assert c in LE.CONDITIONS


def test_mannequin_file_exists_in_repo():
    from parity import paths
    assert (paths.REPO_ROOT / "assets" / "engine" / "characters" / "mannequin" / "mannequin.glb").exists()


def test_sun_direction_convention():
    assert LS.to_toward_sun(90, 0) == pytest.approx([1, 0, 0], abs=1e-9)
    assert LS.to_toward_sun(0, 90) == pytest.approx([0, 1, 0], abs=1e-9)
    assert LS.to_toward_sun(270, 33)[0] < 0                                  # GR-1 は西(-x)から差す


def test_occluded_slab():
    lo, hi = np.array([-1.0, 0.0, -1.0]), np.array([1.0, 2.0, 1.0])
    pos = np.zeros((1, 3, 3))
    pos[0, 0] = [3.0, 0, 0]       # 箱の +x 側。太陽が +x → 遮られない
    pos[0, 1] = [-3.0, 0, 0]      # 箱の -x 側。太陽が +x → 箱が太陽との間にある
    pos[0, 2] = [-3.0, 0, 5.0]    # 箱とは z がずれている
    d = np.array([1.0, 0.3, 0.0])
    d /= np.linalg.norm(d)
    assert LS.occluded(pos, d, [(lo, hi)]).tolist() == [[False, True, False]]


def test_shadow_masks_cover_expected_area():
    s = {x.id: x for x in LS.load_scenes()}["LS-A"]
    m = LS.build_shadow_masks(s, s.camera("main"), s.data["sun"], 320, 180)
    assert m["shadow_tower"].sum() > 300 and m["lit_ground"].sum() > 1000
    assert not (m["lit_ground"] & m["shadow_tower"]).any()                    # 日向の領域は影の領域と重ならない


# ── 評価ハーネス(撮影済み画像から) ────────────────────────────────────────

def _synth_dir(root, wiggle=False):
    s = {x.id: x for x in LS.load_scenes()}["LS-F"]
    cam = s.camera("main")
    sun = s.variants()[0]["sun"]
    w, h = 320, 180
    masks = LS.build_shadow_masks(s, cam, sun, w, h)
    d = root / "LS-F" / "noon" / "main"
    d.mkdir(parents=True)
    # PT: 地面 0.5、塔の領域の左半分を影 0.05。ラスタ: PT と同じ + 日向の 3 画素のアクネ
    pt = np.full((h, w), 0.5, np.float32)
    ys, xs = np.nonzero(masks["shadow_tower"])
    cut = int(np.median(xs))
    pt[ys[xs < cut], xs[xs < cut]] = 0.05
    ras = pt.copy()
    ly, lx = np.nonzero(masks["lit_ground"])
    ras[ly[:3], lx[:3]] = 0.1
    write_pfm(d / "pt.pfm", img(pt))
    write_pfm(d / "raster_epic.pfm", img(ras))
    for k in range(8):
        f = ras * 2.0 if (wiggle and k == 3) else ras
        write_pfm(d / f"raster_epic_t{k:02d}.pfm", img(f))
    return root


def _run(root):
    rc = LE.main(["--from-dir", str(root), "--scenes", "LS-F", "--conditions", "epic", "--no-flip", "--quiet", "--width", "320", "--height", "180"])
    return rc, json.loads((root / "lighting_eval.json").read_text(encoding="utf-8"))


def test_from_dir_end_to_end(tmp_path):
    _synth_dir(tmp_path)
    rc, res = _run(tmp_path)
    assert rc == 0
    c = res["scenes"]["LS-F"]["variants"]["noon"]["cameras"]["main"]["conditions"]["epic"]
    assert c["shadow"]["shadow_tower"]["coverage"]["ratio"] == pytest.approx(1.0)
    assert c["shadow"]["lit_ground"]["acne"]["acne_px"] == 3
    assert c["temporal"]["full"]["max_range"] == 0
    crit = res["scenes"]["LS-F"]["variants"]["noon"]["criteria"]["epic"]
    assert crit["coverage"]["status"] == "PASS" and crit["temporal"]["status"] == "PASS" and crit["acne"]["status"] == "PASS"
    assert any(k.endswith("/shadow/shadow_tower/coverage_ratio") for k in res["scorecard"])
    assert (tmp_path / "summary.txt").exists() and (tmp_path / "LS-F" / "noon" / "main" / "masks.png").exists()


def test_temporal_flicker_fails_and_compare_detects(tmp_path):
    a_dir, b_dir = tmp_path / "a", tmp_path / "b"
    a_dir.mkdir()
    b_dir.mkdir()
    _synth_dir(a_dir)
    _synth_dir(b_dir, wiggle=True)
    _, a = _run(a_dir)
    _, b = _run(b_dir)
    assert LE.compare(a, a)["deterministic"]
    assert b["scenes"]["LS-F"]["variants"]["noon"]["criteria"]["epic"]["temporal"]["status"] == "FAIL"
    diff = LE.compare(a, b)
    assert not diff["deterministic"] and any(k.endswith("temporal/p99_range") for k in diff["outOfTolerance"])


def test_compare_cli(tmp_path):
    _synth_dir(tmp_path)
    _run(tmp_path)
    p = tmp_path / "lighting_eval.json"
    assert LE.main(["--compare", str(p), str(p)]) == 0


# ── レビュー修正(縁の幅の正規化・夕方の後方の遮蔽物・判定・条件) ──────────────────

def test_ls_f_behind_caster_shadow_lands_in_view_at_evening():
    s = {x.id: x for x in LS.load_scenes()}["LS-F"]
    cam = s.camera("main")
    ev = next(v for v in s.variants() if v["id"] == "evening10")
    m = LS.build_shadow_masks(s, cam, ev["sun"], 320, 180)
    assert m["shadow_behind"].sum() > 300                      # 後方の塔の影がカメラの前に落ちる
    cz = s.data["shadowRegions"]["shadow_behind"]["occluders"]
    assert cz == ["ENV_BehindCaster"]
    ent = next(e for e in s.entities if e["name"] == "ENV_BehindCaster")
    assert ent["at"][2] < cam["position"][2]                   # カメラの後ろ(太陽側)にある
    assert s.data["criteria"]["coverage_behind"]["variant"] == "evening10"


def test_judge_variant_scoping_and_information_criteria():
    s = {x.id: x for x in LS.load_scenes()}["LS-F"]
    cov = {"coverage": {"evaluable": True, "ratio": 1.4, "captured": 0.5}, "acne": {"evaluable": False}}
    cams = {"main": {"conditions": {"epic": {"shadow": {"shadow_tower": cov, "shadow_behind": cov}}}}}
    noon = LE.judge(s, "noon", cams, "epic")
    ev = LE.judge(s, "evening10", cams, "epic")
    assert noon["coverage"]["status"] == "FAIL" and "coverage_behind" not in noon       # ratio ではなく captured で判定
    assert ev["coverage_behind"]["status"] == "FAIL" and "coverage" not in ev
    a = {x.id: x for x in LS.load_scenes()}["LS-A"]
    ja = LE.judge(a, "default", {}, "epic")
    assert ja["edge_char"]["status"].startswith("情報のみ")


def test_conditions_use_real_sun_size_and_lane_port():
    assert LE.DEFAULT_PORT == 8855
    rt = dict(LE.CONDITIONS["epic"]["set"])["set_dxr"]
    assert rt["shadowSunAngle"] == pytest.approx(0.53) and LE.SUN_RADIUS_DEG == pytest.approx(0.265)


def test_compare_deterministic_keys_are_tight():
    a = {"scorecard": {"X/default/main/epic/global/ev_mean": 0.5, "X/default/main/epic/temporal/max_range": 3.0}}
    b = {"scorecard": {"X/default/main/epic/global/ev_mean": 0.5 + 1e-3, "X/default/main/epic/temporal/max_range": 5.0}}
    r = LE.compare(a, b)
    assert r["outOfTolerance"] == ["X/default/main/epic/global/ev_mean"]      # max_range は情報のみ(比較に使わない)


def test_entry_ps1_parses_to_expected_statements():
    """入口スクリプトが壊れていない(コメントの `\r` が改行化して `elease` という文になった事故の再発防止)。pwsh の AST で全コマンドの名前を調べる。"""
    import shutil
    import subprocess
    pwsh = shutil.which("pwsh")
    if not pwsh:
        pytest.skip("pwsh が無い")
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for name in ("lighting_eval.ps1", "gi_eval.ps1"):
        path = os.path.join(here, name)
        code = ("$e=$null;$t=$null;$a=[System.Management.Automation.Language.Parser]::ParseFile($env:PS1_PATH,[ref]$t,[ref]$e);"
                "if($e.Count){$e|%{$_.Message};exit 3};"
                "$a.FindAll({param($n)$n -is [System.Management.Automation.Language.CommandAst]},$true)|%{$_.GetCommandName()}")
        r = subprocess.run([pwsh, "-NoProfile", "-Command", code], env={**os.environ, "PS1_PATH": path}, capture_output=True, text=True, encoding="utf-8", errors="replace")
        assert r.returncode == 0, (r.stdout or "") + (r.stderr or "")
        names = {x.strip() for x in (r.stdout or "").splitlines() if x.strip()}
        allowed = {"Test-Path", "Join-Path", "Split-Path", "Write-Error", "Push-Location", "Pop-Location", "exit", "&", "Write-Host", "Write-Output"}
        assert names <= allowed, f"{name}: 想定外のコマンド {sorted(names - allowed)}"
