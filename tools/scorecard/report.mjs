// 品質スコアカード（W05-35）: 要約・決定論の比較・Wave 間の差分レポート。
//
//   node tools/scorecard/report.mjs summarize --run <runDir> [--out tests/scorecard/w0.json]
//        capture.ps1 が撮った 1 回分（stage_*.json と meta.json）から、リポジトリに置ける要約 JSON を作る。
//        絶対パスや個人情報は入れない（画像は ID・サイズ・ハッシュだけ）。
//   node tools/scorecard/report.mjs compare --a <runDirA> --b <runDirB> [--out <compare.json>]
//        2 回撮った結果の差が「決定論の範囲」に収まるかを判定する。収まらなければ終了コード 1。
//   node tools/scorecard/report.mjs diff --base tests/scorecard/w0.json --now <summary.json>
//        W0 との差分レポート（各 Wave のゲート用）。値の増減を表で出す。
//
// 決定論の範囲（compare の合否）:
//   画像      … 画素ハッシュが同じ、または「違う画素 0.1% 以下かつ平均差 0.25 以下」（セッション内の揺れ×2 まで広げるが、2% / 1.0 を上限とする）
//               （tools/bench の既定の許容差と同じ。pixelThreshold 12）。
//   VFX 指標  … 被覆・明るさ・ちらつきが、絶対差 2e-3 以下（A/A 実測で決定）、または相対差 30% 以下。
//               パーティクルの乱数がプロセスごとに変わり絵は 1 画素単位では一致しないので、VFX は指標だけで判定する（画像の差は参考情報）。
//   起動秒数  … EMA が max(0.30 秒, 15%) 以内（実時間の計測なので画像より緩い）。
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { REPO_ROOT } from "../bench/lib/engine.mjs";
import { compareImages } from "../bench/lib/imageDiff.mjs";

const require = createRequire(import.meta.url);

export const TOL = {
  pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25,
  vfxAbs: 2e-3, vfxRel: 0.30,           // A/A（同じバイナリを 2 回）の実測: 小さい指標の揺れは最大 1.4e-3、大きい指標は相対 30% 内。絶対 2e-3 未満の指標は雑音扱い
  maxDiffRatioCap: 0.02, maxMeanAbsCap: 1.0,   // セッション内の揺れを許す場合の上限（無制限にしない）
  startupAbsSec: 0.3, startupRel: 0.15,
};
export const SUMMARY_VERSION = 1;
const STAGE_NAMES = ["splash", "ui", "golden", "templates", "vfx", "startup"];

// 要約に絶対パスを残さない
export function scrub(m) {
  return String(m ?? "").replace(/[A-Za-z]:[\\/][^\s"'`<>|,;)]*/g, "<path>").replace(/\/(?:Users|home)\/[^\s"']*/g, "<path>").slice(0, 400);
}
const readJson = (f) => JSON.parse(fs.readFileSync(f, "utf8"));
function readPng(file) {
  const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));
  const p = PNG.sync.read(fs.readFileSync(file));
  return { width: p.width, height: p.height, data: p.data };
}

// ─── 要約 ────────────────────────────────────────────────────────────────
export function summarizeRun(runDir) {
  const meta = fs.existsSync(path.join(runDir, "meta.json")) ? readJson(path.join(runDir, "meta.json")) : {};
  const stages = {};
  for (const s of STAGE_NAMES) {
    const f = path.join(runDir, `stage_${s}.json`);
    if (!fs.existsSync(f)) { stages[s] = { captured: false }; continue; }
    const st = readJson(f);
    const items = {};
    for (const it of st.items) {
      const o = {};
      for (const k of ["kind", "width", "height", "sha256", "maskedSha256", "count", "deterministicInSession", "selfDiff", "dpi", "layout", "layers", "oneShot", "metrics", "newScene", "scene"])
        if (it[k] !== undefined) o[k] = it[k];
      if (it.kind === "image-seq") o.frameCount = it.count;
      if (it.kind === "metric") for (const k of ["unit", "alpha", "value", "median", "min", "max", "engineStartupJson"]) if (it[k] !== undefined) o[k] = it[k];
      if (it.kind === "metric") o.warmRuns = it.runs.filter((r) => r.kind === "warm").length;
      items[it.id] = o;
    }
    stages[s] = { captured: true, ok: st.ok, errors: st.errors.map((e) => ({ id: e.id, message: scrub(e.message) })), notes: st.notes.map(scrub), items };
  }
  return {
    version: SUMMARY_VERSION,
    wave: "w0",
    meta,
    stages,
    // 後から埋まる欄（このスクリプトでは撮らない）
    pending: {
      "ls-a-f": "LS-A〜F は W1 の NB3 の後で追加する",
      mcp14: "W05-37 が埋める（API を使わない E2E のベースライン）",
      perf: "W05-39 が baseline_perf.json に記録する",
    },
  };
}

