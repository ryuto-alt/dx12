// perf_gate の純ロジック(統計・A/B・A/A・クロック・VRAM の門・履歴と回帰・計測窓のロック)。エンジン呼び出しは注入するので単体テストできる。
//
// 測り方の規約(EXEC_W05_W1 §0.6):
//   ・クロックの固定 API は使わない(Developer Mode が無いとデバイス消失になる)。クロックは記録し、3% 以上ずれた回は捨てる。
//   ・ABAB… を rounds 往復交互に測り、中央値を比べる。同じバイナリ同士の A/A も 1 組取り、閾値 = max(床 ms, 基準の pct%, A/A の差の 2σ)。
//   ・DRED は --no-dred で揃える。
// ここにある数値は「測り方の定数」であって予算ではない(予算は budgets.ts が JSON から読む)。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { appendJsonl, median, quantile, readJsonl, round, sleep } from "./common.ts";

export const PROTOCOL = Object.freeze({
  /** 1 姿勢あたりの繰り返し(中央値を取る)。 */ repeats: 3,
  /** step_frames の枚数(温め)。 */ warmupFrames: 180,
  /** benchmark のフレーム数。 */ benchFrames: 600,
  /** perf_stats の窓。 */ statsWindow: 240,
  /** A/B の往復数。 */ rounds: 5,
  /** 閾値の床(ms)・割合(%)・A/A の σ の倍数。 */ floorMs: 0.05, floorPct: 1, sigmaK: 2,
  /** クロックがこの割合以上ずれた回は捨てる(%)。 */ clockDropPct: 3,
  /** 直近何回の中央値と比べるか・何%悪化で回帰とするか。 */ historyN: 5, regressionPct: 10,
});

export const mean = (xs: number[]): number | null => { const a = xs.filter(Number.isFinite); return a.length ? a.reduce((s, x) => s + x, 0) / a.length : null; };
export function stddev(xs: number[]): number | null {
  const a = xs.filter(Number.isFinite);
  if (a.length < 2) return null;
  const m = mean(a) as number;
  return Math.sqrt(a.reduce((s, x) => s + (x - m) * (x - m), 0) / (a.length - 1));
}

export function abThreshold(base: number | null, aaDiffs: number[], o: { floorMs?: number; floorPct?: number; sigmaK?: number } = {}) {
  const floorMs = o.floorMs ?? PROTOCOL.floorMs, floorPct = o.floorPct ?? PROTOCOL.floorPct, k = o.sigmaK ?? PROTOCOL.sigmaK;
  const sigma = stddev(aaDiffs);
  const parts = { floorMs, pct: Math.abs(base ?? 0) * floorPct / 100, sigma2: sigma === null ? 0 : k * sigma };
  return { threshold: round(Math.max(parts.floorMs, parts.pct, parts.sigma2), 4) as number, sigma: round(sigma, 4), parts: { floorMs, pct: round(parts.pct, 4), sigma2: round(parts.sigma2, 4) }, aaPairs: aaDiffs.length };
}

export type ClockRound = { index: number; clockA?: number; clockB?: number };
/** ref からの差、またはペアの相手との差が pct% 以上の回を捨てる。 */
export function dropByClock(rounds: ClockRound[], pct = PROTOCOL.clockDropPct, refMHz: number | null = null) {
  const all = rounds.flatMap((r) => [r.clockA, r.clockB]).filter((x): x is number => typeof x === "number" && Number.isFinite(x));
  const ref = refMHz !== null && Number.isFinite(refMHz) ? refMHz : median(all);
  const kept: ClockRound[] = [], dropped: { index: number; reason: string; clockA?: number; clockB?: number }[] = [];
  for (const r of rounds) {
    const cs = [r.clockA, r.clockB].filter((x) => x !== undefined) as number[];
    let why: string | null = null;
    if (cs.some((c) => !Number.isFinite(c))) why = "クロックを取得できなかった";
    else {
      if (ref && cs.some((c) => Math.abs(c - ref) / ref * 100 >= pct)) why = `全体の中央値 ${Math.round(ref)}MHz から ${pct}% 以上`;
      if (!why && cs.length === 2 && Math.abs(cs[0] - cs[1]) / Math.max(cs[0], cs[1]) * 100 >= pct) why = `A と B のクロックが ${pct}% 以上違う`;
    }
    if (why) dropped.push({ index: r.index, reason: why, clockA: r.clockA, clockB: r.clockB }); else kept.push(r);
  }
  return { refMHz: ref === null ? null : Math.round(ref), kept, dropped };
}

