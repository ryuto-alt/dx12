// 性能のベースライン（W05-39 / γ3 / W0-10）。golden_core の 5 シーンと新規シーンの GPU / フレーム時間を、
// 計測窓（ビルドの Mutex と %TEMP%\uno-gpu-window.lock）の中で撮り、%USERPROFILE%\dx12-scorecard\w0\baseline_perf.json へ書く。
//
// 規則（HARD RULE）:
//   ・窓を出さない／実マウスとフォーカスに触れない。エンジンは tools/engine_instance.ps1 経由の --headless だけ
//     （tools/bench/lib/engine.mjs の launchInstance。ビルド出力のフォルダは握らない）。
//   ・クロックの固定（クロック固定 API）は使わない（Developer Mode が無いとデバイス消失になる）。
//   ・API キーは使わない。絶対パスは JSON に書かない。
//
// 測り方（EXEC_W05_W1 §0.6）:
//   ・ABABAB… を 5 往復交互に測り、中央値を比べる。同じバイナリ同士の A/A を 1 組取り、
//     閾値 = max(0.05ms, 1%, A/A の 2σ)。各回の SM クロックと温度を nvidia-smi で記録し、
//     クロックが 3% 以上違う回は捨てる。DRED は --no-dred で揃える（改修前のバイナリには DRED が無い）。
//   ・1 回 = シーンごとに 300 フレームのウォームアップ → benchmark 600 フレーム × 3 窓。
//   ・★エンジンの benchmark は p50 を返さない（avg / p95 / min / max / 1% low）。p50 の代わりに「窓の avg の中央値」を
//     frameMsTypical として使い、p95 は窓の p95 の中央値を使う（baseline_perf.json の protocol.p50Note に記録）。
//
// 使い方（通常は baseline_perf.ps1 から。ビルドの Mutex を取ってこれを呼ぶ）:
//   node tools/scorecard/baseline_perf.mjs snapshot --label pre-overhaul --engine-dir <exe のあるフォルダ> [--no-dred]
//   node tools/scorecard/baseline_perf.mjs ab --id dred --a-dir D --a-label ... --a-args "--no-dred" --b-dir D --b-label ... [--rounds 5]
//   node tools/scorecard/baseline_perf.mjs probe --engine-dir <exe のあるフォルダ>   （VRAM の門の値だけを表示）
//   node tools/scorecard/baseline_perf.mjs validate [--file <json>] [--require pre-overhaul,w05-end,dred,f0,hooks]
// 共通: --out <json>（既定 ~/dx12-scorecard/w0/baseline_perf.json。試走の設定では --out の明示が必須）--port 8855 --vram-min-mb 6500
//       --warmup 300 --frames 600 --windows 3 --scenes a,b（絞り込み。試走用）
// 終了コード: 0 成功 / 1 失敗 / 2 引数 / 3 計測窓を取れない / 4 VRAM の余裕が足りず中止（中止の記録は JSON に残す）
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { execFile, spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, launchInstance, connect, sleep } from "../bench/lib/engine.mjs";
import { loadGoldenEntries } from "../bench/lib/goldenStore.mjs";
import { CORE_SCENES, NEW_SCENE_ID, sanitizeMsg, exeKnows, waitReady, openScene, createNewScene } from "./capture_engine.mjs";

export const SCHEMA_VERSION = 1;
export const DEFAULTS = Object.freeze({
  warmup: 300, frames: 600, windows: 3, rounds: 5, vramMinMB: 6500, clockDropPct: 3, floorMs: 0.05, floorPct: 1, sigmaK: 2,
});
export const REQUIRED_SNAPSHOTS = ["pre-overhaul", "w05-end"];
// M2b（W05-05）より後のバイナリで撮る記録。--no-dred の旗をバイナリが知っている（dredFlag === "passed"）ことを要求する。
export const FLAG_REQUIRED_SNAPSHOTS = ["w05-end"];
export const FLAG_REQUIRED_ABS = ["hooks"];
export const REQUIRED_AB = ["dred", "f0", "hooks"];
export const EXPECTED_SCENES = [...CORE_SCENES, NEW_SCENE_ID];

// ─── 統計（純関数） ─────────────────────────────────────────────────────
export function median(xs) {
  const a = xs.filter((x) => Number.isFinite(x)).sort((p, q) => p - q);
  if (!a.length) return null;
  const m = a.length >> 1;
  return a.length % 2 ? a[m] : (a[m - 1] + a[m]) / 2;
}
export function mean(xs) {
  const a = xs.filter((x) => Number.isFinite(x));
  return a.length ? a.reduce((s, x) => s + x, 0) / a.length : null;
}
/** 標本標準偏差（n-1）。2 点未満は null。 */
export function stddev(xs) {
  const a = xs.filter((x) => Number.isFinite(x));
  if (a.length < 2) return null;
  const m = mean(a);
  return Math.sqrt(a.reduce((s, x) => s + (x - m) * (x - m), 0) / (a.length - 1));
}
const r4 = (v) => (v === null || v === undefined ? null : Math.round(v * 10000) / 10000);

/** 窓ごとの結果から、そのシーンの代表値を出す（p50 の代わりに窓 avg の中央値）。 */
export function summarizeWindows(windows) {
  return {
    frameMsTypical: r4(median(windows.map((w) => w.frameMsAvg))),
    frameMsP95: r4(median(windows.map((w) => w.frameMsP95))),
    gpuTotalMs: r4(median(windows.map((w) => w.gpuTotalMs))),
    cpuWorkMs: r4(median(windows.map((w) => w.cpuWorkMs))),
    fps1PercentLow: r4(median(windows.map((w) => w.fps1PercentLow))),
    windows: windows.length,
  };
}

