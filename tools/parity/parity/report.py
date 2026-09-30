"""HTML レポート(run.json / compare の結果 -> report.html)。

並べ比較(基準 | エンジン | ヒートマップ のコンタクトシート + スライダ比較)・指標表と合否・領域別・悪いタイル・性能・警告。
画像は run ディレクトリからの相対パスで参照する(そのまま開ける)。embed=True で base64 埋め込みの単一ファイルにできる。
"""
from __future__ import annotations

import base64
import html
import json
from pathlib import Path

from . import metrics as M

_E = html.escape

CSS = """
:root{--bg:#f6f7f9;--fg:#1a1d23;--mut:#5b6472;--card:#fff;--bd:#dfe3ea;--ok:#0a7d3c;--okbg:#e2f5e9;--ng:#b3261e;--ngbg:#fde7e5;--sk:#8a6a00;--skbg:#fff3cd;--acc:#2b59c3}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#111318;--fg:#e6e9ef;--mut:#9aa4b2;--card:#1a1e26;--bd:#2c323d;--ok:#5fd68a;--okbg:#12301f;--ng:#ff8a80;--ngbg:#3a1a18;--sk:#f0c75e;--skbg:#3a3010;--acc:#8ab4ff}}
:root[data-theme=dark]{--bg:#111318;--fg:#e6e9ef;--mut:#9aa4b2;--card:#1a1e26;--bd:#2c323d;--ok:#5fd68a;--okbg:#12301f;--ng:#ff8a80;--ngbg:#3a1a18;--sk:#f0c75e;--skbg:#3a3010;--acc:#8ab4ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 "Segoe UI","Yu Gothic UI",system-ui,sans-serif}
a{color:var(--acc)}main{max-width:1280px;margin:0 auto;padding:20px 16px 60px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 10px}h3{font-size:14px;margin:16px 0 6px;color:var(--mut)}
.sub{color:var(--mut);margin:0 0 14px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:14px 16px;margin:12px 0}
.badge{display:inline-block;padding:2px 10px;border-radius:999px;font-weight:600;font-size:12px}
.pass{color:var(--ok);background:var(--okbg)}.fail,.error{color:var(--ng);background:var(--ngbg)}.skipped,.captured{color:var(--sk);background:var(--skbg)}
table{border-collapse:collapse;width:100%}th,td{padding:5px 8px;border-bottom:1px solid var(--bd);text-align:left;vertical-align:top}
th{color:var(--mut);font-weight:600;font-size:12px}td.n{text-align:right;font-variant-numeric:tabular-nums}
td.bad{color:var(--ng);font-weight:600}td.good{color:var(--ok)}
.imgs img{max-width:100%;height:auto;border:1px solid var(--bd);border-radius:6px;display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:12px}
.cmp{position:relative;max-width:100%;border:1px solid var(--bd);border-radius:6px;overflow:hidden;line-height:0}
.cmp img{width:100%;height:auto;display:block}.cmp .top{position:absolute;inset:0;clip-path:inset(0 calc(100% - var(--p,50%)) 0 0)}
.cmp .bar{position:absolute;top:0;bottom:0;left:var(--p,50%);width:2px;background:#fff;box-shadow:0 0 0 1px #0006;pointer-events:none}
.cmp .lb{position:absolute;top:6px;padding:1px 7px;background:#000a;color:#fff;font-size:12px;line-height:18px;border-radius:4px}
input[type=range]{width:100%}
.mut{color:var(--mut)}.reasons li{margin:2px 0}.mono{font-family:Consolas,monospace;font-size:12px}
.kv{display:grid;grid-template-columns:max-content 1fr;gap:2px 14px}.kv div:nth-child(odd){color:var(--mut)}
"""

JS = """
document.querySelectorAll('.cmp').forEach(function(c){var r=c.parentElement.querySelector('input[type=range]');
 if(!r)return;function u(){c.style.setProperty('--p',r.value+'%');}r.addEventListener('input',u);u();});
"""


def _b64(p: Path) -> str:
    return "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode("ascii")


def _img_src(base: Path, rel: str, embed: bool) -> str:
    p = base / rel
    if embed and p.exists():
        return _b64(p)
    return _E(rel)