// ─── 比較（決定論） ──────────────────────────────────────────────────────
export function relOk(a, b, absTol, relTol) {
  if (a == null || b == null) return a == b;
  const d = Math.abs(a - b);
  return d <= absTol || d <= relTol * Math.max(Math.abs(a), Math.abs(b));
}

export function cmpImageFiles(fa, fb, self = null) {
  if (!fs.existsSync(fa) || !fs.existsSync(fb)) return { ok: false, why: "画像ファイルが無い" };
  const a = readPng(fa), b = readPng(fb);
  if (a.width !== b.width || a.height !== b.height) return { ok: false, why: `サイズが違う ${a.width}x${a.height} / ${b.width}x${b.height}` };
  const c = compareImages(a, b, TOL.pixelThreshold);
  // 同じセッションの 2 枚でも一致しない項目（パーティクル・RT のノイズ）は、その実測差の 2 倍まで許す。
  const maxR = Math.min(TOL.maxDiffRatioCap, Math.max(TOL.maxDiffRatio, self ? self.diffRatio * 2 : 0));
  const maxM = Math.min(TOL.maxMeanAbsCap, Math.max(TOL.maxMeanAbs, self ? self.meanAbs * 2 : 0));
  const ok = c.diffRatio <= maxR && c.meanAbs <= maxM;
  return { ok, diffRatio: c.diffRatio, meanAbs: c.meanAbs, maxDist: c.maxDist };
}

export function compareRuns(dirA, dirB) {
  const out = { tolerance: TOL, stages: {}, ok: true, mismatches: [] };
  for (const s of STAGE_NAMES) {
    const fa = path.join(dirA, `stage_${s}.json`), fb = path.join(dirB, `stage_${s}.json`);
    if (!fs.existsSync(fa) || !fs.existsSync(fb)) { out.stages[s] = { compared: false }; continue; }
    const A = readJson(fa), B = readJson(fb);
    const byB = new Map(B.items.map((i) => [i.id, i]));
    const rows = [];
    for (const ia of A.items) {
      const ib = byB.get(ia.id);
      const row = { id: ia.id };
      if (!ib) { row.status = "missing"; row.ok = false; rows.push(row); out.mismatches.push(`${s}/${ia.id}`); continue; }
      if (ia.kind === "metric") {
        const ok = relOk(ia.value, ib.value, TOL.startupAbsSec, TOL.startupRel);
        Object.assign(row, { kind: "metric", a: ia.value, b: ib.value, ok });
      } else if (ia.metrics) {
        // VFX: 乱数がプロセスごとに変わるので指標だけで判定し、画像の差は参考として残す。
        const bad = Object.keys(ia.metrics).filter((k) => !relOk(ia.metrics[k], ib.metrics?.[k], TOL.vfxAbs, TOL.vfxRel));
        const img = ia.sha256 === ib.sha256 ? { exact: true } : cmpImageFiles(path.join(dirA, ia.file), path.join(dirB, ib.file));
        Object.assign(row, { kind: "vfx", badMetrics: bad, image: img, ok: bad.length === 0 });
      } else if (ia.kind === "image-seq") {
        const fr = ia.frames.map((f, i) => {
          const g = ib.frames?.[i];
          if (!g) return { ok: false };
          if (f.sha256 === g.sha256) return { ok: true, exact: true };
          return cmpImageFiles(path.join(dirA, f.file), path.join(dirB, g.file));
        });
        const ok = ia.count === ib.count && fr.every((x) => x.ok);
        Object.assign(row, { kind: "image-seq", frames: ia.count, exactFrames: fr.filter((x) => x.exact).length, ok });
      } else {
        const sa = ia.maskedSha256 ?? ia.sha256, sb = ib.maskedSha256 ?? ib.sha256;
        if (sa === sb) Object.assign(row, { kind: "image", exact: true, ok: true });
        else {
          const self = ia.selfDiff ?? ib.selfDiff ?? null;
          const c = cmpImageFiles(path.join(dirA, ia.file), path.join(dirB, ib.file), self);
          Object.assign(row, { kind: "image", exact: false, ...c });
        }
      }
      if (!row.ok) out.mismatches.push(`${s}/${ia.id}`);
      rows.push(row);
    }
    for (const ib of B.items) if (!A.items.some((x) => x.id === ib.id)) { rows.push({ id: ib.id, status: "extra", ok: false }); out.mismatches.push(`${s}/${ib.id}`); }
    const stOk = rows.every((r) => r.ok) && A.ok && B.ok;
    out.stages[s] = { compared: true, ok: stOk, itemsOk: rows.filter((r) => r.ok).length, items: rows.length, rows };
    if (!stOk) { out.ok = false; if (!rows.some((r) => !r.ok)) out.mismatches.push(`${s}: 撮影エラーあり`); }
  }
  return out;
}