/** 閾値 = max(床 ms, 基準値の pct%, A/A の差の 2σ)。aaDiffs は A/A の組ごとの差。 */
export function abThreshold(baseValue, aaDiffs, o = {}) {
  const floorMs = o.floorMs ?? DEFAULTS.floorMs, floorPct = o.floorPct ?? DEFAULTS.floorPct, k = o.sigmaK ?? DEFAULTS.sigmaK;
  const sigma = stddev(aaDiffs ?? []);
  const parts = { floorMs, pct: Math.abs(baseValue ?? 0) * floorPct / 100, sigma2: sigma === null ? 0 : k * sigma };
  return { threshold: r4(Math.max(parts.floorMs, parts.pct, parts.sigma2)), sigma: r4(sigma), parts: { floorMs: r4(parts.floorMs), pct: r4(parts.pct), sigma2: r4(parts.sigma2) }, aaPairs: (aaDiffs ?? []).length };
}

/** 回のクロック（MHz）から捨てる回を決める。ref からの差が pct% 以上、またはペアの相手と pct% 以上違えば捨てる。
 *  rounds: [{ index, clockA, clockB? }]。1 側だけ（snapshot）のときは clockB を省く。 */
export function dropByClock(rounds, pct = DEFAULTS.clockDropPct, refMHz = null) {
  const all = rounds.flatMap((r) => [r.clockA, r.clockB]).filter((x) => Number.isFinite(x));
  const ref = Number.isFinite(refMHz) ? refMHz : median(all);   // refMHz: A/B と A/A で共通の基準（全回の中央値）を渡す
  const kept = [], dropped = [];
  for (const r of rounds) {
    const cs = [r.clockA, r.clockB].filter((x) => x !== undefined);
    let why = null;
    if (cs.some((c) => !Number.isFinite(c))) why = "クロックを取得できなかった";
    else {
      if (ref && cs.some((c) => Math.abs(c - ref) / ref * 100 >= pct)) why = `全体の中央値 ${Math.round(ref)}MHz から ${pct}% 以上`;
      if (!why && cs.length === 2 && Math.abs(cs[0] - cs[1]) / Math.max(cs[0], cs[1]) * 100 >= pct) why = `A と B のクロックが ${pct}% 以上違う`;
    }
    (why ? dropped : kept).push(why ? { index: r.index, reason: why, clockA: r.clockA, clockB: r.clockB } : r);
  }
  return { refMHz: ref === null ? null : Math.round(ref), kept, dropped };
}

/** A/B の比較。aVals / bVals は捨てた後の回ごとの値（同じ順）。aaDiffs は A/A の差。 */
export function compareAB(aVals, bVals, aaDiffs, o = {}) {
  const a = median(aVals), b = median(bVals);
  if (a === null || b === null) return { aMedian: r4(a), bMedian: r4(b), n: Math.min(aVals.length, bVals.length), verdict: "insufficient" };
  const delta = b - a;
  const th = abThreshold(a, aaDiffs, o);
  const verdict = Math.abs(delta) <= th.threshold ? "within" : delta > 0 ? "b-slower" : "b-faster";
  return { aMedian: r4(a), bMedian: r4(b), delta: r4(delta), deltaPct: a ? r4(delta / a * 100) : null, ...th, n: Math.min(aVals.length, bVals.length), verdict };
}

/** ABAB… の順序。rounds 往復なら ["A","B","A","B",…]。 */
export function ababSchedule(rounds) {
  const s = [];
  for (let i = 0; i < rounds; i++) s.push({ round: i, side: "A" }, { round: i, side: "B" });
  return s;
}

/** VRAM の門: DXGI の Budget（OS が他プロセスの使用量を引いた後の、このプロセスの上限）が minMB 以上なら通す。
 *  nvidia-smi の使用量は引かない（Budget が既に他プロセス分を反映しており、二重に引くことになる）。othersUsedMB は記録用。
 *  Budget が取れなければ中止（安全側）。 */
export function vramGate(budgetMB, othersUsedMB, minMB = DEFAULTS.vramMinMB) {
  const others = Number.isFinite(othersUsedMB) ? Math.round(othersUsedMB) : null;
  if (!Number.isFinite(budgetMB) || budgetMB <= 0) return { ok: false, reason: "VRAM の予算（Budget）を取得できない", budgetMB: null, othersUsedMB: others, minMB, rule: "budget>=min" };
  const ok = budgetMB >= minMB;
  return { ok, reason: ok ? null : `Budget ${Math.round(budgetMB)}MB < ${minMB}MB`, budgetMB: Math.round(budgetMB), othersUsedMB: others, minMB, rule: "budget>=min" };
}

