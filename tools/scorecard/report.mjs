// 品質スコアカード（W05-35）: 要約・決定論の比較・Wave 間の差分レポート。
//
//   node tools/scorecard/report.mjs summarize --run <runDir> [--out tests/scorecard/w0.json]
//        capture.ps1 が撮った 1 回分（stage_*.json と meta.json）から、リポジトリに置ける要約 JSON を作る。
//        絶対パスや個人情報は入れない（画像は ID・サイズ・ハッシュだけ）。
//   node tools/scorecard/report.mjs compare --a <runDirA> --b <runDirB> [--out <compare.json>]
//        2 回撮った結果の差が「決定論の範囲」に収まるかを判定する。収まらなければ終了コード 1。
//   node tools/scorecard/report.mjs derive-vfx-tol --runs <r1,r2,r3> [--out vfx_tol.json]   （compare の --vfx-tol へ渡す。--expect-stages all で全段必須。部分比較は終了コード 3）
//   node tools/scorecard/report.mjs mcp14-set --in <result.json> [--out tests/scorecard/w0_mcp14.json]
//        W05-37 の結果（6 課題の採点と再現条件）を検査して w0_mcp14.json へ書く。summarize は同じファイルを mcp14 欄へ併合する
//        （w0.json を summarize で作り直しても mcp14 は消えない）。書式は validateMcp14 を参照。
//   node tools/scorecard/report.mjs diff --base tests/scorecard/w0.json --now <summary.json>
//        W0 との差分レポート（各 Wave のゲート用）。値の増減を表で出す。
//
// 決定論の範囲（compare の合否）:
//   画像      … 画素ハッシュが同じ、または「違う画素 0.1% 以下かつ平均差 0.25 以下」（セッション内の揺れ×2 まで広げるが、2% / 1.0 を上限とする）
//               （tools/bench の既定の許容差と同じ。pixelThreshold 12）。
//   VFX 指標  … 被覆・明るさ・ちらつきが、指標別の絶対差の床（TOL.vfxAbsByMetric、レシピ別の上書き可）以下、または相対差 30% 以下。
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
  // 指標ごとの絶対差の床（各指標の大きさに合わせる。共通の床だと小さい指標が何でも通る）。A/A の暫定値で、
  // 本番は dx12-preoverhaul で 3 回以上撮り、`report.mjs derive-vfx-tol` の出力（レシピ別）を --vfx-tol で渡す。
  vfxAbsByMetric: { coverage: 2e-3, lumaGain: 5e-4, flicker: 4e-4 }, vfxRel: 0.30,
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
// ─── MCP-14（W05-37）の記録 ──────────────────────────────────────────────
// 別ファイル tests/scorecard/w0_mcp14.json に置き、summarize が併合する。「同じ手順」と言える条件を全部ここに残す。
export const MCP14_FILE = path.join(REPO_ROOT, "tests/scorecard/w0_mcp14.json");
export const MCP14_TASK_COUNT = 6;
const HEX64 = /^[0-9a-f]{64}$/, HEXC = /^[0-9a-f]{7,40}$/;
export function validateMcp14(r) {
  const e = [];
  const str = (v) => typeof v === "string" && v.length > 0;
  const need = (k, ok) => { if (!ok(r?.[k])) e.push(`${k} が無い、または形式が違う`); };
  need("subagentModelId", str);            // 例 claude-sonnet-5-5。毎 Wave 同じにする
  need("procedureVersion", str);           // 手順の版（run_e2e.ts のヘッダの版 + task_<id>.md テンプレートの版）
  need("templateSha256", (v) => HEX64.test(v ?? ""));      // task_<id>.md のテンプレートの sha
  need("tasksJsonSha256", (v) => HEX64.test(v ?? ""));     // e2e_tasks.json の sha
  need("e2eToolsCommit", (v) => HEXC.test(v ?? ""));       // run_e2e.ts / score_e2e.ts / replay_e2e.ts を含む commit
  need("preOverhaulCommit", (v) => HEXC.test(v ?? ""));    // タグ pre-overhaul の commit
  need("tsRef", str); need("buildRef", str);
  need("launchMode", (v) => v === "agent-tool" || v === "claude-p");   // どちらで回したか
  need("otherMcpIsolated", (v) => v === true);   // 親セッションのほかの MCP を見せない手段をとった（手段は isolationMethod）
  need("isolationMethod", str);
  need("pointerRegistered", (v) => typeof v === "boolean");   // mcp.pointer.json を登録したか
  need("port", (v) => v === 8856);
  need("noPaidApiKeyChecked", (v) => v === true);   // assertNoPaidApiKey を通った（ANTHROPIC_API_KEY が無い）
  need("replayMatch", (v) => v === true);           // replay_e2e が同じ採点
  const t = r?.tasks;
  if (!Array.isArray(t) || t.length !== MCP14_TASK_COUNT) e.push(`tasks は ${MCP14_TASK_COUNT} 件が要る`);
  else {
    const ids = new Set();
    for (const x of t) {
      if (!x || !str(x.id) || typeof x.score !== "number" || !HEX64.test(x.traceSha256 ?? "")) e.push(`tasks の行が不正: ${JSON.stringify(x)}`);
      else ids.add(x.id);
    }
    if (ids.size !== t.length) e.push("tasks の id が重複している");
  }
  if (/[A-Za-z]:[\\/]/.test(JSON.stringify(r ?? {}))) e.push("絶対パスを含んでいる");
  return e;
}
export function loadMcp14(file = MCP14_FILE) {
  if (!fs.existsSync(file)) return null;
  const r = readJson(file);
  const e = validateMcp14(r);
  if (e.length) throw new Error(`${path.basename(file)} が不正: ${e.join(" / ")}`);
  return r;
}

