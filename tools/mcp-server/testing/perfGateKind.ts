// ジョブ種別 perf_gate(MCP-01)。予算は testing/budgets/*.json を読むだけ(数値を TS に書かない)。
//   mode:"gate"(既定)… 1 つのエンジンで列(dlss / taau / taaNative)ごとに測り、gate 列だけを合否にする。他の列は記録だけ。
//   mode:"ab"        … 2 つのビルドを ABAB… に交互に測り(A/A も取り)、閾値 max(床, 1%, A/A の 2σ)で比べる。
// 手順(姿勢ごと): set_view_size → (あれば)set_quality_preset → step_frames → benchmark(uncap) → perf_stats(window)。これを repeats 回回して中央値。
// 守ること: 計測窓のロック(%TEMP%\uno-gpu-window.lock)→ VRAM の前提確認 → エンジンは --no-dred で起動 → クロックを記録して 3% 以上ずれた回は捨てる。
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { z } from "zod";
import { jobFail, JobFailure, type InprocCtx, type InprocOutcome, type KindDef } from "../jobs/manager.ts";
import type { KindEnv } from "../jobs/env.ts";
import { Aborted, bodyFromThrown, homeDir, median, projectKeyOf, raceAbort, round, sleep, stamp, type EngineCall } from "./common.ts";
import { defaultProvider, type EngineHandle, type EngineProvider } from "./engineProvider.ts";
import {
  BudgetError, COLUMN_IDS, degradeAdvice, evaluateColumn, gateColumnOf, loadBudget, precheckMB, type Budget, type ColumnId, type Measured,
} from "./budgets.ts";
import {
  PROTOCOL, ababSchedule, acquireGpuWindow, appendHistory, clockStats, columnActive, compareAB, detectRegression, dropByClock, historyKey, readHistory,
  readWindow, startClockPoller, summarizeWindows, upscalerSetup, vramGate, type ClockPoller, type Window,
} from "./perfCore.ts";
import { openSceneWait } from "./visualCore.ts";

const pose = z.object({ name: z.string().optional(), position: z.array(z.number()).length(3).optional(), target: z.array(z.number()).length(3).optional() });
const side = z.object({ buildDir: z.string().describe("DX12Engine.exe のあるフォルダ。"), label: z.string().optional(), args: z.array(z.string()).max(8).optional() });
const shape = {
  preset: z.string().optional().describe("予算(testing/budgets の名前。既定 epic_1080p60)。"),
  mode: z.enum(["gate", "ab"]).optional().describe("gate = 予算に当てる(既定)/ ab = 2 ビルドの ABAB 比較(a と b が要る)。"),
  columns: z.array(z.enum(["dlss", "taau", "taaNative"])).optional().describe("測る列(既定: 予算にある全列。合否は gate 列だけ、ほかは記録)。"),
  poses: z.array(pose).max(12).optional().describe("姿勢(カメラ)。省略でいまのカメラ 1 姿勢。"),
  scene: z.string().optional().describe("計測前に開くシーン(assets 相対)。"),
  project: z.string().optional().describe("専用エンジンで開くプロジェクト(省略で束縛エンジン、または tests/golden/projects/golden_core の複製)。"),
  launch: z.boolean().optional().describe("専用エンジンを --no-dred で起動して測る(既定。engine を渡したときだけ束縛エンジンに付く。false で束縛エンジン。付いたエンジンの DRED が ON なら何も変えず中止し、終わりに upscaler などを戻す)。"),
  engine: z.string().optional().describe("束縛するエンジン(id / name / port)。DRED が ON なら中止する。"),
  buildDir: z.string().optional().describe("launch 時の DX12Engine.exe のフォルダ。"),
  repeats: z.number().int().min(1).max(9).optional().describe("姿勢ごとの繰り返し(中央値を取る。既定 3)。"),
  benchFrames: z.number().int().min(30).max(3600).optional().describe("benchmark のフレーム数(既定 600)。"),
  warmupFrames: z.number().int().min(0).max(3600).optional().describe("温めのフレーム数(既定 180)。"),
  statsWindow: z.number().int().min(1).max(1000).optional().describe("perf_stats の窓(既定 240)。"),
  rounds: z.number().int().min(1).max(10).optional().describe("ab: ABAB の往復数(既定 5)。"),
  a: side.optional().describe("ab: A のビルド。"),
  b: side.optional().describe("ab: B のビルド。"),
  waitLockSec: z.number().min(0).max(7200).optional().describe("計測窓のロックを待つ秒数(既定 0 = 取れなければ中止)。"),
  qualityPreset: z.string().optional().describe("set_quality_preset に渡す名前(あれば。省略で preset 名の先頭の語)。"),
  record: z.boolean().optional().describe("履歴(perf-history.jsonl)に追記する(既定 true)。"),
};

