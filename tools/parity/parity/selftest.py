"""`parity doctor --selftest`: 偽エンジンでランナー全体(撮影 -> 基準 -> 比較 -> 性能 -> レポート -> 判定)を通す。GPU も exe も要らない。"""
from __future__ import annotations

import os
import tempfile
from pathlib import Path

from . import spec as S
from .fake_engine import FakeEngine, FakeLauncher
from .runner import RunOptions, run_scene


def sample_spec_data(scene_id: str = "selftest") -> dict:
    return {
        "specVersion": 1, "id": scene_id, "title": "selftest(偽エンジン)", "status": "ready",
        "scene": {"source": "project", "project": "${PARITY_SELFTEST_PROJECT}", "scene": "scenes/fake.json"},
        "cameras": [{"name": "a", "position": [0, 1, 0], "target": [0, 1, 5]},
                    {"name": "b", "position": [3, 1, 0], "target": [0, 1, 5]}],
        "engine": {"resolution": [320, 180], "warmupFrames": 2, "settleFrames": 2},
        "reference": {"kind": "pt", "pt": {"spp": 16, "seeds": [1, 2]}},
        "alignment": {"tonemap": "engine_aces", "sizePolicy": "resize"},
        "gates": {"G1": {"flip_ldr_mean_max": 0.15, "ssim_min": 0.75, "lum_mean_ev_abs_max": 0.5},
                  "G2": {"flip_ldr_mean_max": 0.08, "ssim_min": 0.9, "lum_mean_ev_abs_max": 0.25}},
        "perf": {"enabled": True, "benchFrames": 30, "budgets": {"gpu_ms_max": 8.0, "vram_mb_max": 4096, "load_sec_max": 30}},
    }


def run_selftest() -> dict:
    old_home, old_proj = os.environ.get("PARITY_HOME"), os.environ.get("PARITY_SELFTEST_PROJECT")
    eng = FakeEngine()
    try:
        with tempfile.TemporaryDirectory(prefix="parity_selftest_") as td:
            os.environ["PARITY_HOME"] = td
            os.environ["PARITY_SELFTEST_PROJECT"] = td
            sp = S.from_data(sample_spec_data())
            run = run_scene(sp, RunOptions(stage="G1", out=Path(td) / "run", port=eng.port, launcher=FakeLauncher(),
                                           log=lambda m: None))
            ok = run["verdict"] == "pass" and (Path(td) / "run" / "report.html").exists()
            return {"verdict": "pass" if ok else "fail", "run": run["verdict"], "reasons": run["reasons"]}
    finally:
        eng.close()
        for k, v in (("PARITY_HOME", old_home), ("PARITY_SELFTEST_PROJECT", old_proj)):
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
