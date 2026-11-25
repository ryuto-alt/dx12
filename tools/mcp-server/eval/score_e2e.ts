// W05-36: MCP-14 E2E の採点。LLM は呼ばない。エンジンの状態(probe の結果)とトレースだけから決定論で採点する。
//   node score_e2e.ts --task T1 --trace <file> --port 8856 [--out <score.json>] [--save-probes <file>]
//   node score_e2e.ts --task T1 --trace <file> --probes <saved.json>      (エンジン無しで再採点)
// 合否 = 全 probe の asserts が通ること。呼び出し数・再試行・エラーはトレースから数えて報告する(合否には使わない)。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { readTrace, summarizeTrace } from "./trace.ts";
import type { TraceSummary } from "./trace.ts";

const here = path.dirname(fileURLToPath(import.meta.url));

export type Assert = { path: string; op: string; value?: any; tol?: number };
export type Probe = { id: string; method: string; params: Record<string, unknown>; expectError?: boolean; asserts: Assert[] };
export type Task = { id: string; title: string; prompt: string; maxCalls?: number; probes: Probe[] };
export type ProbeResult = { id: string; ok: boolean; result?: any; error?: string };
export type CheckResult = { id: string; ok: boolean; detail: string };
export type Score = {
  taskId: string; pass: boolean; checks: CheckResult[];
  metrics: { calls: number; retries: number; errors: number; maxCalls: number | null; byTool: Record<string, number> };
};

/** 環境に課金の鍵があれば拒む(MCP-14 の課金禁止ガード)。 */
export function assertNoPaidApiKey(env: Record<string, string | undefined> = process.env): void {
  if (env["ANTHROPIC_API_KEY"]) throw new Error("環境に有料 API の鍵があるため起動を拒否する(サブスクリプションの Claude Code だけで回す)");
}

export function loadTasks(file: string = path.join(here, "e2e_tasks.json")): Task[] {
  return JSON.parse(fs.readFileSync(file, "utf8")).tasks as Task[];
}

// ---- パスの評価: a.b[0].c / a[name=X].b。[k=v] は配列の中で String(el[k]) === v の最初の要素。 ----
export function pathTokens(p: string): Array<{ k: "key" | "idx" | "sel"; key?: string; idx?: number; sk?: string; sv?: string }> {
  const out: Array<{ k: "key" | "idx" | "sel"; key?: string; idx?: number; sk?: string; sv?: string }> = [];
  const re = /\[([^\]=]+)=([^\]]*)\]|\[(\d+)\]|\.?([A-Za-z_][\w-]*)/gy;
  let m: RegExpExecArray | null;
  let last = 0;
  while ((m = re.exec(p)) !== null) {
    last = re.lastIndex;
    if (m[4] !== undefined) out.push({ k: "key", key: m[4] });
    else if (m[3] !== undefined) out.push({ k: "idx", idx: Number(m[3]) });
    else out.push({ k: "sel", sk: m[1], sv: m[2] });
  }
  if (last !== p.length) throw new Error(`パスを解釈できない: ${p}`);
  return out;
}

export function evalPath(root: any, p: string): { found: boolean; value?: any } {
  let cur = root;
  for (const t of pathTokens(p)) {
    if (cur === null || cur === undefined) return { found: false };
    if (t.k === "key") { if (typeof cur !== "object" || !(t.key! in cur)) return { found: false }; cur = cur[t.key!]; }
    else if (t.k === "idx") { if (!Array.isArray(cur) || t.idx! >= cur.length) return { found: false }; cur = cur[t.idx!]; }
    else {
      if (!Array.isArray(cur)) return { found: false };
      const el = cur.find((e) => e && typeof e === "object" && String(e[t.sk!]) === t.sv);
      if (el === undefined) return { found: false };
      cur = el;
    }
  }
  return { found: true, value: cur };
}