def badge(status: str | None) -> str:
    s = status or "-"
    lab = {"pass": "合格", "fail": "不合格", "skipped": "skipped(判定なし)", "error": "エラー", "captured": "撮影のみ"}.get(s, s)
    return f'<span class="badge {_E(s)}">{_E(lab)}</span>'


def _fmt(v, nd=4) -> str:
    if v is None:
        return "-"
    if isinstance(v, (int, float)):
        return f"{v:.{nd}g}"
    return _E(str(v))


def _checks_table(checks: list[dict]) -> str:
    if not checks:
        return '<p class="mut">しきい値の設定なし。</p>'
    rows = []
    for c in checks:
        st = c["status"]
        sym = {"max": "≤", "abs_max": "|x| ≤", "min": "≥"}[c["op"]]
        cls = "bad" if st == "fail" else "good" if st == "pass" else ""
        relax = f' <span class="mut">(固定 {_fmt(c["fixed"])} をノイズ床で緩和)</span>' if c.get("floor_applied") else ""
        rows.append(f'<tr><td>{_E(c["scope"])}</td><td>{_E(M.LABELS.get(c["metric"], c["metric"]))}</td>'
                    f'<td class="n {cls}">{_fmt(c["value"])}</td><td>{sym} {_fmt(c["limit"])}{relax}</td><td>{badge(st if st != "unavailable" else "skipped")}</td></tr>')
    return "<table><tr><th>範囲</th><th>指標</th><th>値</th><th>限界</th><th>判定</th></tr>" + "".join(rows) + "</table>"


def _metrics_table(m: dict, regions: dict) -> str:
    keys = [k for k in M.LABELS if k in m or any(k in r for r in regions.values())]
    if not keys:
        return ""
    head = "".join(f"<th>領域: {_E(n)}</th>" for n in regions)
    rows = []
    for k in keys:
        cells = "".join(f'<td class="n">{_fmt(r.get(k))}</td>' for r in regions.values())
        rows.append(f'<tr><td>{_E(M.LABELS[k])}</td><td class="n">{_fmt(m.get(k))}</td>{cells}</tr>')
    return f"<table><tr><th>指標</th><th>全体</th>{head}</tr>{''.join(rows)}</table>"


