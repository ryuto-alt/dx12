// 性能ゲート(MCP-01 / testing/perfGateKind.ts・perfCore.ts・budgets.ts)のテスト。エンジン不要(偽エンジン)。
//   [1] 統計: 閾値 max(床, 1%, A/A の 2σ) / クロックで捨てる / VRAM の門 / 回帰(+12%)/ 列の設定
//   [2] 予算: JSON を読むだけ・合否の列は gate 列(DLSS)だけ・予算を半分にすると fail で指標ごとの margin
//   [3] ジョブ本体: 合格 / 予算半分で不合格 / DLSS が使えなければ inconclusive(TAA を DLSS と取り違えない)/ 回帰 / DRED ON / VRAM 不足 / ロック
//   [4] A/B(ABAB + A/A): B が遅ければ b-slower、同じなら within
//   [5] 数値を TS に書いていない(予算の数値のソース検査)
import fs from "node:fs";
import path from "node:path";
import "./testEnv.ts";
import os from "node:os";
// 実機の GPU 枠マーカー(%TEMP%\uno-gpu-slot-holders)に左右されないよう、空の置き場へ向ける
process.env.DX12_GPU_MARKER_DIR = fs.mkdtempSync(path.join(os.tmpdir(), "gm-"));
import { tmpDir, rmTree } from "./fleetTestKit.ts";
import { loadJobsConfig } from "./jobs/config.ts";
import type { KindEnv } from "./jobs/env.ts";
import { perfGateKind, columnView } from "./testing/perfGateKind.ts";
import { COLUMN_IDS, evaluateColumn, gateColumnOf, listBudgetPresets, loadBudget, vramLimitMB, type Measured } from "./testing/budgets.ts";
import {
  PROTOCOL, ababSchedule, abThreshold, acquireGpuWindow, columnActive, compareAB, detectRegression, dropByClock, historyKey, upscalerSetup, vramGate,
} from "./testing/perfCore.ts";
import type { EngineProvider } from "./testing/engineProvider.ts";

let failed = 0, total = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  total++;
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${typeof detail === "string" ? detail : JSON.stringify(detail).slice(0, 900)}` : ""}`); }
}
const dirs: string[] = [];
const mk = (l: string) => { const d = tmpDir(l); dirs.push(d); return d; };
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"));
const REPO = path.resolve(here, "..", "..");

