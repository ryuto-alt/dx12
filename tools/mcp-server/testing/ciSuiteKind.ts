// ジョブ種別 ci_suite(MCP-01)。ci.json の checks(layout / errors / playtests / visual / perf / ui_tests)を順に回し、JUnit XML と終了コード相当(exitCode 0/1)を返す。
//   ・落とすのは blocking だけ(警告では落とさない)。GPU が無い環境では visual と perf を skipped:"no-gpu-adapter" にする。
//   ・各検査は実行器を注入できる(テストでは模擬)。既定の実行器は、同じジョブの中で visual / perf の本体(visualKind / perfGateKind)を呼ぶ。
import fs from "node:fs";
import path from "node:path";
import { z } from "zod";
import { jobFail, JobFailure, type InprocCtx, type InprocOutcome, type KindDef } from "../jobs/manager.ts";
import type { KindEnv } from "../jobs/env.ts";
import { Aborted, bodyFromThrown, gpuFromPing, type EngineCall } from "./common.ts";
import { defaultProvider, type EngineProvider } from "./engineProvider.ts";
import { toJunit, type JunitCase } from "./junit.ts";
import { runGate, type PerfDeps } from "./perfGateKind.ts";
import { runGolden, runProject } from "./visualKind.ts";
import { loadDiffLib } from "./visualCore.ts";

export const CI_CHECKS = ["layout", "errors", "playtests", "visual", "perf", "ui_tests"] as const;
export type CiCheck = { check: (typeof CI_CHECKS)[number]; [k: string]: unknown };
export type CheckOutcome = { status: "pass" | "fail" | "skipped" | "error"; message?: string; detail?: unknown };
export type CheckRunner = (c: CiCheck, x: { call: EngineCall; ctx: InprocCtx; args: Record<string, any>; env: KindEnv }) => Promise<CheckOutcome>;
export type CiDeps = { provider: EngineProvider; repoDir: string; runners?: Partial<Record<CiCheck["check"], CheckRunner>>; perf?: Partial<PerfDeps> };

const checkShape = z.object({ check: z.enum(CI_CHECKS) }).passthrough();
const shape = {
  checks: z.array(checkShape).max(40).optional().describe("検査の一覧(例 [{check:'layout'},{check:'errors'},{check:'visual',suite:'golden'},{check:'perf',preset:'epic_1080p60'}])。省略で file を読む。"),
  file: z.string().optional().describe("ci.json のパス(checks の配列、または {checks:[…]})。"),
  junitPath: z.string().optional().describe("JUnit XML の書き出し先(省略で成果物フォルダの junit.xml)。"),
  scenes: z.array(z.string()).max(100).optional().describe("layout 検査で開くシーン(省略で現在のシーン)。"),
  noGpu: z.boolean().optional().describe("true = GPU が無い環境として扱い、visual / perf を skipped にする(既定は ping の gpu から判定)。"),
  project: z.string().optional().describe("visual / perf に渡すプロジェクト。"),
  buildDir: z.string().optional().describe("visual / perf が専用エンジンを起動するときの exe のフォルダ。"),
  engine: z.string().optional().describe("束縛するエンジン(id / name / port)。"),
};

export function parseSuiteFile(file: string): CiCheck[] {
  const j = JSON.parse(fs.readFileSync(file, "utf8"));
  const arr = Array.isArray(j) ? j : j?.checks;
  if (!Array.isArray(arr)) throw new Error(`${file}: checks の配列ではない`);
  for (const c of arr) if (!c || !(CI_CHECKS as readonly string[]).includes(c.check)) throw new Error(`${file}: 未知の check '${c?.check}'(${CI_CHECKS.join(" / ")})`);
  return arr;
}

const GPU_CHECKS = new Set<string>(["visual", "perf"]);