def _camera_section(base: Path, c: dict, embed: bool) -> str:
    out = [f'<section class="card"><h2 style="margin-top:0">{_E(c["name"])} {badge(c.get("status"))}</h2>']
    if c.get("note"):
        out.append(f'<p class="sub">{_E(c["note"])}</p>')
    ref = c.get("reference") or {}
    if ref:
        prov = ref.get("provenance")
        out.append(f'<p class="mut">基準の種別: <b>{_E(str(ref.get("source", "")))}</b>'
                   + (f" / 来歴: {_E(str(prov))}" if prov else "")
                   + (f' / <span class="mono">{_E(str(ref.get("file")))}</span>' if ref.get("file") else "") + "</p>")
    if c.get("reasons"):
        out.append('<ul class="reasons">' + "".join(f"<li>{_E(r)}</li>" for r in c["reasons"]) + "</ul>")
    imgs = c.get("images") or {}
    if imgs.get("contact"):
        out.append(f'<div class="imgs"><img src="{_img_src(base, imgs["contact"], embed)}" alt="並べ比較"></div>')
    if imgs.get("ref") and imgs.get("test"):
        out.append('<h3>スライダ比較(左 = 基準 / 右 = エンジン)</h3><div>'
                   f'<div class="cmp"><img src="{_img_src(base, imgs["test"], embed)}" alt="エンジン">'
                   f'<img class="top" src="{_img_src(base, imgs["ref"], embed)}" alt="基準"><div class="bar"></div>'
                   '<span class="lb" style="left:6px">基準</span><span class="lb" style="right:6px">エンジン</span></div>'
                   '<input type="range" min="0" max="100" value="50"></div>')
    heat = [(k, lab) for k, lab in (("heat_flip_ldr", "LDR-FLIP エラーマップ"), ("heat_flip_hdr", "HDR-FLIP エラーマップ"),
                                     ("heat_de2000", "ΔE2000 マップ(0..10)"), ("diff", "差分 x4"), ("diff_signed", "輝度差(赤 = エンジンが明るい / 青 = 暗い)")) if imgs.get(k)]
    if heat:
        out.append('<h3>ヒートマップ・差分</h3><div class="grid imgs">' + "".join(
            f'<figure style="margin:0"><img src="{_img_src(base, imgs[k], embed)}" alt="{_E(lab)}"><figcaption class="mut">{_E(lab)}</figcaption></figure>'
            for k, lab in heat) + "</div>")
    v = c.get("verdict")
    if v:
        out.append("<h3>判定</h3>" + _checks_table(v.get("checks", [])))
        if v.get("warnings"):
            out.append('<ul class="mut">' + "".join(f"<li>{_E(w)}</li>" for w in v["warnings"]) + "</ul>")
    if c.get("metrics"):
        out.append("<h3>指標(全体・領域別)</h3>" + _metrics_table(c["metrics"], c.get("regions") or {}))
    if c.get("worstTiles"):
        out.append('<h3>悪いタイル(16x16 の LDR-FLIP 平均 上位)</h3><table><tr><th>x</th><th>y</th><th>FLIP 平均</th></tr>'
                   + "".join(f'<tr><td class="n">{t["x"]}</td><td class="n">{t["y"]}</td><td class="n">{_fmt(t["mean"])}</td></tr>' for t in c["worstTiles"][:8])
                   + "</table>")
    if "bitExact" in c:
        bb = c.get("diffBBox")
        out.append(f'<p class="mut">ビット一致: <b>{"はい" if c["bitExact"] else "いいえ"}</b>'
                   + (f' / 違う画素の外接矩形: x={bb["x"]} y={bb["y"]} {bb["w"]}x{bb["h"]}({bb["pixels"]} 画素)' if bb else "") + "</p>")
    al = c.get("align")
    if al:
        out.append(f'<h3>整列</h3><div class="kv"><div>トーンマップ</div><div>{_E(str(al.get("tonemap")))}</div>'
                   f'<div>露出</div><div>{_E(str(al.get("exposure")))}(適用 EV: 基準 {_fmt(al.get("ev_ref_applied"))} / エンジン {_fmt(al.get("ev_test_applied"))})</div>'
                   f'<div>入力の種類</div><div>基準={_E(str(al.get("refKind")))} / エンジン={_E(str(al.get("testKind")))}</div>'
                   f'<div>サイズ</div><div>{_E(json.dumps(c.get("size"), ensure_ascii=False))}</div></div>')
    if c.get("floors"):
        out.append('<h3>ノイズ床(しきい値 = max(固定, 床 x %s))</h3><div class="mono">%s</div>' % (
            _fmt(c.get("floorK", 1.5)), _E(json.dumps({k: round(v, 5) for k, v in c["floors"].items()}, ensure_ascii=False))))
    if c.get("notes"):
        out.append('<ul class="mut">' + "".join(f"<li>{_E(n)}</li>" for n in c["notes"]) + "</ul>")
    out.append("</section>")
    return "".join(out)


def _perf_section(p: dict) -> str:
    m = p.get("measured") or {}
    rows = "".join(f'<tr><td>{_E(lab)}</td><td class="n">{_fmt(m.get(k))}</td></tr>' for k, lab in
                   (("gpu_ms", "GPU ms(total)"), ("frame_ms_avg", "フレーム平均 ms"), ("frame_ms_p95", "フレーム p95 ms"), ("cpu_ms", "CPU 実働 ms"),
                    ("vram_mb", "VRAM MB"), ("load_sec", "ロード秒"), ("fps", "fps")))
    chk = ""
    if p.get("checks"):
        chk = "<h3>予算・履歴との比較</h3><table><tr><th>種別</th><th>項目</th><th>値</th><th>限界 / 基準</th><th>判定</th></tr>" + "".join(
            f'<tr><td>{_E(c["kind"])}</td><td>{_E(c["label"])}</td><td class="n">{_fmt(c["value"])}</td>'
            f'<td>{_fmt(c.get("limit"))}' + (f' (履歴中央値 {_fmt(c.get("baseline"))}, {c.get("pct", 0):+.1f}%)' if c["kind"] == "regression" else "")
            + f'</td><td>{badge(c["status"] if c["status"] != "unavailable" else "skipped")}</td></tr>' for c in p["checks"]) + "</table>"
    return (f'<section class="card"><h2 style="margin-top:0">性能 {badge(p.get("status"))}</h2>'
            f'<p class="mut">カメラ: {_E(str(p.get("camera", "")))}</p><table><tr><th>項目</th><th>値</th></tr>{rows}</table>{chk}'
            + ("".join(f"<p>{_E(r)}</p>" for r in p.get("reasons", []))) + "</section>")


