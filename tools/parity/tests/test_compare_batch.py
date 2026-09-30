"""compare-batch(視覚回帰 MCP 用。判定なしで指標・ヒートマップ・レポートを返す)のテスト。合成 PNG(numpy/PIL)だけ。"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from parity.batch import run_batch
from parity.cli import main

ROOT = Path(__file__).resolve().parents[1]


def _png(path: Path, w=64, h=48, patch=None):
    a = np.zeros((h, w, 3), np.uint8)
    a[..., 0] = np.linspace(20, 200, w, dtype=np.uint8)[None, :]
    a[..., 1] = np.linspace(30, 180, h, dtype=np.uint8)[:, None]
    a[..., 2] = 90
    if patch:
        x, y, pw, ph, col = patch
        a[y:y + ph, x:x + pw] = col
    Image.fromarray(a).save(path)
    return path


def _job(tmp_path, pairs, **extra):
    j = tmp_path / "job.json"
    j.write_text(json.dumps({"title": "t", "pairs": pairs, **extra}), encoding="utf-8")
    return j


def test_bit_exact_and_diff_bbox(tmp_path):
    ref = _png(tmp_path / "ref.png")
    same = _png(tmp_path / "same.png")
    changed = _png(tmp_path / "chg.png", patch=(10, 12, 8, 6, (250, 20, 20)))
    job = _job(tmp_path, [
        {"name": "same", "ref": str(ref), "test": str(same), "verdict": {"status": "pass", "reasons": []}},
        {"name": "chg", "ref": str(ref), "test": str(changed), "verdict": {"status": "fail", "reasons": ["色が変わった"]}},
    ], meta={"gpu": "TestGPU", "driver": "1.0"})
    out = tmp_path / "out"
    r = run_batch(job, out)
    by = {p["name"]: p for p in r["pairs"]}
    assert by["same"]["bitExact"] is True and by["same"]["diffBBox"] is None
    assert by["chg"]["bitExact"] is False
    bb = by["chg"]["diffBBox"]
    assert (bb["x"], bb["y"], bb["w"], bb["h"]) == (10, 12, 8, 6) and bb["pixels"] == 48
    assert by["chg"]["metrics"]["flip_ldr_mean"] > by["same"]["metrics"]["flip_ldr_mean"] == 0
    assert (out / "report.html").exists() and (out / "run.json").exists()
    assert (out / "cam_chg" / "heat_flip_ldr.png").exists() and (out / "cam_chg" / "diff.png").exists()
    run = json.loads((out / "run.json").read_text(encoding="utf-8"))
    cams = {c["name"]: c for c in run["cameras"]}
    assert cams["chg"]["status"] == "fail" and cams["chg"]["reasons"] == ["色が変わった"]
    assert run["verdict"] == "fail"
    html = (out / "report.html").read_text(encoding="utf-8")
    assert "chg" in html and "色が変わった" in html


def test_cli_last_line_is_json_and_exit_zero(tmp_path, capsys):
    ref = _png(tmp_path / "ref.png")
    job = _job(tmp_path, [{"name": "a", "ref": str(ref), "test": str(ref)}])
    code = main(["compare-batch", str(job), "--out", str(tmp_path / "o")])
    assert code == 0
    last = capsys.readouterr().out.strip().splitlines()[-1]
    d = json.loads(last)
    assert d["pairs"][0]["name"] == "a" and Path(d["report"]).exists()


def test_bad_input_exit_code_2(tmp_path, capsys):
    j = tmp_path / "bad.json"
    j.write_text(json.dumps({"pairs": []}), encoding="utf-8")
    assert main(["compare-batch", str(j), "--out", str(tmp_path / "o")]) == 2
    j.write_text(json.dumps({"pairs": [{"name": "x"}]}), encoding="utf-8")
    assert main(["compare-batch", str(j), "--out", str(tmp_path / "o")]) == 2
    assert main(["compare-batch", str(tmp_path / "nope.json"), "--out", str(tmp_path / "o")]) == 2
    ref = _png(tmp_path / "r.png")
    j.write_text(json.dumps({"pairs": [{"name": "d", "ref": str(ref), "test": str(ref)}, {"name": "d", "ref": str(ref), "test": str(ref)}]}), encoding="utf-8")
    assert main(["compare-batch", str(j), "--out", str(tmp_path / "o")]) == 2


def test_missing_image_is_per_pair_error(tmp_path):
    ref = _png(tmp_path / "ref.png")
    job = _job(tmp_path, [{"name": "ok", "ref": str(ref), "test": str(ref)}, {"name": "gone", "ref": str(ref), "test": str(tmp_path / "none.png")}])
    r = run_batch(job, tmp_path / "o")
    by = {p["name"]: p for p in r["pairs"]}
    assert "error" in by["gone"] and by["ok"]["bitExact"] is True
    run = json.loads((tmp_path / "o" / "run.json").read_text(encoding="utf-8"))
    assert run["verdict"] == "error"


def test_module_entry_point(tmp_path):
    ref = _png(tmp_path / "ref.png")
    job = _job(tmp_path, [{"name": "a", "ref": str(ref), "test": str(ref)}])
    p = subprocess.run([sys.executable, "-m", "parity", "compare-batch", str(job), "--out", str(tmp_path / "o")],
                       cwd=str(ROOT), capture_output=True, text=True, encoding="utf-8", env={**__import__("os").environ, "PYTHONUTF8": "1"})
    assert p.returncode == 0, p.stderr
    assert json.loads(p.stdout.strip().splitlines()[-1])["pairs"][0]["bitExact"] is True
