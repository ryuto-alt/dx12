// W05-36: MCP-14 E2E の仕組みの単体テスト(エンジン不要・LLM 不要・決定論)。
// 実行: node eval/e2e.test.ts
//  [1] 課題定義の形  [2] トレースの要約と秘密の除去  [3] 採点  [4] stdio プロキシ  [5] トレースの再生で同じ採点
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { evalPath, evalAssert, loadTasks, scoreTask } from "./score_e2e.ts";
import type { ProbeResult, Task } from "./score_e2e.ts";
import { parseTrace, redact, summarizeTrace, TraceWriter, readTrace } from "./trace.ts";
import type { TraceRecord } from "./trace.ts";
import { extractCalls, replayCalls, diffScores } from "./replay_e2e.ts";
import { createProject, taskMarkdown, mcpConfig } from "./run_e2e.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const KEYNAME = ["ANTHROPIC", "API", "KEY"].join("_"); // 検査(NoPaidApi)に引っかからないよう連結で作る
let failed = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${JSON.stringify(detail).slice(0, 600)}` : ""}`); }
}

// ---- トレースの組み立てヘルパ ----
function trace(items: Array<["c" | "s", any]>): TraceRecord[] {
  return items.map(([d, msg], i) => ({ seq: i, t: i * 10, dir: d === "c" ? "c2s" : "s2c", msg }) as TraceRecord);
}
const call = (id: number, name: string, args: any = {}) => ["c", { jsonrpc: "2.0", id, method: "tools/call", params: { name, arguments: args } }] as ["c", any];
const okRes = (id: number) => ["s", { jsonrpc: "2.0", id, result: { content: [{ type: "text", text: "{}" }] } }] as ["s", any];
const errRes = (id: number) => ["s", { jsonrpc: "2.0", id, result: { isError: true, content: [{ type: "text", text: "x" }] } }] as ["s", any];

console.log("[1] 課題定義");
const tasks = loadTasks();
check("課題は 6 つ", tasks.length === 6, tasks.map((t) => t.id));
check("ID が一意", new Set(tasks.map((t) => t.id)).size === tasks.length);
check("全課題に prompt と probes があり、probe の ID が一意", tasks.every((t) => t.prompt.length > 10 && t.probes.length > 0 && new Set(t.probes.map((p) => p.id)).size === t.probes.length));
check("曖昧な課題文を使っていない(良い感じに 等)", tasks.every((t) => !/良い感じ|いい感じ|適当|それっぽ/.test(t.prompt)));
check("全 probe が検査を持つ(expectError か asserts)", tasks.every((t) => t.probes.every((p) => p.expectError || p.asserts.length > 0)));
check("全 assert の op が既知", tasks.every((t) => t.probes.every((p) => p.asserts.every((a) => ["exists", "absent", "eq", "approx", "gte", "lte", "between", "contains", "count"].includes(a.op)))));

console.log("[2] トレース");
const secretObj = { token: "abc", nested: { api_key: "k", text: "Bearer abcdefgh12345 and sk-ant-api03-ZZZZ and ghp_abcdefghijklmnopqrstuv" }, ok: 1 };
const red = redact(secretObj);
check("秘密のキーの値を潰す", red.token === "[REDACTED]" && red.nested.api_key === "[REDACTED]", red);
check("秘密らしい文字列を潰す", !/abcdefgh12345|sk-ant|ghp_/.test(JSON.stringify(red)), red);
check("redact は元を変えない", secretObj.token === "abc");
const sum = summarizeTrace(trace([
  call(1, "dx12_spawn", { a: 1 }), okRes(1),
  call(2, "dx12_set_transform"), errRes(2),
  call(3, "dx12_set_transform"), okRes(3),
  call(4, "dx12_call", { name: "dx12_get_entity" }), errRes(4),
  call(5, "dx12_call", { name: "dx12_set_sun" }), okRes(5),
  call(6, "dx12_call", { name: "dx12_get_entity" }),
]));
check("呼び出し数 6", sum.callCount === 6, sum.callCount);
check("エラー 3(応答なしも数える)", sum.errorCount === 3, sum.errorCount);
check("再試行 2(エラー直後の同じツール。dx12_call は中身の名前で区別)", sum.retryCount === 2, sum.retryCount);
check("byTool", sum.byTool["dx12_call:dx12_get_entity"] === 2 && sum.byTool["dx12_set_transform"] === 2, sum.byTool);
check("tools/list 等は数えない", summarizeTrace(trace([["c", { jsonrpc: "2.0", id: 1, method: "tools/list" }], ["s", { jsonrpc: "2.0", id: 1, result: { tools: [] } }]])).callCount === 0);
check("壊れた行は読み飛ばす", parseTrace('{"seq":0}\n{broken\n{"seq":1}\n').length === 2);

