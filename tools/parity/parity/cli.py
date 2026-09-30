"""コマンドライン: python -m parity <サブコマンド>。

  compare       2 枚の画像を比べる(指標・ヒートマップ・HTML レポート。--limit で機械判定)
  report        run ディレクトリから report.html を作り直す
  noise-floor   同じシーンを別シードで撮った画像からノイズ床を出す
  calibrate     リニア画像を外部基準(UE のスクショ等)へ合わせるトーンマップと EV の候補を出す
  run           シーン仕様を自動で回す(エンジン起動 -> 撮影 -> 基準 -> 比較 -> 性能 -> レポート -> 合否)
  baseline      視覚回帰: update(候補を撮る)/ approve(理由つきで承認)/ check(差分検出)/ list
  scenes        シーン仕様の一覧・検証
  doctor        依存と環境の確認(--selftest で偽エンジンによる通し確認)

終了コード(compare --limit / run / baseline check): 0 = 合格 / 1 = 不合格 / 2 = エラー / 3 = skipped のみ(--skipped-ok で 0)。
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from . import __version__
from . import baseline as B
from . import gate as G
from . import metrics as M
from . import noisefloor as NF
from . import paths
from . import spec as S
from . import tonemap as T
from .evaluate import evaluate_pair
from .imgio import ImageError, load_image, save_png
from .report import write_report
from .runner import EXIT, RunOptions, dumps, run_scene


def _utf8() -> None:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _parse_limits(items: list[str] | None) -> dict:
    out = {}
    for it in items or []:
        if "=" not in it:
            raise SystemExit(f"--limit は key=value(例 flip_ldr_mean_max=0.15): {it}")
        k, v = it.split("=", 1)
        G.parse_key(k.strip())
        out[k.strip()] = float(v)
    return out


def _parse_masks(items: list[str] | None) -> list[dict]:
    out = []
    for it in items or []:
        if "=" not in it:
            raise SystemExit(f"--mask は name=path.png か name=path.png:r,g,b(ラベルマップ): {it}")
        name, rest = it.split("=", 1)
        color = None
        if rest.count(":") >= 1 and rest.rsplit(":", 1)[1].replace(",", "").isdigit() and "," in rest.rsplit(":", 1)[1]:
            rest, c = rest.rsplit(":", 1)
            color = [int(x) for x in c.split(",")]
        d = {"name": name, "mask": rest}
        if color:
            d["color"] = color
        out.append(d)
    return out


def _print_metrics(m: dict, prefix: str = "  ") -> None:
    for k, lab in M.LABELS.items():
        if k in m:
            print(f"{prefix}{lab:<28} {m[k]:.5g}")


def _print_verdict(v: dict | None) -> None:
    if not v:
        return
    print(f"  判定: {v['status']}")
    for r in v["reasons"]:
        print(f"    - {r}")
    for w in v["warnings"]:
        print(f"    (警告) {w}")


# ── compare ─────────────────────────────────────────────────────────────────

def cmd_compare(a: argparse.Namespace) -> int:
    out = Path(a.out) if a.out else paths.runs_dir() / f"{time.strftime('%Y%m%d-%H%M%S')}_compare"
    ref = load_image(a.reference, a.ref_colorspace)
    test = load_image(a.test, a.test_colorspace)
    limits = _parse_limits(a.limit)
    gate_name, gate_full, gate_regions, floors, k = None, {}, {}, None, 1.5
    if a.gate:
        gate_name = a.gate
        if a.spec:
            sp = S.load_spec(a.spec)
            gate_name, gate_full = G.resolve_gate(sp.data, a.gate)
            gate_regions = G.gate_regions_for(sp.data, gate_name)
        gate_full = {**gate_full, **limits}
    elif limits:
        gate_name, gate_full = "custom", limits
    if a.noise_floor:
        nf = NF.load(a.noise_floor)
        floors, k = nf.get("floors"), a.k or 1.5
    regions = _parse_masks(a.mask)
    if a.spec and not regions:
        regions = S.load_spec(a.spec).data.get("regions") or []
    res = evaluate_pair(ref, test, out / "cam_compare", tonemap=a.tonemap, exposure=a.exposure, size_policy=a.size_policy,
                        metrics=a.metrics, ppd=a.ppd, regions=regions, region_base=Path(a.spec).parent if a.spec else Path.cwd(),
                        gate_name=gate_name, gate_full=gate_full, gate_regions=gate_regions, floors=floors, k=k,
                        strict=a.strict, rel_base=out, ref_label=Path(a.reference).name, test_label=Path(a.test).name)
    status = (res.get("verdict") or {}).get("status", "captured")
    run = {"schema": 1, "runId": out.name, "scene": "compare", "title": f"{Path(a.reference).name} vs {Path(a.test).name}",
           "stage": a.gate or "-", "gate": gate_name or "-", "referenceKind": "files", "specHash": "-",
           "verdict": status if gate_name else "-", "cameras": [{"name": "compare", **res, "status": status,
                                                                "reasons": (res.get("verdict") or {}).get("reasons", []),
                                                                "reference": {"source": "files", "file": str(a.reference)}}],
           "warnings": [], "reasons": [], "gpu": {}, "engine": {}}
    out.mkdir(parents=True, exist_ok=True)
    (out / "run.json").write_text(dumps(run), encoding="utf-8")
    rep = write_report(out, embed=a.embed)
    if a.json:
        print(dumps(res["metrics"]))
    else:
        print(f"基準: {a.reference}\nテスト: {a.test}\nサイズ: {res['size']}  整列: {res['align'].get('tonemap')} / 露出 {res['align'].get('exposure')}")
        _print_metrics(res["metrics"])
        for n, m in res["regions"].items():
            print(f"  [領域 {n}]")
            _print_metrics(m, "    ")
        for note in res["notes"]:
            print(f"  (注) {note}")
        _print_verdict(res.get("verdict"))
        print(f"出力: {out}\nレポート: {rep}")
    if not gate_name:
        return 0
    return {"pass": 0, "fail": 1}.get(status, 2)


# ── report / noise-floor / calibrate ───────────────────────────────────────

def cmd_report(a: argparse.Namespace) -> int:
    p = write_report(a.run_dir, embed=a.embed, out_name=a.name)
    print(p)
    return 0


def cmd_noise(a: argparse.Namespace) -> int:
    imgs = [load_image(p, a.colorspace) for p in a.images]
    res = NF.measure(imgs, a.tonemap, a.exposure, a.pairs, a.correction)
    out = Path(a.out) if a.out else (paths.noise_dir() / a.scene / f"{a.camera}.json" if a.scene and a.camera else None)
    print(f"画像 {len(imgs)} 枚 / ペア {len(res['pairs'])} / 補正 {res['correction']}")
    for k, v in res["floors"].items():
        print(f"  {M.LABELS.get(k, k):<28} 床 = {v:.5g}")
    if out:
        NF.save(out, res, {"images": [str(p) for p in a.images], "k": a.k})
        print(f"保存: {out}")
    return 0


def cmd_calibrate(a: argparse.Namespace) -> int:
    lin = load_image(a.linear, "linear")
    ref = load_image(a.reference, a.ref_colorspace)
    if ref.kind != "display":
        print("基準は PNG など表示参照の画像を渡す(UE のスクリーンショット)", file=sys.stderr)
        return 2
    from .imgio import ensure_same_size
    lin, ref, _ = ensure_same_size(lin, ref, a.size_policy)
    cands = [c for c in a.candidates.split(",")] if a.candidates else None
    ranking = T.rank_tonemaps(lin.rgb, ref, cands)
    print("トーンマップ候補(小さいほど分布が近い。EV は中央輝度を合わせたときの値):")
    for r in ranking:
        print(f"  {r['tonemap']:<18} EV {r['ev']:+.2f}   輝度分位 EMD {r['emd']:.4f}   {r['note']}")
    out = Path(a.out) if a.out else paths.runs_dir() / f"{time.strftime('%Y%m%d-%H%M%S')}_calibrate"
    out.mkdir(parents=True, exist_ok=True)
    best = ranking[0]
    d, _ = T.apply_tonemap(best["tonemap"], T.apply_ev(lin.rgb, best["ev"]))
    save_png(out / "best_tonemapped.png", d)
    (out / "calibrate.json").write_text(dumps({"ranking": ranking, "linear": str(a.linear), "reference": str(a.reference)}), encoding="utf-8")
    print(f"最良: {best['tonemap']} / EV {best['ev']:+.2f}(この組み合わせの画像: {out / 'best_tonemapped.png'})")
    print("注意: グレーカード等の既知輝度が無い前提のヒストグラム合わせ。EV は参考値で、絶対的な校正ではない(docs/PARITY_HARNESS.md §校正)。")
    return 0


# ── run / baseline ─────────────────────────────────────────────────────────

def _opts_from(a: argparse.Namespace, **kw) -> RunOptions:
    o = RunOptions(stage=getattr(a, "stage", "G1"), out=Path(a.out) if getattr(a, "out", None) else None,
                   reference=getattr(a, "reference", None), attach_port=getattr(a, "attach", None), port=getattr(a, "port", 8820),
                   name=getattr(a, "name", "par"), mode=getattr(a, "mode", None), cameras=getattr(a, "camera", None) or None,
                   no_perf=getattr(a, "no_perf", False), strict=getattr(a, "strict", False),
                   project_override=getattr(a, "project", None),
                   test_images_dir=Path(a.test_images) if getattr(a, "test_images", None) else None,
                   external_dir=Path(a.external_dir) if getattr(a, "external_dir", None) else None,
                   progress=getattr(a, "progress", False), want_noise=getattr(a, "want_noise", False),
                   no_perf_history=getattr(a, "no_perf_history", False),
                   baseline_root=Path(a.baseline_root) if getattr(a, "baseline_root", None) else None)
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def cmd_run(a: argparse.Namespace) -> int:
    sp = S.load_spec(a.spec, check_masks=True)
    for w in sp.warnings:
        print(f"(警告) {w}", file=sys.stderr)
    run = run_scene(sp, _opts_from(a))
    print(f"\n判定: {run['verdict']}(終了コード {run['exitCode']})  {run['durationSec']} 秒")
    for c in run["cameras"]:
        m = c.get("metrics") or {}
        line = f"  {c['name']:<16} {c.get('status'):<8}"
        if m:
            line += (f" FLIP={m.get('flip_ldr_mean', float('nan')):.4f} SSIM={m.get('ssim', float('nan')):.4f} "
                     f"dE中央={m.get('de2000_median', float('nan')):.2f} 輝度差={m.get('lum_mean_ev', float('nan')):+.3f}EV")
        print(line)
    if run.get("perf"):
        pm = run["perf"].get("measured", {})
        print(f"  性能: {run['perf']['status']}  GPU {pm.get('gpu_ms')} ms / frame {pm.get('frame_ms_avg')} ms / VRAM {pm.get('vram_mb')} MB / ロード {pm.get('load_sec')} 秒")
    seen = []
    for r in run["reasons"]:
        key = r.split(": ", 1)[-1]                       # カメラ名を除いた文面が同じ理由は 1 回だけ出す
        if key not in seen:
            seen.append(key)
            print(f"  - {r}")
    print(f"レポート: {Path(run['runDir']) / 'report.html'}")
    code = run["exitCode"]
    return 0 if (a.skipped_ok and code == EXIT["skipped"]) else code


def cmd_baseline(a: argparse.Namespace) -> int:
    store = B.BaselineStore(Path(a.baseline_root) if a.baseline_root else None)
    act = a.action
    if act == "list":
        rows = store.list(a.spec_or_id if a.spec_or_id else None)
        if a.json:
            print(dumps(rows))
        else:
            for r in rows:
                print(f"{r['scene']:<28} {r['camera']:<14} {r['kind']:<9} {r['sha256']} gpu={r['gpu']} {r['reason'] or ''}")
            if not rows:
                print("baseline は無い")
        return 0
    if not a.spec_or_id:
        print("シーン仕様(または --scene-id)が要る", file=sys.stderr)
        return 2
    is_file = a.spec_or_id.endswith(".json") or Path(a.spec_or_id).exists()
    sp = S.load_spec(a.spec_or_id) if is_file else None
    sid = sp.id if sp else a.spec_or_id
    if act == "approve":
        try:
            done = store.approve(sid, a.reason or "", a.camera or None, a.approver or "")
        except ValueError as e:
            print(f"承認できない: {e}", file=sys.stderr)
            return 2
        for d in done:
            print(f"承認: {d['scene']}/{d['camera']}  {d['sha256'][:12]}  (前 {str(d['previousSha256'])[:12]})  理由: {d['reason']}")
        return 0
    if act == "discard":
        print(f"pending を {store.discard_pending(sid)} 件捨てた")
        return 0
    if sp is None:
        print("update / check にはシーン仕様の JSON が要る", file=sys.stderr)
        return 2
    if act == "update":
        run = run_scene(sp, _opts_from(a, capture_only=True, no_perf=True))
        print(f"撮影: {run['verdict']}  pending: {store.cameras(sid, 'pending')}")
        for r in run["reasons"]:
            print(f"  - {r}")
        print("承認するには目視の後: parity baseline approve <spec> --reason \"何が変わって、なぜ受け入れるか\"")
        return 0 if run["verdict"] == "pass" else 2
    if act == "check":
        run = run_scene(sp, _opts_from(a, stage="regression", reference="previous-run", no_perf=True))
        print(f"視覚回帰: {run['verdict']}  {run['durationSec']} 秒")
        for c in run["cameras"]:
            m = c.get("metrics") or {}
            bb = c.get("diffBBox")
            print(f"  {c['name']:<16} {c.get('status'):<8} bitExact={c.get('bitExact')} FLIP={m.get('flip_ldr_mean', float('nan')):.5f}"
                  + (f" diffBBox=({bb['x']},{bb['y']},{bb['w']}x{bb['h']})" if bb else ""))
        for r in run["reasons"][:12]:
            print(f"  - {r}")
        code = run["exitCode"]
        return 0 if (a.skipped_ok and code == EXIT["skipped"]) else code
    return 2


# ── scenes / doctor ───────────────────────────────────────────────────────

def cmd_scenes(a: argparse.Namespace) -> int:
    files = [Path(f) for f in a.files] if a.files else S.list_specs()
    bad = 0
    for f in files:
        try:
            sp = S.load_spec(f, check_masks=a.check_masks)
            print(f"ok    {sp.id:<26} {sp.data.get('parityScene') or '-':<5} {sp.status:<6} ref={sp.reference['kind']:<12} cameras={len(sp.data['cameras'])}  {f.name}")
            for w in sp.warnings:
                print(f"      (警告) {w}")
        except S.SpecError as e:
            bad += 1
            print(f"NG    {f.name}: {e}")
    return 1 if bad else 0


def cmd_doctor(a: argparse.Namespace) -> int:
    ok = True
    print(f"parity {__version__} / Python {sys.version.split()[0]}")
    for mod, why in (("numpy", "必須"), ("scipy", "必須"), ("PIL", "必須(Pillow)"), ("skimage", "SSIM / ΔE2000"),
                     ("flip_evaluator", "FLIP(主指標)"), ("jsonschema", "シーン仕様の検証"), ("cv2", "任意: 16bit PNG / EXR / HDR"), ("pytest", "テスト")):
        try:
            __import__(mod)
            try:
                from importlib.metadata import version
                ver = version({"PIL": "pillow", "skimage": "scikit-image", "flip_evaluator": "flip-evaluator", "cv2": "opencv-python-headless"}.get(mod, mod))
            except Exception:
                ver = ""
            print(f"  ok   {mod:<15} {ver}  ({why})")
        except ImportError:
            hard = why.startswith("必須") or mod in ("skimage", "flip_evaluator", "jsonschema")
            ok = ok and not hard
            print(f"  {'NG  ' if hard else 'なし'} {mod:<15} ({why})  pip install -r tools/parity/requirements.txt")
    print(f"  baseline の置き場: {paths.baselines_dir()}")
    print(f"  実行結果の置き場: {paths.runs_dir()}")
    if a.selftest:
        from .selftest import run_selftest
        r = run_selftest()
        print(f"  selftest(偽エンジンで通し): {r['verdict']}")
        ok = ok and r["verdict"] == "pass"
    return 0 if ok else 1


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="parity", description="Uno Engine パリティ基盤", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("compare", help="2 枚の画像を比べる")
    c.add_argument("reference", help="基準画像(PNG / PFM / EXR / HDR)")
    c.add_argument("test", help="比べる画像")
    c.add_argument("--ref-colorspace", default="auto", help="auto|srgb|gamma22|linear(PNG は既定 srgb、PFM/EXR は linear)")
    c.add_argument("--test-colorspace", default="auto")
    c.add_argument("--metrics", default=None, help="カンマ区切り: " + ",".join(M.ALL_METRICS) + "|all(既定 全部)")
    c.add_argument("--tonemap", default="engine_aces", help="リニア画像に掛ける: " + ",".join(T.list_tonemaps()))
    c.add_argument("--exposure", default="none", help="none | auto(中央値合わせ) | fixed:EV")
    c.add_argument("--size-policy", default="error", choices=["error", "resize", "crop"])
    c.add_argument("--mask", action="append", help="領域: name=mask.png か name=labels.png:r,g,b(複数可)")
    c.add_argument("--spec", help="領域・しきい値をシーン仕様から取る")
    c.add_argument("--gate", help="G0/G1/G2 か milestones 名(--spec と併用。--limit のみでも判定できる)")
    c.add_argument("--limit", action="append", help="しきい値 key=value(例 flip_ldr_mean_max=0.15、複数可)")
    c.add_argument("--noise-floor", help="noise-floor が書いた JSON(しきい値を床 x k で緩和)")
    c.add_argument("--k", type=float, default=None)
    c.add_argument("--ppd", type=float, default=None, help="FLIP の pixels per degree(既定 67)")
    c.add_argument("--strict", action="store_true", help="計算できなかった指標をしきい値ありなら不合格にする")
    c.add_argument("--out", help="出力ディレクトリ(既定 %%LOCALAPPDATA%%\\UnoEngine\\parity\\runs\\...)")
    c.add_argument("--embed", action="store_true", help="レポートに画像を埋め込んで単一ファイルにする")
    c.add_argument("--json", action="store_true", help="指標だけ JSON で出す")
    c.set_defaults(fn=cmd_compare)

    r = sub.add_parser("report", help="run ディレクトリから HTML レポートを作る")
    r.add_argument("run_dir")
    r.add_argument("--embed", action="store_true")
    r.add_argument("--name", default="report.html")
    r.set_defaults(fn=cmd_report)

    n = sub.add_parser("noise-floor", help="別シードの画像同士からノイズ床を出す")
    n.add_argument("images", nargs="+", help="同じシーン・カメラを別シードで撮った画像(2 枚以上)")
    n.add_argument("--colorspace", default="auto")
    n.add_argument("--tonemap", default="engine_aces")
    n.add_argument("--exposure", default="none")
    n.add_argument("--pairs", default="consecutive", choices=["consecutive", "all"])
    n.add_argument("--correction", default="pair", choices=["pair", "single"], help="single = Uno(ノイズ 0)vs 基準の比較向けに 1/sqrt(2) を掛ける")
    n.add_argument("--k", type=float, default=1.5)
    n.add_argument("--out", help="保存先 JSON")
    n.add_argument("--scene", help="baseline 置き場の noise/<scene>/<camera>.json に保存")
    n.add_argument("--camera")
    n.set_defaults(fn=cmd_noise)

    k = sub.add_parser("calibrate", help="リニア画像を外部基準へ合わせるトーンマップ/EV の候補")
    k.add_argument("linear", help="Uno のリニア HDR 画像(PFM/EXR。render_reference の出力など)")
    k.add_argument("reference", help="外部基準(UE のスクリーンショット PNG)")
    k.add_argument("--ref-colorspace", default="auto")
    k.add_argument("--candidates", default=None)
    k.add_argument("--size-policy", default="resize", choices=["error", "resize", "crop"])
    k.add_argument("--out")
    k.set_defaults(fn=cmd_calibrate)

    def add_run_flags(p):
        p.add_argument("--stage", default="G1", help="G0/G1/G2、仕様の milestones 名、regression")
        p.add_argument("--reference", choices=["pt", "external", "previous-run"], help="仕様の reference.kind を上書き")
        p.add_argument("--attach", type=int, metavar="PORT", help="起動済みのエンジンへ接続だけする(起動も停止もしない)")
        p.add_argument("--port", type=int, default=8820)
        p.add_argument("--name", default="par")
        p.add_argument("--mode", choices=["headless", "background"])
        p.add_argument("--camera", action="append", help="このカメラだけ(複数可)")
        p.add_argument("--project", help="scene.project を上書き(使い捨てのコピー推奨)")
        p.add_argument("--test-images", help="撮影済みのエンジン画像のフォルダ(<camera>.png。エンジンを起動しない)")
        p.add_argument("--external-dir", help="外部基準の置き場(仕様より優先)")
        p.add_argument("--baseline-root")
        p.add_argument("--skipped-ok", action="store_true", help="skipped(判定なし)を終了コード 0 にする")
        p.add_argument("--out")

    ru = sub.add_parser("run", help="シーン仕様を自動で回す")
    ru.add_argument("spec")
    add_run_flags(ru)
    ru.add_argument("--no-perf", action="store_true")
    ru.add_argument("--no-perf-history", action="store_true")
    ru.add_argument("--strict", action="store_true")
    ru.add_argument("--progress", action="store_true", help="@progress / @result 行を出す(MCP ジョブ用)")
    ru.add_argument("--want-noise", action="store_true", help="PT の複数シードでノイズ床を測る")
    ru.set_defaults(fn=cmd_run)

    b = sub.add_parser("baseline", help="視覚回帰の baseline(update / approve / check / list / discard)")
    b.add_argument("action", choices=["update", "approve", "check", "list", "discard"])
    b.add_argument("spec_or_id", nargs="?", help="シーン仕様の JSON(approve / list / discard はシーン id でも可)")
    add_run_flags(b)
    b.add_argument("--reason", help="approve の理由(必須)")
    b.add_argument("--approver", default="")
    b.add_argument("--json", action="store_true")
    b.set_defaults(fn=cmd_baseline)

    s = sub.add_parser("scenes", help="シーン仕様の一覧・検証")
    s.add_argument("files", nargs="*")
    s.add_argument("--check-masks", action="store_true")
    s.set_defaults(fn=cmd_scenes)

    d = sub.add_parser("doctor", help="依存・環境の確認")
    d.add_argument("--selftest", action="store_true")
    d.set_defaults(fn=cmd_doctor)
    return ap


def main(argv: list[str] | None = None) -> int:
    _utf8()
    a = build_parser().parse_args(argv)
    try:
        return a.fn(a)
    except (ImageError, S.SpecError, G.GateError, ValueError, FileNotFoundError) as e:
        print(f"エラー: {e}", file=sys.stderr)
        return 2
