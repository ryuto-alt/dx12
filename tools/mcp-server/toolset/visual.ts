// dx12_visual / dx12_perf_gate: 検査ジョブ(visual_regression / perf_gate。MCP-01)の薄い入口。
//   dx12_visual {op: capture | compare | approve | list | history}
//     project suite(<project>/.dx12/baselines)はこのツールの中で直接撮って比べる。golden suite の compare はジョブ(visual_regression)を起動する。
//     approve は reason 必須(E_MISSING_PARAM)。tests/golden(リポジトリの基準)の approve は guarded(E_GUARDED)。既定の見た目を黙って変えないため。
//   dx12_perf_gate {…}  perf_gate ジョブを起動して、waitSec の間だけ待つ(長い計測なので既定はすぐ返す)。
// 登録の作法: legacy 面には出さない。長尾(どの面でも tools/list には出さず dx12_call / dx12_tool_search で使う)。
import fs from "node:fs";
import path from "node:path";
import { execFileSync, spawn } from "node:child_process";
import { z } from "zod";
import { engine, errResult, type ToolResult } from "./core.ts";
import { server } from "./core.ts";
import { ENHANCED, SURFACE, TOOL_REGISTRY, ERROR_BODY, recordError } from "../toolRuntime.ts";
import { CORE_GUARDED_TOOL } from "../coreSpec.ts";
import { unknownKeyIssues, bodyFromIssues } from "../validate.ts";
import { envelope, type ErrorBody } from "../errors.ts";
import { isGuardApproved } from "../guardCtx.ts";
import { getJobs, jobsConfig } from "../jobs/runtime.ts";
import { JobFailure } from "../jobs/manager.ts";
import { isTerminal } from "../jobs/types.ts";
import { projectDirOf, StepError } from "../testing/common.ts";
import {
  approveProjectItem, baselineKeyOf, baselinePng, captureFinal, loadDiffLib, openSceneWait, readBaselines, readHistoryLog, safeSeg,
} from "../testing/visualCore.ts";
import { listBudgetPresets } from "../testing/budgets.ts";

export const VISUAL_TOOLS = ["dx12_visual", "dx12_perf_gate"];

const vec3 = () => z.array(z.number()).length(3);
const VISUAL_SHAPE: Record<string, z.ZodTypeAny> = {
  op: z.enum(["capture", "compare", "approve", "list", "history"]).describe("capture = いまの絵を撮って保存(基準は変えない)/ compare = 基準と比べる(project は即時、golden はジョブ)/ approve = 基準を更新(reason 必須。golden は承認が要る)/ list / history。"),
  suite: z.enum(["project", "golden"]).optional().describe("project = <project>/.dx12/baselines(既定)/ golden = tests/golden。"),
  scene: z.string().optional().describe("project: シーン(assets 相対。省略でいまのシーン)。"),
  name: z.string().optional().describe("project: 基準の名前(英数字・_・- と /)。compare は省略で全部。"),
  camera: z.object({ position: vec3(), target: vec3() }).optional().describe("固定カメラ(approve の初回。省略でいまのエディタのカメラ)。"),
  reason: z.string().optional().describe("approve の理由(必須)。history.jsonl に残る。"),
  only: z.array(z.string()).max(200).optional().describe("golden: ID で絞る。"),
  metric: z.enum(["flip"]).optional().describe("段 3(任意): mean FLIP(tools/parity の venv があるときだけ)。"),
  project: z.string().optional().describe("project のフォルダ(省略で ping の baseDir)。"),
  waitSec: z.number().min(0).max(1800).optional().describe("golden の compare: ジョブの完了を待つ秒数(既定 0 = ジョブ id を返すだけ)。"),
  limit: z.number().int().min(1).max(500).optional().describe("history の件数(既定 50)。"),
};