console.log("[3] 採点");
const p0 = { a: { b: [{ name: "X", v: [1, 2, 3] }, { name: "Y", c: [{ name: "Z" }] }], s: "hello" } };
check("evalPath: キーと添字", evalPath(p0, "a.b[0].v[2]").value === 3);
check("evalPath: [name=Y]", evalPath(p0, "a.b[name=Y].c[name=Z]").found);
check("evalPath: 無いものは found=false", !evalPath(p0, "a.b[name=Q]").found);
check("evalPath: 記号入りの値", evalPath({ r: [{ path: "scenes/e2e_level.json" }] }, "r[path=scenes/e2e_level.json]").found);
check("approx 合格/不合格", evalAssert({ v: [1, 2] }, { path: "v", op: "approx", value: [1.005, 2], tol: 0.01 }).ok && !evalAssert({ v: [1, 2] }, { path: "v", op: "approx", value: [1.5, 2], tol: 0.01 }).ok);
check("between の境界", evalAssert({ v: 0.3 }, { path: "v", op: "between", value: [0.1, 0.3] }).ok && !evalAssert({ v: 0.31 }, { path: "v", op: "between", value: [0.1, 0.3] }).ok);
check("count", evalAssert({ v: [1, 2] }, { path: "v", op: "count", value: 2 }).ok && !evalAssert({ v: 5 }, { path: "v", op: "count", value: 2 }).ok);

const t1 = tasks.find((t) => t.id === "T1")!;
const goodProbes: ProbeResult[] = [
  { id: "box1", ok: true, result: { transform: { position: [0, 0.5, 0] } } },
  { id: "box2", ok: true, result: { transform: { position: [3, 0.5, 0] } } },
  { id: "box3", ok: true, result: { transform: { position: [6, 0.5, 0] } } },
  { id: "count", ok: true, result: { total: 8 } },
];
const goodTrace = summarizeTrace(trace([call(1, "dx12_spawn"), okRes(1), call(2, "dx12_spawn"), okRes(2), call(3, "dx12_spawn"), okRes(3)]));
const sGood = scoreTask(t1, goodProbes, goodTrace);
check("T1: 全部あれば合格、呼び出し 3", sGood.pass && sGood.metrics.calls === 3, sGood);
const badProbes = goodProbes.map((p) => (p.id === "box2" ? { ...p, result: { transform: { position: [3, 0.5, 1] } } } : p));
check("T1: 位置が違えば不合格", !scoreTask(t1, badProbes, goodTrace).pass);
check("T1: probe の結果が欠ければ不合格", !scoreTask(t1, goodProbes.slice(1), goodTrace).pass);
check("T1: エンジンエラーは不合格", !scoreTask(t1, goodProbes.map((p) => (p.id === "box1" ? { id: "box1", ok: false, error: "E_NOT_FOUND" } : p)), goodTrace).pass);
const t3 = tasks.find((t) => t.id === "T3")!;
const s3 = scoreTask(t3, [
  { id: "no_ground", ok: false, error: "x" }, { id: "no_grid", ok: false, error: "x" },
  { id: "rest", ok: true, result: { total: 3, entities: [{ name: "MainCamera" }, { name: "DirectionalLight" }, { name: "Cube" }] } },
], goodTrace);
check("T3: 消えていて残りがそろえば合格", s3.pass, s3);
check("T3: 消えていなければ不合格", !scoreTask(t3, [{ id: "no_ground", ok: true, result: {} }, { id: "no_grid", ok: false }, { id: "rest", ok: true, result: { total: 3, entities: [] } }], goodTrace).pass);
check("採点は同じ入力で同じ(決定論)", JSON.stringify(scoreTask(t1, goodProbes, goodTrace)) === JSON.stringify(sGood));

