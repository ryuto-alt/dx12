// W05-36: 記録したトレースを新しいエンジンに再生して採点し、元の採点と同じになることを確かめる。LLM は呼ばない。
//   node replay_e2e.ts --task T1 --trace <trace.jsonl> --expect <score_T1.json> [--port 8856] [--ts-dir <dir>]
//                      [--project <プロジェクトの場所(新しいエンジンに --project で渡したもの)>]
// 新しいエンジンは、課題と同じ手順(run_e2e.ts --phase prepare)で用意しておく。再生はトレースの tools/call を
// 順に MCP サーバ(index.ts を stdio で起動)へ投げ直し、新しいトレースを <trace>.replay.jsonl に書く。
// 同じ採点 = 合否・各チェックの合否・呼び出し数・再試行数・エラー数が一致すること。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { assertNoPaidApiKey, loadTasks, runProbes, scoreTask } from "./score_e2e.ts";
import type { Score } from "./score_e2e.ts";
import { readTrace, summarizeTrace, TraceWriter } from "./trace.ts";

const here = path.dirname(fileURLToPath(import.meta.url));

export type ReplayCall = { name: string; args: any };

/** トレースから tools/call を順に取り出す。 */
export function extractCalls(records: ReturnType<typeof readTrace>): ReplayCall[] {
  const out: ReplayCall[] = [];
  for (const r of records) {
    if ("dir" in r && r.dir === "c2s" && r.msg?.method === "tools/call") out.push({ name: String(r.msg.params?.name ?? ""), args: r.msg.params?.arguments ?? {} });
  }
  return out;
}

/** 呼び出しを順に callTool へ流し、新しいトレース(JSON-RPC の形)を tw に書く。callTool は MCP の CallToolResult か throw。 */
export async function replayCalls(calls: ReplayCall[], callTool: (name: string, args: any) => Promise<any>, tw: TraceWriter): Promise<void> {
  let id = 1;
  for (const c of calls) {
    const rid = id++;
    tw.message("c2s", { jsonrpc: "2.0", id: rid, method: "tools/call", params: { name: c.name, arguments: c.args } });
    try {
      const result = await callTool(c.name, c.args);
      tw.message("s2c", { jsonrpc: "2.0", id: rid, result });
    } catch (e: any) {
      tw.message("s2c", { jsonrpc: "2.0", id: rid, error: { code: -32000, message: String(e?.message ?? e).slice(0, 500) } });
    }
  }
}

/** 2 つの採点が同じか。違えば理由の配列を返す(空なら同じ)。 */
export function diffScores(a: Score, b: Score): string[] {
  const d: string[] = [];
  if (a.taskId !== b.taskId) d.push(`taskId: ${a.taskId} / ${b.taskId}`);
  if (a.pass !== b.pass) d.push(`pass: ${a.pass} / ${b.pass}`);
  if (a.checks.length !== b.checks.length) d.push(`checks の数: ${a.checks.length} / ${b.checks.length}`);
  a.checks.forEach((c, i) => { const o = b.checks[i]; if (o && (c.id !== o.id || c.ok !== o.ok)) d.push(`check[${i}] ${c.id}: ${c.ok} / ${o.ok}`); });
  for (const k of ["calls", "retries", "errors"] as const) if (a.metrics[k] !== b.metrics[k]) d.push(`${k}: ${a.metrics[k]} / ${b.metrics[k]}`);
  return d;
}

function arg(argv: string[], name: string, def?: string): string | undefined {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : def;
}

async function main(argv: string[]) {
  assertNoPaidApiKey();
  const taskId = arg(argv, "--task");
  const tracePath = arg(argv, "--trace");
  const expectPath = arg(argv, "--expect");
  if (!taskId || !tracePath || !expectPath) throw new Error("--task / --trace / --expect が必要");
  const port = Number(arg(argv, "--port", "8856"));
  const tsDir = arg(argv, "--ts-dir", path.resolve(here, ".."))!;
  const task = loadTasks().find((t) => t.id === taskId);
  if (!task) throw new Error(`課題が無い: ${taskId}`);
  const calls = extractCalls(readTrace(tracePath));
  const { Client } = await import("@modelcontextprotocol/sdk/client/index.js");
  const { StdioClientTransport } = await import("@modelcontextprotocol/sdk/client/stdio.js");
  const env: Record<string, string> = {};
  for (const [k, v] of Object.entries(process.env)) if (typeof v === "string") env[k] = v;
  env["DX12_MCP_PORT"] = String(port);
  const transport = new StdioClientTransport({ command: process.execPath, args: [path.join(tsDir, "index.ts")], env });
  const client = new Client({ name: "replay_e2e", version: "1" }, { capabilities: {} });
  await client.connect(transport);
  const replayTrace = tracePath.replace(/\.jsonl$/, "") + ".replay.jsonl";
  fs.rmSync(replayTrace, { force: true });
  const tw = new TraceWriter(replayTrace);
  tw.meta({ replayOf: path.basename(tracePath), task: taskId });
  try {
    await replayCalls(calls, async (name, args) => await client.callTool({ name, arguments: args }, undefined, { timeout: 120000 }), tw);
  } finally { tw.close(); await client.close(); }
  const { EngineClient } = await import("../engineClient.ts");
  const ec = new EngineClient("127.0.0.1", port, 15000);
  let probes;
  try { probes = await runProbes(task, ec); } finally { ec.close(); }
  const score = scoreTask(task, probes, summarizeTrace(readTrace(replayTrace)));
  const expected = JSON.parse(fs.readFileSync(expectPath, "utf8")) as Score;
  const diff = diffScores(expected, score);
  fs.writeFileSync(replayTrace.replace(/\.jsonl$/, ".score.json"), JSON.stringify(score, null, 2));
  if (diff.length === 0) console.log(`[replay_e2e] ${taskId} 同じ採点(合否=${score.pass}, 呼び出し=${score.metrics.calls})`);
  else { console.log(`[replay_e2e] ${taskId} 採点が食い違う:\n  ${diff.join("\n  ")}`); process.exitCode = 1; }
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  main(process.argv.slice(2)).catch((e) => { console.error(String(e?.message ?? e)); process.exit(2); });
}