function defaultRunners(deps: CiDeps): Record<CiCheck["check"], CheckRunner> {
  return {
    async layout(c, { call, args }) {
      const scenes: string[] = (c.scenes as string[] | undefined) ?? args.scenes ?? [];
      let errors = 0, checked = 0; const issues: unknown[] = [];
      for (const s of scenes.length ? scenes : [null]) {
        if (s) await call("open_scene", { path: s }, { timeout: 300_000 });
        const v = await call("validate_layout", {});
        errors += v?.errors ?? 0; checked += v?.checked ?? 0; issues.push(...(v?.issues ?? []).filter((i: any) => i.level === "error").slice(0, 20));
      }
      return errors ? { status: "fail", message: `配置検査のエラー ${errors} 件`, detail: issues } : { status: "pass", detail: { checked } };
    },
    async errors(_c, { call }) {
      const r = await call("get_script_errors", {}).catch(() => null);
      const list: unknown[] = Array.isArray(r?.errors) ? r.errors : [];
      return list.length ? { status: "fail", message: `スクリプトエラー ${list.length} 件`, detail: list.slice(0, 20) } : { status: "pass" };
    },
    async playtests(c, { env, args }) {
      const t = await env.callTool("dx12_run_playtests", { ...(c.tags ? { tags: c.tags } : {}), ...(c.name ? { name: c.name } : {}) }, args.engine);
      let p: any = null; try { p = JSON.parse(t.text); } catch { /* 文字列のまま */ }
      if (t.isError || !p) return { status: "error", message: `プレイテストを実行できない: ${t.text.slice(0, 200)}` };
      return p.failed ? { status: "fail", message: `${p.failed} 本のプレイテストが落ちた`, detail: p.results } : { status: "pass", detail: { ran: p.results?.length ?? null } };
    },
    async visual(c, { call, ctx, args, env }) {
      const lib = await loadDiffLib(deps.repoDir);
      const a: any = { ...c, project: c.project ?? args.project, buildDir: args.buildDir };
      const dd = { env, provider: deps.provider, lib, repoDir: deps.repoDir, outDir: path.join(ctx.artifactsDir, "visual"), ctx };
      const r = a.suite === "project" ? await runProject(a, { ...dd, call }) : await runGolden(a, dd);
      const failed = (r.summary.failed as number) ?? 0;
      return failed ? { status: "fail", message: `視覚回帰 ${failed}/${r.summary.total} 件が不合格`, detail: r.summary.failedNames } : { status: "pass", detail: r.summary };
    },
    async perf(c, { ctx, args, env }) {
      const r: any = await runGate({ ...(c as any), project: c.project ?? args.project, buildDir: args.buildDir, engine: args.engine }, { provider: deps.provider, repoDir: deps.repoDir, ...(deps.perf ?? {}) }, ctx, env);
      return r.verdict === "pass" ? { status: "pass", detail: r.summary } : { status: r.verdict === "inconclusive" ? "error" : "fail", message: `perf_gate: ${r.verdict}`, detail: r.summary };
    },
    async ui_tests(c, { ctx, env }) {
      // ui_tests は process 型のジョブ。MCP サーバのジョブ管理から別ジョブとして起動して待つ(循環 import を避けるため遅延 import)。
      const rt = await import("../jobs/runtime.ts");
      const mgr = rt.getJobs();
      const v = await mgr.start({ kind: "ui_tests", args: { ...(c.category ? { category: c.category } : {}), ...(c.skip ? { skip: c.skip } : {}) } });
      const done = await mgr.wait(v.id, { waitMs: env.cfg.timeoutSec.ui_tests * 1000, until: "done", signal: ctx.signal });
      return done.state === "succeeded" ? { status: "pass", detail: done.summary } : { status: "fail", message: `ui_tests: ${done.state}`, detail: done.summary };
    },
  };
}