// ─── JSON の組み立て・検証（純関数） ─────────────────────────────────────
export function emptyBaseline() {
  return { version: SCHEMA_VERSION, kind: "baseline_perf", protocol: null, hardware: null, snapshots: {}, abTests: {}, aborted: [] };
}
/** 既存の JSON へ 1 件併合する（同じキーは置き換える。ほかのキーは触らない）。 */
export function mergeInto(base, part) {
  const out = { ...emptyBaseline(), ...base, snapshots: { ...(base?.snapshots ?? {}) }, abTests: { ...(base?.abTests ?? {}) }, aborted: [...(base?.aborted ?? [])] };
  if (part.protocol) out.protocol = part.protocol;
  if (part.hardware) out.hardware = part.hardware;
  if (part.snapshot) out.snapshots[part.snapshot.label] = part.snapshot;
  if (part.abTest) out.abTests[part.abTest.id] = part.abTest;
  if (part.aborted) out.aborted.push(part.aborted);
  out.updatedAt = part.at ?? new Date().toISOString();
  return out;
}
const PATH_RE = /[A-Za-z]:[\\/]|\/(?:Users|home)\//;
/** 検査。require は揃っているべき snapshot / ab の名前（既定は何も要求しない）。 */
export function validateBaseline(j, require = []) {
  const errs = [];
  if (!j || j.kind !== "baseline_perf" || j.version !== SCHEMA_VERSION) errs.push("kind / version が違う");
  if (JSON.stringify(j ?? {}).match(PATH_RE)) errs.push("絶対パスが含まれている");
  for (const [label, s] of Object.entries(j?.snapshots ?? {})) {
    if (!s.scenes || !Object.keys(s.scenes).length) errs.push(`snapshot ${label}: シーンが無い`);
    for (const [id, sc] of Object.entries(s.scenes ?? {})) {
      if (!sc.summary || !Number.isFinite(sc.summary.frameMsTypical)) errs.push(`snapshot ${label}/${id}: summary.frameMsTypical が無い`);
      if (!(sc.keptRounds >= 1)) errs.push(`snapshot ${label}/${id}: 使える回が無い`);
    }
    if (!s.engine?.exeSha256) errs.push(`snapshot ${label}: exeSha256 が無い`);
  }
  for (const [id, t] of Object.entries(j?.abTests ?? {})) {
    if (!t.aa || !(t.aa.pairs >= 2)) errs.push(`ab ${id}: A/A が無い（2 組以上要る）`);
    if (!t.scenes || !Object.keys(t.scenes).length) errs.push(`ab ${id}: シーンが無い`);
    for (const [sid, c] of Object.entries(t.scenes ?? {})) if (c.verdict === "insufficient") errs.push(`ab ${id}/${sid}: 使える回が足りない`);
  }
  // --require の対象: 試走（smoke）の記録・DRED を揃えていない記録・シーンの欠けを NG にする。
  const sceneSets = [];
  for (const r of require) {
    const s = j?.snapshots?.[r], t = j?.abTests?.[r];
    const rec = s ?? t;
    if (!rec) { errs.push(`必要な記録が無い: ${r}`); continue; }
    if (rec.smoke) errs.push(`${r}: 試走（smoke）の記録。公式の設定で撮り直す`);
    if (rec.restrictedScenes) errs.push(`${r}: --scenes で絞った記録。全シーンで撮り直す`);
    const ids = Object.keys(rec.scenes ?? {}).sort();
    const missing = EXPECTED_SCENES.filter((x) => !ids.includes(x));
    if (missing.length) errs.push(`${r}: シーンが欠けている: ${missing.join(", ")}`);
    sceneSets.push([r, ids.join(",")]);
    if (s && REQUIRED_SNAPSHOTS.includes(r) && s.noDred !== true) errs.push(`snapshot ${r}: --no-dred で撮っていない（DRED の ON/OFF が揃わない）`);
    if (s && FLAG_REQUIRED_SNAPSHOTS.includes(r) && s.dredFlag !== "passed") errs.push(`snapshot ${r}: --no-dred をバイナリが受け取っていない（dredFlag=${s.dredFlag}）。DRED が ON のまま測られた可能性`);
    if (t) {
      if (FLAG_REQUIRED_ABS.includes(r) && (t.a?.dredFlag !== "passed" || t.b?.dredFlag !== "passed")) errs.push(`ab ${r}: 両側で --no-dred をバイナリが受け取っていない（a=${t.a?.dredFlag}, b=${t.b?.dredFlag}）`);
      if (r === "dred" && ((t.b?.noDred === true) || (t.b?.extraArgs ?? []).includes("--no-dred"))) errs.push("ab dred: B 側が DRED OFF になっている（B は DRED ON でなければ ON/OFF の A/B にならない）");
      if (r === "dred" && ((t.a?.extraArgs ?? []).includes("--dred"))) errs.push("ab dred: A 側に --dred が渡っている");
      if (r === "dred" && t.a?.dredFlag !== "passed") errs.push("ab dred: A 側に --no-dred が渡っていない（旗の無いバイナリでは両側が同じ構成になる）");
      if (r === "dred" && t.sameBinary !== true) errs.push("ab dred: A と B が同じバイナリではない（DRED の ON/OFF だけを変えるのが条件）");
      if (r !== "dred" && (t.a?.noDred !== true || t.b?.noDred !== true)) errs.push(`ab ${r}: 両側を --no-dred で揃えていない`);
    }
  }
  if (new Set(sceneSets.map((x) => x[1])).size > 1) errs.push(`記録の間でシーンの集合が違う: ${sceneSets.map((x) => x[0]).join(", ")}`);
  return { ok: errs.length === 0, errors: errs };
}

// ─── 計測窓のロック（%TEMP%\uno-gpu-window.lock） ────────────────────────
// ロックの書式（NB1 の perf_gate（TS）と共有する約束）: JSON {pid, owner, startedAt}。open("wx") で排他に作る。
// 持ち主の PID が居なければ古いロックとして奪う。奪うときは rename で自分の名前へ退避してから中身を読み直す
// （rmSync だと、他のプロセスが作ったばかりの新しいロックを消して両方が持つ競合がある）。
export const LOCK_FILE = path.join(os.tmpdir(), "uno-gpu-window.lock");
/** 残る競合: rename と link の間に第三者が wx で新しいロックを作ると、link が失敗して二者が同時に持ちうる。
 *  退避したロックの持ち主が生きている場合に限り起きる狭い窓で、計測は後続の A/A とクロックの記録で検出される範囲に収まるとみなす。
 *  古いロックを奪う。退避したものの持ち主が生きていれば（他者の新しいロックだった）link で戻す（既存があれば失敗 = 上書きしない）。 */
function stealStale(file) {
  const moved = `${file}.stale-${process.pid}-${Date.now()}`;
  try { fs.renameSync(file, moved); } catch { return; }   // 既に誰かが消した / 奪った
  let h = null;
  try { h = JSON.parse(fs.readFileSync(moved, "utf8")); } catch { /* 壊れ = 古い扱い */ }
  if (h && Number.isInteger(h.pid) && pidAlive(h.pid)) { try { fs.linkSync(moved, file); } catch { /* 別のロックが既にある */ } }
  try { fs.rmSync(moved, { force: true }); } catch { /* 無視 */ }
}
function pidAlive(pid) { try { process.kill(pid, 0); return true; } catch (e) { return e.code === "EPERM"; } }
/** 排他で取る。古い（持ち主の PID が居ない）ロックは奪う。waitSec 秒まで待つ。取れたら解放関数、だめなら null。 */
export async function acquireGpuWindow({ file = LOCK_FILE, waitSec = 0, owner = "baseline_perf", pollMs = 2000 } = {}) {
  const t0 = Date.now();
  for (;;) {
    try {
      fs.writeFileSync(file, JSON.stringify({ pid: process.pid, owner, startedAt: new Date().toISOString() }), { flag: "wx" });
      let released = false;
      const release = () => { if (released) return; released = true; try { fs.rmSync(file, { force: true }); } catch { /* 無視 */ } };
      process.once("exit", release);
      return release;
    } catch (e) {
      if (e.code !== "EEXIST") throw e;
      let holder = null;
      try { holder = JSON.parse(fs.readFileSync(file, "utf8")); } catch { /* 読めない = 書き込み途中か壊れ */ }
      if (holder && Number.isInteger(holder.pid) && !pidAlive(holder.pid)) { stealStale(file); continue; }
      if (Date.now() - t0 >= waitSec * 1000) return null;
      await sleep(pollMs);
    }
  }
}