export type PerfDeps = {
  provider: EngineProvider;
  clock?: () => ClockPoller;
  lockFile?: string;
  historyDir?: string;
  repoDir: string;
  /** テスト用: 待ちを短くする。 */
  sleepMs?: number;
};

/** 結果の指標の定義(PP-NB1 の「GPU p95/p99」との差を隠さない)。 */
export const METRIC_DEFINITIONS = Object.freeze({
  frameMsP95: "benchmark の CPU 壁時計フレーム時間の p95(姿勢ごと・窓の中央値)。GPU の p95 ではない",
  frameMsP99: "窓ごとの p95 を並べた p95(厳密な p99 ではない)。予算の p99Ms と比べる近似",
  gpuTotalMs: "perf_stats.gpuPassMs.total の窓平均(GPU の p95 ではない)",
  cpu: "perf_stats の窓平均を窓ごとに並べた p95 の近似",
});

type PoseSpec = { name?: string; position?: number[]; target?: number[] };
const poseName = (p: PoseSpec, i: number) => p.name ?? (p.position ? `pose${i + 1}` : "current");

async function setupColumn(call: EngineCall, budget: Budget, col: ColumnId) {
  for (const s of upscalerSetup(col, budget.columns[col]!.renderScale)) await call(s.method, s.params, { timeout: 15_000 });
  await call("step_frames", { frames: 3 }, { timeout: 60_000 });
  const up = await call("get_upscaler", {}, { timeout: 8000 });
  return { up, active: columnActive(col, up) };
}

type Proc = { frames: number; warmup: number; window: number; repeats: number };

/** 1 姿勢を repeats 回測る。クロックを記録し、ずれた回は捨てる(全部ずれたら捨てずに警告)。 */
export async function measurePose(call: EngineCall, ctx: Pick<InprocCtx, "signal" | "log">, deps: PerfDeps, p: PoseSpec, proc: Proc) {
  if (p.position) await call("set_editor_camera", { position: p.position, ...(p.target ? { target: p.target } : {}) });
  if (proc.warmup > 0) await raceAbort(call("step_frames", { frames: proc.warmup }, { timeout: Math.max(60_000, proc.warmup * 200 + 30_000) }), ctx.signal, async () => { await call("cancel", { target: "step_frames" }, { timeout: 4000, retry: false }); });
  const rounds: { w: Window; clock: ReturnType<typeof clockStats> }[] = [];
  for (let r = 0; r < proc.repeats; r++) {
    const poller = (deps.clock ?? (() => startClockPoller()))();
    let bench: any, stats: any, ping: any;
    try {
      bench = await raceAbort(call("benchmark", { frames: proc.frames, uncap: true }, { timeout: Math.max(60_000, proc.frames * 60 + 30_000) }), ctx.signal, async () => { await call("cancel", { target: "benchmark" }, { timeout: 4000, retry: false }); });
      stats = await call("perf_stats", { window: proc.window }, { timeout: 30_000 });
      ping = await call("ping", {}, { timeout: 8000 });
    } finally { rounds.push({ w: null as any, clock: clockStats(await poller.stop()) }); }
    rounds[rounds.length - 1].w = readWindow(bench, stats, ping);
    ctx.log(`  run ${r + 1}/${proc.repeats}: avg=${rounds[r].w.frameMsAvg} p95=${rounds[r].w.frameMsP95} gpu=${rounds[r].w.gpuTotalMs} clock=${rounds[r].clock.smMHz ?? "?"}MHz`);
  }
  const drop = dropByClock(rounds.map((r, i) => ({ index: i, clockA: r.clock.smMHz ?? NaN })));
  const keptIdx = new Set(drop.kept.map((k) => k.index));
  const useAll = keptIdx.size === 0;   // 全部ずれたら捨てず、警告つきで全部使う
  const used = rounds.filter((_, i) => useAll || keptIdx.has(i)).map((r) => r.w);
  const sigma = (() => { const v = used.map((w) => w.frameMsAvg).filter((x): x is number => x !== null); if (v.length < 2) return null; const m = v.reduce((a, b) => a + b, 0) / v.length; return Math.sqrt(v.reduce((s, x) => s + (x - m) ** 2, 0) / (v.length - 1)); })();
  return {
    summary: summarizeWindows(used), windows: used,
    clock: { refMHz: drop.refMHz, perRun: rounds.map((r) => ({ smMHz: r.clock.smMHz, tempC: r.clock.tempC })), dropped: drop.dropped, allDropped: useAll && rounds.length > 0 },
    repeatSigmaMs: round(sigma, 4),
    dred: used.map((w) => w.dred).find((d) => d !== null) ?? null,
  };
}