export function ciSuiteKind(env: KindEnv, injected?: Partial<CiDeps>): KindDef {
  const deps: CiDeps = { provider: injected?.provider ?? defaultProvider(env), repoDir: injected?.repoDir ?? env.cfg.repoDir, runners: injected?.runners, perf: injected?.perf };
  return {
    kind: "ci_suite", executor: "inproc",
    describe: "ci.json の検査(layout / errors / playtests / visual / perf / ui_tests)を順に回し、JUnit XML と exitCode(0/1)を返す。落とすのは blocking だけ。GPU が無い環境では visual / perf を skipped(no-gpu-adapter)にする。",
    group: (a) => `engine:${env.resolveEngineId(a.engine as string | undefined) ?? "ci"}`,
    timeoutSec: env.cfg.timeoutSec.ci_suite, shape,
    check(a) {
      if (!a.checks && !a.file) jobFail({ code: "E_MISSING_PARAM", message: "ci_suite には checks か file(ci.json)が要る", retryable: false, fix: [{ tool: "dx12_job_start", args: { kind: "ci_suite", args: { checks: [{ check: "layout" }, { check: "errors" }] } }, why: "検査の一覧を渡す" }] });
    },
    async run(a, ctx): Promise<InprocOutcome> {
      try {
        let checks: CiCheck[];
        try { checks = (a.checks as CiCheck[] | undefined) ?? parseSuiteFile(String(a.file)); }
        catch (e: any) { return jobFail({ code: "E_VALIDATION_FAILED", message: `ci_suite: ${e?.message ?? e}`, retryable: false }); }
        const h = deps.provider.attach(a.engine as string | undefined);
        if (h.id) ctx.setEngine(h.id);
        const ping = await h.call("ping", {}, { timeout: 8000 }).catch(() => null);
        const gpu = gpuFromPing(ping);
        const noGpu = a.noGpu === true || (!!ping && (!gpu || gpu.isSoftware));
        const runners = { ...defaultRunners(deps), ...(deps.runners ?? {}) };
        const cases: JunitCase[] = []; const detail: any[] = [];
        for (let i = 0; i < checks.length; i++) {
          if (ctx.signal.aborted) throw new Aborted();
          const c = checks[i];
          const name = `${c.check}${c.suite ? `:${c.suite}` : ""}${c.preset ? `:${c.preset}` : ""}`;
          ctx.progress({ phase: "checking", pct: Math.round((i * 100) / checks.length), message: `[${i + 1}/${checks.length}] ${name}` });
          const t0 = Date.now();
          let out: CheckOutcome;
          if (noGpu && GPU_CHECKS.has(c.check)) out = { status: "skipped", message: "no-gpu-adapter" };
          else {
            try { out = await runners[c.check](c, { call: h.call, ctx, args: a, env }); }
            catch (e: any) { if (e instanceof Aborted || e instanceof JobFailure) throw e; out = { status: "error", message: bodyFromThrown(e, c.check).message }; }
          }
          const timeSec = (Date.now() - t0) / 1000;
          cases.push({ name, classname: "ci_suite", timeSec, status: out.status, message: out.message, detail: out.detail !== undefined ? JSON.stringify(out.detail).slice(0, 2000) : undefined });
          detail.push({ check: name, status: out.status, ...(out.status === "skipped" ? { skipped: out.message } : {}), message: out.message, timeSec });
          ctx.log(`${name}: ${out.status}${out.message ? ` (${out.message})` : ""}`);
        }
        const xml = toJunit("ci_suite", cases);
        const junit = a.junitPath ? path.resolve(String(a.junitPath)) : path.join(ctx.artifactsDir, "junit.xml");
        fs.mkdirSync(path.dirname(junit), { recursive: true });
        fs.writeFileSync(junit, xml);
        ctx.addArtifact({ path: junit, kind: "junit" });
        const bad = cases.filter((c) => c.status === "fail" || c.status === "error");
        const summary = { exitCode: bad.length ? 1 : 0, total: cases.length, passed: cases.filter((c) => c.status === "pass").length, failed: bad.length, skipped: cases.filter((c) => c.status === "skipped").length, noGpu, junit, checks: detail };
        return { summary, result: { summary }, ok: bad.length === 0, ...(bad.length ? { error: { code: "E_JOB_FAILED", message: `${bad.length}/${cases.length} 件の検査が不合格(${bad.map((b) => b.name).join(", ")})` } } : {}) };
      } catch (e: any) {
        if (e instanceof JobFailure || e instanceof Aborted) throw e;
        return jobFail(bodyFromThrown(e, "ci_suite"));
      }
    },
  };
}
