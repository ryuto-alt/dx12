// ジョブ種別 visual_regression(MCP-01)。suite:"golden"(tests/golden。golden.mjs と合否が全件一致)/ "project"(<project>/.dx12/baselines)。
// 比較ロジックは visualCore.ts(= tools/bench/lib/imageDiff.mjs を遅延 import)。エンジンは窓を出さない起動(headless / background)だけ。
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { z } from "zod";
import { jobFail, JobFailure, type InprocCtx, type InprocOutcome, type KindDef } from "../jobs/manager.ts";
import type { KindEnv } from "../jobs/env.ts";
import { bodyFromThrown, projectDirOf, sleep } from "./common.ts";
import { defaultProvider, importBench, type EngineHandle, type EngineProvider } from "./engineProvider.ts";
import {
  applyMasks, baselineKeyOf, captureFinal, compareProjectItem, DEFAULT_TOLERANCE, PROJECT_TOLERANCE, judgeGolden, loadDiffLib, newSceneWait, openSceneWait, readBaselineKey,
  readBaselines, uiExtraArgs, writeEvidence, type DiffLib, type GoldenBaseline, type ProjectItemResult, type Tolerance,
} from "./visualCore.ts";
import { readPng, writePng } from "./visualPng.ts";

const shape = {
  suite: z.enum(["golden", "project"]).optional().describe("golden = tests/golden(リポジトリの基準。golden.mjs と合否が一致)/ project = <project>/.dx12/baselines(既定 golden)。"),
  only: z.array(z.string()).max(200).optional().describe("名前(golden は ID、project は scene/name)で絞る。"),
  group: z.string().optional().describe("golden: group:\"repeat\" など(既定は group 無しのシーンだけ)。"),
  uiOnly: z.boolean().optional().describe("golden: kind:\"ui\" だけ回す。"),
  noUi: z.boolean().optional().describe("golden: kind:\"ui\" を回さない。"),
  strict: z.boolean().optional().describe("golden: 環境違い・プロジェクト無しを不合格にする(既定 true。golden.mjs と同じ)。"),
  repeat: z.number().int().min(1).max(5).optional().describe("golden: 同じシーンを何回撮って決定論も確かめるか(既定 1)。"),
  project: z.string().optional().describe("project suite: プロジェクトのフォルダ(省略で束縛エンジンの baseDir)。"),
  metric: z.enum(["flip"]).optional().describe("段 3(任意): mean FLIP。tools/parity の venv があるときだけ。無ければ skipped。"),
  tolerance: z.object({ pixelThreshold: z.number().min(0).optional(), maxDiffRatio: z.number().min(0).optional(), maxMeanAbs: z.number().min(0).optional() }).optional().describe("許容差の上書き(既定は golden.json の defaults)。"),
  buildDir: z.string().optional().describe("golden: DX12Engine.exe のあるフォルダ(省略でフリートと同じ DX12_FLEET_BUILD_DIR、無ければ build/release)。"),
  engine: z.string().optional().describe("project suite: 使うエンジン(id / name / port)。省略で束縛中。"),
};

type GoldenScene = any;

/** golden.mjs の loadConfig と同じ(defaults と許容差の合成)。 */
export async function loadGoldenScenes(repoDir: string): Promise<{ scenes: GoldenScene[]; baseline: { scenes: Record<string, GoldenBaseline> }; goldenDir: string; gated: string[] }> {
  const store = await importBench(repoDir, "goldenStore.mjs");
  const sel = await importBench(repoDir, "sceneSelect.mjs");
  if (!store || !sel) jobFail({ code: "E_UNSUPPORTED", message: "visual_regression {suite:'golden'}: tools/bench/lib が見つからない(リポジトリの外では golden を回せない)", retryable: false });
  const goldenDir = path.join(repoDir, "tests", "golden");
  const cfg = store.loadGoldenEntries(goldenDir);
  const d = cfg.defaults;
  const all = cfg.scenes.map((s: any) => ({
    width: d.width ?? 640, height: d.height ?? 360, settleFrames: d.settleFrames ?? 8, warmupFrames: d.warmupFrames ?? 60,
    ...s, ...(s.kind === "ui" ? { warmupFrames: s.warmupFrames ?? d.uiWarmupFrames ?? 120 } : {}),
    tolerance: { ...DEFAULT_TOLERANCE, ...(d.tolerance ?? {}), ...(s.tolerance ?? {}) },
  }));
  return { scenes: all, baseline: store.loadBaseline(goldenDir), goldenDir, gated: [] , ...{ _sel: sel } } as any;
}