// ─── nvidia-smi ─────────────────────────────────────────────────────────
function smiOnce() {
  return new Promise((resolve) => {
    execFile("nvidia-smi", ["--query-gpu=clocks.sm,temperature.gpu,memory.used,memory.total,name,driver_version", "--format=csv,noheader,nounits"],
      { timeout: 10_000, windowsHide: true }, (err, out) => {
        if (err) return resolve(null);
        const f = String(out).trim().split(/\r?\n/)[0].split(",").map((s) => s.trim());
        const n = (s) => (Number.isFinite(Number(s)) ? Number(s) : null);
        resolve({ smMHz: n(f[0]), tempC: n(f[1]), memUsedMB: n(f[2]), memTotalMB: n(f[3]), name: f[4] ?? null, driver: f[5] ?? null });
      });
  });
}
/** nvidia-smi を 1 本だけ起動したまま -lms で読み続ける（毎秒プロセスを起こすと benchmark の窓に雑音が乗る）。 */
function startClockPoller(everyMs = 1000) {
  const samples = [];
  let buf = "";
  const n = (x) => (Number.isFinite(Number(x)) ? Number(x) : null);
  let child = null;
  try {
    child = spawn("nvidia-smi", ["--query-gpu=clocks.sm,temperature.gpu,memory.used,memory.total", "--format=csv,noheader,nounits", "-lms", String(everyMs)], { windowsHide: true, stdio: ["ignore", "pipe", "ignore"] });
    child.on("error", () => { /* nvidia-smi が無い。サンプル無しで進む */ });
    child.stdout.on("data", (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const f = buf.slice(0, i).trim().split(",").map((x) => x.trim()); buf = buf.slice(i + 1);
        if (f.length >= 4) samples.push({ smMHz: n(f[0]), tempC: n(f[1]), memUsedMB: n(f[2]), memTotalMB: n(f[3]) });
      }
    });
  } catch { child = null; }
  return { async stop() { try { child?.kill(); } catch { /* 無視 */ } return samples; } };
}
export function clockStats(samples) {
  // 負荷が掛かっているサンプルだけ（アイドルの低いクロックを混ぜない）。掛かっていなければ全部。
  const sm = samples.map((s) => s.smMHz).filter(Number.isFinite);
  if (!sm.length) return { smMHz: null, smMin: null, smMax: null, tempC: null, n: 0 };
  const top = Math.max(...sm);
  const busy = sm.filter((c) => c >= top * 0.5);
  return { smMHz: Math.round(mean(busy)), smMin: Math.min(...busy), smMax: Math.max(...busy), tempC: median(samples.map((s) => s.tempC)), n: busy.length };
}

// ─── 計測 1 回（エンジン 1 回起動 = 1 回） ────────────────────────────────
class AbortRun extends Error { constructor(gate) { super(gate.reason); this.gate = gate; } }

function pickWindow(r) {
  return {
    frameMsAvg: r.frameMs?.avg ?? null, frameMsP95: r.frameMs?.p95 ?? null, frameMsMin: r.frameMs?.min ?? null, frameMsMax: r.frameMs?.max ?? null,
    gpuTotalMs: r.gpuPassMs?.total ?? r.gpuPassMs?.Total ?? null, cpuWorkMs: r.cpu?.workMs ?? null, fps1PercentLow: r.fps1PercentLow ?? null,
    renderResolution: r.renderResolution ?? null, upscaler: r.upscaler?.mode ?? r.upscaler?.upscaler ?? null,
  };
}

export function resolveScenes(only) {
  const cfg = loadGoldenEntries(path.join(REPO_ROOT, "tests", "golden"));
  const byName = new Map(cfg.scenes.map((s) => [s.name, s]));
  const ids = EXPECTED_SCENES;
  const missing = ids.filter((n) => !byName.has(n));
  if (missing.length) throw new Error(`tests/golden/entries に無いシーン: ${missing.join(", ")}`);   // 黙って落とさない
  if (only) { const bad = only.filter((n) => !ids.includes(n)); if (bad.length) throw new Error(`--scenes に未知のシーン: ${bad.join(", ")}`); }
  const list = ids.map((n) => byName.get(n)).filter((s) => !only || only.includes(s.name));
  if (!list.length) throw new Error("計測するシーンが無い（--scenes を確認）");
  return { defaults: cfg.defaults, list };
}

/** 作業用のコピー（計測のたびにチェックアウトの中へ _mcp_untitled_* などを増やさない）。 */
function copyProject(src) {
  const dst = fs.mkdtempSync(path.join(os.tmpdir(), "uno-perf-proj-"));
  fs.cpSync(src, dst, { recursive: true });
  return dst;
}
function removeTemp(dir) {
  const root = path.resolve(os.tmpdir());
  const d = path.resolve(dir);
  if (path.dirname(d) !== root || !path.basename(d).startsWith("uno-perf-proj-")) return;   // 自分が作ったものだけ
  try { fs.rmSync(d, { recursive: true, force: true }); } catch { /* 無視 */ }
}

