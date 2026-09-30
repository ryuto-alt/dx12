"""性能ゲート(MCP 書 M10 の perf_gate に相当する最小版)。エンジンの perf_stats / benchmark / ping から取る。

測る値: GPU ms(gpuPassMs.total)・フレーム ms(平均 / p95)・CPU ms(cpu.workMs から待ちを引いた実働)・
VRAM(ping.vramUsedMB)・ロード時間(ランナーが open_scene の前後で測る)。
判定: 仕様の perf.budgets(上限)と、履歴(直近 5 回の中央値)からの悪化率(regression_pct_max、既定 12%)。
★benchmark は計測中だけ FPS 上限/VSync を外す(エンジン側で自動的に戻る)。GPU を専有するので他の計測と重ねない。
"""
from __future__ import annotations

import json
import statistics
import time
from pathlib import Path

from . import paths
from .engine import EngineClient, EngineError

BUDGET_MAP = {  # 予算キー -> perf の値
    "gpu_ms_max": "gpu_ms", "frame_ms_avg_max": "frame_ms_avg", "frame_ms_p95_max": "frame_ms_p95",
    "cpu_ms_max": "cpu_ms", "vram_mb_max": "vram_mb", "load_sec_max": "load_sec",
}
LABELS = {"gpu_ms": "GPU ms", "frame_ms_avg": "フレーム平均 ms", "frame_ms_p95": "フレーム p95 ms", "cpu_ms": "CPU 実働 ms",
          "vram_mb": "VRAM MB", "load_sec": "ロード秒"}


def _get(d: dict, *path, default=None):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return default
        d = d[p]
    return d


def extract(rep: dict) -> dict:
    """benchmark / perf_stats の応答(PerfReportJson の形)から必要な値を取る。"""
    fm_avg = _get(rep, "frameMs", "avg")
    cpu_work = _get(rep, "cpu", "workMs")
    waits = (_get(rep, "cpu", "fenceWaitMs", default=0.0) or 0.0) + (_get(rep, "cpu", "presentMs", default=0.0) or 0.0)
    return {
        "fps": rep.get("fps"),
        "frame_ms_avg": fm_avg,
        "frame_ms_p95": _get(rep, "frameMs", "p95"),
        "gpu_ms": _get(rep, "gpuPassMs", "total"),
        "cpu_ms": (max(0.0, cpu_work - waits) if cpu_work is not None else None),
        "draw_calls": rep.get("drawCalls"),
        "triangles": rep.get("triangles"),
        "gpu_pass_ms": rep.get("gpuPassMs"),
        "render_resolution": rep.get("renderResolution"),
        "verdict": rep.get("verdict"),
    }


def measure(client: EngineClient, frames: int = 300, log=lambda m: None) -> dict:
    log(f"性能計測: benchmark {frames} フレーム")
    rep = client.call("benchmark", {"frames": int(frames), "uncap": True}, timeout=max(180.0, frames / 5.0 + 120.0))
    out = extract(rep)
    if out["gpu_ms"] is None or out["frame_ms_avg"] is None:      # 古い/簡易な応答は perf_stats で補う
        try:
            ps = extract(client.call("perf_stats", {"window": 120}))
            for k, v in ps.items():
                if out.get(k) is None:
                    out[k] = v
        except EngineError:
            pass
    try:
        ping = client.call("ping", timeout=30)
        v = ping.get("vramUsedMB")
        out["vram_mb"] = v if (v is not None and v >= 0) else None
    except EngineError:
        out["vram_mb"] = None
    out["frames"] = frames
    return out


def load_history(scene_id: str) -> list[dict]:
    p = paths.perf_dir() / f"{scene_id}.jsonl"
    if not p.exists():
        return []
    return [json.loads(x) for x in p.read_text(encoding="utf-8").splitlines() if x.strip()]


def append_history(scene_id: str, entry: dict) -> None:
    d = paths.ensure(paths.perf_dir())
    with open(d / f"{scene_id}.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps({"at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), **entry}, ensure_ascii=False) + "\n")


def judge(perf: dict, budgets: dict | None, history: list[dict] | None = None, regression_pct: float = 12.0,
          gpu: str | None = None) -> dict:
    checks, reasons, warnings = [], [], []
    for bkey, limit in (budgets or {}).items():
        key = BUDGET_MAP.get(bkey)
        if key is None:
            continue
        v = perf.get(key)
        c = {"kind": "budget", "metric": key, "label": LABELS[key], "value": v, "limit": limit}
        if v is None:
            c["status"] = "unavailable"
            warnings.append(f"{LABELS[key]}: 取れなかった")
        elif v <= limit:
            c["status"] = "pass"
        else:
            c["status"] = "fail"
            reasons.append(f"{LABELS[key]} = {v:.4g} が予算 {limit:g} を超えた")
        checks.append(c)
    hist = [h for h in (history or []) if (gpu is None or h.get("gpu") in (None, gpu))][-5:]
    if len(hist) >= 3:
        for key in ("gpu_ms", "frame_ms_avg"):
            v = perf.get(key)
            base = [h[key] for h in hist if h.get(key)]
            if v is None or len(base) < 3:
                continue
            med = statistics.median(base)
            pct = (v / med - 1.0) * 100.0 if med > 0 else 0.0
            c = {"kind": "regression", "metric": key, "label": LABELS[key], "value": v, "baseline": med, "pct": pct,
                 "limit": regression_pct, "status": "fail" if pct > regression_pct else "pass"}
            if c["status"] == "fail":
                reasons.append(f"{LABELS[key]} が履歴の中央値 {med:.4g} から {pct:+.1f}% 悪化(上限 {regression_pct:g}%)")
            checks.append(c)
    status = "fail" if reasons else "pass"
    return {"status": status, "checks": checks, "reasons": reasons, "warnings": warnings}