export type GoldenRunDeps = {
  env: KindEnv; provider: EngineProvider; lib: DiffLib; repoDir: string; outDir: string; ctx: Pick<InprocCtx, "progress" | "log" | "signal" | "addArtifact">;
};

export async function runGolden(a: Record<string, any>, deps: GoldenRunDeps): Promise<{ results: any[]; summary: Record<string, unknown> }> {
  const { repoDir, lib, provider, ctx } = deps;
  const loaded: any = await loadGoldenScenes(repoDir);
  const sel = loaded._sel;
  const strict = a.strict !== false;
  const only = Array.isArray(a.only) && a.only.length ? new Set<string>(a.only) : null;
  const picked = sel.selectScenes(loaded.scenes, { hasRoot: !!process.env.DX12_GOLDEN_PROJECTS_ROOT, group: a.group ?? null, only });
  let scenes: GoldenScene[] = picked.scenes;
  const uiScenes = a.noUi ? [] : scenes.filter((s) => s.kind === "ui");
  scenes = a.uiOnly ? [] : scenes.filter((s) => s.kind !== "ui");
  for (const s of uiScenes) if (!["default", "material", "lighting"].includes(s.layout ?? "default")) jobFail({ code: "E_INVALID_PARAM", message: `${s.name}: layout "${s.layout}" は未対応(default / material / lighting)`, retryable: false });
  const tolOverride = a.tolerance ?? {};
  const bench = await importBench(repoDir, "engine.mjs");
  const repeat = Math.max(1, a.repeat ?? 1);
  const buildDir: string | undefined = a.buildDir ?? deps.env.cfg.buildDir;
  const instance = `golden-${createHash("sha1").update(repoDir).digest("hex").slice(0, 8)}`;
  const total = scenes.length + uiScenes.length;
  const results: any[] = [];
  let done = 0;
  const tick = (m: string) => { ctx.progress({ phase: "capturing", pct: total ? Math.round((done * 100) / total) : 100, message: m }); ctx.log(m); };
  const refPng = (p: string) => (fs.existsSync(p) ? readPng(p) : null);
  fs.mkdirSync(deps.outDir, { recursive: true });
  ctx.log(`golden: 3D ${scenes.length} / UI ${uiScenes.length} / strict=${strict}${picked.gated.length ? ` / 対象外 ${picked.gated.join(",")}` : ""}`);

  // 3D: プロジェクトごとに 1 回だけ起動し、シーンは open_scene で切り替える
  const byProject = new Map<string, GoldenScene[]>();
  for (const s of scenes) { if (!byProject.has(s.project)) byProject.set(s.project, []); byProject.get(s.project)!.push(s); }
  for (const [projectCfg, list] of byProject) {
    const project: string | null = bench ? bench.resolveProject(projectCfg) : null;
    if (!project) {
      for (const s of list) results.push({ name: s.name, pass: !strict, skipped: !strict, reasons: [`プロジェクトが見つからない: ${projectCfg}(DX12_GOLDEN_PROJECTS_ROOT で親フォルダを指定できる)`] });
      done += list.length; continue;
    }
    let h: EngineHandle | null = null;
    try {
      h = await provider.launch({ name: instance, project, mode: "headless", buildDir, scene: list[0].scene ?? list[0].launchScene });
      const call = h.call;
      let dxr: { status: string; reason: string } | null = null;
      if (list.some((x) => x.requiresDxr)) {
        const judgeDxr = (await importBench(repoDir, "../golden.mjs"))?.judgeDxr;
        try { const g = await call("get_dxr"); dxr = judgeDxr ? judgeDxr(g) : { status: "run", reason: "" }; } catch (e: any) { dxr = judgeDxr ? judgeDxr(e instanceof Error ? e : new Error(String(e))) : { status: "fail", reason: String(e?.message ?? e) }; }
      }
      for (const s of list) {
        if (ctx.signal.aborted) throw new Error("aborted");
        const r: any = { name: s.name, pass: true, reasons: [] };
        results.push(r);
        tick(`[${done + 1}/${total}] ${s.name}`);
        try {
          if (s.requiresDxr && dxr) {
            if (dxr.status === "fail") { r.pass = false; r.reasons.push(dxr.reason); continue; }
            if (dxr.status === "skip") { r.skipped = true; r.reasons.push(dxr.reason); continue; }
          }
          if (s.newScene) await newSceneWait(call); else await openSceneWait(call, s.scene, ctx.signal);
          const shots: { img: any; file: string }[] = [];
          for (let k = 0; k < repeat; k++) shots.push(await captureFinal(call, path.join(deps.outDir, `${s.name}${repeat > 1 ? `.${k}` : ""}.native.png`), { camera: s.camera, warmupFrames: s.warmupFrames, settleFrames: s.settleFrames }));
          const first = shots[0];
          const v = judgeGolden(lib, { native: first.img, ref: refPng(path.join(loaded.goldenDir, `${s.name}.png`)), baseline: loaded.baseline.scenes[s.name], width: s.width, height: s.height, tol: { ...s.tolerance, ...tolOverride }, strict });
          Object.assign(r, v);
          if (shots.length > 1) {
            r.deterministic = shots.every((x) => lib.sha256(x.img) === v.sha);
            if (!r.deterministic) { const c = lib.compareImages(first.img, shots.at(-1)!.img, s.tolerance.pixelThreshold); r.determinismDiff = { diffRatio: c.diffRatio, meanAbs: c.meanAbs, maxDist: c.maxDist }; }
          }
          if (!v.pass && !v.skipped) { const ref = refPng(path.join(loaded.goldenDir, `${s.name}.png`)); if (ref) { const small = lib.resampleArea(first.img, s.width, s.height); Object.assign(r, writeEvidence(deps.outDir, s.name, ref, small)); } }
        } catch (e: any) { r.pass = false; r.reasons.push(`撮影失敗: ${e?.message ?? e}`); }
        done++;
      }
    } catch (e: any) {
      if (e?.message === "aborted") throw e;
      for (const s of list) if (!results.find((r) => r.name === s.name)) results.push({ name: s.name, pass: false, reasons: [`エンジンを起動できない: ${e?.message ?? e}`] });
    } finally { await h?.close(); }
  }

  // UI: シーンごとにインスタンスを起動(DPI・テーマが起動引数のため)
  const uiProjectSrc = path.join(loaded.goldenDir, "ui_project");
  for (const s of uiScenes) {
    if (ctx.signal.aborted) throw new Error("aborted");
    const r: any = { name: s.name, pass: true, reasons: [], ui: true };
    results.push(r);
    tick(`[${done + 1}/${total}] ${s.name}`);
    let h: EngineHandle | null = null;
    try {
      const dst = path.join(deps.outDir, "ui_project");
      fs.rmSync(dst, { recursive: true, force: true });
      fs.cpSync(uiProjectSrc, dst, { recursive: true });
      h = await provider.launch({ name: `${instance}-ui`, project: dst, mode: "background", buildDir, extraArgs: uiExtraArgs(s, a.forceTheme ?? null) });
      const call = h.call;
      for (let i = 0; i < 240; i++) { try { await call("ping"); await call("step_frames", { frames: 2 }, { timeout: 60_000 }); break; } catch (e) { if (i === 239) throw e; await sleep(500); } }
      await call("imgui_virtual_input", { enable: true });
      const layout = s.layout ?? "default";
      if (layout !== "default") { await call("editor_command_run", { id: `workspace.${layout}` }, { timeout: 30_000 }); await call("step_frames", { frames: 4, deterministic: true }, { timeout: 60_000 }); }
      if (s.warmupFrames > 0) await call("step_frames", { frames: s.warmupFrames, deterministic: true }, { timeout: 300_000 });
      const shots: any[] = [];
      for (let k = 0; k < Math.max(2, repeat); k++) {
        const file = path.join(deps.outDir, `${s.name}.${k}.native.png`);
        await call("step_frames", { frames: s.settleFrames, deterministic: true }, { timeout: 120_000 });
        const rr = await call("imgui_screenshot", { hideCursor: true, path: file }, { timeout: 120_000 });
        shots.push(readPng(rr.path));
      }
      const masked = shots.map((x) => applyMasks(x, s.masks));
      const sha = lib.sha256(masked[0]);
      const det = masked.every((x) => lib.sha256(x) === sha);
      if (!det) { r.pass = false; r.deterministic = false; r.reasons.push("同じセッションで撮り直した 2 枚が一致しない(UI に実時間・乱数が混ざっている)。masks を足すか、原因を直す"); } else r.deterministic = true;
      const refFile = path.join(loaded.goldenDir, "ui", `${s.name}.png`);
      const v = judgeGolden(lib, { native: masked[0], ref: refPng(refFile), baseline: loaded.baseline.scenes[s.name], width: s.width, height: s.height, tol: { ...s.tolerance, ...tolOverride }, strict, ui: true });
      r.pass = r.pass && v.pass; r.skipped = v.skipped; r.reasons.push(...v.reasons);
      Object.assign(r, { native: v.native, sha: v.sha, bitExact: v.bitExact, diffRatio: v.diffRatio, meanAbs: v.meanAbs, maxDist: v.maxDist, envMismatch: v.envMismatch });
    } catch (e: any) { r.pass = false; r.reasons.push(`撮影失敗: ${e?.message ?? e}`); }
    finally { await h?.close(); }
    done++;
  }

  const failed = results.filter((r) => !r.pass).length;
  const skipped = results.filter((r) => r.skipped).length;
  const summary = {
    suite: "golden", total: results.length, passed: results.length - failed - skipped, failed, skipped, bitExact: results.filter((r) => r.bitExact === true).length,
    allSkipped: results.length > 0 && skipped === results.length, dir: deps.outDir,
    failedNames: results.filter((r) => !r.pass).map((r) => r.name),
  };
  return { results, summary };
}