/** 他プロセスの VRAM 使用量が基準（実行の最初の起動前の値）付近に戻るまで待つ。直前のエンジンの解放待ち。 */
async function waitVramSettle(baselineMB, toleranceMB = 150, timeoutMs = 90_000) {
  const t0 = Date.now();
  let cur = null;
  while (Date.now() - t0 < timeoutMs) {
    cur = (await smiOnce())?.memUsedMB ?? null;
    if (cur === null || baselineMB === null || cur <= baselineMB + toleranceMB) return { settled: true, memUsedMB: cur, waitedMs: Date.now() - t0 };
    await sleep(1500);
  }
  return { settled: false, memUsedMB: cur, waitedMs: Date.now() - t0 };
}

/** cfg = { label, engineDir, extra[] }。シーンごとに windows 個の窓を返す。 */
export async function measureSession(cfg, sceneList, o) {
  // VRAM: 実行の最初の 1 回だけ門で判定する。以降は、直前のエンジンの解放を待ってから起動し、足りなくても警告に留める。
  const st = (o.vramState ??= { baselineMB: null, gated: false });
  let smiBefore = await smiOnce();   // 起動前の使用量 = 他のプロセスの使用量
  let settle = null;
  if (st.gated && st.baselineMB !== null) { settle = await waitVramSettle(st.baselineMB); smiBefore = await smiOnce(); }
  const projectSrc = path.resolve(REPO_ROOT, sceneList[0].project);
  const project = copyProject(projectSrc);
  let eng = null;
  let conn;
  const out = { scenes: {}, clockSamples: [], vramSettle: settle };
  try {
    eng = await launchInstance({
      name: o.instance, project, port: o.port, buildDir: cfg.engineDir, mode: "headless",
      extraArgs: [...cfg.extra, "--scene", sceneList[0].scene ?? sceneList[0].launchScene],
    });
    conn = await connect(eng.port);
    await waitReady(conn);
    const ping = await conn.call("ping");
    const gate = vramGate(ping.vramBudgetMB, smiBefore?.memUsedMB, o.vramMinMB);
    gate.smiTotalMB = smiBefore?.memTotalMB ?? null;
    out.vramCheck = gate;
    if (!st.gated) {
      st.gated = true; st.baselineMB = smiBefore?.memUsedMB ?? null;
      if (!gate.ok && o.gateMode !== "report") throw new AbortRun(gate);
    } else if (!gate.ok) {
      out.vramLow = true;   // 解放を待っても門を割った回。呼び出し側が捨てる（記録は残す）
      console.log(`[perf] 警告: VRAM が門を割った（${gate.reason}）。この回は捨てる`);
    }
    if (o.probeOnly) return out;
    for (const s of sceneList) {
      if (s.newScene) await createNewScene(conn); else await openScene(conn, s.scene);
      await conn.call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
      const poller = startClockPoller();
      const windows = [];
      try {
        if (o.warmup > 0) await conn.call("benchmark", { frames: o.warmup, uncap: true }, 900_000);   // 計測と同じ uncap で温める（結果は捨てる）
        for (let w = 0; w < o.windows; w++) windows.push(pickWindow(await conn.call("benchmark", { frames: o.frames, uncap: true }, 900_000)));
      } finally { out.scenes[s.name] = { windows, clock: clockStats(await poller.stop()) }; }
    }
  } finally {
    try { conn?.close(); } catch { /* 切れている */ }
    try { eng?.kill(); } catch { /* 無視 */ }
    removeTemp(project);
  }
  return out;
}

function engineExtra(engineDir, noDred, extraArgs) {
  const extra = [...extraArgs];
  let dredFlag = "n/a";
  if (noDred) {
    const known = exeKnows(path.join(engineDir, "DX12Engine.exe"), "--no-dred");
    dredFlag = known ? "passed" : "binary-has-no-dred";   // 改修前のバイナリには DRED も旗も無い。渡さずに記録する
    if (known) extra.push("--no-dred");
  }
  return { extra, dredFlag };
}
function engineInfo(engineDir, label) {
  const exe = path.join(engineDir, "DX12Engine.exe");
  let nightly = null;
  try { nightly = JSON.parse(fs.readFileSync(path.join(engineDir, "nightly.json"), "utf8")); } catch { /* 無ければ null */ }
  return {
    label, exeSha256: createHash("sha256").update(fs.readFileSync(exe)).digest("hex"),
    ref: nightly?.ref ?? null, commit: nightly?.commit ?? null, engineVersion: nightly?.engineVersion ?? null, dlss: nightly?.dlss ?? null,
    hasAgilityDir: fs.existsSync(path.join(engineDir, "D3D12")),
  };
}

// ─── コマンド ───────────────────────────────────────────────────────────
function protocolOf(o) {
  return {
    warmupFrames: o.warmup, windowFrames: o.frames, windowsPerScene: o.windows, rounds: o.rounds,
    order: "ABAB… を rounds 往復（snapshot は同じ構成を rounds 回）。A/A は A を 2 回の組",
    threshold: `max(${DEFAULTS.floorMs}ms, ${DEFAULTS.floorPct}%, A/A の差の ${DEFAULTS.sigmaK}σ)`, clockDropPct: DEFAULTS.clockDropPct, vramMinMB: o.vramMinMB,
    metric: "gpuTotalMs（GpuTimer の Total の窓平均の、回ごとの中央値）。frameMsTypical は p50 の代わり",
    p50Note: "エンジンの benchmark は p50 を返さない。frameMsTypical = 窓の avg の中央値、frameMsP95 = 窓の p95 の中央値。",
    noClockLock: "クロック固定 API は使わない。クロックは nvidia-smi で記録し、ずれた回は捨てる",
  };
}
/** 記録ごとの設定（試走かどうか・絞り込みの有無）。併合で上書きされないよう snapshot / abTest が持つ。 */
function recordProto(o) {
  return { smoke: !!o.smoke, restrictedScenes: !!o.scenes, protocol: protocolOf(o) };
}

/** 全体の protocol には試走の設定を入れない（試走が公式の記録の protocol を上書きしない）。 */
function officialProtocol(o) { return o.smoke ? null : protocolOf(o); }