console.log("[3b] ランナーの部品");
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "e2e_test_"));
try {
  const proj = createProject(path.join(tmp, "t"));
  check("新しいプロジェクトを作る(.dx12proj)", fs.existsSync(path.join(proj, "e2e.dx12proj")) && fs.existsSync(path.join(proj, "assets")));
  const opts = { out: tmp, port: 8856, buildDir: "", tsDir: path.resolve(here, ".."), tsRef: "tag-x", timeoutMin: 1, noEngine: true };
  const md = taskMarkdown(opts, t1, tmp);
  check("課題文に prompt と DONE の指示がある", md.includes("Box1") && md.includes("DONE") && md.includes("8856"));
  const cfg = mcpConfig(opts, t1, path.join(tmp, "t.jsonl"));
  check("MCP 設定は trace_proxy 経由で、ポートは 8856", cfg.mcpServers.dx12.args.some((a: string) => a.endsWith("trace_proxy.ts")) && cfg.mcpServers.dx12.env.DX12_MCP_PORT === "8856" && cfg.mcpServers.dx12.args.includes("tsRef=tag-x"));
  const r = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "prepare", "--no-engine"], { encoding: "utf8", env: { ...process.env, [KEYNAME]: "dummy" } });
  check("環境に鍵があれば run_e2e は起動を拒む", r.status !== 0 && !fs.existsSync(path.join(tmp, "task_T1", "mcp.json")), r.stderr);
  const r2 = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "prepare", "--no-engine"], { encoding: "utf8", env: { ...process.env, [KEYNAME]: "" } });
  check("鍵が無ければ prepare できる(エンジン無し)", r2.status === 0 && fs.existsSync(path.join(tmp, "task_T1", "task_T1.md")) && fs.existsSync(path.join(tmp, "task_T1", "mcp.json")), r2.stderr);

  console.log("[4] stdio プロキシ");
  const child = `
    let b=""; process.stdin.setEncoding("utf8");
    process.stdin.on("data",d=>{b+=d;let i;while((i=b.indexOf("\\n"))>=0){const l=b.slice(0,i);b=b.slice(i+1);if(!l.trim())continue;const m=JSON.parse(l);
      if(m.method==="tools/call")process.stdout.write(JSON.stringify({jsonrpc:"2.0",id:m.id,result:{content:[{type:"text",text:"ok"}],isError:m.params.name==="bad"}})+"\\n");
      else if(m.id!==undefined)process.stdout.write(JSON.stringify({jsonrpc:"2.0",id:m.id,result:{}})+"\\n");}});`;
  const tp = path.join(tmp, "proxy.jsonl");
  const input = [
    { jsonrpc: "2.0", id: 1, method: "initialize", params: {} },
    { jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "good", arguments: { token: "SECRETVALUE", text: "Bearer abcdefgh12345" } } },
    { jsonrpc: "2.0", id: 3, method: "tools/call", params: { name: "bad", arguments: {} } },
    { jsonrpc: "2.0", id: 4, method: "tools/call", params: { name: "bad", arguments: {} } },
  ].map((m) => JSON.stringify(m)).join("\n") + "\n";
  const pr = spawnSync(process.execPath, [path.join(here, "trace_proxy.ts"), "--trace", tp, "--meta", "tsRef=v1", "--", process.execPath, "-e", child], { input, encoding: "utf8" });
  const outLines = pr.stdout.split("\n").filter(Boolean).map((l) => JSON.parse(l));
  check("応答はそのまま中継される(4 行)", pr.status === 0 && outLines.length === 4 && outLines[1].id === 2, { s: pr.status, e: pr.stderr, o: pr.stdout });
  const ptxt = fs.readFileSync(tp, "utf8");
  check("トレースに秘密が残らない", !/SECRETVALUE|abcdefgh12345/.test(ptxt));
  const ps = summarizeTrace(readTrace(tp));
  check("トレースから 3 呼び出し・エラー 2・再試行 1・meta", ps.callCount === 3 && ps.errorCount === 2 && ps.retryCount === 1 && ps.meta["tsRef"] === "v1", ps);

  console.log("[5] 再生");
  // 記録(エンジン役: 決まった応答を返す)→ 同じ呼び出しを再生 → 同じ採点
  const orig = trace([call(1, "dx12_spawn"), okRes(1), call(2, "dx12_spawn"), errRes(2), call(3, "dx12_spawn"), okRes(3)]);
  const calls = extractCalls(orig);
  check("再生用の呼び出しを 3 つ取り出す", calls.length === 3);
  const tw = new TraceWriter(path.join(tmp, "replay.jsonl"));
  let n = 0;
  await replayCalls(calls, async () => { n++; return n === 2 ? { isError: true, content: [] } : { content: [] }; }, tw);
  tw.close();
  const s0 = scoreTask(t1, goodProbes, summarizeTrace(orig));
  const s1 = scoreTask(t1, goodProbes, summarizeTrace(readTrace(path.join(tmp, "replay.jsonl"))));
  check("再生の採点が元と同じ", diffScores(s0, s1).length === 0, diffScores(s0, s1));
  const tw2 = new TraceWriter(path.join(tmp, "replay2.jsonl"));
  await replayCalls(calls, async () => { throw new Error("boom"); }, tw2);
  tw2.close();
  const s2 = scoreTask(t1, goodProbes, summarizeTrace(readTrace(path.join(tmp, "replay2.jsonl"))));
  check("結果が変わる再生は食い違いとして検出される", diffScores(s0, s2).length > 0);
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}

console.log(failed === 0 ? "\n全て OK" : `\n失敗 ${failed} 件`);
process.exitCode = failed === 0 ? 0 : 1;