export type ABVerdict = "within" | "b-slower" | "b-faster" | "insufficient";
export function compareAB(aVals: number[], bVals: number[], aaDiffs: number[]) {
  const a = median(aVals), b = median(bVals);
  const n = Math.min(aVals.length, bVals.length);
  if (a === null || b === null) return { aMedian: round(a, 4), bMedian: round(b, 4), n, verdict: "insufficient" as ABVerdict };
  const delta = b - a;
  const th = abThreshold(a, aaDiffs);
  const verdict: ABVerdict = Math.abs(delta) <= th.threshold ? "within" : delta > 0 ? "b-slower" : "b-faster";
  return { aMedian: round(a, 4), bMedian: round(b, 4), delta: round(delta, 4), deltaPct: a ? round(delta / a * 100, 3) : null, ...th, n, verdict };
}

/** ABAB… の順序。 */
export function ababSchedule(rounds: number): { round: number; side: "A" | "B" }[] {
  const s: { round: number; side: "A" | "B" }[] = [];
  for (let i = 0; i < rounds; i++) s.push({ round: i, side: "A" }, { round: i, side: "B" });
  return s;
}

/** VRAM の門。Budget は OS が他プロセスの使用量を引いた後の値なので、二重に引かない(othersUsedMB は記録用)。 */
export function vramGate(budgetMB: number | null | undefined, othersUsedMB: number | null | undefined, minMB: number) {
  const others = typeof othersUsedMB === "number" && Number.isFinite(othersUsedMB) ? Math.round(othersUsedMB) : null;
  if (typeof budgetMB !== "number" || !Number.isFinite(budgetMB) || budgetMB <= 0) return { ok: false, reason: "VRAM の予算(Budget)を取得できない", budgetMB: null, othersUsedMB: others, minMB };
  const ok = budgetMB >= minMB;
  return { ok, reason: ok ? null : `Budget ${Math.round(budgetMB)}MB < ${minMB}MB`, budgetMB: Math.round(budgetMB), othersUsedMB: others, minMB };
}

// ── 1 回の窓の読み取り ─────────────────────────────────────────────────────────
export type Window = {
  frameMsAvg: number | null; frameMsP95: number | null; gpuTotalMs: number | null;
  leafMs: Record<string, number>;
  cpu: { gameThreadMs: number | null; renderRecordMs: number | null; editorUiMs: number | null };
  vramUsedMB: number | null; vramBudgetMB: number | null;
  upscaler: { mode: string | null; effective: string | null; renderResolution: [number, number] | null };
  dred: boolean | null;
};
const num = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);
const sumOf = (o: any, keys: string[]): number | null => { if (!o) return null; let s = 0, any = false; for (const k of keys) { const v = num(o[k]); if (v !== null) { s += v; any = true; } } return any ? s : null; };

/** benchmark と perf_stats の応答から窓を作る。 */
export function readWindow(bench: any, stats: any, ping: any): Window {
  const rr = stats?.renderResolution;
  return {
    frameMsAvg: num(bench?.frameMs?.avg), frameMsP95: num(bench?.frameMs?.p95),
    gpuTotalMs: num(stats?.gpuPassMs?.total ?? stats?.gpuPassMs?.Total ?? bench?.gpuPassMs?.total),
    leafMs: Object.fromEntries(Object.entries<any>(stats?.gpuLeafMs ?? {}).filter(([, v]) => typeof v === "number")),
    cpu: {
      gameThreadMs: sumOf(stats?.cpuScopeMs, ["update", "buildList", "listSort", "lights"]),
      renderRecordMs: sumOf(stats?.cpuScopeMs, ["shadowRec", "mainRec", "prepass", "rtFeed", "rtBuild"]),
      editorUiMs: num(stats?.cpuScopeMs?.editorUi),
    },
    vramUsedMB: num(ping?.vramUsedMB) !== null && ping.vramUsedMB >= 0 ? ping.vramUsedMB : null,
    vramBudgetMB: num(ping?.vramBudgetMB) !== null && ping.vramBudgetMB > 0 ? ping.vramBudgetMB : null,
    upscaler: {
      mode: stats?.upscaler?.mode ?? stats?.upscaler?.requested ?? null,
      effective: stats?.upscaler?.effective ?? null,
      renderResolution: rr && typeof rr.width === "number" ? [rr.width, rr.height] : null,
    },
    dred: typeof stats?.dred === "boolean" ? stats.dred : null,
  };
}