export function evalAssert(root: any, a: Assert): CheckResult {
  const label = `${a.path} ${a.op}${a.value !== undefined ? " " + JSON.stringify(a.value) : ""}`;
  let r: { found: boolean; value?: any };
  try { r = evalPath(root, a.path); } catch (e: any) { return { id: label, ok: false, detail: String(e.message ?? e) }; }
  if (a.op === "exists") return { id: label, ok: r.found, detail: r.found ? "あった" : "見つからない" };
  if (a.op === "absent") return { id: label, ok: !r.found, detail: r.found ? "あってはいけないものがある" : "無い(期待どおり)" };
  if (!r.found) return { id: label, ok: false, detail: "パスが見つからない" };
  const v = r.value;
  const fail = (d: string): CheckResult => ({ id: label, ok: false, detail: d });
  const ok = (): CheckResult => ({ id: label, ok: true, detail: "OK" });
  switch (a.op) {
    case "eq": return JSON.stringify(v) === JSON.stringify(a.value) ? ok() : fail(`実際: ${JSON.stringify(v)}`);
    case "approx": {
      const tol = a.tol ?? 1e-3;
      const want = Array.isArray(a.value) ? a.value : [a.value];
      const got = Array.isArray(v) ? v : [v];
      if (got.length !== want.length || got.some((x: any) => typeof x !== "number")) return fail(`実際: ${JSON.stringify(v)}`);
      return got.every((x: number, i: number) => Math.abs(x - want[i]) <= tol) ? ok() : fail(`実際: ${JSON.stringify(v)}`);
    }
    case "gte": return typeof v === "number" && v >= a.value ? ok() : fail(`実際: ${JSON.stringify(v)}`);
    case "lte": return typeof v === "number" && v <= a.value ? ok() : fail(`実際: ${JSON.stringify(v)}`);
    case "between": return typeof v === "number" && v >= a.value[0] && v <= a.value[1] ? ok() : fail(`実際: ${JSON.stringify(v)}`);
    case "contains":
      if (typeof v === "string") return v.includes(String(a.value)) ? ok() : fail(`実際: ${v.slice(0, 200)}`);
      if (Array.isArray(v)) return v.some((x) => JSON.stringify(x) === JSON.stringify(a.value)) ? ok() : fail("含まれない");
      return fail("文字列でも配列でもない");
    case "count": return Array.isArray(v) && v.length === a.value ? ok() : fail(`実際: ${Array.isArray(v) ? v.length : "配列ではない"}`);
    default: return fail(`未知の op: ${a.op}`);
  }
}

/** probe の結果(エンジンの応答)とトレースの要約から採点する。純関数。 */
export function scoreTask(task: Task, probes: ProbeResult[], summary: TraceSummary): Score {
  const checks: CheckResult[] = [];
  for (const p of task.probes) {
    const pr = probes.find((x) => x.id === p.id);
    if (!pr) { checks.push({ id: `${p.id}`, ok: false, detail: "probe の結果が無い" }); continue; }
    if (p.expectError) {
      checks.push({ id: `${p.id}: エラーで返る`, ok: !pr.ok, detail: pr.ok ? "成功してしまった(あってはいけない)" : "エラー(期待どおり)" });
      continue;
    }
    if (!pr.ok) { checks.push({ id: `${p.id}: 応答`, ok: false, detail: `エラー: ${pr.error ?? ""}` }); continue; }
    for (const a of p.asserts) {
      const c = evalAssert({ result: pr.result }, a);
      checks.push({ id: `${p.id}: ${c.id}`, ok: c.ok, detail: c.detail });
    }
  }
  return {
    taskId: task.id,
    pass: checks.length > 0 && checks.every((c) => c.ok),
    checks,
    metrics: { calls: summary.callCount, retries: summary.retryCount, errors: summary.errorCount, maxCalls: task.maxCalls ?? null, byTool: summary.byTool },
  };
}

/** エンジンへ probe を投げる(client は EngineClient 互換: call(method, params))。 */
export async function runProbes(task: Task, client: { call(m: string, p: Record<string, unknown>): Promise<any> }): Promise<ProbeResult[]> {
  const out: ProbeResult[] = [];
  for (const p of task.probes) {
    try { out.push({ id: p.id, ok: true, result: await client.call(p.method, p.params) }); }
    catch (e: any) { out.push({ id: p.id, ok: false, error: String(e?.message ?? e).slice(0, 300) }); }
  }
  return out;
}

function arg(argv: string[], name: string): string | undefined {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : undefined;
}

async function main(argv: string[]) {
  const taskId = arg(argv, "--task");
  const tracePath = arg(argv, "--trace");
  if (!taskId || !tracePath) throw new Error("--task と --trace が必要");
  const task = loadTasks().find((t) => t.id === taskId);
  if (!task) throw new Error(`課題が無い: ${taskId}`);
  let probes: ProbeResult[];
  const saved = arg(argv, "--probes");
  if (saved) probes = JSON.parse(fs.readFileSync(saved, "utf8"));
  else {
    const port = Number(arg(argv, "--port") ?? "8856");
    const { EngineClient } = await import("../engineClient.ts");
    const client = new EngineClient("127.0.0.1", port, 15000);
    try { probes = await runProbes(task, client); } finally { client.close(); }
    const sp = arg(argv, "--save-probes");
    if (sp) fs.writeFileSync(sp, JSON.stringify(probes, null, 2));
  }
  const score = scoreTask(task, probes, summarizeTrace(readTrace(tracePath)));
  const outFile = arg(argv, "--out");
  if (outFile) fs.writeFileSync(outFile, JSON.stringify(score, null, 2));
  console.log(JSON.stringify(score, null, 2));
  process.exitCode = score.pass ? 0 : 1;
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  main(process.argv.slice(2)).catch((e) => { console.error(String(e?.message ?? e)); process.exit(2); });
}