const PERF_SHAPE: Record<string, z.ZodTypeAny> = {
  preset: z.string().optional().describe(`予算(${listBudgetPresets().join(" / ") || "testing/budgets の名前"}。既定 epic_1080p60)。`),
  mode: z.enum(["gate", "ab"]).optional().describe("gate = 予算に当てる / ab = 2 ビルドの ABAB 比較。"),
  columns: z.array(z.enum(["dlss", "taau", "taaNative"])).optional().describe("測る列。合否は gate 列(W1 は dlss)だけ。"),
  poses: z.array(z.object({ name: z.string().optional(), position: vec3().optional(), target: vec3().optional() })).max(12).optional().describe("姿勢。"),
  scene: z.string().optional(), project: z.string().optional(), launch: z.boolean().optional(), engine: z.string().optional(), buildDir: z.string().optional(),
  repeats: z.number().int().min(1).max(9).optional(), benchFrames: z.number().int().min(30).max(3600).optional(), warmupFrames: z.number().int().min(0).max(3600).optional(),
  statsWindow: z.number().int().min(1).max(1000).optional(), rounds: z.number().int().min(1).max(10).optional(),
  a: z.object({ buildDir: z.string(), label: z.string().optional(), args: z.array(z.string()).max(8).optional() }).optional(),
  b: z.object({ buildDir: z.string(), label: z.string().optional(), args: z.array(z.string()).max(8).optional() }).optional(),
  waitLockSec: z.number().min(0).max(7200).optional(), qualityPreset: z.string().optional(), record: z.boolean().optional(),
  waitSec: z.number().min(0).max(1800).optional().describe("ジョブの完了を待つ秒数(既定 0 = ジョブ id を返すだけ)。計測は 数分かかる。"),
};

const VISUAL_DESC =
  "視覚回帰の基準と比較(MCP-01)。判定は 3 段: bitExact(ネイティブ解像度の SHA-256)→ 面積平均で縮めた絵の許容差(合否はここ)→ 任意の FLIP。"
  + "返り値 {pass, bitExact, diffRatio, maxDelta, diffBBox, diffPng, sideBySide}。adapter が違う基準とは比べず skipped。"
  + "project suite は <project>/.dx12/baselines の基準を使う。approve は reason 必須で history.jsonl に approvedBy:'ai' で残り、人が見るまで needsHumanReview。"
  + "tests/golden の approve は guarded(dx12_call_guarded)。golden の compare は visual_regression ジョブを起動する。"
  + "例: {op:'approve', name:'hall', reason:'初回の基準', camera:{position:[0,3,8],target:[0,1,0]}} / {op:'compare'} / {op:'compare', suite:'golden', waitSec:600}。";
const PERF_DESC =
  "性能の合否判定(MCP-01。perf_gate ジョブの薄い入口)。予算は testing/budgets の JSON(列 dlss / taau / taaNative)。合否は gate 列(W1 は DLSS)だけで、ほかは記録。"
  + "エンジンは --no-dred で起動、計測窓のロックと VRAM の前提確認つき。履歴から回帰(直近 5 回の中央値より 10% 以上悪化)も検出。mode:'ab' は 2 ビルドの ABAB 比較。"
  + "計測に数分かかるので、既定はジョブ id を返す(dx12_job_status で待つ)。例: {preset:'epic_1080p60', launch:true, project:'<dir>', waitSec:300}。";

type Args = Record<string, any>;
function errorResult(body: ErrorBody): ToolResult {
  const res: ToolResult = { content: [{ type: "text", text: JSON.stringify(envelope(body)) }], isError: true };
  ERROR_BODY.set(res, body);
  return res;
}
const call = (m: string, p: Record<string, unknown>, o?: { timeout?: number }) => engine.call(m, p, o);
const bad = (code: ErrorBody["code"], message: string, fix?: ErrorBody["fix"]): never => { throw new StepError({ code, message, retryable: false, ...(fix ? { fix } : {}) }); };

async function currentScene(): Promise<string | null> {
  const p = await call("ping", {}).catch(() => null);
  return typeof p?.currentScene === "string" && p.currentScene ? p.currentScene : null;
}

