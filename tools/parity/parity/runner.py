"""自動ランナー: シーン仕様 -> (エンジン起動 -> 撮影 -> 基準の取得 -> 比較 -> 性能) -> レポート -> 機械判定。

  parity run scenes/ps1_indoor_corridor.json --stage G1
  終了コード: 0 = 合格 / 1 = 不合格 / 2 = エラー(設定・接続・実行時) / 3 = skipped のみ(基準が無い等。合格扱いにしない)

エンジンは tools/engine_instance.ps1 で背景起動(名前 par / ポート 8820)。--attach で起動済みのエンジンへ接続だけもできる
(MCP ジョブ `dx12_job_start {kind:'external'}` は環境変数 DX12_MCP_PORT で接続先を渡すので、それも attach として扱う)。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import adapters as A
from . import baseline as B
from . import gate as G
from . import noisefloor as NF
from . import paths
from . import perf as P
from .engine import DEFAULT_NAME, DEFAULT_PORT, EngineClient, EngineError, PwshLauncher, engine_session
from .evaluate import evaluate_pair
from .imgio import Img, load_image
from .spec import Spec, resolve_project

EXIT = {"pass": 0, "fail": 1, "error": 2, "skipped": 3}


@dataclass
class RunOptions:
    stage: str = "G1"
    out: Path | None = None
    reference: str | None = None            # 仕様の reference.kind の上書き
    attach_port: int | None = None
    port: int = DEFAULT_PORT
    name: str = DEFAULT_NAME
    mode: str | None = None                 # headless / background(仕様 engine.mode の上書き)
    launcher: object | None = None          # テスト用に差し替え
    cameras: list[str] | None = None
    no_perf: bool = False
    no_perf_history: bool = False
    strict: bool = False
    project_override: str | None = None
    test_images_dir: Path | None = None     # 撮影済みのエンジン画像(エンジンを起動しない)
    external_dir: Path | None = None
    baseline_root: Path | None = None
    capture_only: bool = False              # 撮るだけ(baseline update)
    progress: bool = False                  # @progress 行を出す(MCP ジョブ用)
    want_noise: bool = False                # PT の複数シードでノイズ床を測って保存する
    keep_going: bool = True
    log: object | None = None
    ping_timeout: float = 240.0


class _Log:
    def __init__(self, path: Path | None, progress: bool, sink=None):
        self.fp = open(path, "a", encoding="utf-8") if path else None
        self.progress_on, self.sink = progress, sink

    def __call__(self, msg: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        if self.fp:
            self.fp.write(line + "\n")
            self.fp.flush()
        if self.sink is not None:
            self.sink(line)
        else:
            print(line, flush=True)

    def prog(self, pct: int, phase: str, msg: str = "") -> None:
        if self.progress_on:
            print("@progress " + json.dumps({"pct": int(pct), "phase": phase, "msg": msg}, ensure_ascii=False), flush=True)

    def close(self):
        if self.fp:
            self.fp.close()
            self.fp = None


def _json_default(o):
    if isinstance(o, (np.floating, np.integer)):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    if isinstance(o, Path):
        return str(o)
    raise TypeError(f"JSON にできない型: {type(o)}")


def dumps(obj) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False, default=_json_default)


# ── 準備 ────────────────────────────────────────────────────────────────────

def _run_generator(spec: Spec, workdir: Path, log) -> tuple[Path, str]:
    sc = spec.data["scene"]
    script = Path(sc["script"])
    for base in (spec.base_dir, paths.TOOL_DIR):
        cand = script if script.is_absolute() else base / script
        if cand.exists():
            script = cand
            break
    else:
        raise RuntimeError(f"シーン生成スクリプトが無い: {sc['script']}")
    proj = workdir / "project"
    cmd = [sys.executable, str(script), "--out", str(proj), "--id", spec.id, "--args-json", json.dumps(sc.get("args") or {})]
    log(f"シーンを生成: {script.name}")
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    if r.returncode != 0:
        raise RuntimeError(f"シーン生成に失敗(exit {r.returncode}): {(r.stderr or r.stdout).strip()[:600]}")
    return proj, sc.get("scene") or f"scenes/{spec.id}.json"


def _open_scene(client: EngineClient, scene: str, log, timeout: float = 300.0) -> None:
    """★起動直後は --scene のロード中で open_scene が「already in progress」で弾かれる。一致するまで待って撃ち直す。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = client.call("ping", timeout=60)
        if (p.get("currentScene") or "").replace("\\", "/") == scene.replace("\\", "/"):
            return
        try:
            client.call("open_scene", {"path": scene}, timeout=300)
        except EngineError as e:
            if "already in progress" not in str(e):
                raise
        time.sleep(0.5)
    raise EngineError(f"シーンが開かない: {scene}")