console.log("[1] 統計");
{
  const th = abThreshold(2, [0.01, -0.02, 0.015]);
  check("閾値 = max(0.05ms, 1%, 2σ): 小さい値では床 0.05", th.threshold === 0.05 && th.parts.floorMs === 0.05, th);
  const th2 = abThreshold(100, [0, 0]);
  check("基準が大きければ 1% が効く", th2.threshold === 1, th2);
  const th3 = abThreshold(1, [1, -1, 1, -1]);
  check("A/A の揺れが大きければ 2σ が効く", th3.threshold > 2 && th3.parts.sigma2 === th3.threshold, th3);
  const cmp = compareAB([10, 10.1, 10], [11, 11.1, 11], [0.01, -0.01]);
  check("B が閾値を超えて遅ければ b-slower", cmp.verdict === "b-slower" && (cmp.delta as number) > 0.9, cmp);
  check("閾値以内なら within", compareAB([10, 10, 10], [10.02, 10.03, 10], []).verdict === "within");
  check("データ不足は insufficient", compareAB([], [1], []).verdict === "insufficient");
  const d = dropByClock([{ index: 0, clockA: 2500 }, { index: 1, clockA: 2510 }, { index: 2, clockA: 2400 }, { index: 3, clockA: 2490 }], 3);
  check("クロックが 3% 以上ずれた回を捨てる(中央値基準)", d.dropped.length === 1 && d.dropped[0].index === 2 && d.kept.length === 3, d);
  const dp = dropByClock([{ index: 0, clockA: 2500, clockB: 2300 }], 3, 2500);
  check("A と B のクロックが 3% 以上違う回は捨てる", dp.dropped.length === 1, dp);
  check("ABAB の順序", ababSchedule(2).map((s) => s.side).join("") === "ABAB" && ababSchedule(5).length === 10);
  check("VRAM の門: Budget が足りなければ不合格・取れなければ中止", !vramGate(6000, 100, 6656).ok && vramGate(7000, 100, 6656).ok && !vramGate(null, 0, 6656).ok);
  const hist = Array.from({ length: 5 }, (_, i) => ({ at: `t${i}`, key: "k", preset: "p", column: "dlss", pose: "current", frameMsP95: 10, gpuTotalMs: 8 }));
  const r12 = detectRegression(hist, "k", { frameMsP95: 11.2, gpuTotalMs: 8 });
  check("履歴に人工的な +12% を入れると regression", r12.status === "regression" && r12.metrics[0].regression === true, r12);
  check("+5% は regression ではない", detectRegression(hist, "k", { frameMsP95: 10.5, gpuTotalMs: 8.2 }).status === "ok");
  check("履歴が無ければ insufficient", detectRegression([], "k", { frameMsP95: 10, gpuTotalMs: 8 }).status === "insufficient");
  check("直近 5 回だけを見る(古い悪い値は無視)", detectRegression([{ ...hist[0], frameMsP95: 100 }, ...hist, ...hist.slice(0, 0)], "k", { frameMsP95: 10, gpuTotalMs: 8 }).compared === 5);
  const dl = upscalerSetup("dlss", 0.6667), tn = upscalerSetup("taaNative", 1);
  check("DLSS 列は set_upscaler dlss(品質は renderScale から)、TAA 列は taa + render scale", dl[0].params.mode === "dlss" && dl[0].params.quality === "quality" && tn[0].params.mode === "taa" && tn[1].method === "set_render_scale" && tn[1].params.scale === 1, [dl, tn]);
  check("DLSS 列で TAA に落ちていたら使えない(取り違えない)", !columnActive("dlss", { effective: "taa", reason: "no dll" }).ok && columnActive("dlss", { effective: "dlss" }).ok && !columnActive("taaNative", { effective: "dlss" }).ok);
}

console.log("[2] 予算");
const presets = listBudgetPresets();
{
  check("3 つの予算がある", ["cinematic_1080p30", "epic_1080p60", "high_1080p60"].every((p) => presets.includes(p)), presets);
  const epic = loadBudget("epic_1080p60");
  check("Epic は dlss / taau / taaNative の 3 列", COLUMN_IDS.every((c) => epic.columns[c]));
  check("合否の列は DLSS だけ(TAAU は W2-BD の後・TAA native は記録)", gateColumnOf(epic) === "dlss" && epic.columns.taau?.gate === "gateAfterW2BD" && epic.columns.taaNative?.gate === "record");
  const m = fullMeasured(epic, "dlss", 0.8);
  const ok = evaluateColumn(epic, "dlss", m);
  check("予算の 80% なら全行が合格", ok.pass && ok.failed.length === 0, ok.failed);
  check("vfx の仮の行(leavesPending)は pending で落とさない", ok.checks.filter((c) => c.status === "pending").length >= 1, ok.checks.filter((c) => c.status === "pending"));
  const proj = mk("budproj");
  fs.mkdirSync(path.join(proj, ".dx12"), { recursive: true });
  fs.writeFileSync(path.join(proj, ".dx12", "budgets.json"), JSON.stringify({ p95Ms: epic.p95Ms / 2, rows: { Ddgi: 0.1 } }));
  const half = loadBudget("epic_1080p60", { projectDir: proj });
  const bad = evaluateColumn(half, "dlss", m);
  const p95 = bad.checks.find((c) => c.id === "frameMsP95")!;
  check("プロジェクトの .dx12/budgets.json で上書きでき、予算を絞ると fail + 指標ごとの margin", !bad.pass && p95.status === "fail" && (p95.margin as number) < 0 && bad.checks.find((c) => c.id === "Ddgi")!.status === "fail" && (half.overridden ?? []).includes("p95Ms"), bad.failed);
  check("VRAM の実効上限 = min(固定の上限, 予算 - 余白)", Math.abs((vramLimitMB(epic, 7000).limitMB as number) - Math.min(epic.vram.capGb! * 1024, 7000 - epic.vram.marginGb * 1024)) < 1e-6);
  const cin = loadBudget("cinematic_1080p30");
  check("capGb:null なら 予算 - 余白 だけ", cin.vram.capGb === null && vramLimitMB(cin, 7000).basis === "budget-margin");
}