/** 窓の集合 → 代表値(中央値)。CPU の p95 は窓の平均の p95(エンジンはフレームごとの値を返さないため)。 */
export function summarizeWindows(ws: Window[]) {
  const col = (f: (w: Window) => number | null) => ws.map(f).filter((x): x is number => x !== null);
  const leafNames = [...new Set(ws.flatMap((w) => Object.keys(w.leafMs)))];
  const p95 = (xs: number[]) => quantile(xs, 0.95);
  return {
    windows: ws.length,
    frameMsAvg: median(col((w) => w.frameMsAvg)),
    frameMsP95: median(col((w) => w.frameMsP95)),
    frameMsP99: ws.length >= 3 ? p95(col((w) => w.frameMsP95)) : null,
    gpuTotalMs: median(col((w) => w.gpuTotalMs)),
    leafMs: Object.fromEntries(leafNames.map((n) => [n, median(ws.map((w) => w.leafMs[n]).filter((x) => typeof x === "number"))] as [string, number | null]).filter(([, v]) => v !== null)) as Record<string, number>,
    cpu: {
      gameThreadP95Ms: p95(col((w) => w.cpu.gameThreadMs)),
      renderRecordP95Ms: p95(col((w) => w.cpu.renderRecordMs)),
      editorUiP95Ms: p95(col((w) => w.cpu.editorUiMs)),
    },
    vramUsedMB: median(col((w) => w.vramUsedMB)),
    vramBudgetMB: median(col((w) => w.vramBudgetMB)),
  };
}

// ── アップスケーラ列の設定 ─────────────────────────────────────────────────────
/** 列 → エンジンの呼び出し列。renderScale は予算 JSON の値から決める(ここに数値を書かない)。DLSS の品質は renderScale に最も近いもの。 */
export function upscalerSetup(column: "dlss" | "taau" | "taaNative", renderScale: number): { method: string; params: Record<string, unknown> }[] {
  if (column === "dlss") {
    const table: [string, number][] = [["dlaa", 1], ["quality", 2 / 3], ["balanced", 0.58], ["performance", 0.5]];
    const q = table.reduce((a, b) => (Math.abs(b[1] - renderScale) < Math.abs(a[1] - renderScale) ? b : a))[0];
    return [{ method: "set_upscaler", params: { mode: "dlss", quality: q } }];
  }
  return [{ method: "set_upscaler", params: { mode: "taa" } }, { method: "set_render_scale", params: { scale: renderScale } }];
}

/** 列の設定が実際に効いたか(DLSS が使えず TAA に落ちた等を検出する)。 */
export function columnActive(column: "dlss" | "taau" | "taaNative", up: any): { ok: boolean; reason?: string } {
  const eff = String(up?.effective ?? up?.mode ?? "").toLowerCase();
  if (column === "dlss") return eff.includes("dlss") ? { ok: true } : { ok: false, reason: `DLSS が有効にならなかった(effective=${eff || "不明"}${up?.reason ? `, ${up.reason}` : ""}${up?.fallbackReason ? `, ${up.fallbackReason}` : ""})` };
  return eff.includes("dlss") ? { ok: false, reason: "TAA 列なのに DLSS が動いている" } : { ok: true };
}

// ── 履歴と回帰 ────────────────────────────────────────────────────────────────
export type HistoryEntry = { at: string; key: string; preset: string; column: string; pose: string; frameMsP95: number | null; gpuTotalMs: number | null; pass?: boolean };
export function historyFile(dir: string): string { return path.join(dir, "perf-history.jsonl"); }
export function readHistory(dir: string): HistoryEntry[] { return readJsonl<HistoryEntry>(historyFile(dir), 5000); }
export function appendHistory(dir: string, e: HistoryEntry) { appendJsonl(historyFile(dir), e); }
export const historyKey = (preset: string, column: string, pose: string) => `${preset}|${column}|${pose}`;

/** 直近 N 回の中央値より pct% 以上悪化したら regression。履歴が 1 件も無ければ insufficient。 */
export function detectRegression(history: HistoryEntry[], key: string, current: { frameMsP95: number | null; gpuTotalMs: number | null }, o: { n?: number; pct?: number } = {}) {
  const n = o.n ?? PROTOCOL.historyN, pct = o.pct ?? PROTOCOL.regressionPct;
  const past = history.filter((h) => h.key === key).slice(-n);
  if (!past.length) return { status: "insufficient" as const, compared: 0, metrics: [] as any[] };
  const metrics = (["frameMsP95", "gpuTotalMs"] as const).map((m) => {
    const base = median(past.map((h) => h[m]).filter((x): x is number => typeof x === "number"));
    const cur = current[m];
    if (base === null || cur === null || base <= 0) return { metric: m, baseline: base, current: cur, deltaPct: null, regression: false };
    const d = (cur - base) / base * 100;
    return { metric: m, baseline: round(base, 4), current: round(cur, 4), deltaPct: round(d, 2), regression: d >= pct };
  });
  return { status: metrics.some((m) => m.regression) ? ("regression" as const) : ("ok" as const), compared: past.length, metrics };
}