// ─── 差分レポート（Wave 間） ────────────────────────────────────────────
export function diffSummaries(base, now) {
  const lines = [];
  const rows = [];
  for (const s of STAGE_NAMES) {
    const bi = base.stages?.[s]?.items ?? {}, ni = now.stages?.[s]?.items ?? {};
    for (const id of new Set([...Object.keys(bi), ...Object.keys(ni)])) {
      const b = bi[id], n = ni[id];
      if (!b || !n) { rows.push({ stage: s, id, change: !b ? "added" : "removed" }); continue; }
      if (b.kind === "metric") rows.push({ stage: s, id, base: b.value, now: n.value, delta: n.value - b.value, pct: b.value ? (n.value - b.value) / b.value : null });
      else if (b.metrics) {
        for (const k of Object.keys(b.metrics)) rows.push({ stage: s, id: `${id}.${k}`, base: b.metrics[k], now: n.metrics?.[k], delta: (n.metrics?.[k] ?? NaN) - b.metrics[k] });
      } else {
        const same = (b.maskedSha256 ?? b.sha256) === (n.maskedSha256 ?? n.sha256);
        rows.push({ stage: s, id, change: same ? "identical" : "changed" });
      }
    }
  }
  for (const r of rows) {
    if (r.change) lines.push(`${r.stage.padEnd(9)} ${r.id.padEnd(34)} ${r.change}`);
    else lines.push(`${r.stage.padEnd(9)} ${r.id.padEnd(34)} ${fmt(r.base)} -> ${fmt(r.now)}  (${r.delta >= 0 ? "+" : ""}${fmt(r.delta)}${r.pct != null ? `, ${(r.pct * 100).toFixed(1)}%` : ""})`);
  }
  return { rows, text: lines.join("\n") };
}
const fmt = (x) => (typeof x === "number" ? (Math.abs(x) >= 100 ? x.toFixed(1) : x.toPrecision(4)) : String(x));

// ─── CLI ─────────────────────────────────────────────────────────────────
function parse(argv) {
  const o = { cmd: argv[0] };
  for (let i = 1; i < argv.length; i++) {
    const k = argv[i];
    if (!k.startsWith("--")) throw new Error(`不明な引数: ${k}`);
    if (i + 1 >= argv.length) throw new Error(`${k} に値が要る`);
    o[k.slice(2)] = argv[++i];
  }
  return o;
}
function main() {
  const o = parse(process.argv.slice(2));
  if (o.cmd === "summarize") {
    if (!o.run) throw new Error("--run が要る");
    const sum = summarizeRun(path.resolve(o.run));
    const text = JSON.stringify(sum, null, 2) + "\n";
    fs.writeFileSync(path.join(path.resolve(o.run), "summary.json"), text);
    if (o.out) { fs.mkdirSync(path.dirname(path.resolve(o.out)), { recursive: true }); fs.writeFileSync(path.resolve(o.out), text); }
    const bad = Object.entries(sum.stages).filter(([, v]) => v.captured && !v.ok).map(([k]) => k);
    console.log(`[scorecard] summary: ${Object.entries(sum.stages).map(([k, v]) => `${k}=${v.captured ? Object.keys(v.items).length : "-"}`).join(" ")}${bad.length ? `  ERRORS: ${bad.join(",")}` : ""}`);
    return bad.length ? 1 : 0;
  }
  if (o.cmd === "compare") {
    if (!o.a || !o.b) throw new Error("--a と --b が要る");
    const r = compareRuns(path.resolve(o.a), path.resolve(o.b));
    if (o.out) fs.writeFileSync(path.resolve(o.out), JSON.stringify(r, null, 2));
    for (const [s, v] of Object.entries(r.stages)) console.log(`${s.padEnd(10)} ${v.compared ? `${v.ok ? "ok  " : "FAIL"} ${v.itemsOk}/${v.items}` : "(未比較)"}`);
    for (const m of r.mismatches) console.log(`  範囲外: ${m}`);
    console.log(r.ok ? "決定論の範囲に収まっている" : "決定論の範囲を超えた差がある");
    return r.ok ? 0 : 1;
  }
  if (o.cmd === "diff") {
    if (!o.base || !o.now) throw new Error("--base と --now が要る");
    console.log(diffSummaries(readJson(path.resolve(o.base)), readJson(path.resolve(o.now))).text);
    return 0;
  }
  console.log("使い方: report.mjs summarize|compare|diff （ファイル先頭のコメント参照）");
  return 2;
}
const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) { try { process.exit(main()); } catch (e) { console.error(e.message); process.exit(2); } }
