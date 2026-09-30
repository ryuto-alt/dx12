"""compare-batch: 複数の (基準 PNG, 現在 PNG) ペアを 1 回の Python 起動でまとめて評価する(MCP の視覚回帰 dx12_visual_regression 用)。

判定はしない。指標・ヒートマップ・run.json・report.html を作って返すだけ(合否は呼び出し側 = TS が決める)。
入力 JSON:
  {"title": "...", "pairs": [{"name": "front", "ref": "baseline.png", "test": "current.png",
                              "verdict": {"status": "pass|fail|skipped", "reasons": [...]}, "floors": {...}}],
   "meta": {"gpu": "...", "driver": "...", ...}}
出力: DIR/cam_<name>/ に画像、DIR/run.json、DIR/report.html。標準出力の最後の 1 行に
  {"pairs": [{"name", "metrics", "bitExact", "diffBBox", "images", "error"?}], "report": "...", "run": "..."}
PNG は Uno の出力(gamma 2.2)として読む(baseline check と同じ)。
"""
from __future__ import annotations

import json
import time
import traceback
from pathlib import Path

from .evaluate import evaluate_pair
from .imgio import ImageError, load_image
from .report import write_report
from .runner import dumps

_SAFE = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"


def _safe(name: str) -> str:
    s = "".join(c if c in _SAFE else "_" for c in str(name))
    return s or "pair"


def run_batch(job_path: str | Path, out_dir: str | Path, embed: bool = False) -> dict:
    job = json.loads(Path(job_path).read_text(encoding="utf-8"))
    pairs = job.get("pairs")
    if not isinstance(pairs, list) or not pairs:
        raise ValueError("pairs が空か配列でない")
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    meta = job.get("meta") or {}
    cams: list[dict] = []
    result_pairs: list[dict] = []
    statuses: list[str] = []
    seen: set[str] = set()
    for p in pairs:
        if not isinstance(p, dict) or not p.get("name") or not p.get("ref") or not p.get("test"):
            raise ValueError("pairs[] は name / ref / test が必須")
        name = _safe(p["name"])
        if name in seen:
            raise ValueError(f"pairs の name が重複: {name}")
        seen.add(name)
        v = p.get("verdict") or {}
        status = v.get("status") or "captured"
        reasons = list(v.get("reasons") or [])
        cam: dict = {"name": name, "status": status, "reasons": reasons,
                     "reference": {"source": "approved baseline", "file": str(p["ref"])}}
        item: dict = {"name": name}
        try:
            ref = load_image(p["ref"], "gamma22")
            test = load_image(p["test"], "gamma22")
            res = evaluate_pair(ref, test, out / f"cam_{name}", tonemap="engine_aces", exposure=None, size_policy="resize",
                                gate_name=None, regression=True, rel_base=out, ref_label="承認済み baseline", test_label="現在")
            cam.update(res)
            cam["status"] = status
            cam["reasons"] = reasons
            if p.get("floors"):
                cam["floors"] = p["floors"]
                cam["floorK"] = 1.5
            item.update(metrics=res["metrics"], bitExact=res.get("bitExact"), diffBBox=res.get("diffBBox"), images=res.get("images"))
        except Exception as e:  # 1 組の失敗で全体を止めない(その組だけ error として返す)
            cam["status"] = "error"
            cam["reasons"] = reasons + [f"比較に失敗: {e}"]
            cam["traceback"] = traceback.format_exc()[-1200:]
            item["error"] = str(e)
        statuses.append(cam["status"])
        cams.append(cam)
        result_pairs.append(item)
    overall = "error" if "error" in statuses else "fail" if "fail" in statuses else "pass" if "pass" in statuses else "skipped"
    run = {"schema": 1, "runId": out.name, "scene": job.get("title") or "visual", "title": job.get("title") or "視覚回帰",
           "stage": "regression", "gate": "regression", "referenceKind": "previous-run", "specHash": "-", "verdict": overall,
           "cameras": cams, "warnings": list(job.get("warnings") or []), "reasons": list(job.get("reasons") or []),
           "gpu": {"name": meta.get("gpu") or ""}, "engine": {k: v for k, v in meta.items() if k not in ("gpu",)},
           "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S")}
    (out / "run.json").write_text(dumps(run), encoding="utf-8")
    rep = write_report(out, embed=embed)
    return {"pairs": result_pairs, "report": str(rep), "run": str(out / "run.json")}


def main_batch(a) -> int:
    import sys
    try:
        r = run_batch(a.job, a.out, embed=a.embed)
    except (ImageError, ValueError, FileNotFoundError, json.JSONDecodeError) as e:
        print(f"エラー: {e}", file=sys.stderr)
        return 2
    print(json.dumps(r, ensure_ascii=False, default=str))
    return 0