export async function runProject(a: Record<string, any>, deps: GoldenRunDeps & { call: EngineCall2 }): Promise<{ results: ProjectItemResult[]; summary: Record<string, unknown> }> {
  const { call, lib, ctx } = deps;
  const projectDir = a.project ?? (await projectDirOf(call));
  if (!projectDir) jobFail({ code: "E_NOT_FOUND", message: "visual_regression {suite:'project'}: プロジェクトのフォルダが分からない(project を渡す)", retryable: false });
  const file = readBaselines(projectDir as string);
  let items = Object.values(file.baselines);
  if (Array.isArray(a.only) && a.only.length) { const o = new Set<string>(a.only); items = items.filter((b) => o.has(baselineKeyOf(b.scene, b.name)) || o.has(b.name)); }
  const tol: Tolerance = { ...PROJECT_TOLERANCE, ...(a.tolerance ?? {}) };
  const curKey = await readBaselineKey(call);
  const results: ProjectItemResult[] = [];
  for (let i = 0; i < items.length; i++) {
    if (ctx.signal.aborted) throw new Error("aborted");
    const rec = items[i];
    ctx.progress({ phase: "comparing", pct: Math.round((i * 100) / items.length), message: `[${i + 1}/${items.length}] ${rec.scene}/${rec.name}` });
    try { results.push(await compareProjectItem({ lib, call, projectDir: projectDir as string, rec, curKey, tol, outDir: deps.outDir, repoDir: deps.repoDir, metric: a.metric })); }
    catch (e: any) { results.push({ key: baselineKeyOf(rec.scene, rec.name), scene: rec.scene, name: rec.name, status: "fail", reasons: [`撮影失敗: ${e?.message ?? e}`] } as ProjectItemResult); }
  }
  const failed = results.filter((r) => r.status === "fail").length, skipped = results.filter((r) => r.status === "skipped" || r.status === "missing").length;
  const summary = {
    suite: "project", projectDir, total: results.length, passed: results.filter((r) => r.status === "pass").length, failed, skipped,
    bitExact: results.filter((r) => r.bitExact === true).length, needsHumanReview: results.filter((r) => r.needsHumanReview).map((r) => r.key), dir: deps.outDir,
    failedNames: results.filter((r) => r.status === "fail").map((r) => r.key), adapter: curKey.adapter,
    // 何も比べていない(全部 skipped / missing)ときの印。ci_suite はこれを合格にしない。
    allSkipped: results.length > 0 && skipped === results.length,
  };
  return { results, summary };
}
type EngineCall2 = (method: string, params?: Record<string, unknown>, opts?: { timeout?: number; retry?: boolean }) => Promise<any>;

