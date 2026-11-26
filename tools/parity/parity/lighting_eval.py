"""ライティング評価ハーネス(LS-00b)。検証シーン LS-A〜F を「条件ごとのラスタ」と「パストレーサー(PT)」で撮り、影・暗部・時間的な安定の指標を JSON にする。

  pwsh -NoProfile -File tools\\parity\\lighting_eval.ps1                          # 全シーン × 各シーンの既定の条件(LS-A は High と Epic)
  pwsh -NoProfile -File tools\\parity\\lighting_eval.ps1 --scenes LS-A --conditions epic
  pwsh -NoProfile -File tools\\parity\\lighting_eval.ps1 --from-dir <出力先>      # 撮影済みの画像から指標だけ再計算(エンジン不要)
  pwsh -NoProfile -File tools\\parity\\lighting_eval.ps1 --compare a.json b.json   # 2 回の結果の差(決定論の範囲か)を判定

流れ: 使い捨てプロジェクト(検証シーンの JSON + マネキンの glb)を書く → engine_instance.ps1 で背景起動(headless。窓は出ない・マウスに触らない)
  → シーン・variant(太陽)・カメラごとに { PT を 1 回 / 条件ごとにラスタ(決定論モード) / 時間のシーンは静止 N フレーム } → 指標 → JSON → エンジン停止。
出力: <repo>/.dx12/parity/lighting/<日時>/(git 管理外)。 lighting_eval.json(指標・合否)/ scorecard(平らなキー → 数値。--compare と W0 スコアカード用)/ summary.txt /
  <scene>/<variant>/<camera>/{pt.pfm, raster_<cond>.pfm, raster_<cond>_t00..tNN.pfm, sheet_<cond>.png, masks.png}。

★条件(CONDITIONS): どれも **DLSS OFF**(set_upscaler mode off。時間のフレーム列だけ TAA)・**決定論モード**(screenshot_final deterministic:true)で撮る。毎回全項目を明示する。
  flat = 全部 OFF(PT のための基準の状態)/ high = RT OFF(CSM + PCSS + コンタクトシャドウ + SSAO)/ epic = RT 影 ON + DDGI ON + コンタクトシャドウ + SSAO /
  epic_fog = epic + 体積霧(LS-E)。プリセット名で切り替える set_quality_preset は C7 の後で使えるようになる。それまでは各項目を直接指定する(下の表)。
★判定(criteria)はシーン JSON が持つ(coverage ≥ 0.95・アクネ < 0.1%・静止 8 フレームの差 ≤ 1/255 など)。ベースラインの段階では PASS/FAIL を数えず、数値を記録する。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import sys
import time
from pathlib import Path

import numpy as np

from . import gi_eval as GE
from . import gi_metrics as GM
from . import gi_scene as GS
from . import heatmap as H
from . import lighting_scene as LS
from . import paths
from . import shadow_metrics as SM
from .engine import EngineClient, EngineError, PwshLauncher, wait_ping
from .imgio import load_image, save_png

DEFAULT_PORT = 8855            # lane-d(NB)の MCP ポート。他のレーンのポートは使わない
DEFAULT_NAME = "lighteval"
SCHEMA = 1

# ── 条件 ────────────────────────────────────────────────────────────────────
_COMMON = [("set_ssgi", {"enabled": False}), ("set_ssr", {"enabled": False}), ("set_upscaler", {"mode": "off"}),
           ("set_volumetric_fog", {"enabled": False})]
_RT_OFF = ("set_dxr", {"shadowEnabled": False, "aoEnabled": False, "ddgiEnabled": False, "forceBuildTlas": False})
# 太陽の大きさ: 実際の太陽の角直径 0.53 度。Epic は RT 影に shadowSunAngle=0.53(エンジンの既定 = 実際の Epic の値)、PT は半径 0.265 度の円盤(SUN_RADIUS_DEG)。
SUN_DIAMETER_DEG = 0.53
SUN_RADIUS_DEG = SUN_DIAMETER_DEG / 2.0
_RT_ON = ("set_dxr", {"shadowEnabled": True, "shadowSunAngle": SUN_DIAMETER_DEG, "aoEnabled": False, "ddgiEnabled": True, "ddgiBounceIntensity": 1.0,
                      "ddgiIntensity": 1.0, "ddgiHysteresis": 0.97, "ddgiRayLength": 30.0, "ddgiNormalBias": 0.02, "{grid}": True})
CONDITIONS: dict[str, dict] = {
    "flat": {"title": "全 OFF(PT 撮影時の基準の状態。CSM は PCF)",
             "set": _COMMON + [("set_ssao", {"enabled": False}), ("set_contact_shadow", {"enabled": False}),
                               ("set_shadow_pcss", {"enabled": False}), _RT_OFF]},
    "high": {"title": "High(RT OFF): CSM + PCSS + コンタクトシャドウ + SSAO",
             "set": _COMMON + [("set_ssao", {"enabled": True}), ("set_contact_shadow", {"enabled": True}),
                               ("set_shadow_pcss", {"enabled": True}), _RT_OFF]},
    "epic": {"title": "Epic(RT ON): RT 影 + DDGI + コンタクトシャドウ + SSAO(動的な影は CSM + PCSS)",
             "set": _COMMON + [("set_ssao", {"enabled": True}), ("set_contact_shadow", {"enabled": True}),
                               ("set_shadow_pcss", {"enabled": True}), _RT_ON]},
    "epic_fog": {"title": "Epic + 体積霧(LS-E。PT は霧を持たない)",
                 "set": _COMMON[:-1] + [("set_ssao", {"enabled": True}), ("set_contact_shadow", {"enabled": True}),
                                        ("set_shadow_pcss", {"enabled": True}), _RT_ON,
                                        ("set_volumetric_fog", {"enabled": True, "density": 0.02, "sunIntensity": 1.0, "lightScattering": True,
                                                                "temporal": False})]},
}
PT_CONDITION = "flat"


def _split_grid(sets: list, scene: GS.GiScene) -> list:
    out = []
    for method, args in sets:
        a = dict(args)
        if "{grid}" in a:
            a.pop("{grid}")
            if scene.data["ddgi"].get("follow"):
                # 開けた大きな地面のシーン: 固定の格子だと外側が黒く落ちるので、カメラ追従の 2 カスケード(GI の既定の構成)にする
                a.update({"ddgiFollowCamera": True, "ddgiSpacing": 0.8, "ddgiSpacing1": 2.0, "ddgiProbeCountX": 22,
                          "ddgiProbeCountY": 8, "ddgiProbeCountZ": 22, "ddgiBudgetMs": 1.0})
            else:
                a.update(GE._grid_params(scene.data["ddgi"]))
        out.append((method, a))
    return out


def apply_condition(cli: EngineClient, name: str, scene: GS.GiScene, upscaler: str | None = None) -> None:
    for method, args in _split_grid(CONDITIONS[name]["set"], scene):
        if upscaler and method == "set_upscaler":
            args = {"mode": upscaler}
        cli.call(method, args, timeout=60)


# ── 撮影 ────────────────────────────────────────────────────────────────────

def write_project(scenes: list[GS.GiScene], out: Path) -> dict[str, str]:
    rels = GS.write_project(scenes, out)
    src = paths.REPO_ROOT / "assets" / "engine" / "characters" / "mannequin" / "mannequin.glb"
    if any(s.data.get("models") for s in scenes):
        if not src.exists():
            raise FileNotFoundError(f"マネキンが無い: {src}(NB14)")
        dst = out / "assets" / "models"
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst / "mannequin.glb")
    return rels


def capture_native(cli: EngineClient, base: Path) -> str:
    """撮影サイズを指定せず(= 描画解像度のまま。--view-size で固定済み)screenshot_final で撮る。解像度の切り替えも履歴の破棄も起きない。"""
    r = cli.call("screenshot_final", {"path": str(base), "format": "pfm", "gizmos": False}, timeout=240)
    cand = (r.get("files") or {}).get("pfm") if isinstance(r.get("files"), dict) else None
    f = Path(cand) if cand else base.with_suffix(".pfm")
    if not f.exists():
        raise RuntimeError(f"ラスタの PFM が書かれていない: {f}(応答: {str(r)[:300]})")
    return str(f)


def place_models(cli: EngineClient, scene: GS.GiScene, walk: bool) -> None:
    for m in LS.mannequin_models(scene):
        cli.call("spawn_model", {"name": m["name"], "path": "models/mannequin.glb", "position": m["at"]}, timeout=120)
        if walk:
            cli.call("play_anim", {"name": m["name"], "clipName": "Walk", "loop": True}, timeout=60)
    GE.step(cli, 4)


def cdir_of(out: Path, sid: str, vid: str, cam: str) -> Path:
    return out / sid / vid / cam


def capture_all(a, scenes: list[GS.GiScene], out: Path, log) -> dict:
    proj = out / "_project"
    rels = write_project(scenes, proj)
    from .runner import _open_scene
    size = (a.width, a.height)
    manifest: dict = {"engine": {}, "retries": [], "scenes": {}}
    state = {"launcher": None, "client": None}

    def start():
        # 起動直後の ping で接続が切れる(別のプロセスが GPU を占有している等)ことがあるので、起動し直して 3 回まで試す
        for attempt in range(3):
            try:
                _start()
                return
            except (EngineError, OSError) as e:
                manifest["retries"].append({"start": attempt, "error": str(e)[:200]})
                log(f"  !! エンジンの起動に失敗({type(e).__name__}: {str(e)[:120]})")
                stop()
                if a.attach or attempt == 2:
                    raise
                time.sleep(5)

    def _start():
        if a.attach:
            port = a.attach
        else:
            port = a.port
            state["launcher"] = PwshLauncher(name=DEFAULT_NAME, port=port, mode="headless", build_dir=a.build_dir)
            log(f"エンジンを起動: name={DEFAULT_NAME} port={port}")
            # ★描画解像度を撮影サイズに固定して起動する。固定しないと、撮影ごとに解像度が切り替わり(screenshot_final の width/height)、
            #   TAA の履歴が毎回捨てられて静止フレームが「蓄積の無い絵」になる(時間的な安定の指標がちらつきの偽物になる)。
            state["launcher"].start(str(proj), [f"--view-size={size[0]}x{size[1]}"])
        state["client"] = EngineClient(port, connect_timeout=(30 if a.attach else 120))
        manifest["engine"] = {"ping": wait_ping(state["client"], 240.0)}

    def stop():
        if state["client"] is not None:
            state["client"].close()
            state["client"] = None
        if state["launcher"] is not None:
            state["launcher"].stop()

    def do_scene(s: GS.GiScene) -> None:
        cl = state["client"]
        log(f"[{s.id}] {s.data['title']}")
        _open_scene(cl, rels[s.id], log)
        place_models(cl, s, a.walk)
        conds = a.conditions or s.data.get("conditions", ["epic"])
        tmp = s.data.get("temporal")
        for v in s.variants():
            cl.call("set_sun", dict(v["sun"]), timeout=60)
            for cam in s.cameras:
                d = cdir_of(out, s.id, v["id"], cam["name"])
                d.mkdir(parents=True, exist_ok=True)
                cl.call("set_editor_camera", {"position": cam["position"], "target": cam["target"]}, timeout=60)
                if a.pt:
                    apply_condition(cl, PT_CONDITION, s)
                    GE.step(cl, 30)
                    log(f"  {v['id']}/{cam['name']}: PT {a.spp}spp / {a.bounces} bounces")
                    GE.render_pt(cl, d / "pt", size, a.spp, a.bounces, a.seed, a.frame_budget_ms, a.pt_timeout, log,
                                 extra={"sunAngularRadiusDeg": SUN_RADIUS_DEG})
                for cn in conds:
                    apply_condition(cl, cn, s)
                    t0 = time.time()
                    GE.capture_raster(cl, d / f"raster_{cn}", size, "det", 0, a.det_settle)
                    log(f"  {v['id']}/{cam['name']}/{cn}: ラスタ {time.time() - t0:.0f}s")
                    if tmp:
                        apply_condition(cl, cn, s, upscaler="taa")
                        cl.call("step_frames", {"frames": int(tmp.get("settle", 120)), "deterministic": True}, timeout=300)
                        for k in range(int(tmp.get("frames", 8))):
                            capture_native(cl, d / f"raster_{cn}_t{k:02d}")
                            cl.call("step_frames", {"frames": 1, "deterministic": True}, timeout=60)
                        log(f"  {v['id']}/{cam['name']}/{cn}: 静止 {tmp.get('frames', 8)} フレーム(TAA)")
        manifest["scenes"][s.id] = {"conditions": conds}

    try:
        start()
        for si, s in enumerate(scenes):
            if si > 0 and not a.attach:
                stop()               # GI 評価と同じ理由: シーンごとに起動し直して DDGI の格子の変更で TDR を踏まない
                start()
            for attempt in range(3):
                try:
                    do_scene(s)
                    break
                except (EngineError, OSError, RuntimeError) as e:
                    manifest["retries"].append({"scene": s.id, "attempt": attempt, "error": str(e)[:300]})
                    log(f"  !! {s.id} で失敗({type(e).__name__}: {str(e)[:160]})")
                    if a.attach or attempt == 2:
                        raise
                    stop()
                    start()
    finally:
        stop()
    return manifest


# ── 評価 ────────────────────────────────────────────────────────────────────

def _find(d: Path, name: str) -> Path | None:
    p = d / name
    return p if p.exists() else None


def _display8(lin: np.ndarray, ev: float) -> np.ndarray:
    return SM.to_8bit(GE._tm(lin, ev))


def evaluate_camera(a, s: GS.GiScene, v: dict, cam: dict, out: Path, conds: list[str], log) -> dict:
    d = cdir_of(out, s.id, v["id"], cam["name"])
    pt_f = _find(d, "pt.pfm")
    res: dict = {"conditions": {}}
    if cam.get("contaminated"):
        res["contaminated"] = cam["contaminated"]       # 既知のエンジン不具合で絵が汚れているカメラ(ベースラインとして使わない)
    if pt_f is None:
        res["error"] = "PT の画像が無い"
        return res
    pt = load_image(str(pt_f), "linear").rgb
    h, w = pt.shape[:2]
    masks = GS.build_masks(s, cam, w, h)
    smasks = LS.build_shadow_masks(s, cam, v["sun"], w, h)
    if cam.get("shadowRegions") is not None:       # このカメラで意味のある影の領域だけ(遠景のカメラで近景の影を測らない)
        smasks = {k: m for k, m in smasks.items() if k in cam["shadowRegions"]}
    ev = GE._display_ev(pt, cam)
    pt_disp = GE._tm(pt, ev)
    res.update({"size": [w, h], "displayEv": ev,
                "regionPixels": {**{k: int(m.sum()) for k, m in masks.items()}, **{k: int(m.sum()) for k, m in smasks.items()}}})
    allm = {**masks, **smasks}
    if allm:
        save_png(d / "masks.png", GE._draw_labels(GS.label_overlay(allm, pt_disp), allm))
    for cn in conds:
        rf = _find(d, f"raster_{cn}.pfm")
        if rf is None:
            res["conditions"][cn] = {"skipped": "ラスタの画像が無い"}
            continue
        ras = load_image(str(rf), "linear").rgb
        if ras.shape != pt.shape:
            res["conditions"][cn] = {"error": f"解像度が違う: {ras.shape[:2]} / {pt.shape[:2]}"}
            continue
        flip_map, flip_used, flip_err = (None, None, "--no-flip") if a.no_flip else GE.compute_flip(pt, ras)
        c: dict = {"file": str(rf), "regions": GM.all_region_stats(ras, pt, masks, flip_map),
                   "global": GM.global_stats(ras, pt, flip_map), "flipError": flip_err}
        if flip_map is not None:
            c["global"]["flip_hdr_p95"] = float(np.percentile(flip_map, 95))
        c["shadow"] = {rn: SM.shadow_stats(ras, pt, m) for rn, m in smasks.items()}
        frames = sorted(d.glob(f"raster_{cn}_t[0-9][0-9].pfm"))
        if len(frames) >= 2:
            stack = np.stack([_display8(load_image(str(f), "linear").rgb, ev) for f in frames])
            c["temporal"] = {"full": SM.temporal_stats(stack, None),
                             "regions": {rn: SM.temporal_stats(stack, m) for rn, m in smasks.items() if int(m.sum()) > 0}}
        if allm:
            sheet = H.contact_sheet([(f"raster [{cn}] (EV {ev:+.1f})", GE._tm(ras, ev)), ("path tracer", pt_disp),
                                     ("log2(raster/PT) red=raster brighter", GE.signed_ev_map(ras, pt))], d / f"sheet_{cn}.png", max_width=3000)
            c["sheet"] = str(sheet)
        res["conditions"][cn] = c
    return res


def texel_report(a, s: GS.GiScene) -> list[dict] | None:
    bands = s.data.get("texelBands")
    if not bands:
        return None
    return SM.density_bands(bands, near=0.1, far=400.0, fov_y_deg=GS.FOV_V_DEG, aspect=a.width / a.height, height_px=a.height,
                            res=a.csm_res, n=4, lam=a.csm_lambda)


def judge(s: GS.GiScene, vid: str, cams: dict, cn: str) -> dict:
    """シーン JSON の criteria を、条件 cn の数値から判定する。数値が無ければ 判定不能。ベースライン段階の参考(合否の集計には使わない)。
    criteria のキーの先頭語が種類: coverage* / acne* / temporal* を判定する。"variant" を持つ基準はその variant だけで判定する。
    それ以外(edge_char・information など)は判定せず「情報のみ(判定しない)」と出す。
    ★coverage の合否は captured(PT の影画素のうち、ラスタも影にした割合)。ratio(画素数の比)はずれた影でも 1 になり得るので参考値。
    ★temporal は影の領域(shadowRegions)の 8bit 最大差で判定する(C6 の目的は影のちらつき)。全画面は幾何の縁などを含むので full_max_range に情報として出す。"""
    out: dict = {}
    cr = s.data.get("criteria", {})
    sc = lambda cam, reg: ((cams.get(cam, {}).get("conditions", {}).get(cn, {}).get("shadow") or {}).get(reg))  # noqa: E731
    for key, c in cr.items():
        if c.get("variant") not in (None, vid):
            continue
        kind = key.split("_", 1)[0]
        if kind == "coverage":
            st = sc(c["camera"], c["region"])
            cov = st and st["coverage"]
            if cov and cov.get("evaluable"):
                out[key] = {"status": "PASS" if cov["captured"] >= c["min_ratio"] else "FAIL", "captured": cov["captured"],
                            "ratio": cov["ratio"], "need": c["min_ratio"]}
            else:
                out[key] = {"status": "判定不能"}
        elif kind == "acne":
            st = sc(c["camera"], c["region"])
            ac = st and st["acne"]
            if ac and ac.get("evaluable"):
                out[key] = {"status": "PASS" if ac["rate"] < c["max_rate"] else "FAIL", "rate": ac["rate"], "need_below": c["max_rate"]}
            else:
                out[key] = {"status": "判定不能"}
        elif kind == "temporal":
            tl = [(cm.get("conditions", {}).get(cn, {}).get("temporal") or {}) for cm in cams.values()]
            reg = [r for t_ in tl for r in (t_.get("regions") or {}).values() if r.get("evaluable")]
            full = [t_["full"] for t_ in tl if (t_.get("full") or {}).get("evaluable")]
            if reg:
                mx = max(x["max_range"] for x in reg)
                out[key] = {"status": "PASS" if mx <= c["max_range"] else "FAIL", "max_range": mx, "need": c["max_range"], "scope": "shadow_regions",
                            "full_max_range": max((x["max_range"] for x in full), default=None)}
            else:
                out[key] = {"status": "判定不能"}
        else:
            out[key] = {"status": "情報のみ(判定しない)"}
    return out


def _scorecard_put(sc: dict, key: str, val) -> None:
    if isinstance(val, (int, float, np.floating, np.integer)) and not isinstance(val, bool) and np.isfinite(val):
        sc[key] = float(val)


def build_scorecard(result: dict) -> dict:
    """平らな {キー: 数値}。--compare と W0 スコアカードが読む。キー = <scene>/<variant>/<camera>/<condition>/<指標>。"""
    sc: dict = {}
    for sid, srec in result["scenes"].items():
        for vid, vrec in srec["variants"].items():
            for cam, crec in vrec["cameras"].items():
                for cn, c in (crec.get("conditions") or {}).items():
                    base = f"{sid}/{vid}/{cam}/{cn}"
                    for k in ("ev_mean", "black_rate_raster", "flip_hdr_mean", "flip_hdr_p95"):
                        _scorecard_put(sc, f"{base}/global/{k}", (c.get("global") or {}).get(k))
                    for rn, r in (c.get("regions") or {}).items():
                        _scorecard_put(sc, f"{base}/region/{rn}/ev_mean", r.get("ev_mean"))
                        _scorecard_put(sc, f"{base}/region/{rn}/flip", r.get("flip_hdr_mean"))
                    for rn, r in (c.get("shadow") or {}).items():
                        _scorecard_put(sc, f"{base}/shadow/{rn}/coverage_ratio", r["coverage"].get("ratio"))
                        _scorecard_put(sc, f"{base}/shadow/{rn}/coverage_iou", r["coverage"].get("iou"))
                        _scorecard_put(sc, f"{base}/shadow/{rn}/acne_rate", r["acne"].get("rate"))
                        _scorecard_put(sc, f"{base}/shadow/{rn}/edge_width_raster_px", r["edge"]["raster"].get("width_px"))
                        _scorecard_put(sc, f"{base}/shadow/{rn}/edge_width_pt_px", r["edge"]["pt"].get("width_px"))
                        _scorecard_put(sc, f"{base}/shadow/{rn}/edge_error_rel", r["edge"].get("error_rel"))
                    t = (c.get("temporal") or {}).get("full") or {}
                    _scorecard_put(sc, f"{base}/temporal/max_range", t.get("max_range"))
                    _scorecard_put(sc, f"{base}/temporal/sigma_mean", t.get("sigma_mean"))
                    _scorecard_put(sc, f"{base}/temporal/p99_range", t.get("p99_range"))
                    for rn, tr in ((c.get("temporal") or {}).get("regions") or {}).items():
                        _scorecard_put(sc, f"{base}/temporal/{rn}/max_range", tr.get("max_range"))
                        _scorecard_put(sc, f"{base}/temporal/{rn}/p99_range", tr.get("p99_range"))
        for b in srec.get("texelBands") or []:
            _scorecard_put(sc, f"{sid}/texel/{int(b['distance'])}m/texels_per_px", b["texels_per_px"])
    return dict(sorted(sc.items()))


def evaluate(a, scenes: list[GS.GiScene], out: Path, log) -> dict:
    result: dict = {"schema": SCHEMA, "scenes": {}}
    for s in scenes:
        conds = a.conditions or s.data.get("conditions", ["epic"])
        srec: dict = {"title": s.data["title"], "conditions": conds, "variants": {}, "texelBands": texel_report(a, s)}
        for v in s.variants():
            vrec = {"sun": v["sun"], "cameras": {}}
            for cam in s.cameras:
                if not cdir_of(out, s.id, v["id"], cam["name"]).exists():
                    continue
                vrec["cameras"][cam["name"]] = evaluate_camera(a, s, v, cam, out, conds, log)
            vrec["criteria"] = {cn: judge(s, v["id"], vrec["cameras"], cn) for cn in conds}
            srec["variants"][v["id"]] = vrec
        result["scenes"][s.id] = srec
    result["scorecard"] = build_scorecard(result)
    return result


def format_summary(result: dict) -> str:
    L = ["== ライティング評価(LS-00b) =="]
    for sid, srec in result["scenes"].items():
        L.append(f"-- {sid} {srec['title']} --")
        for vid, vrec in srec["variants"].items():
            for cn, cr in vrec["criteria"].items():
                if cr:
                    L.append(f"  [{vid}/{cn}] " + "  ".join(f"{k}={x['status']}" for k, x in cr.items()))
            for cam, crec in vrec["cameras"].items():
                if crec.get("contaminated"):
                    L.append(f"  !! {vid}/{cam}: 汚染(ベースラインに使わない): {crec['contaminated']}")
                for cn, c in (crec.get("conditions") or {}).items():
                    g = c.get("global") or {}
                    if g:
                        f = g.get("flip_hdr_mean")
                        L.append(f"  {vid}/{cam}/{cn}: 全体 EV {g['ev_mean']:+.2f}  HDR-FLIP " + (f"{f:.3f}" if f is not None else "n/a"))
                    for rn, r in (c.get("shadow") or {}).items():
                        cov, ac, ed = r["coverage"], r["acne"], r["edge"]
                        L.append(f"     {rn:20} coverage {cov['ratio'] if cov['ratio'] is None else round(cov['ratio'], 3)}  acne "
                                 f"{ac['rate'] if ac['rate'] is None else round(ac['rate'], 5)}  edge(px) r/pt "
                                 f"{ed['raster'].get('width_px')}/{ed['pt'].get('width_px')}")
        if srec.get("texelBands"):
            L.append("  テクセル密度[テクセル/px]: " + "  ".join(f"{int(b['distance'])}m={b['texels_per_px']:.2f}" for b in srec["texelBands"]))
    return "\n".join(L)


# ── 2 回の差(決定論の範囲か) ───────────────────────────────────────────────

# 決定論モードのラスタ・PT 由来の指標はビット一致のはず(実測の最大差 4.6e-7)なので、ほぼ 0 の許容(1e-5)。
# 緩めるのは TAA の時間のフレーム列(max_range / p99_range / sigma_mean)と縁の幅だけ(下の実測)。
DET_TOL = 1e-5
# 実測(--view-size 固定で起動・解像度の切り替え無し・同条件 2 回): max_range は最大 28 段(49 と 21。外れ値の順序統計なので揺れる)・p99_range は最大 1 段・sigma_mean は最大 0.012。
TEMPORAL_TOL = {"max_range": 30.0, "p99_range": 2.0, "sigma_mean": 0.03}
# 縁の幅は「半影の画素数 / 境界の長さ」で、しきい値の際の 1 画素が入れ替わると動く(実測の最大差: 幅 4.6e-4 px・相対誤差 5e-5)。
EDGE_TOL = {"edge_width_raster_px": 0.01, "edge_width_pt_px": 0.01, "edge_error_rel": 0.005}
TOLERANCE = {"texels_per_px": 1e-9, **TEMPORAL_TOL, **EDGE_TOL}


def compare(a_json: dict, b_json: dict) -> dict:
    sa, sb = a_json.get("scorecard", {}), b_json.get("scorecard", {})
    rows, bad = [], []
    for k in sorted(set(sa) | set(sb)):
        if k not in sa or k not in sb:
            bad.append(k)
            rows.append({"key": k, "a": sa.get(k), "b": sb.get(k), "ok": False, "why": "片方に無い"})
            continue
        tol = TOLERANCE.get(k.rsplit("/", 1)[-1], DET_TOL)
        d = abs(sa[k] - sb[k])
        ok = d <= tol
        rows.append({"key": k, "a": sa[k], "b": sb[k], "diff": d, "tol": tol, "ok": ok})
        if not ok:
            bad.append(k)
    return {"keys": len(rows), "outOfTolerance": bad, "deterministic": not bad, "rows": rows}


def _json_default(o):
    if isinstance(o, (np.floating, np.integer)):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    return str(o)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="lighting_eval", description=__doc__.split("\n")[0])
    ap.add_argument("--scenes", default="all", help="LS-A,LS-B,… のカンマ区切り(既定 all)")
    ap.add_argument("--conditions", default="", help="条件のカンマ区切り(既定 = 各シーンの conditions)。候補: " + ", ".join(CONDITIONS))
    ap.add_argument("--out")
    ap.add_argument("--from-dir", help="撮影済みのディレクトリから指標だけ再計算(エンジンを起動しない)")
    ap.add_argument("--compare", nargs=2, metavar=("A_JSON", "B_JSON"), help="2 つの lighting_eval.json の scorecard を比べて決定論の範囲か判定")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--spp", type=int, default=1024)
    ap.add_argument("--bounces", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--frame-budget-ms", type=float, default=None)
    ap.add_argument("--pt-timeout", type=float, default=1800.0)
    ap.add_argument("--no-pt", dest="pt", action="store_false", help="PT を撮り直さない(--out の既存 pt.pfm を使う)")
    ap.add_argument("--det-settle", type=int, default=240)
    ap.add_argument("--walk", action="store_true", help="マネキンに歩きのループを再生させる(既定は静止 = リグの既定の姿勢。PT と同じ姿勢にするため)")
    ap.add_argument("--csm-res", type=int, default=2048, help="テクセル密度の計算に使う CSM の解像度(エンジン既定 2048)")
    ap.add_argument("--csm-lambda", type=float, default=0.7)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--attach", type=int)
    ap.add_argument("--build-dir", help="engine_instance.ps1 -BuildDir(exe + DLL + shaders。ビルドしないレーンは他のレーン / 本流の build\\release を指す)")
    ap.add_argument("--no-flip", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    log = (lambda m: None) if a.quiet else (lambda m: print(m, flush=True))

    if a.compare:
        r = compare(json.loads(Path(a.compare[0]).read_text(encoding="utf-8")), json.loads(Path(a.compare[1]).read_text(encoding="utf-8")))
        print(f"キー {r['keys']} 件・範囲外 {len(r['outOfTolerance'])} 件 → " + ("決定論の範囲" if r["deterministic"] else "差あり"))
        for k in r["outOfTolerance"]:
            print("  範囲外:", k)
        return 0 if r["deterministic"] else 1

    a.conditions = [c.strip() for c in a.conditions.split(",") if c.strip()]
    for c in a.conditions:
        if c not in CONDITIONS:
            raise SystemExit(f"条件が不明: {c}(候補: {', '.join(CONDITIONS)})")
    scenes = LS.load_scenes()
    if a.scenes != "all":
        want = [x.strip() for x in a.scenes.split(",")]
        scenes = [s for s in scenes if s.id in want]
        if not scenes:
            raise SystemExit(f"シーンが無い: {a.scenes}")

    if a.from_dir:
        out = Path(a.from_dir).resolve()
    else:
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        out = Path(a.out).resolve() if a.out else (paths.REPO_ROOT / ".dx12" / "parity" / "lighting" / stamp)
        out.mkdir(parents=True, exist_ok=True)
        log(f"出力: {out}")
        manifest = capture_all(a, scenes, out, log)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False, default=_json_default), encoding="utf-8")

    log("指標を計算中(HDR-FLIP を含む)…")
    result = evaluate(a, scenes, out, log)
    result["meta"] = {"out": str(out), "size": [a.width, a.height], "spp": a.spp, "bounces": a.bounces, "seed": a.seed,
                      "capture": "deterministic(screenshot_final deterministic:true・settleFrames=%d)・DLSS OFF・TAA OFF(静止フレーム列だけ TAA)" % a.det_settle,
                      "csm": {"res": a.csm_res, "lambda": a.csm_lambda, "cascades": 4, "near": 0.1, "far": 400.0},
                      "thresholds": {"shadow_s": SM.SHADOW_S, "lit_s": SM.LIT_S, "acne_s": SM.ACNE_S, "penum": [SM.PENUM_LO, SM.PENUM_HI]}}
    (out / "lighting_eval.json").write_text(json.dumps(result, indent=1, ensure_ascii=False, default=_json_default), encoding="utf-8")
    summary = format_summary(result)
    (out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
    print(summary)
    print(f"\n結果: {out / 'lighting_eval.json'}")
    return 0 if result["scenes"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