function toMeasured(s: ReturnType<typeof summarizeWindows>): Measured {
  return { frameMsP95: s.frameMsP95, frameMsP99: s.frameMsP99, gpuTotalMs: s.gpuTotalMs, leafMs: s.leafMs, cpu: s.cpu, vramUsedMB: s.vramUsedMB, vramBudgetMB: s.vramBudgetMB };
}

export async function runGate(a: Record<string, any>, deps: PerfDeps, ctx: InprocCtx, env: KindEnv) {
  const presetName: string = a.preset ?? "epic_1080p60";
  const proc: Proc = { frames: a.benchFrames ?? PROTOCOL.benchFrames, warmup: a.warmupFrames ?? PROTOCOL.warmupFrames, window: a.statsWindow ?? PROTOCOL.statsWindow, repeats: a.repeats ?? PROTOCOL.repeats };
  const poses: PoseSpec[] = a.poses?.length ? a.poses : [{}];

  const release = await acquireGpuWindow({ file: deps.lockFile, waitSec: a.waitLockSec ?? 0, owner: "perf_gate", signal: ctx.signal });
  if (!release) jobFail({ code: "E_ENGINE_BUSY", message: "計測窓のロックを取れない(他の計測が走っている)", retryable: true, cause: "%TEMP%\\uno-gpu-window.lock を別のプロセスが持っている。GPU の計測は同時に 1 つだけ", fix: [{ tool: "dx12_job_start", args: { kind: "perf_gate", args: { ...a, waitLockSec: 600 } }, why: "ロックが空くのを待つ" }] });
  let h: EngineHandle | null = null;
  let restore: (() => Promise<unknown>) | null = null;
  const tmpProject: string[] = [];
  try {
    // 既定は専用エンジンを --no-dred で起動する。束縛エンジン(DRED ON のエディタ)に付くのは launch:false か engine 指定のときだけ。
    const launching = a.launch === true || (a.launch !== false && !a.engine);
    if (launching) {
      const proj = a.project ?? path.join(deps.repoDir, "tests", "golden", "projects", "golden_core");
      if (!fs.existsSync(proj)) jobFail({ code: "E_NOT_FOUND", message: `プロジェクトが無い: ${proj}`, retryable: false });
      const copy = fs.mkdtempSync(path.join(os.tmpdir(), "uno-perf-proj-")); tmpProject.push(copy);
      fs.cpSync(proj, copy, { recursive: true });
      ctx.progress({ phase: "launching", pct: 2, message: "専用エンジンを起動(--no-dred)" });
      h = await deps.provider.launch({ name: `perfgate-${ctx.job.id.slice(-6)}`, project: copy, mode: "headless", buildDir: a.buildDir ?? env.cfg.buildDir, extraArgs: ["--no-dred"] });
    } else {
      h = deps.provider.attach(a.engine);
      if (h.id) ctx.setEngine(h.id);
      // 何も変える前に DRED を確かめる(ON のエンジンの設定を触らない)。変える設定は終わりに戻す。
      const pre = await h.call("perf_stats", { window: 1 }, { timeout: 30_000 }).catch(() => null);
      if (pre?.dred === true) jobFail({ code: "E_ENGINE_BUSY", message: "perf_gate: DRED が ON のエンジンでは測れない(--no-dred で起動したエンジンだけ比べられる)。エンジンの設定は変えていない", retryable: false, fix: [{ tool: "dx12_job_start", args: { kind: "perf_gate", args: { ...a, launch: true, engine: undefined } }, why: "専用エンジンを --no-dred で起動して測る" }] });
      const hc = h.call;
      const before: Record<string, any> = {};
      for (const m of ["get_upscaler", "get_render_scale", "get_quality_preset"]) before[m] = await hc(m, {}, { timeout: 8000 }).catch(() => null);
      restore = async () => {
        const up = before.get_upscaler, rs = before.get_render_scale, qp = before.get_quality_preset;
        const tryCall = (m: string, p: any) => hc(m, p, { timeout: 15_000 }).catch(() => null);
        if (qp && typeof (qp.preset ?? qp.name) === "string") await tryCall("set_quality_preset", { preset: qp.preset ?? qp.name });
        if (up && typeof up.mode === "string") await tryCall("set_upscaler", { mode: up.mode, ...(typeof up.quality === "string" ? { quality: up.quality } : {}) });
        if (rs && typeof (rs.scale ?? rs.renderScale) === "number") await tryCall("set_render_scale", { scale: rs.scale ?? rs.renderScale });
      };
    }
    const call = h.call;
    ctx.progress({ phase: "preparing", pct: 3, message: "エンジンへ接続" });
    const ping0 = await call("ping", {}, { timeout: 8000 });
    let budget: Budget;
    try { budget = loadBudget(presetName, { projectDir: ping0?.baseDir ?? null }); }
    catch (e) { if (e instanceof BudgetError) return jobFail({ code: e.code, message: e.message, retryable: false, ...(e.validValues ? { details: { validValues: e.validValues } } : {}) }); throw e; }

    // VRAM の前提確認(Budget は他プロセスの使用量が引かれた後の値)
    const gate = vramGate(ping0?.vramBudgetMB, ping0?.vramUsedMB, precheckMB(budget));
    if (!gate.ok) jobFail({ code: "E_FLEET_RESOURCE", message: `perf_gate: VRAM の余裕が足りないので計測を中止した(${gate.reason})`, retryable: true, details: { vramCheck: gate }, cause: "他のプロセス(ゲーム・ビルドなど)が VRAM を使っている間は、予算の判定が歪むので測らない" });

    await call("set_view_size", { width: budget.output[0], height: budget.output[1] }, { timeout: 15_000 }).catch(() => {});
    const qp = a.qualityPreset ?? presetName.split("_")[0];
    let qualityPreset: string | null = null;
    try { await call("set_quality_preset", { preset: qp }, { timeout: 15_000 }); qualityPreset = qp; } catch { /* W1-C3 より前のエンジンには無い */ }
    if (a.scene) await openSceneWait(call, a.scene, ctx.signal);

    const cols: ColumnId[] = (a.columns?.length ? a.columns : COLUMN_IDS.filter((c) => budget.columns[c])) as ColumnId[];
    for (const c of cols) if (!budget.columns[c]) jobFail({ code: "E_INVALID_PARAM", message: `予算 ${budget.preset} に列 ${c} が無い`, retryable: false, details: { validValues: Object.keys(budget.columns) } });
    const hist = deps.historyDir ?? path.join(homeDir("DX12_PERF_HOME", "perf"), projectKeyOf(ping0?.baseDir));
    const history = readHistory(hist);
    const out: Record<string, any> = {};
    const incomplete: string[] = [];
    let dredState: boolean | null = null;
    const total = cols.length * poses.length;
    let n = 0;
    for (const col of cols) {
      const c = budget.columns[col]!;
      const colRes: any = { gate: c.gate, gating: c.gate === "gate", renderScale: c.renderScale, totalMsBudget: c.totalMs, poses: {} };
      out[col] = colRes;
      ctx.progress({ phase: "setup", pct: Math.round(5 + (n * 90) / total), message: `列 ${col} を設定` });
      const { up, active } = await setupColumn(call, budget, col);
      colRes.upscaler = { mode: up?.mode ?? null, effective: up?.effective ?? null, reason: up?.reason ?? up?.fallbackReason ?? null };
      if (!active.ok) { colRes.status = "unavailable"; colRes.reason = active.reason; colRes.pass = c.gate === "gate" ? null : undefined; n += poses.length; continue; }
      let colPass = true, colIncomplete = false;
      for (let i = 0; i < poses.length; i++) {
        const name = poseName(poses[i], i);
        ctx.progress({ phase: "measuring", pct: Math.round(5 + (n * 90) / total), message: `列 ${col} / 姿勢 ${name}(${proc.repeats} 回 × ${proc.frames} フレーム)` });
        ctx.log(`${col} / ${name}`);
        const m = await measurePose(call, ctx, deps, poses[i], proc);
        if (m.dred === true) jobFail({ code: "E_ENGINE_BUSY", message: "perf_gate: DRED が ON のエンジンでは測れない(--no-dred で起動したエンジンだけ比べられる)", retryable: false, fix: [{ tool: "dx12_job_start", args: { kind: "perf_gate", args: { ...a, launch: true, engine: undefined } }, why: "専用エンジンを --no-dred で起動して測る" }] });
        dredState = m.dred;
        const ev = evaluateColumn(budget, col, toMeasured(m.summary));
        const key = historyKey(budget.preset, col, name);
        const reg = detectRegression(history, key, { frameMsP95: m.summary.frameMsP95, gpuTotalMs: m.summary.gpuTotalMs });
        colRes.poses[name] = {
          measured: { frameMsAvg: round(m.summary.frameMsAvg, 3), frameMsP95: round(m.summary.frameMsP95, 3), gpuTotalMs: round(m.summary.gpuTotalMs, 3), cpu: m.summary.cpu, vramUsedMB: m.summary.vramUsedMB, windows: m.summary.windows },
          checks: ev.checks, pass: ev.pass, complete: ev.complete, missing: ev.missing, failed: ev.failed.map((f) => f.id), unmeasured: ev.unmeasured.map((f) => f.id),
          degrade: degradeAdvice(budget, col, ev.failed), regression: reg, clock: m.clock, repeatSigmaMs: m.repeatSigmaMs,
        };
        if (!ev.pass) colPass = false;
        if (!ev.complete) { colIncomplete = true; if (c.gate === "gate") incomplete.push(`${col}/${name}: ${ev.missing.join(", ")}`); }
        if (a.record !== false) appendHistory(hist, { at: new Date().toISOString(), key, preset: budget.preset, column: col, pose: name, frameMsP95: m.summary.frameMsP95, gpuTotalMs: m.summary.gpuTotalMs, pass: ev.pass });
        n++;
      }
      colRes.status = "measured"; colRes.pass = colPass; colRes.complete = !colIncomplete;
    }
    const gateCol = gateColumnOf(budget);
    const gated = gateCol && out[gateCol] ? out[gateCol] : null;
    const regsOf = (c: any) => Object.entries<any>(c?.poses ?? {}).filter(([, p]) => p.regression?.status === "regression").map(([k]) => k);
    // 回帰で判定に効くのは gate 列だけ。ほかの列(記録のみ)の悪化は info として報告するだけ。
    const regs = gated ? regsOf(gated) : [];
    const regsInfo = Object.entries<any>(out).filter(([k]) => k !== gateCol).map(([k, c]) => ({ column: k, poses: regsOf(c) })).filter((x) => x.poses.length);
    let verdict: "pass" | "fail" | "inconclusive" | "regression";
    if (!gated || gated.status === "unavailable" || gated.status !== "measured") verdict = "inconclusive";
    else if (!gated.pass) verdict = "fail";
    else if (incomplete.length) verdict = "inconclusive";   // 指標が測れていない合格は合格にしない
    else if (regs.length) verdict = "regression";
    else verdict = "pass";
    const summary = {
      preset: budget.preset, mode: "gate", verdict, gateColumn: gateCol, overridden: budget.overridden ?? [], qualityPreset, noDred: dredState === false ? true : dredState,
      vramCheck: gate, columns: Object.fromEntries(Object.entries<any>(out).map(([k, v]) => [k, { gate: v.gate, status: v.status, pass: v.pass ?? null, reason: v.reason ?? null, poses: Object.fromEntries(Object.entries<any>(v.poses).map(([pk, pv]) => [pk, { pass: pv.pass, failed: pv.failed, frameMsP95: pv.measured.frameMsP95, gpuTotalMs: pv.measured.gpuTotalMs, regression: pv.regression.status }])) }])),
      regressions: regs, regressionsInfo: regsInfo, ...(incomplete.length ? { unmeasuredInGateColumn: incomplete } : {}),
      metricDefinitions: METRIC_DEFINITIONS,
    };
    const file = path.join(ctx.artifactsDir, "perf_gate.json");
    fs.mkdirSync(ctx.artifactsDir, { recursive: true });
    fs.writeFileSync(file, JSON.stringify({ summary, budget: { preset: budget.preset, p95Ms: budget.p95Ms, p99Ms: budget.p99Ms, output: budget.output }, detail: out }, null, 2));
    ctx.addArtifact({ path: file, kind: "manifest" });
    return { summary, result: { summary, detail: out }, verdict };
  } finally {
    try { await (restore as (() => Promise<unknown>) | null)?.(); } catch { /* 戻せなくても結果は返す */ }
    await h?.close();
    release();
    for (const d of tmpProject) { try { fs.rmSync(d, { recursive: true, force: true }); } catch { /* 無視 */ } }
  }
}