function fullMeasured(b: ReturnType<typeof loadBudget>, col: "dlss" | "taau" | "taaNative", k: number): Measured {
  const c = b.columns[col]!;
  const leafMs: Record<string, number> = {};
  for (const r of c.rows) if (!r.leavesPending) for (const l of r.leaves) leafMs[l] = (r.ms * k) / r.leaves.length;
  return {
    frameMsP95: b.p95Ms * k, frameMsP99: b.p99Ms * k, gpuTotalMs: c.totalMs * k, leafMs,
    cpu: { gameThreadP95Ms: b.cpu.gameThreadP95Ms * k, renderRecordP95Ms: b.cpu.renderRecordP95Ms * k, editorUiP95Ms: b.cpu.editorUiP95Ms * k },
    vramUsedMB: 4000, vramBudgetMB: 7000,
  };
}

// ── 偽エンジン(列と倍率で値が変わる) ───────────────────────────────────────────
const epic = loadBudget("epic_1080p60");
type World = { k: number; mode: string; scale: number; dlssAvailable: boolean; dred: boolean; vramBudget: number; calls: string[]; view: number[] | null; kByBuild: Record<string, number>; viewLog: number[][]; scene?: string };
function makeEngine(w: World, buildKey = "") {
  return async (method: string, params: any = {}): Promise<any> => {
    w.calls.push(method);
    const col = w.mode === "dlss" ? "dlss" : w.scale >= 0.99 ? "taaNative" : "taau";
    const k = w.k * (w.kByBuild[buildKey] ?? 1);
    switch (method) {
      case "ping": return { baseDir: null, currentScene: w.scene, vramUsedMB: 4000, vramBudgetMB: w.vramBudget, gpu: { name: "FakeGPU" } };
      case "set_view_size": w.view = [params.width, params.height]; w.viewLog.push([params.width, params.height]); return {};
      case "get_render_scale": { const v = w.view ?? [1920, 1080]; return { scale: w.scale, renderResolution: w.mode === "dlss" ? [Math.round(v[0] * 2 / 3), Math.round(v[1] * 2 / 3)] : v }; }
      case "open_scene": w.scene = params.path; return {};
      case "set_quality_preset": case "set_editor_camera": case "step_frames": case "cancel": return {};
      case "set_upscaler": if (params.mode) w.mode = params.mode; return {};
      case "set_render_scale": w.scale = params.scale; return {};
      case "get_upscaler": return { mode: w.mode, effective: w.mode === "dlss" && !w.dlssAvailable ? "taa" : w.mode, reason: w.mode === "dlss" && !w.dlssAvailable ? "nvngx_dlss.dll が無い" : undefined };
      case "benchmark": { const m = fullMeasured(epic, col as any, k); return { frameMs: { avg: m.frameMsP95! * 0.9, p95: m.frameMsP95 } }; }
      case "perf_stats": {
        const m = fullMeasured(epic, col as any, k);
        return { gpuPassMs: { total: m.gpuTotalMs }, gpuLeafMs: m.leafMs, cpuScopeMs: { update: m.cpu.gameThreadP95Ms, shadowRec: m.cpu.renderRecordP95Ms, editorUi: m.cpu.editorUiP95Ms }, upscaler: { mode: w.mode, effective: w.mode === "dlss" && !w.dlssAvailable ? "taa" : w.mode }, dred: w.dred };
      }
      default: throw new Error(`unknown ${method}`);
    }
  };
}
const newWorld = (o: Partial<World> = {}): World => ({ k: 0.8, mode: "taa", scale: 1, dlssAvailable: true, dred: false, vramBudget: 7000, calls: [], view: null, kByBuild: {}, viewLog: [], ...o });
const cfg = { ...loadJobsConfig({ ...process.env, DX12_JOBS_DIR: mk("jobs") }), repoDir: REPO };
const env = { cfg, resolveEngineId: () => undefined, enginePort: () => undefined, callEngine: async () => ({}), fleet: () => null, callTool: async () => ({ text: "", isError: true, images: 0 }), now: () => Date.now() } as unknown as KindEnv;
const clock = () => ({ stop: async () => [{ smMHz: 2500, tempC: 60 }] });
const mkCtx = () => ({ job: { id: "j-perf01" }, signal: new AbortController().signal, progress() {}, log() {}, artifactsDir: mk("art"), addArtifact() {}, setEngine() {} }) as any;
function kindFor(w: World, extra: any = {}) {
  const provider: EngineProvider = { attach: () => ({ call: makeEngine(w) as any, close: async () => {} }), launch: async (o) => ({ call: makeEngine(w, o.buildDir ?? "") as any, close: async () => {} }) };
  return perfGateKind(env, { provider, repoDir: REPO, clock, lockFile: path.join(mk("lock"), "gpu.lock"), historyDir: mk("hist"), gpuMemUsedMB: async () => null, ...extra });
}
const small = { benchFrames: 60, warmupFrames: 0, statsWindow: 10, repeats: 3 };

