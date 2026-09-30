"""ランナーの統合テスト: 偽エンジン(実エンジンと同じ行 JSON の TCP)で 撮影 -> 基準 -> 比較 -> 性能 -> レポート -> 判定 を通す。"""
import json
from pathlib import Path

import numpy as np
import pytest

from parity import baseline as B
from parity import cli
from parity import spec as S
from parity.fake_engine import synth_scene
from parity.imgio import save_png, write_pfm
from parity.runner import EXIT, RunOptions, run_scene
from parity.tonemap import apply_tonemap
from conftest import spec_data


def opts(fake, launcher, tmp_path, **kw):
    return RunOptions(out=tmp_path / "run", port=fake.port, launcher=launcher, log=lambda m: None, **kw)


def test_pt_reference_pass_and_everything_is_written(fake, launcher, tmp_path):
    run = run_scene(S.from_data(spec_data()), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass" and run["exitCode"] == 0
    assert [c["status"] for c in run["cameras"]] == ["pass", "pass"]
    out = tmp_path / "run"
    for f in ("run.json", "report.html", "spec.json", "log.txt", "cam_a/engine.png", "cam_a/heat_flip_ldr.png", "cam_a/contact.png", "cam_b/diff.png"):
        assert (out / f).exists(), f
    html = (out / "report.html").read_text(encoding="utf-8")
    assert "パリティ・レポート" in html and "cam_a/contact.png" in html and "合格" in html
    saved = json.loads((out / "run.json").read_text(encoding="utf-8"))
    assert saved["cameras"][0]["metrics"]["ssim"] > 0.99
    assert launcher.started == 1 and launcher.stopped == 1               # 起動したら必ず止める
    # 実エンジンの流儀どおりの呼び出し順(open_scene の再試行・決定論スクショ)
    methods = [m for m, _ in fake.calls]
    assert methods.count("open_scene") == 2                              # 1 回目は already in progress で弾かれて撃ち直した
    shot = [p for m, p in fake.calls if m == "screenshot_final"][0]
    assert shot["deterministic"] is True and shot["gizmos"] is False and shot["settleFrames"] == 1


def test_exposure_error_fails_with_reasons_and_exit_code(fake, launcher, tmp_path):
    fake.test_gain = 2.0                                                  # エンジンが +1EV 明るい
    run = run_scene(S.from_data(spec_data()), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "fail" and run["exitCode"] == 1
    assert any("LDR-FLIP" in r for r in run["reasons"])
    assert run["cameras"][0]["worstTiles"]
    assert launcher.stopped == 1


def test_stage_g2_is_stricter_than_g1(fake, launcher, tmp_path):
    fake.test_gain = 1.12                                                 # 約 +0.16 EV
    sp = S.from_data(spec_data())
    g1 = run_scene(sp, opts(fake, launcher, tmp_path / "g1", stage="G1"))
    g2 = run_scene(sp, opts(fake, launcher, tmp_path / "g2", stage="G2"))
    assert g1["verdict"] == "pass"
    assert g2["verdict"] == "fail"


def test_auto_exposure_hides_error_but_ev_gate_catches_it(fake, launcher, tmp_path):
    fake.test_gain = 2.0
    d = spec_data(alignment={"tonemap": "engine_aces", "exposure": "auto", "sizePolicy": "resize"},
                  gates={"G1": {"lum_mean_ev_abs_max": 0.5, "exposure_ev_applied_abs_max": 0.5}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    c = run["cameras"][0]
    assert abs(c["metrics"]["lum_mean_ev"]) < 0.2                          # 整列で見かけ上は消える
    assert abs(c["metrics"]["exposure_ev_applied"]) > 0.8                  # だが適用 EV が残り、ゲートで落とせる
    assert run["verdict"] == "fail"


def test_missing_render_reference_is_skipped_not_passed(fake, launcher, tmp_path):
    fake.has_render_reference = False
    run = run_scene(S.from_data(spec_data()), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "skipped" and run["exitCode"] == EXIT["skipped"] == 3
    assert all(c["status"] == "skipped" for c in run["cameras"])
    assert "render_reference" in run["cameras"][0]["reasons"][0]
    assert launcher.stopped == 1


def test_external_reference_from_folder_with_sidecar(fake, launcher, tmp_path):
    refdir = tmp_path / "refs"
    for name, gain in (("a", 1.0), ("b", 1.0)):
        cam = {"a": ([0, 1, 0], [0, 1, 5]), "b": ([3, 1, 0], [0, 1, 5])}[name]
        d, _ = apply_tonemap("aces_narkowicz", synth_scene(fake.w, fake.h, *cam) * gain)
        save_png(refdir / f"{name}.png", d)
    (refdir / "a.json").write_text(json.dumps({"provenance": "UE 5.x 手動撮影", "colorspace": "srgb"}), encoding="utf-8")
    d = spec_data(reference={"kind": "external", "external": {"dir": str(refdir)}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass"
    assert run["cameras"][0]["reference"]["provenance"] == "UE 5.x 手動撮影"
    assert "render_reference" not in [m for m, _ in fake.calls]            # PT は呼ばれない


def test_external_reference_missing_is_skipped_with_help(fake, launcher, tmp_path):
    d = spec_data(reference={"kind": "external", "external": {"dir": str(tmp_path / "nowhere")}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "skipped"
    assert "外部基準が無い" in run["cameras"][0]["reasons"][0]


def test_size_policy_resize_and_error_for_reference_of_other_resolution(fake, launcher, tmp_path):
    refdir = tmp_path / "refs"
    for name, cam in (("a", ([0, 1, 0], [0, 1, 5])), ("b", ([3, 1, 0], [0, 1, 5]))):
        d, _ = apply_tonemap("engine_aces", synth_scene(fake.w * 2, fake.h * 2, *cam))
        save_png(refdir / f"{name}.png", d)
    base = dict(reference={"kind": "external", "external": {"dir": str(refdir), "colorspace": "gamma22"}})
    ok = run_scene(S.from_data(spec_data(**base)), opts(fake, launcher, tmp_path / "r1"))
    assert ok["verdict"] == "pass" and ok["cameras"][0]["size"]["sizePolicy"] == "resize"
    strict = spec_data(alignment={"sizePolicy": "error"}, **base)
    bad = run_scene(S.from_data(strict), opts(fake, launcher, tmp_path / "r2"))
    assert bad["verdict"] == "error" and bad["exitCode"] == 2
    assert "大きさが違う" in bad["cameras"][0]["reasons"][0]


def test_regions_are_aggregated_and_gated(fake, launcher, tmp_path):
    from PIL import Image
    mask = np.zeros((fake.h, fake.w), np.uint8)
    mask[: fake.h // 2, :] = 255                                          # 上半分 = 空
    Image.fromarray(mask).save(tmp_path / "sky.png")
    fake.mutate = lambda lin: np.where((np.mgrid[0:lin.shape[0], 0:lin.shape[1]][0] < lin.shape[0] // 2)[..., None], lin * 0.1, lin)
    d = spec_data(regions=[{"name": "sky", "mask": str(tmp_path / "sky.png"), "gates": {"G1": {"lum_mean_ev_abs_max": 0.5}}}],
                  gates={"G1": {"flip_ldr_mean_max": 0.9}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    c = run["cameras"][0]
    assert abs(c["regions"]["sky"]["lum_mean_ev"]) > 1.0
    assert run["verdict"] == "fail" and any("領域:sky" in r for r in run["reasons"])


def test_perf_budget_and_history_regression(fake, launcher, tmp_path):
    d = spec_data(perf={"enabled": True, "benchFrames": 30, "budgets": {"gpu_ms_max": 5.0, "vram_mb_max": 4096}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path / "p1"))
    assert run["perf"]["status"] == "fail" and run["verdict"] == "fail"
    assert any("GPU ms" in r for r in run["reasons"])
    assert run["perf"]["measured"]["vram_mb"] == 2048.0 and run["perf"]["measured"]["load_sec"] is not None
    # 履歴(直近 5 回の中央値)から悪化
    fake.gpu_ms = 3.0
    ok = spec_data(perf={"enabled": True, "benchFrames": 30, "budgets": {"gpu_ms_max": 20.0}, "regression_pct_max": 12})
    sp = S.from_data(ok)
    for i in range(3):
        assert run_scene(sp, opts(fake, launcher, tmp_path / f"h{i}"))["verdict"] == "pass"
    fake.gpu_ms = 4.0                                                     # +33%
    reg = run_scene(sp, opts(fake, launcher, tmp_path / "hreg"))
    assert reg["verdict"] == "fail" and any("悪化" in r for r in reg["reasons"])


def test_offline_test_images_do_not_start_the_engine(launcher, tmp_path):
    imgs = tmp_path / "imgs"
    refs = tmp_path / "refs"
    for n in ("a", "b"):
        d, _ = apply_tonemap("engine_aces", synth_scene(160, 90, [0, 1, 0], [0, 1, 5]))
        save_png(imgs / f"{n}.png", d)
        save_png(refs / f"{n}.png", d)
    d = spec_data(reference={"kind": "external", "external": {"dir": str(refs), "colorspace": "gamma22"}})
    run = run_scene(S.from_data(d), RunOptions(out=tmp_path / "run", test_images_dir=imgs, launcher=launcher, log=lambda m: None))
    assert run["verdict"] == "pass" and launcher.started == 0
    assert run["cameras"][0]["metrics"]["flip_ldr_mean"] == 0


def test_stub_spec_is_skipped_without_engine(launcher, tmp_path):
    run = run_scene(S.from_data(spec_data(status="stub")), RunOptions(out=tmp_path / "run", launcher=launcher, log=lambda m: None))
    assert run["verdict"] == "skipped" and launcher.started == 0


def test_generator_scene_and_setup_calls(fake, launcher, tmp_path):
    gen = tmp_path / "gen.py"
    gen.write_text("import argparse,json,pathlib\n"
                   "a=argparse.ArgumentParser();a.add_argument('--out');a.add_argument('--id');a.add_argument('--args-json')\n"
                   "x=a.parse_args();p=pathlib.Path(x.out);(p/'assets'/'scenes').mkdir(parents=True);(p/'assets'/'scenes'/(x.id+'.json')).write_text('{}')\n", encoding="utf-8")
    d = spec_data(scene={"source": "generator", "script": str(gen), "args": {}},
                  engine={"warmupFrames": 1, "settleFrames": 1, "setup": [
                      {"method": "set_sun", "params": {"intensity": 1.0}, "optional": True}]})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass"
    assert any("set_sun" in w for w in run["warnings"])                   # 偽エンジンに無い method は optional なので警告だけ
    assert [p for m, p in fake.calls if m == "open_scene"][-1]["path"] == "scenes/t_scene.json"


def test_attach_does_not_start_or_stop(fake, launcher, tmp_path):
    run = run_scene(S.from_data(spec_data()), opts(fake, launcher, tmp_path, attach_port=fake.port))
    assert run["verdict"] == "pass" and launcher.started == 0 and launcher.stopped == 0


def test_mcp_job_env_port_means_attach(fake, launcher, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("DX12_MCP_PORT", str(fake.port))
    run = run_scene(S.from_data(spec_data()), opts(fake, launcher, tmp_path, progress=True))
    out = capsys.readouterr().out
    assert launcher.started == 0
    prog = [json.loads(l[len("@progress "):]) for l in out.splitlines() if l.startswith("@progress ")]
    assert prog and prog[0]["pct"] < prog[-1]["pct"]
    res = [json.loads(l[len("@result "):]) for l in out.splitlines() if l.startswith("@result ")]
    assert res[-1]["verdict"] == "pass" and res[-1]["exitCode"] == 0


# ── baseline(視覚回帰) ───────────────────────────────────────────────────

def test_baseline_update_approve_check_cycle(fake, launcher, tmp_path):
    sp = S.from_data(spec_data(reference={"kind": "previous-run"}))
    store = B.BaselineStore()
    cap = run_scene(sp, opts(fake, launcher, tmp_path / "c", capture_only=True))
    assert cap["verdict"] == "pass" and store.cameras("t_scene", "pending") == ["a", "b"]
    # 承認前は基準が無い = skipped(合格扱いにしない)
    r0 = run_scene(sp, opts(fake, launcher, tmp_path / "r0", stage="regression"))
    assert r0["verdict"] == "skipped"
    with pytest.raises(ValueError):
        store.approve("t_scene", "")
    with pytest.raises(ValueError):
        store.approve("t_scene", "ok")
    done = store.approve("t_scene", "初回の基準", approver="test")
    assert len(done) == 2 and store.cameras("t_scene", "approved") == ["a", "b"] and not store.cameras("t_scene", "pending")
    assert store.history("t_scene")[0]["reason"] == "初回の基準"
    # 同じ絵 = ビット一致で合格
    ok = run_scene(sp, opts(fake, launcher, tmp_path / "r1", stage="regression"))
    assert ok["verdict"] == "pass" and all(c["bitExact"] for c in ok["cameras"])
    # 色を 1 箇所変える = fail + diffBBox
    def paint(lin):
        lin = lin.copy()
        lin[20:40, 30:60] = [2.0, 0.0, 0.0]
        return lin
    fake.mutate = paint
    bad = run_scene(sp, opts(fake, launcher, tmp_path / "r2", stage="regression"))
    assert bad["verdict"] == "fail"
    bb = bad["cameras"][0]["diffBBox"]
    assert 25 <= bb["x"] <= 32 and 15 <= bb["y"] <= 22 and 25 <= bb["w"] <= 35 and 15 <= bb["h"] <= 25
    assert bad["cameras"][0]["bitExact"] is False


def test_baseline_of_another_gpu_is_skipped(fake, launcher, tmp_path, monkeypatch):
    sp = S.from_data(spec_data(reference={"kind": "previous-run"}))
    run_scene(sp, opts(fake, launcher, tmp_path / "c", capture_only=True))
    B.BaselineStore().approve("t_scene", "別 GPU 判定の準備")
    monkeypatch.setenv("PARITY_GPU_KEY", "OtherGPU")
    r = run_scene(sp, opts(fake, launcher, tmp_path / "r", stage="regression"))
    assert r["verdict"] == "skipped" and "別の GPU" in r["cameras"][0]["reasons"][0]


def test_approve_updates_history_with_previous_sha(fake, launcher, tmp_path):
    sp = S.from_data(spec_data(reference={"kind": "previous-run"}))
    store = B.BaselineStore()
    run_scene(sp, opts(fake, launcher, tmp_path / "c1", capture_only=True))
    store.approve("t_scene", "1 回目")
    fake.test_gain = 1.5
    run_scene(sp, opts(fake, launcher, tmp_path / "c2", capture_only=True))
    d = store.approve("t_scene", "露出を意図して変えた", cameras=["a"])
    assert d[0]["previousSha256"] and d[0]["sha256"] != d[0]["previousSha256"]
    assert store.cameras("t_scene", "pending") == ["b"]


# ── CLI ───────────────────────────────────────────────────────────────────

def test_cli_compare_exit_codes_and_outputs(tmp_path, make_png):
    from conftest import gradient
    a = make_png("a.png", gradient())
    b = make_png("b.png", np.clip(gradient() * 0.4, 0, 1))
    out = tmp_path / "o"
    assert cli.main(["compare", a, a, "--out", str(out / "same"), "--gate", "G1", "--limit", "flip_ldr_mean_max=0.01"]) == 0
    assert cli.main(["compare", a, b, "--out", str(out / "diff"), "--limit", "lum_mean_ev_abs_max=0.5"]) == 1
    assert (out / "diff" / "report.html").exists() and (out / "diff" / "cam_compare" / "heat_flip_ldr.png").exists()
    assert cli.main(["compare", a, b, "--out", str(out / "nogate")]) == 0            # しきい値なし = 指標を出すだけ
    assert cli.main(["compare", a, str(tmp_path / "nope.png")]) == 2


def test_cli_compare_size_mismatch_and_regions(tmp_path, make_png):
    from PIL import Image
    from conftest import gradient
    a = make_png("a.png", gradient(90, 160))
    b = make_png("b.png", gradient(45, 80))
    assert cli.main(["compare", a, b, "--out", str(tmp_path / "e")]) == 2
    assert cli.main(["compare", a, b, "--size-policy", "resize", "--out", str(tmp_path / "r")]) == 0
    m = np.zeros((90, 160), np.uint8)
    m[:, :80] = 255
    Image.fromarray(m).save(tmp_path / "left.png")
    assert cli.main(["compare", a, a, "--mask", f"left={tmp_path / 'left.png'}", "--out", str(tmp_path / "m")]) == 0
    run = json.loads((tmp_path / "m" / "run.json").read_text(encoding="utf-8"))
    assert "left" in run["cameras"][0]["regions"]


def test_cli_noise_floor_and_calibrate(tmp_path, make_png):
    from conftest import gradient
    rng = np.random.default_rng(0)
    imgs = [make_png(f"n{i}.png", np.clip(gradient() + rng.normal(0, 0.02, (90, 160, 3)), 0, 1)) for i in range(3)]
    out = tmp_path / "nf.json"
    assert cli.main(["noise-floor", *imgs, "--out", str(out)]) == 0
    nf = json.loads(out.read_text(encoding="utf-8"))
    assert nf["floors"]["flip_ldr_mean"] > 0 and nf["n_images"] == 3
    # 床つきの compare: 固定値だけなら落ちる厳しさでもノイズ床で通る
    a = make_png("clean.png", gradient())
    tight = ["--limit", "flip_ldr_mean_max=0.001"]
    assert cli.main(["compare", a, imgs[0], "--out", str(tmp_path / "c1"), *tight]) == 1
    assert cli.main(["compare", a, imgs[0], "--out", str(tmp_path / "c2"), *tight, "--noise-floor", str(out)]) == 0
    # calibrate
    from parity.imgio import write_pfm
    lin = (gradient() * 4).astype(np.float32)
    write_pfm(tmp_path / "lin.pfm", lin)
    d, _ = apply_tonemap("aces_hill", lin)
    ref = make_png("ref.png", d)
    assert cli.main(["calibrate", str(tmp_path / "lin.pfm"), ref, "--out", str(tmp_path / "cal")]) == 0
    cal = json.loads((tmp_path / "cal" / "calibrate.json").read_text(encoding="utf-8"))
    assert cal["ranking"][0]["tonemap"] == "aces_hill"


def test_cli_run_uses_exit_codes(fake, launcher, tmp_path, monkeypatch):
    spec_file = tmp_path / "s.json"
    spec_file.write_text(json.dumps(spec_data()), encoding="utf-8")
    monkeypatch.setattr("parity.runner.PwshLauncher", lambda *a, **k: launcher)
    assert cli.main(["run", str(spec_file), "--stage", "G1", "--port", str(fake.port), "--out", str(tmp_path / "o1")]) == 0
    fake.has_render_reference = False
    assert cli.main(["run", str(spec_file), "--port", str(fake.port), "--out", str(tmp_path / "o2")]) == 3
    assert cli.main(["run", str(spec_file), "--port", str(fake.port), "--out", str(tmp_path / "o3"), "--skipped-ok"]) == 0
    fake.has_render_reference = True
    fake.test_gain = 3.0
    assert cli.main(["run", str(spec_file), "--port", str(fake.port), "--out", str(tmp_path / "o4")]) == 1
    assert cli.main(["run", str(tmp_path / "missing.json")]) == 2


def test_cli_baseline_commands(fake, launcher, tmp_path, monkeypatch):
    spec_file = tmp_path / "s.json"
    spec_file.write_text(json.dumps(spec_data(reference={"kind": "previous-run"})), encoding="utf-8")
    monkeypatch.setattr("parity.runner.PwshLauncher", lambda *a, **k: launcher)
    base = ["--port", str(fake.port)]
    assert cli.main(["baseline", "update", str(spec_file), *base]) == 0
    assert cli.main(["baseline", "approve", "t_scene"]) == 2                          # 理由なし
    assert cli.main(["baseline", "approve", "t_scene", "--reason", "初回の基準"]) == 0
    assert cli.main(["baseline", "check", str(spec_file), *base]) == 0
    fake.test_gain = 1.4
    assert cli.main(["baseline", "check", str(spec_file), *base]) == 1
    assert cli.main(["baseline", "list"]) == 0


def test_doctor_selftest_and_scenes():
    assert cli.main(["doctor", "--selftest"]) == 0
    assert cli.main(["scenes"]) == 0


def test_pt_size_params_pass_the_engine_image_size(fake, launcher, tmp_path):
    d = spec_data(reference={"kind": "pt", "pt": {"spp": 8, "api": "legacy", "sizeParams": ["width", "height"], "params": {"bounces": 4}}})
    run = run_scene(S.from_data(d), opts(fake, launcher, tmp_path))
    assert run["verdict"] == "pass"
    req = [p for m, p in fake.calls if m == "render_reference"][0]
    assert (req["width"], req["height"]) == (fake.w, fake.h) and req["bounces"] == 4 and req["spp"] == 8 and req["path"].endswith(".pfm")