function scenePack(sceneIds, sessions, key) {
  // sessions: [{ index, scenes:{id:{windows,clock}} }] → シーンごと: 回ごとの値と代表値
  const out = {};
  for (const id of sceneIds) {
    const rounds = sessions.map((s) => ({ index: s.index, ...s.scenes[id], summary: summarizeWindows(s.scenes[id].windows) }));
    out[id] = { rounds, key, value: median(rounds.map((r) => r.summary[key])) };
  }
  return out;
}

async function cmdSnapshot(o) {
  const { list } = resolveScenes(o.scenes);
  const ids = list.map((s) => s.name);
  const { extra, dredFlag } = engineExtra(o.engineDir, o.noDred, []);
  const cfg = { label: o.label, engineDir: o.engineDir, extra };
  const sessions = [];
  for (let i = 0; i < o.rounds; i++) {
    console.log(`[perf] snapshot ${o.label}: round ${i + 1}/${o.rounds}`);
    try { const s = await measureSession(cfg, list, o); sessions.push({ index: i, ...s }); }
    catch (e) { e.partial = { completedRounds: sessions.length, rounds: sessions.map((x) => ({ index: x.index, scenes: Object.fromEntries(Object.entries(x.scenes).map(([id, v]) => [id, summarizeWindows(v.windows)])) })) }; throw e; }
  }
  const perScene = {};
  const clocks = sessions.map((s, i) => ({ index: i, clockA: mean(Object.values(s.scenes).map((x) => x.clock.smMHz).filter(Number.isFinite)) }));
  const vramDropped = sessions.filter((s) => s.vramLow).map((s) => ({ index: s.index, reason: "VRAM が門を割った（解放待ちの後も）" }));
  const drop0 = dropByClock(clocks.filter((c) => !sessions[c.index].vramLow).map((c) => ({ ...c, clockA: Number.isFinite(c.clockA) ? c.clockA : NaN })), DEFAULTS.clockDropPct);
  const drop = { ...drop0, dropped: [...vramDropped, ...drop0.dropped] };
  const keptIdx = new Set(drop.kept.map((r) => r.index));
  for (const id of ids) {
    const rounds = sessions.map((s) => ({ index: s.index, clock: s.scenes[id].clock, summary: summarizeWindows(s.scenes[id].windows), windows: s.scenes[id].windows }));
    const kept = rounds.filter((r) => keptIdx.has(r.index));
    perScene[id] = {
      keptRounds: kept.length, droppedRounds: rounds.length - kept.length,
      summary: summarizeWindows(kept.flatMap((r) => r.windows)),
      perRound: rounds.map((r) => ({ round: r.index, kept: keptIdx.has(r.index), clock: r.clock, vramCheck: sessions[r.index].vramCheck ?? null, vramSettle: sessions[r.index].vramSettle ?? null, ...r.summary })),
    };
  }
  const snapshot = {
    label: o.label, capturedAt: new Date().toISOString(), ...recordProto(o), noDred: o.noDred, dredFlag, engine: engineInfo(o.engineDir, o.label),
    vramCheck: sessions[0].vramCheck, vramChecks: sessions.map((s) => ({ round: s.index, check: s.vramCheck ?? null, settle: s.vramSettle ?? null })), clockRefMHz: drop.refMHz, discardedRounds: drop.dropped, scenes: perScene,
  };
  return { protocol: officialProtocol(o), hardware: await hardwareInfo(), snapshot, at: snapshot.capturedAt };
}

async function hardwareInfo() {
  const s = await smiOnce();
  return { gpu: s?.name ?? null, driver: s?.driver ?? null, vramTotalMB: s?.memTotalMB ?? null, os: `${os.type()} ${os.release()}`, cpus: os.cpus()[0]?.model ?? null };
}

/** VRAM の門の値だけを表示する（Budget と nvidia-smi の使用量。実測を見て定義を決めるため）。 */
async function cmdProbe(o) {
  const { list } = resolveScenes(["core_pbr_spheres"].filter((n) => EXPECTED_SCENES.includes(n)).length ? ["core_pbr_spheres"] : [EXPECTED_SCENES[0]]);
  o.probeOnly = true; o.gateMode = "report";
  const s = await measureSession({ label: "probe", engineDir: o.engineDir, extra: [] }, list, o);
  const g = s.vramCheck;
  console.log(JSON.stringify({ budgetMB: g.budgetMB, nvidiaSmiUsedMB: g.othersUsedMB, nvidiaSmiTotalMB: g.smiTotalMB, minMB: g.minMB, rule: g.rule, gateOk: g.ok }, null, 2));
  return 0;
}