/** golden の approve(承認後)。golden.mjs --update をそのまま使う(判定・保存形式の出どころを 1 つにする)。 */
function runGoldenUpdate(repoDir: string, only: string[]): Promise<{ code: number | null; tail: string }> {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [path.join(repoDir, "tools", "bench", "golden.mjs"), "--update", "--only", only.join(",")], { cwd: repoDir, windowsHide: true });
    let out = "";
    // 止まらないまま居座らないよう、上限(既定 20 分、DX12_GOLDEN_UPDATE_TIMEOUT_SEC)で打ち切る。
    const limitMs = (Number(process.env.DX12_GOLDEN_UPDATE_TIMEOUT_SEC) || 1200) * 1000;
    const timer = setTimeout(() => { out += "[timeout] golden.mjs --update を打ち切った"; try { child.kill(); } catch { /* 無視 */ } }, limitMs);
    child.on("exit", () => clearTimeout(timer));
    child.stdout.on("data", (d) => { out += d; });
    child.stderr.on("data", (d) => { out += d; });
    child.on("close", (code) => resolve({ code, tail: out.split(/\r?\n/).slice(-30).join("\n") }));
    child.on("error", (e) => resolve({ code: -1, tail: String(e.message) }));
  });
}

export async function runVisual(a: Args): Promise<unknown> {
  const cfg = jobsConfig();
  const suite = a.suite ?? "project";
  if (suite === "golden") {
    if (a.op === "approve") {
      if (!a.reason || !String(a.reason).trim()) bad("E_MISSING_PARAM", "approve には reason が要る(何が変わったから基準を更新するのか)");
      if (!Array.isArray(a.only) || !a.only.length) bad("E_MISSING_PARAM", "golden の approve には only(更新する ID)が要る(全件を一度に更新しない)");
      // 基準の更新は統合ブランチ(overhaul/wave0 など)で行う。レーン(w05/lane-x)のブランチでは更新しない。
      const branch = (() => { try { return execFileSync("git", ["-C", cfg.repoDir, "rev-parse", "--abbrev-ref", "HEAD"], { encoding: "utf8", windowsHide: true, timeout: 8000 }).trim(); } catch { return null; } })();
      if (branch && /^w\d+\/lane-/.test(branch)) bad("E_UNSUPPORTED", `golden の approve はレーンのブランチ(${branch})では行わない。統合ブランチで行う(基準 PNG のマージ衝突を避けるため)`);
      const r = await runGoldenUpdate(cfg.repoDir, a.only);
      return { updated: r.code === 0, exitCode: r.code, only: a.only, reason: a.reason, approvedBy: "ai", note: "tests/golden の基準を golden.mjs --update で撮り直した。差分(git diff tests/golden)を人が確認してからコミットする", tail: r.tail };
    }
    if (a.op === "capture") bad("E_UNSUPPORTED", "golden の capture は未対応(基準を変えずに撮るだけの経路が無い)。比べるなら op:\"compare\"");
    if (a.op === "compare") {
      const v = await getJobs().start({ kind: "visual_regression", args: { suite: "golden", ...(a.only ? { only: a.only } : {}), ...(a.metric ? { metric: a.metric } : {}) }, approved: isGuardApproved() });
      if (a.waitSec && a.waitSec > 0) { const done = await getJobs().wait(v.id, { waitMs: a.waitSec * 1000, until: "done" }); return { job: done.id, state: done.state, summary: done.summary, error: done.error ?? null }; }
      return { job: v.id, state: v.state, next: "dx12_job_status {id, waitSec:30} → dx12_job_result" };
    }
    if (a.op === "list") {
      const store: any = await (await import("../testing/engineProvider.ts")).importBench(cfg.repoDir, "goldenStore.mjs");
      if (!store) bad("E_UNSUPPORTED", "tools/bench/lib が無い(リポジトリの外)");
      const dir = path.join(cfg.repoDir, "tests", "golden");
      const c = store.loadGoldenEntries(dir), b = store.loadBaseline(dir);
      return { suite: "golden", entries: c.scenes.map((s: any) => ({ name: s.name, kind: s.kind ?? "3d", baseline: !!b.scenes[s.name], nativeSha256: b.scenes[s.name]?.nativeSha256 ?? null })) };
    }
    bad("E_UNSUPPORTED", `golden suite は compare / approve / list(history は project のみ)`);
  }

  const projectDir = await projectDirOf(call, a.project ?? null);
  if (!projectDir) bad("E_NOT_FOUND", "プロジェクトのフォルダが分からない(project を渡すか、エンジンを繋ぐ)");
  const dir = projectDir as string;
  switch (a.op) {
    case "list": {
      const f = readBaselines(dir);
      return { suite: "project", projectDir: dir, baselines: Object.values(f.baselines).map((b) => ({ key: baselineKeyOf(b.scene, b.name), adapter: b.key.adapter, nativeSha256: b.nativeSha256, size: `${b.width}x${b.height}`, approvedBy: b.approvedBy, approvedAt: b.approvedAt, reason: b.reason, needsHumanReview: b.approvedBy === "ai" && !b.humanReviewed })) };
    }
    case "history": return { suite: "project", history: readHistoryLog(dir, a.limit ?? 50) };
    case "capture": {
      const scene = a.scene ?? (await currentScene());
      if (!scene) bad("E_MISSING_PARAM", "scene が要る(いまのシーンも分からない)");
      const name = a.name ?? "capture";
      const out = path.join(dir, ".dx12", "baselines", "_current", safeSeg(String(scene)), `${safeSeg(String(name))}.png`);
      fs.mkdirSync(path.dirname(out), { recursive: true });
      const lib = await loadDiffLib(cfg.repoDir);
      const ec = (m: string, p: Record<string, unknown>, o?: { timeout?: number }) => call(m, p, o);
      if (a.scene) await openSceneWait(ec as any, a.scene);
      const cam = a.camera ?? (await call("get_editor_camera", {}).then((c) => ({ position: c.position, target: c.target })).catch(() => undefined));
      const cap = await captureFinal(ec as any, out, { camera: cam, warmupFrames: 60, settleFrames: 8 });
      return { saved: cap.file, size: `${cap.img.width}x${cap.img.height}`, nativeSha256: lib.sha256(cap.img), note: "基準は変えていない。基準にするなら approve" };
    }
    case "approve": {
      const scene = a.scene ?? (await currentScene());
      if (!scene) bad("E_MISSING_PARAM", "scene が要る(いまのシーンも分からない)");
      if (!a.name) bad("E_MISSING_PARAM", "name が要る(基準の名前)");
      const lib = await loadDiffLib(cfg.repoDir);
      const cam = a.camera ?? (readBaselines(dir).baselines[baselineKeyOf(scene, a.name)]?.camera) ?? (await call("get_editor_camera", {}).then((c) => ({ position: c.position, target: c.target })).catch(() => undefined));
      const rec = await approveProjectItem({ lib, call: call as any, projectDir: dir, scene, name: a.name, reason: a.reason, camera: cam, approvedBy: "ai" });
      return { approved: baselineKeyOf(scene, a.name), png: baselinePng(dir, scene, a.name), approvedBy: rec.approvedBy, needsHumanReview: true, reason: rec.reason, note: "AI が基準を更新した。人が絵を確認するまで、quality_gate などは needsHumanReview として扱う" };
    }
    case "compare": {
      const v = await getJobs().start({ kind: "visual_regression", args: { suite: "project", project: dir, ...(a.name ? { only: [a.scene ? baselineKeyOf(a.scene, a.name) : a.name] } : {}), ...(a.metric ? { metric: a.metric } : {}) }, approved: isGuardApproved() });
      const done = await getJobs().wait(v.id, { waitMs: Math.max(1, a.waitSec ?? 600) * 1000, until: "done" });
      if (!isTerminal(done.state)) return { job: done.id, state: done.state, next: "dx12_job_status {id, waitSec:30}" };
      const res = getJobs().result(done.id);
      return { job: done.id, state: done.state, ...(res.result as any) };
    }
    default: return bad("E_BAD_ENUM", `知らない op: ${a.op}`);
  }
}

