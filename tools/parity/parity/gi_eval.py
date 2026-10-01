"""GI 評価ハーネス(S1)。検証シーン GR-1〜GR-4 を「構成ごとのラスタ」と「パストレーサー」で撮り、領域別・指標別に比べて A1〜A4 を機械判定する。

  pwsh -NoProfile -File tools\\parity\\gi_eval.ps1                         # 全シーン × legacy / ddgi_current / gi_new(未実装なら飛ばす)
  pwsh -NoProfile -File tools\\parity\\gi_eval.ps1 -- --scenes gr1 --configs legacy
  pwsh -NoProfile -File tools\\parity\\gi_eval.ps1 -- --from-dir .dx12\\parity\\gi\\<日時>    # 撮影済みの画像から指標だけ再計算(エンジン不要)

流れ: 使い捨てプロジェクト(検証シーンの JSON)を書く → engine_instance.ps1 で背景起動(headless) → シーンごとに
  open_scene → set_sun → カメラごとに { PT を 1 回 / 構成ごとにラスタ } → 画像・指標・並べ画像・JSON を出力 → エンジン停止。
出力: <repo>/.dx12/parity/gi/<日時>/ (git 管理外)。 gi_eval.json(全指標と判定)・<scene>/<camera>/{pt.pfm, raster_<config>.pfm, sheet_<config>.png, flip_<config>.png, masks.png}。

★構成(CONFIGS): legacy = GI 全 OFF(SSAO/SSGI/SSR/DDGI/RT 影 すべて OFF・CSM 影)/ ddgi_current = DDGI ON・多重バウンス 0.85・RT 影 ON・SSGI/SSR OFF /
  gi_new = S3 以降の新モード(エンジンに `set_gi_mode` が入るまで未実装として飛ばす)。
★ラスタの撮り方(--capture-mode): free(既定)= step_frames で十分回して DDGI を収束させてから非決定論で撮る(deterministic:true は DDGI のレイ回転を
  frameIndex=0 に固定するので、64 本の固定パターンしか使えず積分が荒い。時間平均で収束させる free のほうが実際に見える絵に近い)/ det = deterministic:true・settleFrames=240。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

from . import gi_metrics as GM
from . import gi_scene as GS
from . import heatmap as H
from . import paths
from . import tonemap as T
from .engine import EngineClient, EngineError, PwshLauncher, wait_ping
from .imgio import load_image, save_png

SCENES_DIR = paths.TOOL_DIR / "scenes" / "gi"
DEFAULT_PORT = 8821
DEFAULT_NAME = "gi"
BASELINE_CONFIG = "legacy"

# ── 構成 ────────────────────────────────────────────────────────────────────
# set: 構成を切り替える MCP method と引数({grid} はシーンの DDGI 格子で置換)。毎回全項目を明示する(前の構成を引きずらない)。
_OFF = [("set_ssao", {"enabled": False}), ("set_ssgi", {"enabled": False}), ("set_ssr", {"enabled": False}),
        ("set_contact_shadow", {"enabled": False}), ("set_taa", {"enabled": False})]

CONFIGS: dict[str, dict] = {
    "legacy": {
        "title": "GI 全 OFF(CSM 影・SSAO/SSGI/SSR/DDGI/RT 全て OFF)",
        "set": _OFF + [("set_dxr", {"shadowEnabled": False, "aoEnabled": False, "ddgiEnabled": False, "forceBuildTlas": False})],
    },
    "ddgi_current": {
        "title": "DDGI ON(多重バウンス 0.85・RT 影 ON・SSGI/SSR OFF。格子は部屋を覆う)",
        "set": _OFF + [("set_dxr", {"shadowEnabled": True, "shadowSunAngle": 0, "aoEnabled": False, "ddgiEnabled": True,
                                    "ddgiBounceIntensity": 0.85, "ddgiIntensity": 1.0, "ddgiHysteresis": 0.97,
                                    "ddgiRayLength": 30.0, "ddgiNormalBias": 0.02, "{grid}": True})],
    },
    "gi_new": {
        "title": "新 GI モード(S3 以降。エンジンに set_gi_mode が入ったら有効になる)",
        "requires": "set_gi_mode",
        "set": _OFF + [("set_gi_mode", {"mode": "new"})],
        "reset": [("set_gi_mode", {"mode": "legacy"})],
    },
}


def _grid_params(ddgi: dict) -> dict:
    g = GS.ddgi_grid(ddgi)
    return {"ddgiSpacing": g["spacing"], "ddgiProbeCountX": g["probeCountX"], "ddgiProbeCountY": g["probeCountY"],
            "ddgiProbeCountZ": g["probeCountZ"], "ddgiOriginX": g["originX"], "ddgiOriginY": g["originY"], "ddgiOriginZ": g["originZ"]}


# ── エンジン操作 ────────────────────────────────────────────────────────────

def apply_config(cli: EngineClient, name: str, scene: GS.GiScene) -> None:
    cfg = CONFIGS[name]
    for method, args in cfg["set"]:
        a = dict(args)
        if "{grid}" in a:
            del a["{grid}"]
            a.update(_grid_params(scene.data["ddgi"]))
        cli.call(method, a, timeout=60)


def reset_config(cli: EngineClient, name: str) -> None:
    for method, args in CONFIGS[name].get("reset", []):
        try:
            cli.call(method, args, timeout=60)
        except EngineError:
            pass


def step(cli: EngineClient, frames: int) -> None:
    left = frames
    while left > 0:
        n = min(left, 600)
        cli.call("step_frames", {"frames": n}, timeout=300)
        left -= n


def render_pt(cli: EngineClient, base: Path, size: tuple[int, int], spp: int, bounces: int, seed: int,
              frame_budget_ms: float | None, timeout_s: float, log) -> dict:
    params = {"spp": spp, "bounces": bounces, "size": list(size), "seed": seed, "output": str(base), "formats": ["pfm"]}
    if frame_budget_ms:
        params["frameBudgetMs"] = frame_budget_ms
    r = cli.call("render_reference", params, timeout=120)
    if r.get("accepted") is False:
        raise RuntimeError(f"render_reference が受理されなかった: {r}")
    t0 = time.time()
    last: dict = {}
    while True:
        last = cli.call("render_reference_status", {}, timeout=60)
        st = last.get("state")
        if st == "done":
            break
        if st in ("failed", "cancelled"):
            raise RuntimeError(f"render_reference が {st}: {last.get('error') or last.get('progress')}")
        if time.time() - t0 > timeout_s:
            try:
                cli.call("render_reference_cancel", {}, timeout=30)
            except EngineError:
                pass
            raise RuntimeError(f"render_reference が {timeout_s:g} 秒で終わらない(samples={last.get('samples')})")
        time.sleep(1.0)
    files = (last.get("output") or {}).get("files") or []
    pfm = next((f for f in files if str(f).lower().endswith(".pfm")), None)
    if not pfm:
        raise RuntimeError(f"PT の出力に .pfm が無い: {files}")
    log(f"    PT 完了 {time.time() - t0:.0f}s: {Path(pfm).name}")
    return {"file": str(pfm), "spp": (last.get("samples") or {}).get("done"), "elapsedSec": round(time.time() - t0, 1),
            "meta": {k: last.get(k) for k in ("gpu", "scene") if k in last}}


def capture_raster(cli: EngineClient, base: Path, size: tuple[int, int], mode: str, settle_frames: int, det_settle: int) -> str:
    args = {"path": str(base), "format": "pfm", "width": size[0], "height": size[1], "gizmos": False}
    if mode == "det":
        args.update({"deterministic": True, "settleFrames": det_settle})
    else:
        step(cli, settle_frames)
    r = cli.call("screenshot_final", args, timeout=240)
    f = base.with_suffix(".pfm")
    files = r.get("files") or {}
    cand = files.get("pfm") if isinstance(files, dict) else None
    if cand:
        f = Path(cand)
    if not f.exists():
        raise RuntimeError(f"ラスタの PFM が書かれていない: {f}(応答: {str(r)[:300]})")
    return str(f)


def capture_all(a, scenes: list[GS.GiScene], out: Path, log) -> dict:
    """エンジンを起動して全撮影を行い、manifest(撮影したファイルの表)を返す。
    GPU が TDR で落ちる(他のエージェントが同じ GPU を使っているとき等)ことがあるので、シーン単位で最大 2 回まで起動し直して撮り直す。"""
    proj = out / "_project"
    rels = GS.write_project(scenes, proj)
    size = (a.width, a.height)
    manifest: dict = {"scenes": {}, "engine": {}, "retries": []}
    from .runner import _open_scene
    state = {"launcher": None, "client": None}

    def start():
        if a.attach:
            port = a.attach
        else:
            port = a.port
            state["launcher"] = PwshLauncher(name=DEFAULT_NAME, port=port, mode="headless")
            log(f"エンジンを起動: name={DEFAULT_NAME} port={port}")
            state["launcher"].start(str(proj))
        state["client"] = EngineClient(port, connect_timeout=(30 if a.attach else 120))
        manifest["engine"] = {"ping": wait_ping(state["client"], 240.0)}
        manifest["configAvailable"] = {c: (CONFIGS[c].get("requires") is None or state["client"].has_method(CONFIGS[c]["requires"]))
                                       for c in a.configs}

    def stop():
        if state["client"] is not None:
            state["client"].close()
            state["client"] = None
        if state["launcher"] is not None:
            state["launcher"].stop()

    def do_scene(s: GS.GiScene) -> dict:
        client = state["client"]
        have_methods = manifest["configAvailable"]
        log(f"[{s.id}] {s.data['title']}")
        _open_scene(client, rels[s.id], log)
        client.call("set_sun", dict(s.data["sun"]), timeout=60)
        ms = {"cameras": {}}
        for cam in s.cameras:
            cdir = out / s.id / cam["name"]
            cdir.mkdir(parents=True, exist_ok=True)
            client.call("set_editor_camera", {"position": cam["position"], "target": cam["target"]}, timeout=60)
            entry = {"configs": {}}
            if a.pt:
                apply_config(client, BASELINE_CONFIG, s)
                step(client, 30)
                log(f"  カメラ {cam['name']}: PT {a.spp}spp / {a.bounces} bounces")
                entry["pt"] = render_pt(client, cdir / "pt", size, a.spp, a.bounces, a.seed, a.frame_budget_ms, a.pt_timeout, log)
            for cn in a.configs:
                if not have_methods[cn]:
                    entry["configs"][cn] = {"skipped": f"エンジンに MCP method '{CONFIGS[cn]['requires']}' が無い(未実装)"}
                    log(f"  構成 {cn}: 未実装(飛ばす)")
                    continue
                apply_config(client, cn, s)
                t0 = time.time()
                f = capture_raster(client, cdir / f"raster_{cn}", size, a.capture_mode, a.settle, a.det_settle)
                entry["configs"][cn] = {"file": f, "elapsedSec": round(time.time() - t0, 1)}
                reset_config(client, cn)
                log(f"  構成 {cn}: ラスタ撮影 {time.time() - t0:.0f}s")
            ms["cameras"][cam["name"]] = entry
        return ms

    try:
        start()
        for si, s in enumerate(scenes):
            if si > 0 and not a.attach:
                # ★シーンごとにエンジンを起動し直す: DDGI を ON にしたままシーンを切り替えて格子の大きさが変わると、
                #   次に DDGI を入れた瞬間に GPU が TDR で落ちることがある(実測。エンジン側の不具合として報告)。
                stop()
                start()
            for attempt in range(3):
                try:
                    manifest["scenes"][s.id] = do_scene(s)
                    break
                except (EngineError, OSError, RuntimeError) as e:
                    manifest["retries"].append({"scene": s.id, "attempt": attempt, "error": str(e)[:300]})
                    log(f"  !! {s.id} で失敗({type(e).__name__}: {str(e)[:160]})")
                    if a.attach or attempt == 2:
                        raise
                    log("  エンジンを起動し直して撮り直す")
                    stop()
                    start()
    finally:
        stop()
    return manifest


# ── 評価 ────────────────────────────────────────────────────────────────────

def _display_ev(pt_lin: np.ndarray, cam: dict) -> float:
    if cam.get("displayEv") is not None:
        return float(cam["displayEv"])
    y = GM.luminance(pt_lin)
    y = y[y > 1e-6]
    if y.size < 100:
        return 0.0
    gm = float(np.exp(np.mean(np.log(y))))
    return float(np.clip(np.log2(0.18 / gm), -4.0, 8.0))


def _tm(lin: np.ndarray, ev: float) -> np.ndarray:
    d, _ = T.apply_tonemap("engine_aces", lin * float(2.0 ** ev))
    return np.clip(d, 0, 1).astype(np.float32)


def signed_ev_map(raster: np.ndarray, pt: np.ndarray, span: float = 3.0) -> np.ndarray:
    d = np.log2((GM.luminance(raster) + GM.EV_EPS) / (GM.luminance(pt) + GM.EV_EPS))
    t = np.clip(d / span, -1.0, 1.0)[..., None].astype(np.float32)
    pos = np.concatenate([np.ones_like(t), 1 - t, 1 - t], axis=2)      # ラスタが明るい = 赤
    neg = np.concatenate([1 + t, 1 + t, np.ones_like(t)], axis=2)      # ラスタが暗い = 青
    return np.where(t >= 0, pos, neg).astype(np.float32)


def _draw_labels(img: np.ndarray, masks: dict[str, np.ndarray]) -> np.ndarray:
    from PIL import Image, ImageDraw
    im = Image.fromarray(np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    f = H._font(14)
    for n, m in masks.items():
        ys, xs = np.nonzero(m)
        if ys.size < 50:
            continue
        x, y = int(xs.mean()), int(ys.mean())
        d.rectangle([x - 2, y - 2, x + d.textlength(n, font=f) + 4, y + 16], fill=(0, 0, 0))
        d.text((x, y), n, fill=(255, 255, 255), font=f)
    return np.asarray(im, dtype=np.float32) / 255.0


def compute_flip(pt: np.ndarray, raster: np.ndarray) -> tuple[np.ndarray | None, dict | None, str | None]:
    from . import metrics as M
    # ★flip-evaluator は真っ黒に近い基準だと露出範囲が決まらず、例外ではなくプロセスごと終了する(実測)。先に弾く。
    if float(np.percentile(GM.luminance(pt), 99.9)) < 1e-3:
        return None, None, "PT がほぼ真っ黒なので HDR-FLIP は計算しない(露出範囲が決まらない)"
    try:
        err, used = M._flip(pt, raster, "HDR", {})
        return err, used, None
    except (Exception, SystemExit) as e:  # noqa: BLE001  FLIP が無い / 真っ黒な画像で SystemExit を投げる環境でも他の指標は出す
        return None, None, f"{type(e).__name__}: {e}"


def evaluate(a, scenes: list[GS.GiScene], out: Path, manifest: dict, log) -> dict:
    result: dict = {"scenes": {}}
    stats_all: dict = {}        # [scene][config][camera] -> {regions, global}
    for s in scenes:
        sm = manifest["scenes"].get(s.id)
        if not sm:
            continue
        rs = {"title": s.data["title"], "cameras": {}}
        for cam in s.cameras:
            ce = sm["cameras"].get(cam["name"])
            if not ce:
                continue
            cdir = out / s.id / cam["name"]
            pt_file = (ce.get("pt") or {}).get("file")
            if not pt_file or not Path(pt_file).exists():
                cand = cdir / "pt.pfm"
                pt_file = str(cand) if cand.exists() else None
            if not pt_file:
                rs["cameras"][cam["name"]] = {"error": "PT の画像が無い"}
                continue
            pt = load_image(pt_file, "linear").rgb
            h, w = pt.shape[:2]
            masks = GS.build_masks(s, cam, w, h)
            ev_disp = _display_ev(pt, cam)
            rc = {"size": [w, h], "displayEv": ev_disp, "pt": ce.get("pt") or {"file": pt_file},
                  "regionPixels": {k: int(v.sum()) for k, v in masks.items()}, "configs": {}}
            pt_disp = _tm(pt, ev_disp)
            save_png(cdir / "masks.png", _draw_labels(GS.label_overlay(masks, pt_disp), masks))
            for cn in a.configs:
                cc = ce["configs"].get(cn, {})
                rf = cc.get("file")
                if not rf or not Path(rf).exists():
                    cand = cdir / f"raster_{cn}.pfm"
                    rf = str(cand) if cand.exists() else None
                if not rf:
                    rc["configs"][cn] = {"skipped": cc.get("skipped") or "ラスタの画像が無い"}
                    continue
                ras = load_image(rf, "linear").rgb
                if ras.shape != pt.shape:
                    rc["configs"][cn] = {"error": f"解像度が違う: raster {ras.shape[:2]} / PT {pt.shape[:2]}"}
                    continue
                flip_map, flip_used, flip_err = (None, None, "--no-flip") if a.no_flip else compute_flip(pt, ras)
                regions = GM.all_region_stats(ras, pt, masks, flip_map)
                glob = GM.global_stats(ras, pt, flip_map)
                if flip_map is not None:
                    glob["flip_hdr_p95"] = float(np.percentile(flip_map, 95))
                    H.save_error_heatmap(cdir / f"flip_{cn}.png", flip_map, 1.0, "HDR-FLIP error")
                sheet = H.contact_sheet([(f"raster [{cn}]  (EV {ev_disp:+.1f})", _tm(ras, ev_disp)), ("path tracer", pt_disp),
                                         ("log2(raster/PT)  red=raster brighter  +-3EV", signed_ev_map(ras, pt))],
                                        cdir / f"sheet_{cn}.png", max_width=3000)
                rc["configs"][cn] = {"file": rf, "regions": regions, "global": glob, "flipError": flip_err,
                                     "flipParams": flip_used, "sheet": str(sheet)}
                stats_all.setdefault(s.id, {}).setdefault(cn, {})[cam["name"]] = {"regions": regions}
            rs["cameras"][cam["name"]] = rc
        result["scenes"][s.id] = rs
    result["criteria"] = judge_all(scenes, stats_all, a.configs, manifest)
    return result


def judge_all(scenes: list[GS.GiScene], stats_all: dict, configs: list[str], manifest: dict) -> dict:
    """構成ごとに A1〜A4(+情報)を判定する。構成が飛ばされた(未実装)なら全項目 未実装。"""
    avail = manifest.get("configAvailable", {})
    byid = {s.id: s for s in scenes}
    out: dict = {}
    for cn in configs:
        row: dict = {}
        if avail.get(cn) is False:
            for k in ("A1", "A2", "A3", "A4"):
                row[k] = {"status": GM.NOT_IMPL, "reason": f"構成 {cn} はエンジンが未対応"}
            out[cn] = row
            continue

        def cam_stats(sid, cams):
            d = stats_all.get(sid, {}).get(cn, {})
            return {c: d[c]["regions"] for c in cams if c in d}

        # A1 / A3 / A4: GR-1
        s1 = byid.get("gr1")
        if s1 and "A1" in s1.data["criteria"]:
            cr = s1.data["criteria"]
            row["A1"] = GM.judge_a1(cam_stats("gr1", cr["A1"]["cameras"]), cr["A1"]["regions"])
            c3 = cr["A3"]
            reg = cam_stats("gr1", [c3["camera"]]).get(c3["camera"], {}).get(c3["region"])
            row["A3"] = GM.judge_a3(reg)
            c4 = cr["A4"]
            reg_c = cam_stats("gr1", [c4["camera"]]).get(c4["camera"], {}).get(c4["region"])
            leg = stats_all.get("gr1", {}).get(BASELINE_CONFIG, {}).get(c4["camera"], {}).get("regions", {}).get(c4["region"])
            row["A4"] = GM.judge_a4(reg_c, leg, cn == BASELINE_CONFIG)
        else:
            for k in ("A1", "A3", "A4"):
                row[k] = {"status": GM.UNJUDGEABLE, "reason": "GR-1 が評価対象に無い"}
        # A2: GR-3
        s3 = byid.get("gr3")
        if s3 and "A2" in s3.data["criteria"]:
            c2 = s3.data["criteria"]["A2"]
            reg = cam_stats("gr3", [c2["camera"]]).get(c2["camera"], {}).get(c2["region"])
            row["A2"] = GM.judge_a2(reg)
        else:
            row["A2"] = {"status": GM.UNJUDGEABLE, "reason": "GR-3 が評価対象に無い"}
        # 情報(GR-2 / GR-4)
        info = {}
        for sid, s in byid.items():
            ic = s.data["criteria"].get("information")
            if ic:
                info[sid] = GM.judge_information(cam_stats(sid, ic["cameras"]), ic["regions"])
        if info:
            row["information"] = info
        out[cn] = row
    return out


# ── 表示 ────────────────────────────────────────────────────────────────────

def format_summary(result: dict, configs: list[str]) -> str:
    lines = []
    crit = result["criteria"]
    lines.append("== 合否 ==")
    lines.append(f"{'':14}" + "".join(f"{c:>16}" for c in configs))
    for k in ("A1", "A2", "A3", "A4"):
        lines.append(f"{k:14}" + "".join(f"{crit.get(c, {}).get(k, {}).get('status', '-'):>16}" for c in configs))
    lines.append("")
    lines.append("== 領域別 平均輝度の EV 差(ラスタ - PT。+ = ラスタが明るい) ==")
    for sid, rs in result["scenes"].items():
        for cn_, rc in rs["cameras"].items():
            if "configs" not in rc:
                lines.append(f"{sid}/{cn_}: {rc.get('error')}")
                continue
            lines.append(f"-- {sid}/{cn_}  (表示 EV {rc['displayEv']:+.1f}) --")
            regs = sorted({r for c in rc["configs"].values() for r in (c.get("regions") or {})})
            lines.append(f"{'region':16}{'px':>8}" + "".join(f"{c:>16}" for c in configs))
            for r in regs:
                px = rc["regionPixels"].get(r, 0)
                cells = []
                for c in configs:
                    s = (rc["configs"].get(c, {}).get("regions") or {}).get(r)
                    cells.append(f"{s['ev_mean']:>+16.2f}" if s and s.get("evaluable") else f"{'-':>16}")
                lines.append(f"{r:16}{px:>8}" + "".join(cells))
            for c in configs:
                g = rc["configs"].get(c, {}).get("global")
                if g:
                    f = g.get("flip_hdr_mean")
                    lines.append(f"  [{c}] 全体 EV {g['ev_mean']:+.2f}  HDR-FLIP 平均 " + (f"{f:.3f}" if f is not None else "n/a")
                                 + f"  NaN(raster/pt) {g['nan_raster']}/{g['nan_pt']}")
    return "\n".join(lines)


def _json_default(o):
    if isinstance(o, (np.floating, np.integer)):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    return str(o)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="gi_eval", description=__doc__.split("\n")[0])
    ap.add_argument("--scenes", default="all", help="gr1,gr2,gr3,gr4 のカンマ区切り(既定 all)")
    ap.add_argument("--configs", default="legacy,ddgi_current,gi_new", help="構成のカンマ区切り。候補: " + ", ".join(CONFIGS))
    ap.add_argument("--out", help="出力先(既定 <repo>/.dx12/parity/gi/<日時>)")
    ap.add_argument("--from-dir", help="撮影済みのディレクトリから指標だけ再計算(エンジンを起動しない)")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--spp", type=int, default=1024)
    ap.add_argument("--bounces", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--frame-budget-ms", type=float, default=None, help="PT の 1 フレーム GPU 予算(既定 = エンジン既定の 12)")
    ap.add_argument("--pt-timeout", type=float, default=1800.0)
    ap.add_argument("--no-pt", dest="pt", action="store_false", help="PT を撮り直さない(--out の既存 pt.pfm を使う)")
    ap.add_argument("--capture-mode", choices=("free", "det"), default="free")
    ap.add_argument("--settle", type=int, default=600, help="free: 撮る前に回すフレーム数")
    ap.add_argument("--det-settle", type=int, default=240, help="det: settleFrames(最大 240)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--attach", type=int, help="起動済みエンジンへ接続だけ(起動も停止もしない)")
    ap.add_argument("--no-flip", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    log = (lambda m: None) if a.quiet else (lambda m: print(m, flush=True))

    a.configs = [c.strip() for c in a.configs.split(",") if c.strip()]
    for c in a.configs:
        if c not in CONFIGS:
            raise SystemExit(f"構成が不明: {c}(候補: {', '.join(CONFIGS)})")
    scenes = GS.discover_scenes(SCENES_DIR)
    if a.scenes != "all":
        want = [x.strip() for x in a.scenes.split(",")]
        scenes = [s for s in scenes if s.id in want]
        if not scenes:
            raise SystemExit(f"シーンが無い: {a.scenes}")

    if a.from_dir:
        out = Path(a.from_dir).resolve()
        mf = out / "manifest.json"
        manifest = json.loads(mf.read_text(encoding="utf-8")) if mf.exists() else {"scenes": {}}
        # manifest が無い / 足りないときは、ディレクトリの中身(pt.pfm と raster_<構成>.pfm)から組み立てる
        for s in scenes:
            for cam in s.cameras:
                cdir = out / s.id / cam["name"]
                if not cdir.exists():
                    continue
                ce = manifest["scenes"].setdefault(s.id, {"cameras": {}})["cameras"].setdefault(cam["name"], {"configs": {}})
                if (cdir / "pt.pfm").exists():
                    ce.setdefault("pt", {"file": str(cdir / "pt.pfm")})
                for cn in a.configs:
                    if (cdir / f"raster_{cn}.pfm").exists():
                        ce["configs"].setdefault(cn, {"file": str(cdir / f"raster_{cn}.pfm")})
        manifest["configAvailable"] = {c: any(out.glob(f"*/*/raster_{c}.pfm")) or CONFIGS[c].get("requires") is None for c in a.configs}
    else:
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        out = Path(a.out).resolve() if a.out else (paths.REPO_ROOT / ".dx12" / "parity" / "gi" / stamp)
        out.mkdir(parents=True, exist_ok=True)
        log(f"出力: {out}")
        if not a.pt:
            log("--no-pt: 既存の pt.pfm を使う")
        manifest = capture_all(a, scenes, out, log)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False, default=_json_default), encoding="utf-8")

    log("指標を計算中(HDR-FLIP を含む)…")
    result = evaluate(a, scenes, out, manifest, log)
    result["meta"] = {"configs": a.configs, "captureMode": a.capture_mode if not a.from_dir else "(from-dir)", "spp": a.spp, "bounces": a.bounces,
                      "size": [a.width, a.height], "out": str(out), "configAvailable": manifest.get("configAvailable"),
                      "engine": (manifest.get("engine") or {}).get("ping", {}),
                      "thresholds": {"A1_ev_abs": GM.A1_EV_ABS, "A2_black_pt": GM.A2_BLACK_PT, "A2_ev_max": GM.A2_EV_MAX,
                                     "A3_ratio_tol": GM.A3_RATIO_TOL, "A4_ev_max": GM.A4_EV_MAX, "eps": GM.EV_EPS, "black_y": GM.BLACK_Y}}
    (out / "gi_eval.json").write_text(json.dumps(result, indent=1, ensure_ascii=False, default=_json_default), encoding="utf-8")
    summary = format_summary(result, a.configs)
    (out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
    print(summary)
    print(f"\n結果: {out / 'gi_eval.json'}")
    # 終了コード: 0 = 判定できた(合否は JSON を読む)/ 2 = 何も評価できなかった
    return 0 if result["scenes"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