async function cmdAb(o) {
  const { list } = resolveScenes(o.scenes);
  const ids = list.map((s) => s.name);
  const A = (() => { const { extra, dredFlag } = engineExtra(o.aDir, o.aNoDred, o.aArgs); return { label: o.aLabel, engineDir: o.aDir, extra, dredFlag }; })();
  const B = (() => { const { extra, dredFlag } = engineExtra(o.bDir, o.bNoDred, o.bArgs); return { label: o.bLabel, engineDir: o.bDir, extra, dredFlag }; })();
  const key = "gpuTotalMs";
  const val = (sess, id) => summarizeWindows(sess.scenes[id].windows)[key];
  const clockOf = (sess) => mean(Object.values(sess.scenes).map((x) => x.clock.smMHz).filter(Number.isFinite));
  // ABAB… と A/A（A を 2 回の組）。A/A は A/B の後に取る（同じ温度条件に近づけるため同数）。順序は ababSchedule。
  const vramOf = (sess) => ({ vramCheck: sess.vramCheck ?? null, vramSettle: sess.vramSettle ?? null });
  const runPairs = async (cfgA, cfgB, tag, pairs) => {
    let a = null;
    for (const it of ababSchedule(o.rounds)) {
      console.log(`[perf] ab ${o.id} ${tag}: round ${it.round + 1}/${o.rounds} ${it.side}`);
      try {
        if (it.side === "A") a = await measureSession(cfgA, list, o);
        else {
          const b = await measureSession(cfgB, list, o);
          pairs.push({ index: it.round, a, b, clockA: clockOf(a) ?? NaN, clockB: clockOf(b) ?? NaN, vramLow: !!(a.vramLow || b.vramLow) });
        }
      } catch (e) { e.partial = partialOf(); throw e; }
    }
    return pairs;
  };
  const pairLog = (p) => ({ round: p.index, clockA: p.clockA, clockB: p.clockB, vramLow: p.vramLow, a: vramOf(p.a), b: vramOf(p.b) });
  // 途中で止まったときに、そこまでの結果を aborted に残すための要約
  const done = { ab: [], aa: [] };
  const sceneVals = (p) => Object.fromEntries(ids.map((id) => [id, { a: val(p.a, id), b: val(p.b, id) }]));
  const partialOf = () => ({ completedAbRounds: done.ab.length, completedAaRounds: done.aa.length,
    ab: done.ab.map((p) => ({ ...pairLog(p), scenes: sceneVals(p) })),
    aa: done.aa.map((p) => ({ ...pairLog(p), scenes: sceneVals(p) })) });
  const abPairs = await runPairs(A, B, "A/B", done.ab);
  const aaPairs = await runPairs(A, { ...A, label: `${A.label}#2` }, "A/A", done.aa);
  // クロックの基準は全回（A/B と A/A）で共通の中央値。VRAM が門を割った回は先に捨てる（記録は pairs に残る）。
  const refAll = median([...abPairs, ...aaPairs].flatMap((p) => [p.clockA, p.clockB]).filter((x) => Number.isFinite(x)));
  const dropPairs = (pairs) => {
    const vramDropped = pairs.filter((p) => p.vramLow).map((p) => ({ index: p.index, reason: "VRAM が門を割った（解放待ちの後も）" }));
    const d = dropByClock(pairs.filter((p) => !p.vramLow).map((p) => ({ index: p.index, clockA: p.clockA, clockB: p.clockB })), DEFAULTS.clockDropPct, refAll);
    return { ...d, dropped: [...vramDropped, ...d.dropped] };
  };
  const abDrop = dropPairs(abPairs);
  const aaDrop = dropPairs(aaPairs);
  const abKept = abPairs.filter((p) => abDrop.kept.some((k) => k.index === p.index));
  const aaKept = aaPairs.filter((p) => aaDrop.kept.some((k) => k.index === p.index));
  const scenes = {};
  for (const id of ids) {
    const aaDiffs = aaKept.map((p) => val(p.b, id) - val(p.a, id));
    scenes[id] = {
      [key]: compareAB(abKept.map((p) => val(p.a, id)), abKept.map((p) => val(p.b, id)), aaDiffs),
      frameMsTypical: compareAB(abKept.map((p) => summarizeWindows(p.a.scenes[id].windows).frameMsTypical), abKept.map((p) => summarizeWindows(p.b.scenes[id].windows).frameMsTypical),
        aaKept.map((p) => summarizeWindows(p.b.scenes[id].windows).frameMsTypical - summarizeWindows(p.a.scenes[id].windows).frameMsTypical)),
    };
    scenes[id].verdict = scenes[id][key].verdict;
  }
  const test = {
    id: o.id, note: o.note ?? null, capturedAt: new Date().toISOString(), ...recordProto(o), metric: key,
    a: { label: A.label, noDred: o.aNoDred, dredFlag: A.dredFlag, extraArgs: o.aArgs, engine: engineInfo(A.engineDir, A.label) },
    b: { label: B.label, noDred: o.bNoDred, dredFlag: B.dredFlag, extraArgs: o.bArgs, engine: engineInfo(B.engineDir, B.label) },
    sameBinary: engineInfo(A.engineDir, "x").exeSha256 === engineInfo(B.engineDir, "x").exeSha256,
    clockRefMHz: refAll === null ? null : Math.round(refAll), discardedRounds: abDrop.dropped, rounds: abKept.length,
    aa: { pairs: aaKept.length, discardedRounds: aaDrop.dropped, clockRefMHz: refAll === null ? null : Math.round(refAll) },
    pairs: { ab: abPairs.map(pairLog), aa: aaPairs.map(pairLog) },   // 回ごとのクロックと VRAM の門・解放待ち
    scenes,
  };
  return { protocol: officialProtocol(o), hardware: await hardwareInfo(), abTest: test, at: test.capturedAt };
}