console.log("[2b] 測れていない指標のある合格は合格にしない");
{
  const nul: Measured = { frameMsP95: null, gpuTotalMs: null, leafMs: {}, cpu: { gameThreadP95Ms: null, renderRecordP95Ms: null, editorUiP95Ms: null }, vramUsedMB: null, vramBudgetMB: null };
  const ev = evaluateColumn(epic, "dlss", nul);
  check("全部 null は pass=true でも complete=false(必須指標が missing)", ev.pass === true && ev.complete === false && ev.missing.includes("frameMsP95") && ev.missing.includes("gpuTotalMs") && ev.missing.includes("vramMB"), ev.missing);
  const full = fullMeasured(epic, "dlss", 0.8);
  check("全部測れていれば complete=true", evaluateColumn(epic, "dlss", full).complete === true);
  check("VRAM だけ欠けても complete=false", evaluateColumn(epic, "dlss", { ...full, vramUsedMB: null }).complete === false);
  check("計装済みの行が欠ければ complete=false(leavesPending の行は除く)", (() => { const r = epic.columns.dlss!.rows.find((x) => !x.leavesPending); if (!r) return true; const lm = { ...full.leafMs }; for (const l of r.leaves) delete lm[l]; return evaluateColumn(epic, "dlss", { ...full, leafMs: lm }).complete === false; })());
}