// ── 計測窓のロック(%TEMP%\uno-gpu-window.lock。書式は tools/scorecard/baseline_perf.mjs と共有: JSON {pid, owner, startedAt}) ──
export const LOCK_FILE = path.join(os.tmpdir(), "uno-gpu-window.lock");
const pidAlive = (pid: number) => { try { process.kill(pid, 0); return true; } catch (e: any) { return e.code === "EPERM"; } };
function stealStale(file: string) {
  const moved = `${file}.stale-${process.pid}-${Date.now()}`;
  try { fs.renameSync(file, moved); } catch { return; }
  let h: any = null;
  try { h = JSON.parse(fs.readFileSync(moved, "utf8")); } catch { /* 壊れ = 古い */ }
  if (h && Number.isInteger(h.pid) && pidAlive(h.pid)) { try { fs.linkSync(moved, file); } catch { /* 別のロックが既にある */ } }
  try { fs.rmSync(moved, { force: true }); } catch { /* 無視 */ }
}
export async function acquireGpuWindow(o: { file?: string; waitSec?: number; owner?: string; pollMs?: number; signal?: AbortSignal } = {}): Promise<(() => void) | null> {
  const file = o.file ?? LOCK_FILE, waitSec = o.waitSec ?? 0, pollMs = o.pollMs ?? 2000;
  const t0 = Date.now();
  for (;;) {
    try {
      fs.writeFileSync(file, JSON.stringify({ pid: process.pid, owner: o.owner ?? "perf_gate", startedAt: new Date().toISOString() }), { flag: "wx" });
      let released = false;
      return () => { if (released) return; released = true; try { fs.rmSync(file, { force: true }); } catch { /* 無視 */ } };
    } catch (e: any) {
      if (e.code !== "EEXIST") throw e;
      let holder: any = null;
      try { holder = JSON.parse(fs.readFileSync(file, "utf8")); } catch { /* 書き込み途中か壊れ */ }
      if (holder && Number.isInteger(holder.pid) && !pidAlive(holder.pid)) { stealStale(file); continue; }
      if (o.signal?.aborted || Date.now() - t0 >= waitSec * 1000) return null;
      await sleep(pollMs);
    }
  }
}

// ── クロックの記録(nvidia-smi。無ければサンプル無しで進む) ──────────────────────
export type ClockSample = { smMHz: number; tempC: number; memUsedMB?: number };
export type ClockPoller = { stop(): Promise<ClockSample[]> };
export function startClockPoller(everyMs = 1000): ClockPoller {
  const samples: ClockSample[] = [];
  let child: ReturnType<typeof spawn> | null = null;
  try {
    child = spawn("nvidia-smi", ["--query-gpu=clocks.sm,temperature.gpu,memory.used", "--format=csv,noheader,nounits", "-lms", String(everyMs)], { windowsHide: true, stdio: ["ignore", "pipe", "ignore"] });
    child.on("error", () => { /* nvidia-smi が無い */ });
    let buf = "";
    child.stdout?.on("data", (d) => {
      buf += d.toString();
      let i: number;
      while ((i = buf.indexOf("\n")) >= 0) {
        const p = buf.slice(0, i).split(",").map((x) => Number(x.trim())); buf = buf.slice(i + 1);
        if (p.length >= 2 && Number.isFinite(p[0])) samples.push({ smMHz: p[0], tempC: p[1], memUsedMB: p[2] });
      }
    });
  } catch { /* 無視 */ }
  return { async stop() { try { child?.kill(); } catch { /* 無視 */ } return samples; } };
}
/** 負荷が掛かっているサンプルだけで代表値を出す(アイドルの低いクロックを混ぜない)。 */
export function clockStats(samples: ClockSample[]) {
  const sm = samples.map((s) => s.smMHz).filter(Number.isFinite);
  if (!sm.length) return { smMHz: null as number | null, smMin: null as number | null, smMax: null as number | null, tempC: null as number | null, n: 0 };
  const top = Math.max(...sm);
  const busy = sm.filter((c) => c >= top * 0.5);
  return { smMHz: Math.round(mean(busy) as number), smMin: Math.min(...busy), smMax: Math.max(...busy), tempC: median(samples.map((s) => s.tempC)), n: busy.length };
}