// ─── 引数 ───────────────────────────────────────────────────────────────
export function parseArgs(argv) {
  const cmd = argv[0];
  const o = {
    cmd, out: path.join(os.homedir(), "dx12-scorecard", "w0", "baseline_perf.json"), port: 8855, instance: null,
    warmup: DEFAULTS.warmup, frames: DEFAULTS.frames, windows: DEFAULTS.windows, rounds: DEFAULTS.rounds, vramMinMB: DEFAULTS.vramMinMB,
    noDred: false, aNoDred: false, bNoDred: false, aArgs: [], bArgs: [], scenes: null, lockWaitSec: 0, require: [], file: null,
  };
  if (!["snapshot", "ab", "validate", "probe"].includes(cmd)) throw new Error("コマンドは snapshot | ab | probe | validate");
  for (let i = 1; i < argv.length; i++) {
    const k = argv[i];
    const v = () => { if (i + 1 >= argv.length) throw new Error(`${k} に値が要る`); return argv[++i]; };
    const n = () => { const x = Number(v()); if (!Number.isFinite(x) || x < 0) throw new Error(`${k} は 0 以上の数`); return x; };
    switch (k) {
      case "--out": o.out = path.resolve(v()); o.outExplicit = true; break;
      case "--file": o.file = path.resolve(v()); break;
      case "--port": o.port = n(); break;
      case "--instance": o.instance = v(); break;
      case "--label": o.label = v(); break;
      case "--engine-dir": o.engineDir = path.resolve(v()); break;
      case "--no-dred": o.noDred = true; break;
      case "--id": o.id = v(); break;
      case "--note": o.note = v(); break;
      case "--a-dir": o.aDir = path.resolve(v()); break;
      case "--b-dir": o.bDir = path.resolve(v()); break;
      case "--a-label": o.aLabel = v(); break;
      case "--b-label": o.bLabel = v(); break;
      case "--a-no-dred": o.aNoDred = true; break;
      case "--b-no-dred": o.bNoDred = true; break;
      case "--a-args": o.aArgs = v().split(/\s+/).filter(Boolean); break;
      case "--b-args": o.bArgs = v().split(/\s+/).filter(Boolean); break;
      case "--rounds": o.rounds = Math.max(2, n()); break;
      case "--warmup": o.warmup = n(); break;
      case "--frames": o.frames = Math.max(30, n()); break;
      case "--windows": o.windows = Math.max(1, n()); break;
      case "--scenes": o.scenes = v().split(",").filter(Boolean); break;
      case "--vram-min-mb": o.vramMinMB = n(); break;
      case "--lock-wait-min": o.lockWaitSec = n() * 60; break;
      case "--require": o.require = v().split(",").filter(Boolean); break;
      default: throw new Error(`不明な引数: ${k}`);
    }
  }
  if (cmd === "probe" && !o.engineDir) throw new Error("--engine-dir が要る");
  if (cmd === "snapshot") { if (!o.label || !/^[A-Za-z0-9._-]+$/.test(o.label)) throw new Error("--label が要る（英数字と . _ -）"); if (!o.engineDir) throw new Error("--engine-dir が要る"); }
  if (cmd === "ab") {
    // --a-args / --b-args に DRED の旗が入っていたら専用の旗へ読み替える（`--a-args "--no-dred"` と `--a-no-dred` を同じ意味にする）
    for (const side of ["a", "b"]) {
      const args = o[`${side}Args`];
      if (args.includes("--dred")) throw new Error(`--${side}-args に --dred は渡せない（DRED ON は旗なしが既定）`);
      if (args.includes("--no-dred")) { o[`${side}NoDred`] = true; o[`${side}Args`] = args.filter((x) => x !== "--no-dred"); }
    }
    if (!o.id || !/^[A-Za-z0-9._-]+$/.test(o.id)) throw new Error("--id が要る（英数字と . _ -）");
    for (const [k, x] of [["--a-dir", o.aDir], ["--b-dir", o.bDir], ["--a-label", o.aLabel], ["--b-label", o.bLabel]]) if (!x) throw new Error(`${k} が要る`);
  }
  o.smoke = o.warmup !== DEFAULTS.warmup || o.frames !== DEFAULTS.frames || o.windows !== DEFAULTS.windows || o.rounds !== DEFAULTS.rounds || !!o.scenes;   // vramMinMB は試走の判定に入れない（protocol に値を残す）
  o.instance ??= `perf-baseline-${createHash("sha1").update(REPO_ROOT).digest("hex").slice(0, 8)}`;
  return o;
}

function writeJson(file, j) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(j, null, 2) + "\n");
  fs.renameSync(tmp, file);
}
function readJson(file) { try { return JSON.parse(fs.readFileSync(file, "utf8")); } catch { return null; } }

async function main() {
  const o = parseArgs(process.argv.slice(2));
  if (o.cmd === "validate") {
    const file = o.file ?? o.out;
    const j = readJson(file);
    if (!j) { console.log("[perf] 読めない: " + path.basename(file)); return 1; }
    const v = validateBaseline(j, o.require);
    console.log(`[perf] validate: ${v.ok ? "ok" : "NG"}`);
    for (const e of v.errors) console.log(`  - ${e}`);
    return v.ok ? 0 : 1;
  }
  if (o.smoke && !o.outExplicit && o.cmd !== "probe") { console.log("[perf] 試走の設定（--frames などを既定から変えた）で公式の出力先へは書かない。--out で別の JSON を指定する。"); return 2; }
  const release = await acquireGpuWindow({ waitSec: o.lockWaitSec });
  if (!release) { console.log("[perf] 計測窓（uno-gpu-window.lock）を取れない。他の計測が走っている。--lock-wait-min で待てる。"); return 3; }
  try { os.setPriority(0, os.constants.priority.PRIORITY_BELOW_NORMAL); } catch { /* 無視 */ }
  try {
    if (o.cmd === "probe") return await cmdProbe(o);
    const part = o.cmd === "snapshot" ? await cmdSnapshot(o) : await cmdAb(o);
    const merged = mergeInto(readJson(o.out) ?? emptyBaseline(), part);
    writeJson(o.out, merged);
    console.log(`[perf] ${o.cmd} 完了: ${path.basename(o.out)}${o.smoke ? "（試走の設定。公式の数値ではない）" : ""}`);
    return 0;
  } catch (e) {
    if (e instanceof AbortRun) {
      const rec = { at: new Date().toISOString(), command: o.cmd, target: o.label ?? o.id, smoke: !!o.smoke, reason: e.gate.reason, vramCheck: e.gate, partial: e.partial ?? null };
      writeJson(o.out, mergeInto(readJson(o.out) ?? emptyBaseline(), { aborted: rec, at: rec.at }));
      console.log(`[perf] 中止: ${e.gate.reason}（記録した）`);
      return 4;
    }
    if (e?.partial && o.cmd !== "probe") {
      const rec = { at: new Date().toISOString(), command: o.cmd, target: o.label ?? o.id, smoke: !!o.smoke, reason: sanitizeMsg(e?.message ?? e), partial: e.partial };
      try { writeJson(o.out, mergeInto(readJson(o.out) ?? emptyBaseline(), { aborted: rec, at: rec.at })); console.log("[perf] 途中までの結果を aborted に記録した"); } catch { /* 記録できなくても失敗は返す */ }
    }
    console.log(`[perf] 失敗: ${sanitizeMsg(e?.stack ?? e)}`);
    return 1;
  } finally { release(); }
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main().then((c) => process.exit(c), (e) => { console.error(sanitizeMsg(e?.message ?? e)); process.exit(2); });