console.log("[3] ジョブ本体");
{
  const w = newWorld();
  const r = await kindFor(w).run({ preset: "epic_1080p60", engine: "fake", mode: "gate", ...small }, mkCtx());
  const s: any = r.summary;
  check("予算の 80% で合格(gate 列 = dlss)", r.ok === true && s.verdict === "pass" && s.gateColumn === "dlss", s);
  check("3 列とも測り、dlss だけが gating", s.columns.dlss.gate === "gate" && s.columns.taau.gate === "gateAfterW2BD" && s.columns.taaNative.gate === "record" && s.columns.taaNative.status === "measured", s.columns);
  check("DRED は OFF(--no-dred)を記録", s.noDred === true, s.noDred);
  check("set_view_size は予算の出力解像度(DLSS / TAA native 列)", w.view?.[0] === epic.output[0] && w.view?.[1] === epic.output[1], w.view);
  const tv = columnView(epic, "taau");
  check("TAAU 列は view size を出力×renderScale にする(固定中は render_scale が効かないため)", tv.width < epic.output[0] && tv.height < epic.output[1] && Math.abs(tv.width - epic.output[0] * epic.columns.taau!.renderScale) <= 2 && w.viewLog.some((v) => v[0] === tv.width && v[1] === tv.height), { tv, log: w.viewLog });
  check("TAAU 列の描画解像度は狙いと一致して測られる", (r.result as any).detail.taau.status === "measured" && (r.result as any).detail.taau.renderResolution?.[0] === tv.width, (r.result as any).detail.taau);
  check("A/A 的な繰り返しのばらつき(repeatSigma)とクロックを記録", (r.result as any).detail.dlss.poses.current.clock.perRun.length === 3 && (r.result as any).detail.dlss.poses.current.clock.refMHz === 2500, (r.result as any).detail.dlss.poses.current.clock);

  const wbad = newWorld({ k: 1.2 });
  const r2 = await kindFor(wbad).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  const d2: any = (r2.result as any).detail.dlss.poses.current;
  check("予算の 120% なら不合格で、指標ごとの margin(負)が出る", r2.ok === false && (r2.summary as any).verdict === "fail" && d2.checks.find((c: any) => c.id === "frameMsP95").margin < 0 && d2.failed.includes("frameMsP95"), d2.failed);
  check("縮退の案内が超過の大きい順に付く", Array.isArray(d2.degrade) && d2.degrade.length > 0 && d2.degrade[0].over >= d2.degrade.at(-1).over, d2.degrade);

  const wno = newWorld({ dlssAvailable: false });
  const r3 = await kindFor(wno).run({ preset: "epic_1080p60", engine: "fake", ...small }, mkCtx());
  const s3: any = r3.summary;
  check("DLSS が使えなければ dlss 列は unavailable → inconclusive(TAA を DLSS として合格にしない)", r3.ok === false && s3.verdict === "inconclusive" && s3.columns.dlss.status === "unavailable" && /DLSS/.test(s3.columns.dlss.reason), s3);
  check("そのとき TAA native 列は記録される", s3.columns.taaNative.status === "measured", s3.columns.taaNative);

  const hdir = mk("hist2");
  const k1 = kindFor(newWorld(), { historyDir: hdir });
  for (let i = 0; i < 3; i++) await k1.run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  const r4 = await kindFor(newWorld({ k: 0.8 * 1.12 }), { historyDir: hdir }).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  check("履歴に対して +12% 悪化すれば regression", (r4.summary as any).verdict === "regression" && r4.ok === false && (r4.summary as any).regressions.includes("current"), r4.summary);
  const hist = fs.readFileSync(path.join(hdir, "perf-history.jsonl"), "utf8").trim().split("\n");
  check("perf-history.jsonl に追記される", hist.length === 4 && JSON.parse(hist[0]).key === historyKey("epic_1080p60", "dlss", "current"), hist.length);

  // 実運用(既定の専用エンジン)では、プロジェクトは毎回別の一時フォルダへ複製される。履歴は元のプロジェクトで引き当てる。
  {
    const ph = mk("perfhome"); const old = process.env.DX12_PERF_HOME; process.env.DX12_PERF_HOME = ph;
    try {
      const noHist = (x: any) => ({ ...x });
      const kp = kindFor(newWorld(), { historyDir: undefined });
      for (let i = 0; i < 2; i++) await kp.run(noHist({ preset: "epic_1080p60", launch: true, ...small, columns: ["dlss"] }), mkCtx());
      const rr = await kindFor(newWorld({ k: 0.8 * 1.12 }), { historyDir: undefined }).run({ preset: "epic_1080p60", launch: true, ...small, columns: ["dlss"] }, mkCtx());
      check("launch(毎回別の一時コピー)でも履歴が引き当たり、+12% で regression", (rr.summary as any).verdict === "regression", (rr.result as any)?.detail?.dlss?.poses?.current?.regression);
      const rs = await kindFor(newWorld({ k: 0.8 * 1.12 }), { historyDir: undefined }).run({ preset: "epic_1080p60", launch: true, scene: "other.scene", ...small, columns: ["dlss"] }, mkCtx());
      check("別のシーンの履歴は混ざらない", (rs.summary as any).verdict !== "regression" && (rs.result as any).detail.dlss.poses.current.regression.status === "insufficient", (rs.result as any).detail.dlss.poses.current.regression);
    } finally { if (old === undefined) delete process.env.DX12_PERF_HOME; else process.env.DX12_PERF_HOME = old; }
  }
  {
    const g1 = vramGate(7000, 1000, 6656, 1100), g2 = vramGate(7000, 1000, 6656, 3000), g3 = vramGate(7000, 1000, 6656, null);
    check("VRAM の門: Budget − 他プロセス(nvidia-smi 全体 − 自分)で判定", g1.ok && g1.othersUsedMB === 100 && !g2.ok && g2.othersUsedMB === 2000 && g2.availableMB === 5000 && g3.ok && g3.othersUsedMB === null, { g1, g2, g3 });
    let ev: any = null;
    try { await kindFor(newWorld(), { gpuMemUsedMB: async () => 5000 }).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx()); } catch (x: any) { ev = x.body; }
    check("ゲーム等が VRAM を使っていれば(Budget が減らなくても)計測を中止", ev?.code === "E_FLEET_RESOURCE" && ev.details?.vramCheck?.othersUsedMB === 1000, ev);
  }

  let e: any = null;
  try { await kindFor(newWorld({ dred: true })).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx()); } catch (x: any) { e = x.body; }
  check("DRED が ON のエンジンでは測らない", /DRED/.test(e?.message ?? ""), e);
  e = null;
  try { await kindFor(newWorld({ vramBudget: 5000 })).run({ preset: "epic_1080p60", engine: "fake", ...small }, mkCtx()); } catch (x: any) { e = x.body; }
  check("VRAM の Budget が前提未満なら中止(E_FLEET_RESOURCE)", e?.code === "E_FLEET_RESOURCE" && !!e?.details?.vramCheck, e);
  e = null;
  try { await kindFor(newWorld()).run({ preset: "nope", engine: "fake", ...small }, mkCtx()); } catch (x: any) { e = x.body; }
  check("未知の予算は E_NOT_FOUND + validValues", e?.code === "E_NOT_FOUND" && (e.details?.validValues ?? []).includes("epic_1080p60"), e);

  // perf_stats の形が変わって指標が取れない → verdict は pass にならない
  const wdrift = newWorld();
  const base = makeEngine(wdrift);
  const driftProvider: EngineProvider = { attach: () => ({ call: (async (m: string, p: any) => { const r = await base(m, p); return m === "perf_stats" ? { ...r, gpuPassMs: {} } : r; }) as any, close: async () => {} }), launch: async () => { throw new Error("no"); } };
  const rd = await perfGateKind(env, { provider: driftProvider, repoDir: REPO, clock, lockFile: path.join(mk("lock"), "gpu.lock"), historyDir: mk("hist"), gpuMemUsedMB: async () => null }).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  check("gpuPassMs.total が取れなければ inconclusive(pass にならない)", (rd.summary as any).verdict === "inconclusive" && rd.ok === false && (rd.summary as any).unmeasuredInGateColumn?.length > 0, rd.summary);

  // gate 列以外の悪化は verdict に効かない(info)
  const hd = mk("hist3");
  const kk = kindFor(newWorld(), { historyDir: hd });
  for (let i = 0; i < 3; i++) await kk.run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss", "taaNative"] }, mkCtx());
  const lines = fs.readFileSync(path.join(hd, "perf-history.jsonl"), "utf8").trim().split(String.fromCharCode(10)).map((x) => JSON.parse(x));
  // taaNative の履歴だけ良い値に書き換えて、今回が悪化に見えるようにする
  fs.writeFileSync(path.join(hd, "perf-history.jsonl"), lines.map((l) => JSON.stringify(l.column === "taaNative" ? { ...l, frameMsP95: l.frameMsP95 / 2, gpuTotalMs: l.gpuTotalMs / 2 } : l)).join(String.fromCharCode(10)) + String.fromCharCode(10));
  const rg = await kindFor(newWorld(), { historyDir: hd }).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss", "taaNative"] }, mkCtx());
  check("記録だけの列(taaNative)の悪化は verdict に効かず、regressionsInfo に出る", (rg.summary as any).verdict === "pass" && rg.ok === true && (rg.summary as any).regressionsInfo?.some((x: any) => x.column === "taaNative"), rg.summary);

  // 既定では専用エンジンを起動し、束縛エンジン(DRED ON)の設定を触らない
  const wl = newWorld(); const launched: string[] = [];
  const prov2: EngineProvider = { attach: () => { throw new Error("attach しない"); }, launch: async (o) => { launched.push(...(o.extraArgs ?? [])); return { call: makeEngine(wl) as any, close: async () => {} }; } };
  const rl = await perfGateKind(env, { provider: prov2, repoDir: REPO, clock, lockFile: path.join(mk("lock"), "gpu.lock"), historyDir: mk("hist") }).run({ preset: "epic_1080p60", ...small, columns: ["dlss"] }, mkCtx());
  check("engine / project / launch 無しでも専用エンジンを --no-dred で起動する", rl.ok === true && launched.includes("--no-dred"), launched);
  const wa = newWorld({ dred: true });
  try { await kindFor(wa).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx()); } catch { /* DRED で拒否 */ }
  check("DRED ON の束縛エンジンは、設定を何も変える前に拒否する", !wa.calls.some((m) => ["set_view_size", "set_quality_preset", "set_upscaler", "set_render_scale"].includes(m)), wa.calls);
  const wr = newWorld({ mode: "taa", scale: 0.9 });
  await kindFor(wr).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  check("束縛エンジンの upscaler は終わりに元へ戻す", wr.mode === "taa", wr.mode);

  const lockFile = path.join(mk("lock2"), "gpu.lock");
  const held = await acquireGpuWindow({ file: lockFile, owner: "test" });
  e = null;
  try { await kindFor(newWorld(), { lockFile }).run({ preset: "epic_1080p60", engine: "fake", ...small }, mkCtx()); } catch (x: any) { e = x.body; }
  check("計測窓のロックを他が持っていれば取れずに中止", e?.code === "E_ENGINE_BUSY", e);
  held!();
  await kindFor(newWorld(), { lockFile }).run({ preset: "epic_1080p60", engine: "fake", ...small, columns: ["dlss"] }, mkCtx());
  check("終わったらロックは解放される", !fs.existsSync(lockFile));
  fs.writeFileSync(lockFile, JSON.stringify({ pid: 2147483000, owner: "dead", startedAt: "x" }));
  const stolen = await acquireGpuWindow({ file: lockFile });
  check("持ち主の PID が居ない古いロックは奪える", typeof stolen === "function");
  stolen?.();
  const md = mk("markers");
  fs.writeFileSync(path.join(md, "1-1.json"), JSON.stringify({ pid: process.ppid, slot: 1 }));
  const blocked = await acquireGpuWindow({ file: lockFile, markerDir: md });
  check("GPU の枠(golden / UI テスト / エンジン)の生きたマーカーがあれば窓を取らない(取った窓は返す)", blocked === null && !fs.existsSync(lockFile));
  fs.writeFileSync(path.join(md, "1-1.json"), JSON.stringify({ pid: 2147483000, slot: 1 }));
  const after = await acquireGpuWindow({ file: lockFile, markerDir: md });
  check("死んだ持ち主のマーカーは無視して取れる", typeof after === "function");
  after?.();
}