export function summarizeRun(runDir, opts = {}) {
  const meta = fs.existsSync(path.join(runDir, "meta.json")) ? readJson(path.join(runDir, "meta.json")) : {};
  const stages = {};
  for (const s of STAGE_NAMES) {
    const f = path.join(runDir, `stage_${s}.json`);
    if (!fs.existsSync(f)) { stages[s] = { captured: false }; continue; }
    const st = readJson(f);
    const items = {};
    for (const it of st.items) {
      const o = {};
      for (const k of ["kind", "width", "height", "sha256", "maskedSha256", "count", "deterministicInSession", "selfDiff", "dpi", "uiDeterministicFlag", "hideCursor", "layout", "layers", "oneShot", "metrics", "newScene", "scene"])
        if (it[k] !== undefined) o[k] = it[k];
      if (it.kind === "image-seq") o.frameCount = it.count;
      if (it.kind === "metric") for (const k of ["unit", "alpha", "quantity", "value", "median", "min", "max"]) if (it[k] !== undefined) o[k] = it[k];
      if (it.kind === "metric") o.warmRuns = it.runs.filter((r) => r.kind === "warm").length;
      items[it.id] = o;
    }
    stages[s] = { captured: true, ok: st.ok, errors: st.errors.map((e) => ({ id: e.id, message: scrub(e.message) })), notes: st.notes.map(scrub), items };
  }
  const mcp14 = opts.mcp14 ?? null;
  const out = {
    version: SUMMARY_VERSION,
    wave: "w0",
    meta,
    stages,
    ...(mcp14 ? { mcp14 } : {}),
    // 後から埋まる欄（このスクリプトでは撮らない）
    pending: {
      "ls-a-f": "LS-A〜F は W1 の NB3 の後で追加する",
      ...(mcp14 ? {} : { mcp14: "W05-37 が埋める（tests/scorecard/w0_mcp14.json を report.mjs mcp14-set で書く。summarize が併合する）" }),
      startupQuantity: "W7 の 1.4s 目標を startup_ready_ema（--background・スプラッシュ無し）と ST-1 のスプラッシュ経路 EMA のどちらで判定するか、main セッションの決定待ち",
      perf: "W05-39 が baseline_perf.json に記録する",
    },
  };
  return out;
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

// derive-vfx-tol の出力: { "<itemId>": { coverage, lumaGain, flicker } }（レシピ別の絶対差の床）
export function vfxFloor(vfxTol, id, metric) {
  return vfxTol?.[id]?.[metric] ?? TOL.vfxAbsByMetric[metric] ?? TOL.vfxAbsByMetric.coverage;
}

/** 3 回以上の撮影（同じバイナリ）から、レシピ別・指標別の揺れ（最大の組み合わせ差の 1.5 倍、床は TOL）を求める。 */
export function deriveVfxTol(runDirs) {
  const per = runDirs.map((d) => new Map(readJson(path.join(d, "stage_vfx.json")).items.map((i) => [i.id, i.metrics])));
  const tol = {};
  for (const id of per[0].keys()) {
    tol[id] = {};
    for (const m of Object.keys(TOL.vfxAbsByMetric)) {
      let mx = 0;
      for (let i = 0; i < per.length; i++) for (let j = i + 1; j < per.length; j++) {
        const x = per[i].get(id)?.[m], y = per[j].get(id)?.[m];
        if (x != null && y != null) mx = Math.max(mx, Math.abs(x - y));
      }
      tol[id][m] = Math.max(TOL.vfxAbsByMetric[m], mx * 1.5);
    }
  }
  return tol;
}

export function compareRuns(dirA, dirB, opts = {}) {
  const out = { tolerance: TOL, stages: {}, ok: true, partial: [], mismatches: [] };
  const vfxTol = opts.vfxTol ?? null;
  if (opts.expectStages?.includes("vfx") && !vfxTol) { out.ok = false; out.mismatches.push("vfx: 受け入れの比較にはレシピ別の許容差（derive-vfx-tol の出力を --vfx-tol）が要る"); }
  for (const s of STAGE_NAMES) {
    const fa = path.join(dirA, `stage_${s}.json`), fb = path.join(dirB, `stage_${s}.json`);
    const ea = fs.existsSync(fa), eb = fs.existsSync(fb);
    if (ea !== eb) { out.stages[s] = { compared: false, asymmetric: true }; out.ok = false; out.mismatches.push(`${s}: 片方の撮影にしか無い段`); continue; }
    if (!ea) { out.stages[s] = { compared: false }; if (opts.expectStages?.includes(s)) { out.ok = false; out.mismatches.push(`${s}: 両方の撮影に無い（必須の段）`); } else out.partial.push(s); continue; }
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
        const bad = Object.keys(ia.metrics).filter((k) => !relOk(ia.metrics[k], ib.metrics?.[k], vfxFloor(vfxTol, ia.id, k), TOL.vfxRel));
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
    const sum = summarizeRun(path.resolve(o.run), { mcp14: loadMcp14(o.mcp14 ? path.resolve(o.mcp14) : MCP14_FILE) });
    const text = JSON.stringify(sum, null, 2) + "\n";
    fs.writeFileSync(path.join(path.resolve(o.run), "summary.json"), text);
    if (o.out) { fs.mkdirSync(path.dirname(path.resolve(o.out)), { recursive: true }); fs.writeFileSync(path.resolve(o.out), text); }
    const bad = Object.entries(sum.stages).filter(([, v]) => v.captured && !v.ok).map(([k]) => k);
    console.log(`[scorecard] summary: ${Object.entries(sum.stages).map(([k, v]) => `${k}=${v.captured ? Object.keys(v.items).length : "-"}`).join(" ")}${bad.length ? `  ERRORS: ${bad.join(",")}` : ""}`);
    return bad.length ? 1 : 0;
  }
  if (o.cmd === "mcp14-set") {
    if (!o.in) throw new Error("--in が要る");
    const r = readJson(path.resolve(o.in)), e = validateMcp14(r);
    if (e.length) { for (const m of e) console.log(`  不正: ${m}`); return 1; }
    const out = path.resolve(o.out ?? MCP14_FILE);
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, JSON.stringify(r, null, 2) + "\n");
    console.log(`[scorecard] mcp14: ${r.tasks.length} 課題を ${path.basename(out)} に書いた`);
    return 0;
  }
  if (o.cmd === "compare") {
    if (!o.a || !o.b) throw new Error("--a と --b が要る");
    const r = compareRuns(path.resolve(o.a), path.resolve(o.b), {
      vfxTol: o["vfx-tol"] ? readJson(path.resolve(o["vfx-tol"])) : null,
      expectStages: o["expect-stages"] === "all" ? STAGE_NAMES : o["expect-stages"] ? o["expect-stages"].split(",") : [],
    });
    if (o.out) fs.writeFileSync(path.resolve(o.out), JSON.stringify(r, null, 2));
    for (const [s, v] of Object.entries(r.stages)) console.log(`${s.padEnd(10)} ${v.compared ? `${v.ok ? "ok  " : "FAIL"} ${v.itemsOk}/${v.items}` : "(未比較)"}`);
    for (const m of r.mismatches) console.log(`  範囲外: ${m}`);
    if (!r.ok) { console.log("決定論の範囲を超えた差がある"); return 1; }
    if (r.partial.length) { console.log(`部分比較（未比較の段: ${r.partial.join(",")}）。比べた段は範囲内だが、受け入れの合格ではない（--expect-stages all で全段を必須にする）`); return 3; }
    console.log("決定論の範囲に収まっている");
    return 0;
  }
  if (o.cmd === "derive-vfx-tol") {
    if (!o.runs) throw new Error("--runs a,b,c（3 回以上）が要る");
    const dirs = o.runs.split(",").map((x) => path.resolve(x));
    if (dirs.length < 3) throw new Error("同じバイナリの撮影が 3 回以上要る");
    const text = JSON.stringify(deriveVfxTol(dirs), null, 2) + "\n";
    if (o.out) fs.writeFileSync(path.resolve(o.out), text); else console.log(text);
    return 0;
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