def render(run: dict, base: Path, embed: bool = False) -> str:
    v = run.get("verdict") or ("-" if not run.get("cameras") else "pass")
    cams = run.get("cameras", [])
    sumrows = []
    for c in cams:
        m = c.get("metrics") or {}
        sumrows.append(
            f'<tr><td><a href="#cam-{_E(c["name"])}">{_E(c["name"])}</a></td><td>{badge(c.get("status"))}</td>'
            f'<td class="n">{_fmt(m.get("flip_ldr_mean"))}</td><td class="n">{_fmt(m.get("flip_ldr_p95"))}</td>'
            f'<td class="n">{_fmt(m.get("ssim"))}</td><td class="n">{_fmt(m.get("de2000_median"))}</td>'
            f'<td class="n">{_fmt(m.get("lum_mean_ev"))}</td><td class="n">{_fmt(m.get("hist_emd_ev"))}</td>'
            f'<td>{_E("; ".join(c.get("reasons", [])[:2]))}</td></tr>')
    meta = [("シーン", f'{run.get("scene")}({run.get("parityScene") or "-"})'), ("段階(ゲート)", f'{run.get("stage")} → {run.get("gate")}'),
            ("基準の種別", run.get("referenceKind")), ("仕様ハッシュ", run.get("specHash")),
            ("GPU", (run.get("gpu") or {}).get("name") or None), ("エンジン", json.dumps(run.get("engine") or {}, ensure_ascii=False)),
            ("ロード", f'{run.get("loadSec")} 秒' if run.get("loadSec") is not None else "-"),
            ("開始", run.get("startedAt")), ("所要", f'{run["durationSec"]} 秒' if run.get("durationSec") is not None else None)]
    body = [f'<main><h1>パリティ・レポート: {_E(str(run.get("title") or run.get("scene") or ""))} {badge(v)}</h1>',
            f'<p class="sub">Uno Engine — 基準との並べ比較。指標は必要条件であって十分条件ではない。各節目で人が目視して承認する(AI 単独で「合格」と宣言しない)。</p>',
            '<div class="card"><div class="kv">' + "".join(f"<div>{_E(k)}</div><div>{_E(str(x))}</div>" for k, x in meta if x not in (None, "", "-", "{}", "None 秒")) + "</div></div>"]
    if run.get("reasons"):
        body.append('<div class="card"><b>理由</b><ul class="reasons">' + "".join(f"<li>{_E(r)}</li>" for r in run["reasons"][:20]) + "</ul></div>")
    if run.get("warnings"):
        body.append('<div class="card mut"><b>警告</b><ul>' + "".join(f"<li>{_E(w)}</li>" for w in run["warnings"][:30]) + "</ul></div>")
    if cams:
        body.append('<h2>カメラ別の結果</h2><div class="card"><table><tr><th>カメラ</th><th>判定</th><th>FLIP 平均</th><th>FLIP p95</th><th>SSIM</th><th>ΔE 中央</th>'
                    '<th>輝度差 EV</th><th>ヒスト EMD</th><th>理由</th></tr>' + "".join(sumrows) + "</table></div>")
    if run.get("perf"):
        body.append(_perf_section(run["perf"]))
    for c in cams:
        body.append(f'<a id="cam-{_E(c["name"])}"></a>' + _camera_section(base, c, embed))
    body.append("</main>")
    return ('<!doctype html><html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>パリティ・レポート {_E(str(run.get("scene", "")))}</title><style>{CSS}</style></head><body>' + "".join(body)
            + f"<script>{JS}</script></body></html>")


def write_report(run_dir: str | Path, embed: bool = False, out_name: str = "report.html") -> Path:
    d = Path(run_dir)
    run = json.loads((d / "run.json").read_text(encoding="utf-8"))
    p = d / out_name
    p.write_text(render(run, d, embed), encoding="utf-8")
    return p