console.log("[4] A/B(ABAB + A/A)");
{
  const w = newWorld({ kByBuild: { "C:/b/slow": 1.1 } });
  const slow = await kindFor(w).run({ mode: "ab", preset: "epic_1080p60", a: { buildDir: "C:/b/base" }, b: { buildDir: "C:/b/slow" }, rounds: 3, ...small, repeats: 1, columns: ["dlss"] }, mkCtx());
  const s: any = slow.summary;
  check("B が 10% 遅ければ b-slower(A/A の揺れが 0 なので 1% の閾値を超える)", s.verdict === "b-slower" && s.slower.includes("current.avg") && slow.ok === false, s);
  check("ABAB と A/A を回した記録(クロックの基準・捨てた回)", s.rounds === 3 && s.clockRefMHz === 2500 && Array.isArray(s.discarded.ab), s);
  const same = await kindFor(newWorld()).run({ mode: "ab", preset: "epic_1080p60", a: { buildDir: "C:/b/base" }, b: { buildDir: "C:/b/same" }, rounds: 2, ...small, repeats: 1, columns: ["dlss"] }, mkCtx());
  check("同じ性能なら within-or-faster で合格", (same.summary as any).verdict === "within-or-faster" && same.ok === true, same.summary);
  const noDlss = await kindFor(newWorld({ dlssAvailable: false })).run({ mode: "ab", preset: "epic_1080p60", a: { buildDir: "C:/b/base" }, b: { buildDir: "C:/b/same" }, rounds: 2, ...small, repeats: 1, columns: ["dlss"] }, mkCtx());
  check("A/B で列が有効にならなければ inconclusive(pass にしない)", (noDlss.summary as any).verdict === "inconclusive" && noDlss.ok === false && /有効にならなかった/.test(((noDlss.summary as any).inconclusiveReasons ?? []).join()), noDlss.summary);
  const lowV = await kindFor(newWorld({ vramBudget: 5000 })).run({ mode: "ab", preset: "epic_1080p60", a: { buildDir: "C:/b/base" }, b: { buildDir: "C:/b/same" }, rounds: 2, ...small, repeats: 1, columns: ["dlss"] }, mkCtx());
  check("A/B で VRAM の前提を満たさなければ inconclusive", (lowV.summary as any).verdict === "inconclusive" && lowV.ok === false, lowV.summary);
  const sk = kindFor(newWorld(), { clock: () => ({ stop: async () => [] }) });
  const nc = await sk.run({ mode: "ab", preset: "epic_1080p60", a: { buildDir: "C:/b/base" }, b: { buildDir: "C:/b/same" }, rounds: 2, ...small, repeats: 1, columns: ["dlss"] }, mkCtx());
  check("A/B でクロックが取れず全回が捨てられたら inconclusive(within-or-faster にしない)", (nc.summary as any).verdict === "inconclusive" && nc.ok === false, nc.summary);
  let e: any = null;
  try { await kindFor(newWorld()).run({ mode: "ab", ...small }, mkCtx()); } catch (x: any) { e = x.body; }
  check("a / b が無ければ E_MISSING_PARAM", e?.code === "E_MISSING_PARAM", e);
}