async function runPerf(a: Args): Promise<unknown> {
  const { waitSec, ...jobArgs } = a;
  const v = await getJobs().start({ kind: "perf_gate", args: jobArgs, approved: isGuardApproved() });
  if (waitSec && waitSec > 0) {
    const done = await getJobs().wait(v.id, { waitMs: waitSec * 1000, until: "done" });
    if (isTerminal(done.state)) { const r = getJobs().result(done.id); return { job: done.id, state: done.state, verdict: (done.summary as any)?.verdict ?? null, summary: done.summary, error: done.error ?? null, result: r.result }; }
    return { job: done.id, state: done.state, progress: done.progress, next: "dx12_job_status {id, waitSec:30}" };
  }
  return { job: v.id, state: v.state, next: "dx12_job_status {id, waitSec:30} → dx12_job_result" };
}

function register(name: string, title: string, description: string, SHAPE: Record<string, z.ZodTypeAny>, run: (a: Args) => Promise<unknown>, extraKeywords: string, guardedOp?: (a: Args) => ErrorBody | null) {
  const declared = Object.keys(SHAPE);
  const annotations = { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: false };
  const invoke = async (args: any): Promise<ToolResult> => {
    const issues = unknownKeyIssues(args ?? {}, declared);
    if (issues.length > 0) return errorResult(bodyFromIssues(name, args ?? {}, issues, declared));
    const g = guardedOp?.(args ?? {});
    if (g) { recordError({ at: Date.now(), tool: name, code: g.code, message: g.message }); return errorResult(g); }
    try {
      const data = await run(args ?? {});
      return { content: [{ type: "text", text: JSON.stringify(data) }], structuredContent: { result: data as any } };
    } catch (e: any) {
      if (e instanceof StepError) return errorResult(e.body);
      if (e instanceof JobFailure) return errorResult(e.body);
      return errResult(e);
    }
  };
  // 長尾: どの面でも tools/list には出さない(full 面の総バイト数は M0 基準以下という toolSurface.test の約束を守る)。dx12_call / dx12_tool_search で使う。
  const hidden = true;
  const registered = server.registerTool(name, { title, description, inputSchema: z.object(SHAPE).passthrough() as any, annotations: { title, ...annotations } }, async (args: any) => invoke(args));
  if (hidden) registered.disable();
  TOOL_REGISTRY.set(name, { name, title, description, shape: SHAPE, annotations, tier: "core", core: false, coreDescription: description, extraKeywords, invoke: (args: any) => invoke(args), listed: !hidden, registered });
}

