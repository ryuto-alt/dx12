"""SPK-1 の準備: 多灯シーン(scenes/manylights/ml_64, ml_256)の PT 参照とラスタ基準線を撮る。

  tools\\parity\\.venv\\Scripts\\python.exe -m parity.spk1_capture --build-dir <build\\release のあるフォルダ> [--scenes ml_64] [--spp 256]

gi_eval の撮影(capture_all)をそのまま使い、シーンの置き場だけ scenes/manylights に替える。評価(合否)はしない。
出力 <out>/<シーン>/<カメラ>/pt.pfm・raster_legacy.pfm と、見るための PNG(pt.png / raster_legacy.png)・capture.json。
エンジンは engine_instance.ps1 で背景起動(name=gi)して終わりに必ず止める。窓は出ない。詳細は tools/parity/spk1_protocol.md。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

import numpy as np

from . import gi_eval as GE
from . import gi_metrics as GM
from . import gi_scene as GS
from . import paths
from .imgio import load_image, save_png

MANYLIGHTS_DIR = paths.TOOL_DIR / "scenes" / "manylights"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="spk1_capture")
    ap.add_argument("--scenes", default="all", help="ml_64,ml_256 のカンマ区切り(シーン id は ML-64 / ML-256。どちらの綴りでも可)")
    ap.add_argument("--configs", default="legacy", help="ラスタの構成(gi_eval.CONFIGS から。既定 legacy = 現行のラスタ)")
    ap.add_argument("--out")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--spp", type=int, default=1024)
    ap.add_argument("--bounces", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pt-timeout", type=float, default=1800.0)
    ap.add_argument("--no-pt", dest="pt", action="store_false")
    ap.add_argument("--capture-mode", choices=("free", "det"), default="det")
    ap.add_argument("--settle", type=int, default=600)
    ap.add_argument("--det-settle", type=int, default=240)
    ap.add_argument("--port", type=int, default=GE.DEFAULT_PORT)
    ap.add_argument("--attach", type=int)
    ap.add_argument("--build-dir")
    ap.add_argument("--frame-budget-ms", type=float, default=None)
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
        if c not in GE.CONFIGS:
            raise SystemExit(f"構成が不明: {c}")
    scenes = GS.discover_scenes(MANYLIGHTS_DIR)
    if a.scenes != "all":
        want = {x.strip().lower().replace("-", "_") for x in a.scenes.split(",")}
        scenes = [s for s in scenes if s.id.lower().replace("-", "_") in want]
        if not scenes:
            raise SystemExit(f"シーンが無い: {a.scenes}")
    out = Path(a.out).resolve() if a.out else paths.REPO_ROOT / ".dx12" / "parity" / "spk1" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    log(f"出力: {out}")
    manifest = GE.capture_all(a, scenes, out, log)
    summary: dict = {"scenes": {}}
    for sid, sm in manifest["scenes"].items():
        for cn, ce in sm["cameras"].items():
            files = {"pt": (ce.get("pt") or {}).get("file")}
            files.update({f"raster_{k}": v.get("file") for k, v in ce["configs"].items()})
            row = {}
            pt_ev = None
            for key, f in files.items():
                if not f or not Path(f).exists():
                    continue
                lin = np.asarray(load_image(f, "linear").rgb, dtype=np.float32)
                y = GM.luminance(lin)
                if key == "pt":
                    pt_ev = GE._display_ev(lin, {})
                if pt_ev is not None:
                    save_png(Path(f).with_suffix(".png"), GE._tm(lin, pt_ev))
                row[key] = {"file": f, "meanLuminance": float(np.mean(y)), "maxLuminance": float(np.max(y))}
            summary["scenes"].setdefault(sid, {})[cn] = row
    (out / "capture.json").write_text(json.dumps({"manifest": manifest, "summary": summary}, indent=1, ensure_ascii=False, default=GE._json_default),
                                      encoding="utf-8")
    for sid, cams in summary["scenes"].items():
        for cn, row in cams.items():
            print(f"{sid}/{cn}: " + ", ".join(f"{k} mean={v['meanLuminance']:.4f}" for k, v in row.items()))
    print(f"結果: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