export function visualRegressionKind(env: KindEnv, injected?: { provider?: EngineProvider; repoDir?: string }): KindDef {
  const provider = injected?.provider ?? defaultProvider(env);
  const repoDir = injected?.repoDir ?? env.cfg.repoDir;
  return {
    kind: "visual_regression", executor: "inproc",
    describe: "視覚回帰。suite:'golden' は tests/golden を golden.mjs と同じ手順・同じ判定で回し(窓を出さない専用エンジン)、suite:'project' は <project>/.dx12/baselines を束縛エンジンで比べる。判定は bitExact → 許容差 → 任意の FLIP の 3 段。adapter が違う基準は比べず skipped。",
    group: (a) => (a.suite === "project" ? `engine:${env.resolveEngineId(a.engine as string | undefined) ?? "default"}` : "engine:golden"),
    timeoutSec: env.cfg.timeoutSec.visual_regression, shape,
    normalize(a) { return { suite: "golden", ...a }; },
    async run(a, ctx): Promise<InprocOutcome> {
      try {
        const lib = await loadDiffLib(repoDir);
        const outDir = ctx.artifactsDir;
        const deps: GoldenRunDeps = { env, provider, lib, repoDir, outDir, ctx };
        let results: any[]; let summary: Record<string, unknown>;
        if (a.suite === "project") {
          const h = provider.attach(a.engine as string | undefined);
          if (h.id) ctx.setEngine(h.id);
          ({ results, summary } = await runProject(a, { ...deps, call: h.call }));
        } else ({ results, summary } = await runGolden(a, deps));
        const manifest = path.join(outDir, "visual_regression.json");
        fs.mkdirSync(outDir, { recursive: true });
        fs.writeFileSync(manifest, JSON.stringify({ summary, results }, null, 2));
        ctx.addArtifact({ path: manifest, kind: "manifest" });
        for (const r of results) for (const k of ["diffPng", "sideBySide"]) if (typeof r[k] === "string" && fs.existsSync(r[k])) ctx.addArtifact({ path: r[k], kind: k === "diffPng" ? "diff" : "side_by_side" });
        const failed = (summary.failed as number) ?? 0;
        return {
          summary, result: { summary, results }, ok: failed === 0,
          ...(failed ? { error: { code: "E_JOB_FAILED", message: `${failed}/${summary.total} 件が不合格(${(summary.failedNames as string[]).slice(0, 6).join(", ")})`, details: { failedNames: summary.failedNames } } } : {}),
        };
      } catch (e: any) {
        if (e instanceof JobFailure) throw e;
        if (e?.message === "aborted") throw e;
        const b = bodyFromThrown(e, "visual_regression");
        return jobFail(b);
      }
    },
  };
}

export { writePng };