if (ENHANCED && SURFACE !== "legacy") {
  register(
    "dx12_visual", "視覚回帰", VISUAL_DESC, VISUAL_SHAPE, runVisual,
    "visual regression 視覚回帰 ゴールデン golden 基準 baseline approve compare capture 画像比較 スクショ比較 差分 FLIP bitExact",
    (a) => {
      // tests/golden(リポジトリの基準)の approve は承認が要る。既定の見た目を黙って変えない。
      if (a.op === "approve" && a.suite === "golden" && !isGuardApproved()) {
        const split = SURFACE === "core"; const via = split ? CORE_GUARDED_TOOL : "dx12_call";
        return {
          code: "E_GUARDED", message: "dx12_visual {op:\"approve\", suite:\"golden\"} は tests/golden の基準を書き換える。人が差分を確認してから、人の承認で実行する",
          cause: "リポジトリの見た目の基準を AI が黙って更新して回帰を隠す事故を防ぐため", retryable: false,
          fix: [{ tool: via, args: split ? { name: "dx12_visual", args: { ...a } } : { name: "dx12_visual", args: { ...a }, confirm: true }, why: "人が差分を確認し、承認したうえで実行する" }],
          details: { effect: "guarded", via },
        };
      }
      return null;
    },
  );
  register("dx12_perf_gate", "性能ゲート", PERF_DESC, PERF_SHAPE, runPerf, "perf gate 性能 ゲート 予算 budget フレーム時間 16.6ms GPU p95 回帰 regression ABAB A/B DLSS TAA VRAM");
}