/** A/B。ビルドごとに専用エンジンを起動して測る(--no-dred)。ABAB…の後に A/A を同じ回数取り、閾値を決める。 */
export async function runAB(a: Record<string, any>, deps: PerfDeps, ctx: InprocCtx, env: KindEnv) {
  if (!a.a || !a.b) jobFail({ code: "E_MISSING_PARAM", message: "perf_gate {mode:'ab'} には a と b(buildDir)が要る", retryable: false });
  const presetName: string = a.preset ?? "epic_1080p60";
  const budget = loadBudget(presetName);
  const col: ColumnId = (a.columns?.[0] ?? gateColumnOf(budget) ?? "dlss") as ColumnId;
  const proc: Proc = { frames: a.benchFrames ?? PROTOCOL.benchFrames, warmup: a.warmupFrames ?? PROTOCOL.warmupFrames, window: a.statsWindow ?? PROTOCOL.statsWindow, repeats: a.repeats ?? 1 };
  const poses: PoseSpec[] = a.poses?.length ? a.poses : [{}];
  const rounds = a.rounds ?? PROTOCOL.rounds;
  const release = await acquireGpuWindow({ file: deps.lockFile, waitSec: a.waitLockSec ?? 0, owner: "perf_gate:ab", signal: ctx.signal });
  if (!release) jobFail({ code: "E_ENGINE_BUSY", message: "計測窓のロックを取れない(他の計測が走っている)", retryable: true });
  const proj = a.project ?? path.join(deps.repoDir, "tests", "golden", "projects", "golden_core");
  const tmp: string[] = [];
  const oneSession = async (sd: { buildDir: string; args?: string[] }, tag: string) => {
    const copy = fs.mkdtempSync(path.join(os.tmpdir(), "uno-perf-proj-")); tmp.push(copy);
    fs.cpSync(proj, copy, { recursive: true });
    const h = await deps.provider.launch({ name: `perfab-${ctx.job.id.slice(-6)}`, project: copy, mode: "headless", buildDir: sd.buildDir, extraArgs: ["--no-dred", ...(sd.args ?? [])] });
    try {
      const ping = await h.call("ping", {}, { timeout: 8000 });
      const g = vramGate(ping?.vramBudgetMB, ping?.vramUsedMB, precheckMB(budget));
      await h.call("set_view_size", { width: budget.output[0], height: budget.output[1] }, { timeout: 15_000 }).catch(() => {});
      const { active } = await setupColumn(h.call, budget, col);
      if (a.scene) await openSceneWait(h.call, a.scene, ctx.signal);
      const per: Record<string, { avg: number | null; p95: number | null; gpu: number | null }> = {};
      const clocks: number[] = [];
      for (let i = 0; i < poses.length; i++) {
        const m = await measurePose(h.call, ctx, deps, poses[i], proc);
        per[poseName(poses[i], i)] = { avg: m.summary.frameMsAvg, p95: m.summary.frameMsP95, gpu: m.summary.gpuTotalMs };
        if (m.clock.refMHz) clocks.push(m.clock.refMHz);
      }
      return { tag, per, clock: clocks.length ? clocks.reduce((x, y) => x + y, 0) / clocks.length : NaN, vramOk: g.ok, columnOk: active.ok };
    } finally { await h.close(); }
  };
  try {
    const abRounds: { index: number; a: any; b: any }[] = [];
    const sched = ababSchedule(rounds);
    const tmpRound: Record<number, any> = {};
    for (let i = 0; i < sched.length; i++) {
      if (ctx.signal.aborted) throw new Aborted();
      const s = sched[i];
      ctx.progress({ phase: "ab", pct: Math.round((i * 70) / sched.length), message: `AB ${s.round + 1}/${rounds} ${s.side}` });
      const r = await oneSession(s.side === "A" ? a.a : a.b, s.side);
      (tmpRound[s.round] ??= {})[s.side] = r;
    }
    for (let k = 0; k < rounds; k++) abRounds.push({ index: k, a: tmpRound[k].A, b: tmpRound[k].B });
    const aaRounds: { index: number; a: any; b: any }[] = [];
    for (let k = 0; k < rounds; k++) {
      if (ctx.signal.aborted) throw new Aborted();
      ctx.progress({ phase: "aa", pct: 70 + Math.round((k * 25) / rounds), message: `A/A ${k + 1}/${rounds}` });
      const r1 = await oneSession(a.a, "A"); const r2 = await oneSession(a.a, "A");
      aaRounds.push({ index: k, a: r1, b: r2 });
    }
    const all = [...abRounds, ...aaRounds].flatMap((r) => [r.a.clock, r.b.clock]).filter((x) => Number.isFinite(x));
    const ref = median(all);
    const keep = (rs: typeof abRounds) => { const d = dropByClock(rs.map((r) => ({ index: r.index, clockA: r.a.clock, clockB: r.b.clock })), PROTOCOL.clockDropPct, ref); const ks = new Set(d.kept.map((x) => x.index)); return { rs: rs.filter((r) => ks.has(r.index)), dropped: d.dropped }; };
    const abK = keep(abRounds), aaK = keep(aaRounds);
    const metrics: Record<string, any> = {};
    for (const [pk] of Object.entries(abK.rs[0]?.a.per ?? abRounds[0].a.per)) {
      for (const mname of ["avg", "p95", "gpu"] as const) {
        const aaDiffs = aaK.rs.map((r) => (r.b.per[pk][mname] ?? NaN) - (r.a.per[pk][mname] ?? NaN)).filter(Number.isFinite);
        metrics[`${pk}.${mname}`] = compareAB(abK.rs.map((r) => r.a.per[pk][mname]).filter((x: any) => x !== null), abK.rs.map((r) => r.b.per[pk][mname]).filter((x: any) => x !== null), aaDiffs);
      }
    }
    const slower = Object.entries(metrics).filter(([, v]) => v.verdict === "b-slower").map(([k]) => k);
    // 比べてよい条件が揃っているか。揃っていなければ pass にしない(inconclusive)。
    const sessions = [...abRounds, ...aaRounds].flatMap((r) => [r.a, r.b]);
    const reasons: string[] = [];
    if (sessions.some((x) => !x.columnOk)) reasons.push(`列 ${col} が有効にならなかったセッションがある(A/B が別の経路で測られた可能性)`);
    if (sessions.some((x) => !x.vramOk)) reasons.push("VRAM の前提(Budget)を満たさないセッションがある");
    const minKept = Math.max(1, Math.ceil(rounds / 2));
    if (abK.rs.length < minKept) reasons.push(`クロックが揃った ABAB の回が足りない(${abK.rs.length}/${rounds}、最低 ${minKept}。nvidia-smi が無いと全部捨てられる)`);
    if (aaK.rs.length < Math.min(2, rounds)) reasons.push(`クロックが揃った A/A の回が足りない(${aaK.rs.length}/${rounds})`);
    const verdictV = Object.values<any>(metrics);
    if (!verdictV.length || verdictV.every((v) => v.verdict === "insufficient")) reasons.push("比べられた指標が無い(すべて insufficient)");
    const verdictAB = reasons.length ? "inconclusive" : slower.length ? "b-slower" : "within-or-faster";
    const summary = { preset: budget.preset, mode: "ab", column: col, verdict: verdictAB, ...(reasons.length ? { inconclusiveReasons: reasons } : {}), slower, rounds, clockRefMHz: ref === null ? null : Math.round(ref), discarded: { ab: abK.dropped, aa: aaK.dropped }, metrics, metricDefinitions: METRIC_DEFINITIONS };
    const file = path.join(ctx.artifactsDir, "perf_gate_ab.json");
    fs.mkdirSync(ctx.artifactsDir, { recursive: true });
    fs.writeFileSync(file, JSON.stringify(summary, null, 2));
    ctx.addArtifact({ path: file, kind: "manifest" });
    return { summary, result: { summary }, verdict: verdictAB === "inconclusive" ? "inconclusive" : slower.length ? "fail" : "pass" } as any;
  } finally { release(); for (const d of tmp) { try { fs.rmSync(d, { recursive: true, force: true }); } catch { /* 無視 */ } } }
}