console.log("[5] 数値を TS に書いていない");
{
  const files = ["budgets.ts", "perfCore.ts", "perfGateKind.ts"].map((f) => fs.readFileSync(path.join(here, "testing", f), "utf8"));
  const src = files.join("\n");
  const stripped = src.replace(/\/\/.*$/gm, "").replace(/"(?:[^"\\]|\\.)*"|`(?:[^`\\]|\\.)*`/g, '""');
  const budgetNums = [String(epic.p95Ms), String(epic.p99Ms), String(epic.vram.precheckGb), String(epic.vram.precheckGb * 1024), String(epic.cpu.gameThreadP95Ms * 1024), "6144", "14.0", "2.5"];
  const hits = budgetNums.filter((n) => new RegExp(`(?<![\\d.])${n.replace(".", "\\.")}(?![\\d.])`).test(stripped));
  check("予算の数値(p95 / p99 / VRAM の上限・前提 など)がソースに無い", hits.length === 0, hits);
  check("クロック固定 API(SetStablePowerState)を使っていない", !/SetStablePowerState/.test(src.replace(/\/\/.*$/gm, "")));
  check("測り方の定数は PROTOCOL に集まっている", PROTOCOL.rounds === 5 && PROTOCOL.clockDropPct === 3 && PROTOCOL.regressionPct === 10);
}

for (const d of dirs) rmTree(d);
console.log(failed ? `\nNG: ${failed}/${total} 件失敗` : `\nOK: perf_gate ${total} 項目すべて通過`);
process.exit(failed ? 1 : 0);