def _floors_for(spec: Spec, camera: str) -> tuple[dict | None, float]:
    nf = spec.data.get("noiseFloor") or {}
    k = float(nf.get("k", 1.5))
    if nf.get("inline"):
        return dict(nf["inline"]), k
    if nf.get("file"):
        p = Path(nf["file"])
        p = p if p.is_absolute() else spec.base_dir / p
        if p.exists():
            return NF.load(p).get("floors"), k
    p = paths.noise_dir() / spec.id / f"{camera}.json"
    if p.exists():
        return NF.load(p).get("floors"), k
    return None, k


def _engine_image(path: Path, spec: Spec) -> Img:
    return load_image(path, spec.engine.get("png", "gamma22"))


def _offline_image(dir_: Path, camera: str, spec: Spec) -> Path | None:
    for ext in (".png", ".pfm", ".exr"):
        p = Path(dir_) / f"{camera}{ext}"
        if p.exists():
            return p
    return None


# ── 本体 ────────────────────────────────────────────────────────────────────

def run_scene(spec: Spec, opts: RunOptions | None = None) -> dict:
    opts = opts or RunOptions()
    if "DX12_MCP_PORT" in os.environ and opts.attach_port is None:      # MCP ジョブ(external)経由
        opts.attach_port = int(os.environ["DX12_MCP_PORT"])
    gate_name, gate_full = G.resolve_gate(spec.data, opts.stage)
    gate_regions = G.gate_regions_for(spec.data, gate_name) if gate_name != "REG" else {}
    run_id = f"{time.strftime('%Y%m%d-%H%M%S')}_{spec.id}_{gate_name}"
    out = Path(opts.out) if opts.out else paths.runs_dir() / run_id
    out.mkdir(parents=True, exist_ok=True)
    log = _Log(out / "log.txt", opts.progress, opts.log)
    t_start = time.time()
    kind = opts.reference or spec.reference["kind"]

    run: dict = {
        "schema": 1, "runId": run_id, "scene": spec.id, "title": spec.data.get("title", spec.id),
        "parityScene": spec.data.get("parityScene"), "stage": opts.stage, "gate": gate_name, "gateThresholds": gate_full,
        "specHash": spec.hash(), "specPath": str(spec.path) if spec.path else None,
        "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "referenceKind": kind, "cameras": [], "perf": None,
        "warnings": list(spec.warnings), "reasons": [], "gpu": B.gpu_info(), "engine": {}, "strict": opts.strict,
        "runDir": str(out),
    }
    (out / "spec.json").write_text(dumps(spec.data), encoding="utf-8")

    def finish(verdict: str) -> dict:
        run["verdict"] = verdict
        run["exitCode"] = EXIT[verdict]
        run["durationSec"] = round(time.time() - t_start, 1)
        (out / "run.json").write_text(dumps(run), encoding="utf-8")
        try:
            from .report import write_report
            write_report(out)
        except Exception as e:                                   # レポートの失敗で判定を潰さない
            run["warnings"].append(f"レポート生成に失敗: {e}")
            (out / "run.json").write_text(dumps(run), encoding="utf-8")
        if opts.progress:
            print("@result " + json.dumps({"verdict": verdict, "exitCode": EXIT[verdict], "runDir": str(out),
                                           "reasons": run["reasons"][:5]}, ensure_ascii=False), flush=True)
        log(f"結果: {verdict}({out})")
        if hasattr(log, "close"):
            log.close()
        return run

    try:
        if spec.status == "stub":
            run["reasons"].append("仕様が stub(枠だけ。中身は各機能の段階で作る)")
            return finish("skipped")

        cameras = [c for c in spec.cameras if not opts.cameras or c["name"] in opts.cameras]
        if not cameras:
            raise RuntimeError("対象のカメラが無い(--camera の名前を確認)")

        log.prog(2, "prepare", f"{spec.id} / {gate_name} / 基準={kind}")
        need_engine = opts.test_images_dir is None or kind == "pt"
        captured: dict[str, dict] = {}
        client: EngineClient | None = None
        skip_all: str | None = None
        engine_cm = None

        # ── エンジン ──
        if need_engine:
            proj, scene_rel, missing = resolve_project(spec, opts.project_override)
            workdir = out / "work"
            if spec.data["scene"]["source"] == "generator":
                proj, scene_rel = _run_generator(spec, workdir, log)
            elif missing:
                raise RuntimeError(f"環境変数が未定義: {', '.join(missing)}(シーン仕様の scene.project)")
            if proj is not None and not Path(proj).exists():
                raise RuntimeError(f"プロジェクトが無い: {proj}")
            mode = opts.mode or spec.engine.get("mode", "headless")
            launcher = opts.launcher or PwshLauncher(opts.name, opts.port, mode)
            port = opts.attach_port or opts.port
            engine_cm = engine_session(port, launcher, str(proj) if proj else None, spec.engine.get("launchArgs"),
                                       attach=opts.attach_port is not None, ping_timeout=opts.ping_timeout, log=log)
            client, ping = engine_cm.__enter__()
            run["engine"] = {k: ping.get(k) for k in ("engineVersion", "pid", "vramBudgetMB", "protocolVersion")}
            run["engine"]["port"] = port

        try:
            # 前提の method
            if client is not None:
                for m in spec.engine.get("requiresMethods") or []:
                    if not client.has_method(m):
                        skip_all = f"必要な MCP method '{m}' がこのエンジンに無い"
                        break
            if skip_all is None:
                run["loadSec"] = None
                if client is not None and spec.data["scene"]["source"] != "current":
                    log.prog(8, "open_scene", scene_rel)
                    t0 = time.time()
                    _open_scene(client, scene_rel, log)
                    run["loadSec"] = round(time.time() - t0, 2)
                    log(f"シーンを開いた: {scene_rel}({run['loadSec']} 秒)")
                if client is not None:
                    for i, s in enumerate(spec.engine.get("setup") or []):
                        try:
                            client.call(s["method"], s.get("params") or {}, timeout=120)
                        except EngineError as e:
                            if s.get("optional"):
                                run["warnings"].append(f"setup[{i}] {s['method']} が失敗(optional なので続行): {e}")
                            else:
                                raise
                adapter = A.make_adapter(kind, spec, client, out / "work", want_noise=opts.want_noise,
                                         external_dir=opts.external_dir,
                                         store=B.BaselineStore(opts.baseline_root) if opts.baseline_root else None)
                warm = int(spec.engine.get("warmupFrames", 60))
                settle = int(spec.engine.get("settleFrames", 8))
                n = len(cameras)
                for i, cam in enumerate(cameras):
                    log.prog(10 + int(60 * i / n), "capture", cam["name"])
                    cdir = out / f"cam_{cam['name']}"
                    cdir.mkdir(exist_ok=True)
                    item: dict = {"camera": cam}
                    try:
                        if client is not None:
                            client.call("set_editor_camera", {"position": cam["position"], "target": cam["target"]}, timeout=60)
                            if warm > 0:
                                client.call("step_frames", {"frames": warm}, timeout=300)
                            png = cdir / "engine.png"
                            client.call("screenshot_final", {"deterministic": True, "gizmos": False, "settleFrames": settle,
                                                             "path": str(png)}, timeout=300)
                            if not png.exists():
                                raise RuntimeError(f"screenshot_final がファイルを作らなかった: {png}")
                            item["test_path"] = png
                        else:
                            p = _offline_image(opts.test_images_dir, cam["name"], spec)
                            if p is None:
                                raise RuntimeError(f"--test-images に {cam['name']}.png が無い: {opts.test_images_dir}")
                            item["test_path"] = p
                        item["test"] = _engine_image(item["test_path"], spec)
                        if not opts.capture_only:
                            item["ref"] = adapter.get(cam, item["test"].size)
                    except (EngineError, RuntimeError, OSError) as e:
                        item["error"] = str(e)
                        log(f"カメラ {cam['name']} が失敗: {e}")
                        if not opts.keep_going:
                            captured[cam["name"]] = item
                            break
                    captured[cam["name"]] = item

                # 性能(撮影の後・同じエンジンで)
                pcfg = spec.data.get("perf") or {}
                if (client is not None and not opts.no_perf and not opts.capture_only and pcfg.get("enabled", True)
                        and (pcfg.get("budgets") or pcfg.get("enabled"))):
                    log.prog(75, "perf", "benchmark")
                    pc = (pcfg.get("cameras") or [cameras[0]["name"]])[0]
                    cam = spec.camera(pc)
                    client.call("set_editor_camera", {"position": cam["position"], "target": cam["target"]}, timeout=60)
                    client.call("step_frames", {"frames": 60}, timeout=300)
                    pm = P.measure(client, int(pcfg.get("benchFrames", 300)), log)
                    pm["load_sec"] = run.get("loadSec")
                    hist = P.load_history(spec.id)
                    pv = P.judge(pm, pcfg.get("budgets"), hist, float(pcfg.get("regression_pct_max", 12.0)), run["gpu"]["name"])
                    pv["measured"] = pm
                    pv["camera"] = pc
                    run["perf"] = pv
                    if not opts.no_perf_history:
                        P.append_history(spec.id, {"scene": spec.id, "camera": pc, "gpu": run["gpu"]["name"],
                                                   **{k: pm.get(k) for k in ("gpu_ms", "frame_ms_avg", "frame_ms_p95", "cpu_ms", "vram_mb", "load_sec")}})
        finally:
            if engine_cm is not None:
                engine_cm.__exit__(None, None, None)                # ★必ず停止する(自分が起動した pid だけ)
                log("エンジンを停止した")

        # ── 保存(baseline update)──
        if opts.capture_only:
            store = B.BaselineStore(opts.baseline_root)
            for name, item in captured.items():
                if "error" in item:
                    run["cameras"].append({"name": name, "status": "error", "reasons": [item["error"]]})
                    continue
                m = store.save_pending(spec.id, name, item["test_path"], {"specHash": spec.hash(), "eotf": spec.engine.get("png", "gamma22"),
                                                                            "engine": run["engine"]})
                run["cameras"].append({"name": name, "status": "captured", "sha256": m["sha256"]})
            run["reasons"].append("pending に保存した(承認するまで基準にならない: parity baseline approve --reason ...)")
            errs = [c for c in run["cameras"] if c["status"] == "error"]
            return finish("error" if errs else "pass")

        if skip_all is not None:
            run["reasons"].append(skip_all)
            return finish("skipped")

        # ── 比較 ──
        statuses: list[str] = []
        for i, cam in enumerate(cameras):
            item = captured.get(cam["name"])
            log.prog(78 + int(20 * i / max(1, len(cameras))), "compare", cam["name"])
            entry: dict = {"name": cam["name"], "note": cam.get("note"), "position": cam["position"], "target": cam["target"]}
            run["cameras"].append(entry)
            if item is None:
                entry.update(status="error", reasons=["撮影されなかった"])
                statuses.append("error")
                continue
            if "error" in item:
                entry.update(status="error", reasons=[item["error"]])
                statuses.append("error")
                continue
            ref_res: A.RefResult = item["ref"]
            entry["reference"] = {"source": ref_res.source, **(ref_res.meta or {})}
            if ref_res.img is None:
                entry.update(status="skipped", reasons=[ref_res.skip_reason or "基準が無い"])
                statuses.append("skipped")
                continue
            floors, k = _floors_for(spec, cam["name"])
            if opts.want_noise and ref_res.noise_imgs:
                nf = NF.measure([ref_res.img] + ref_res.noise_imgs, ref_res.align.get("tonemap") or spec.alignment.get("tonemap", "engine_aces"))
                nfp = paths.ensure(paths.noise_dir() / spec.id) / f"{cam['name']}.json"
                NF.save(nfp, nf, {"scene": spec.id, "camera": cam["name"], "specHash": spec.hash()})
                floors = nf["floors"]
                entry["noiseFloorFile"] = str(nfp)
            al = spec.alignment
            reg_names = cam.get("regions")
            regs = [r for r in spec.data.get("regions") or [] if (reg_names is None or r["name"] in reg_names)]
            g_full = dict(gate_full)
            g_full.update((cam.get("gates") or {}).get(gate_name, {}))
            try:
                res = evaluate_pair(
                    ref_res.img, item["test"], out / f"cam_{cam['name']}",
                    tonemap=ref_res.align.get("tonemap") or al.get("tonemap", "engine_aces"),
                    exposure=ref_res.align.get("exposure", al.get("exposure")),
                    size_policy=al.get("sizePolicy", "resize"), metrics=al.get("metrics"), ppd=al.get("ppd"),
                    regions=regs, region_base=spec.base_dir, gate_name=gate_name, gate_full=g_full,
                    gate_regions=gate_regions, floors=floors, k=k, strict=opts.strict, regression=(gate_name == "REG"),
                    ref_label={"pt": "パストレ基準", "external": "外部基準", "previous-run": "承認済み baseline"}.get(kind, "基準"),
                    test_label="Uno", rel_base=out)
            except Exception as e:
                entry.update(status="error", reasons=[f"比較に失敗: {e}"], traceback=traceback.format_exc()[-1500:])
                statuses.append("error")
                continue
            entry.update(res)
            entry["floors"] = floors
            entry["floorK"] = k
            entry["status"] = res["verdict"]["status"]
            entry["reasons"] = res["verdict"]["reasons"]
            entry["engineImage"] = str(item["test_path"])
            if ref_res.warnings:
                entry["verdict_warnings"] = ref_res.warnings
            for w in ref_res.warnings:
                run["warnings"].append(f"{cam['name']}: {w}")
            statuses.append(entry["status"])

        if run["perf"]:
            statuses.append(run["perf"]["status"])
            run["reasons"] += [f"性能: {r}" for r in run["perf"]["reasons"]]
        for c in run["cameras"]:
            run["reasons"] += [f"{c['name']}: {r}" for r in c.get("reasons", [])] if c.get("status") in ("fail", "error", "skipped") else []
        overall = "error" if "error" in statuses else G.overall(statuses)
        return finish(overall)

    except Exception as e:
        run["reasons"].append(f"実行エラー: {e}")
        run["traceback"] = traceback.format_exc()[-3000:]
        log(f"エラー: {e}")
        return finish("error")
