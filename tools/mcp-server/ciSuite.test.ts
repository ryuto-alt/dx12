// ci_suite(MCP-01 / testing/ciSuiteKind.ts・junit.ts)のテスト。エンジン不要(検査の実行器を注入)。
//   [1] JUnit XML の形(pass / fail / skipped / error・エスケープ)
//   [2] ci_suite: exitCode 0/1・blocking だけ落とす・ci.json の読み込み・不正な check
//   [3] GPU なし(ping の gpu が無い / ソフトウェア)では visual / perf を skipped:"no-gpu-adapter"
import fs from "node:fs";
import path from "node:path";
import "./testEnv.ts";
import { tmpDir, rmTree } from "./fleetTestKit.ts";
import { loadJobsConfig } from "./jobs/config.ts";
import type { KindEnv } from "./jobs/env.ts";
import { ciSuiteKind, parseSuiteFile, type CheckRunner } from "./testing/ciSuiteKind.ts";
import { toJunit } from "./testing/junit.ts";
import type { EngineProvider } from "./testing/engineProvider.ts";

let failed = 0, total = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  total++;
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${typeof detail === "string" ? detail : JSON.stringify(detail).slice(0, 900)}` : ""}`); }
}
const dirs: string[] = [];
const mk = (l: string) => { const d = tmpDir(l); dirs.push(d); return d; };

console.log("[1] JUnit XML");
{
  const x = toJunit("s", [
    { name: "a", status: "pass", timeSec: 1.5 }, { name: "b<&>", status: "fail", message: "bad \"q\"", detail: "x<y" },
    { name: "c", status: "skipped", message: "no-gpu-adapter" }, { name: "d", status: "error", message: "boom" },
  ]);
  check("集計(tests / failures / errors / skipped)", /tests="4" failures="1" errors="1" skipped="1"/.test(x), x);
  check("failure / skipped / error の要素", /<failure message="bad &quot;q&quot;">x&lt;y<\/failure>/.test(x) && /<skipped message="no-gpu-adapter"\/>/.test(x) && /<error message="boom">/.test(x), x);
  check("名前のエスケープ", /name="b&lt;&amp;&gt;"/.test(x));
}