export function perfGateKind(env: KindEnv, injected?: Partial<PerfDeps>): KindDef {
  const deps: PerfDeps = { provider: injected?.provider ?? defaultProvider(env), repoDir: injected?.repoDir ?? env.cfg.repoDir, clock: injected?.clock, lockFile: injected?.lockFile, historyDir: injected?.historyDir };
  return {
    kind: "perf_gate", executor: "inproc",
    describe: "性能の合否判定。予算(testing/budgets)に GPU の葉・フレーム p95・CPU p95・VRAM を当てる。合否は gate 列(W1 は DLSS)だけで、TAAU / TAA native は記録。エンジンは --no-dred、計測窓のロックと VRAM の前提確認つき。mode:'ab' は 2 ビルドの ABAB 比較(A/A と閾値つき)。",
    group: () => "engine:perf",
    timeoutSec: env.cfg.timeoutSec.perf_gate, shape,
    normalize(a) { return { preset: "epic_1080p60", mode: "gate", ...a }; },
    async run(a, ctx): Promise<InprocOutcome> {
      try {
        const r: any = a.mode === "ab" ? await runAB(a, deps, ctx, env) : await runGate(a, deps, ctx, env);
        const ok = r.verdict === "pass";
        return {
          summary: r.summary, result: r.result, ok,
          ...(ok ? {} : { error: { code: r.verdict === "inconclusive" ? "E_UNSUPPORTED" : "E_JOB_FAILED", message: `perf_gate: ${r.verdict}${r.verdict === "inconclusive" ? "(gate 列を測れなかった。DLSS が使えるビルドで測る)" : ""}`, details: { verdict: r.verdict } } }),
        };
      } catch (e: any) {
        if (e instanceof JobFailure || e instanceof Aborted) throw e;
        return jobFail(bodyFromThrown(e, "perf_gate"));
      }
    },
  };
}

export { sleep, stamp };