console.log("[2] ci_suite の合否");
const cfg = { ...loadJobsConfig({ ...process.env, DX12_JOBS_DIR: mk("jobs") }), repoDir: path.resolve(".") };
const env = { cfg, resolveEngineId: () => undefined, enginePort: () => undefined, callEngine: async () => ({}), fleet: () => null, callTool: async () => ({ text: "", isError: true, images: 0 }), now: () => Date.now() } as unknown as KindEnv;
const mkCtx = () => ({ job: { id: "j-ci0001" }, signal: new AbortController().signal, progress() {}, log() {}, artifactsDir: mk("art"), addArtifact() {}, setEngine() {} }) as any;
const provider = (ping: any): EngineProvider => ({ attach: () => ({ call: (async (m: string) => (m === "ping" ? ping : {})) as any, close: async () => {} }), launch: async () => { throw new Error("no"); } });
const ran: string[] = [];
const ok: CheckRunner = async (c) => { ran.push(c.check); return { status: "pass" }; };
const allOk = { layout: ok, errors: ok, playtests: ok, visual: ok, perf: ok, ui_tests: ok };
const GPU = { gpu: { name: "RTX", driver: "1" } };
{
  const k = ciSuiteKind(env, { provider: provider(GPU), runners: allOk });
  const r = await k.run({ checks: [{ check: "layout" }, { check: "errors" }, { check: "visual" }, { check: "perf" }] }, mkCtx());
  check("全部通れば exitCode 0・ok", r.ok === true && (r.summary as any).exitCode === 0 && (r.summary as any).passed === 4, r.summary);
  check("JUnit XML が書かれる", fs.existsSync((r.summary as any).junit) && /<testsuites/.test(fs.readFileSync((r.summary as any).junit, "utf8")));
  ran.length = 0;
  const bad = ciSuiteKind(env, { provider: provider(GPU), runners: { ...allOk, errors: async () => ({ status: "fail", message: "スクリプトエラー 2 件" }) } });
  const r2 = await bad.run({ checks: [{ check: "layout" }, { check: "errors" }, { check: "playtests" }] }, mkCtx());
  check("1 件でも落ちれば exitCode 1・ok:false・E_JOB_FAILED(残りの検査も走る)", r2.ok === false && (r2.summary as any).exitCode === 1 && (r2.error as any)?.code === "E_JOB_FAILED" && (r2.summary as any).checks.length === 3, r2.summary);
  const thrown = ciSuiteKind(env, { provider: provider(GPU), runners: { ...allOk, layout: async () => { throw new Error("エンジンが落ちた"); } } });
  const r3 = await thrown.run({ checks: [{ check: "layout" }, { check: "errors" }] }, mkCtx());
  check("実行器が例外を投げても error として記録して続ける", (r3.summary as any).checks[0].status === "error" && (r3.summary as any).checks[1].status === "pass" && (r3.summary as any).exitCode === 1, r3.summary);

  const dir = mk("ci");
  const f = path.join(dir, "ci.json");
  fs.writeFileSync(f, JSON.stringify([{ check: "layout" }, { check: "errors" }]));
  check("ci.json(配列)を読める", parseSuiteFile(f).length === 2);
  fs.writeFileSync(f, JSON.stringify({ checks: [{ check: "perf", preset: "epic_1080p60" }] }));
  check("ci.json({checks})を読める", parseSuiteFile(f)[0].preset === "epic_1080p60");
  fs.writeFileSync(f, JSON.stringify([{ check: "nope" }]));
  let e: any = null; try { parseSuiteFile(f); } catch (x: any) { e = x; }
  check("未知の check は拒否", /未知の check/.test(e?.message ?? ""), e?.message);
  const r4 = await k.run({ file: f }, mkCtx()).catch((x: any) => ({ thrown: x.body }));
  check("不正な ci.json は E_VALIDATION_FAILED", (r4 as any).thrown?.code === "E_VALIDATION_FAILED", r4);
  let e2: any = null; try { k.check!({}, {} as any); } catch (x: any) { e2 = x.body; }
  check("checks も file も無ければ E_MISSING_PARAM", e2?.code === "E_MISSING_PARAM", e2);
}

console.log("[3] GPU なしでは visual / perf を skipped");
{
  for (const [label, ping] of [["gpu 情報なし", {}], ["ソフトウェアレンダラ", { gpu: { name: "Microsoft Basic Render Driver" } }]] as const) {
    ran.length = 0;
    const k = ciSuiteKind(env, { provider: provider(ping), runners: allOk });
    const r = await k.run({ checks: [{ check: "layout" }, { check: "visual" }, { check: "perf" }, { check: "errors" }] }, mkCtx());
    const s: any = r.summary;
    check(`${label}: visual と perf は skipped(no-gpu-adapter)・実行されない・全体は合格`, s.checks[1].skipped === "no-gpu-adapter" && s.checks[2].skipped === "no-gpu-adapter" && !ran.includes("visual") && !ran.includes("perf") && s.exitCode === 0 && s.skipped === 2 && r.ok === true, s);
  }
  const k = ciSuiteKind(env, { provider: provider(GPU), runners: allOk });
  const r = await k.run({ noGpu: true, checks: [{ check: "visual" }] }, mkCtx());
  check("noGpu:true で明示的に skipped", (r.summary as any).skipped === 1);
}

for (const d of dirs) rmTree(d);
console.log(failed ? `\nNG: ${failed}/${total} 件失敗` : `\nOK: ci_suite ${total} 項目すべて通過`);
process.exit(failed ? 1 : 0);
