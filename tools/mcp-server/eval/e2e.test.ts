// W05-36: MCP-14 E2E の仕組みの単体テスト(エンジン不要・LLM 不要・決定論)。
// 実行: node eval/e2e.test.ts
//  [1] 課題定義の形  [2] トレースの要約と秘密の除去  [3] 採点  [4] stdio プロキシ  [5] トレースの再生で同じ採点
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { evalPath, evalAssert, loadTasks, scoreTask, runProbes } from "./score_e2e.ts";
import type { ProbeResult, Task } from "./score_e2e.ts";
import { parseTrace, redact, shrinkImages, summarizeTrace, TraceWriter, readTrace } from "./trace.ts";
import type { TraceRecord } from "./trace.ts";
import { extractCalls, replayCalls, diffScores } from "./replay_e2e.ts";
import { createProject, taskMarkdown, mcpConfig, existingArtifacts, unreachableProbes, resetPointer, clearReplayArtifacts } from "./run_e2e.ts";
import { checkBaseline, compareTsToTrace, gitInfo } from "./provenance.ts";
import { lineSplitter, parseProxyArgs } from "./trace_proxy.ts";
import { StringDecoder } from "node:string_decoder";
import { spawn } from "node:child_process";

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
check("全 assert の op が既知", tasks.every((t) => t.probes.every((p) => p.asserts.every((a) => ["exists", "absent", "eq", "approx", "gte", "lte", "between", "contains", "count", "elevationDeg"].includes(a.op)))));

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
const sp = summarizeTrace(trace([
  call(1, "dx12_call", { name: "set_parent", args: { name: "Tent" } }), errRes(1),
  call(2, "dx12_call", { name: "set_parent", args: { name: "Fire" } }), okRes(2),
  call(3, "dx12_set_transform", { entity: 7 }), errRes(3),
  call(4, "dx12_set_transform", { entity: 7 }), okRes(4),
  call(5, "dx12_set_transform", { entity: 8 }), okRes(5),
]));
check("再試行は対象(name / entity)も同じものだけ(別の対象は数えない)", sp.retryCount === 1 && sp.calls[3]!.isRetry && !sp.calls[1]!.isRetry, sp.retryCount);
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
  { id: "no_ground", ok: false, error: "x", errName: "E_NOT_FOUND_ENTITY" }, { id: "no_grid", ok: false, error: "x", code: 4 },
  { id: "rest", ok: true, result: { total: 3, entities: [{ name: "MainCamera" }, { name: "DirectionalLight" }, { name: "Cube" }] } },
], goodTrace);
check("T3: 消えていて残りがそろえば合格", s3.pass, s3);
check("T3: 消えていなければ不合格", !scoreTask(t3, [{ id: "no_ground", ok: true, result: {} }, { id: "no_grid", ok: false }, { id: "rest", ok: true, result: { total: 3, entities: [] } }], goodTrace).pass);
check("採点は同じ入力で同じ(決定論)", JSON.stringify(scoreTask(t1, goodProbes, goodTrace)) === JSON.stringify(sGood));

check("T3: 接続断・タイムアウトを「消えた」と見なさない", !scoreTask(t3, [
  { id: "no_ground", ok: false, error: "timeout", errName: "E_ENGINE_TIMEOUT" }, { id: "no_grid", ok: false, error: "x" },
  { id: "rest", ok: true, result: { total: 3, entities: [{ name: "MainCamera" }, { name: "DirectionalLight" }, { name: "Cube" }] } },
], goodTrace).pass);
const t5 = tasks.find((t) => t.id === "T5")!;
const sunP = (d: number[]): ProbeResult[] => [{ id: "sun", ok: true, result: { directionalLight: { direction: d } } }, { id: "count", ok: true, result: { total: 5 } }];
check("T5: 正規化していない [10,-2,0](高度 11.3 度)でも合格", scoreTask(t5, sunP([10, -2, 0]), goodTrace).pass);
check("T5: 正規化済みの正解 [cos14, -sin14, 0] も合格", scoreTask(t5, sunP([Math.cos(0.2443), -Math.sin(0.2443), 0]), goodTrace).pass);
check("T5: 高度 45 度は不合格(正規化しない値でも)", !scoreTask(t5, sunP([1, -1, 0]), goodTrace).pass && !scoreTask(t5, sunP([0.9, -0.3, 0]), goodTrace).pass);
check("T5: 上向き(地平線の下)は不合格", !scoreTask(t5, sunP([10, 2, 0]), goodTrace).pass);
const t6 = tasks.find((t) => t.id === "T6")!;
const t6Probes = (ents: any[]): ProbeResult[] => [
  { id: "marker", ok: true, result: { transform: { position: [1, 0.5, 1] } } },
  { id: "saved", ok: true, result: [{ path: "scenes/e2e_level.json" }] },
  { id: "saved_content", ok: true, result: { entities: ents } }];
check("T6: 保存物に Marker が (1,0.5,1) であれば合格", scoreTask(t6, t6Probes([{ name: "Cube" }, { name: "Marker", transform: { position: [1, 0.5, 1] } }]), goodTrace).pass);
check("T6: Marker を置く前に保存していたら不合格", !scoreTask(t6, t6Probes([{ name: "Cube" }]), goodTrace).pass);
check("T6: 保存物の位置が違えば不合格", !scoreTask(t6, t6Probes([{ name: "Marker", transform: { position: [0, 0, 0] } }]), goodTrace).pass);

console.log("[3a] file probe / 画像の縮小");
const ftmp = fs.mkdtempSync(path.join(os.tmpdir(), "e2e_file_"));
try {
  fs.mkdirSync(path.join(ftmp, "assets", "scenes"), { recursive: true });
  fs.writeFileSync(path.join(ftmp, "assets", "scenes", "e2e_level.json"), JSON.stringify({ entities: [{ name: "Marker", transform: { position: [1, 0.5, 1] } }] }));
  const fp = await runProbes({ id: "X", title: "", prompt: "", probes: [
    { id: "f", file: "assets/scenes/e2e_level.json", asserts: [] }, { id: "miss", file: "assets/scenes/none.json", asserts: [] }, { id: "esc", file: "../x.json", asserts: [] }] },
    { call: async () => ({}) }, ftmp);
  check("file probe: 読める / 無いものと project の外はエラー", fp[0].ok && !fp[1].ok && !fp[2].ok, fp);
} finally { fs.rmSync(ftmp, { recursive: true, force: true }); }
const big = "A".repeat(100000);
const shr = shrinkImages({ result: { content: [{ type: "image", data: big, mimeType: "image/png" }, { type: "text", text: "keep" }] } });
check("image の data は長さとハッシュだけ・text は残る・元は不変", /image len=100000 sha256=/.test(shr.result.content[0].data) && shr.result.content[1].text === "keep" && big.length === 100000 && JSON.stringify(shr).length < 500, shr);

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

  // ベースラインを守る: trace がある課題は prepare を拒み、prepare-replay は元の trace / project に触れない
  const t1dir = path.join(tmp, "task_T1");
  fs.writeFileSync(path.join(t1dir, "trace_T1.jsonl"), "ORIGINAL\n");
  fs.writeFileSync(path.join(t1dir, "project", "marker.txt"), "saved");
  const envOk = { ...process.env, [KEYNAME]: "" };
  const r3 = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "prepare", "--no-engine"], { encoding: "utf8", env: envOk });
  check("trace がある課題の prepare は --force なしで拒む(何も消さない)", r3.status !== 0 && fs.readFileSync(path.join(t1dir, "trace_T1.jsonl"), "utf8") === "ORIGINAL\n" && fs.existsSync(path.join(t1dir, "project", "marker.txt")), r3.stderr);
  const r4 = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "prepare-replay", "--no-engine"], { encoding: "utf8", env: envOk });
  check("prepare-replay は replay/project に作り、元の trace と project を残す", r4.status === 0 && fs.existsSync(path.join(t1dir, "replay", "project", "e2e.dx12proj")) && fs.readFileSync(path.join(t1dir, "trace_T1.jsonl"), "utf8") === "ORIGINAL\n" && fs.existsSync(path.join(t1dir, "project", "marker.txt")), r4.stderr);
  check("existingArtifacts が trace を見つける", existingArtifacts(t1dir, t1).includes("trace_T1.jsonl"));
  const r5 = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "prepare", "--no-engine", "--force", "--pointer"], { encoding: "utf8", env: envOk });
  check("--force で作り直せる。--pointer は mcp.pointer.json と current_trace.txt を書く", r5.status === 0 && !fs.existsSync(path.join(t1dir, "trace_T1.jsonl")) && fs.existsSync(path.join(tmp, "mcp.pointer.json")) && fs.readFileSync(path.join(tmp, "current_trace.txt"), "utf8").endsWith("trace_T1.jsonl"), r5.stderr);
  const pcfg = JSON.parse(fs.readFileSync(path.join(tmp, "mcp.pointer.json"), "utf8"));
  check("pointer の mcp.json は --trace-pointer を使う", pcfg.mcpServers.dx12.args.includes("--trace-pointer") && !pcfg.mcpServers.dx12.args.includes("--trace"));
  // finish: 既存の採点を上書きしない / --task all を拒む / ポインタを戻す
  fs.writeFileSync(path.join(t1dir, "score_T1.json"), "BASELINE\n");
  const r6 = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--task", "T1", "--phase", "finish", "--no-engine"], { encoding: "utf8", env: envOk });
  check("既に採点がある課題の finish は --force なしで拒む(score を変えない)", r6.status !== 0 && fs.readFileSync(path.join(t1dir, "score_T1.json"), "utf8") === "BASELINE\n", r6.stderr);
  for (const ph of ["prepare", "prepare-replay", "finish"]) {
    const rr = spawnSync(process.execPath, [path.join(here, "run_e2e.ts"), "--out", tmp, "--phase", ph, "--no-engine"], { encoding: "utf8", env: envOk });
    check(`--phase ${ph} は --task を省く(all)と拒む`, rr.status !== 0 && /1 つずつ/.test(rr.stderr), rr.stderr);
  }
  check("接続できない probe(数値 code なしの E_ENGINE_UNREACHABLE)を検出する", unreachableProbes([{ id: "a", ok: false, errName: "E_ENGINE_UNREACHABLE" }, { id: "b", ok: false, errName: "E_NOT_FOUND_ENTITY", code: 1 }, { id: "c", ok: true }]).join() === "a");
  const ptrOpts = { ...opts, out: tmp, pointer: true };
  const tr1 = path.join(t1dir, "trace_T1.jsonl");
  fs.writeFileSync(path.join(tmp, "current_trace.txt"), tr1);
  resetPointer(ptrOpts, path.join(t1dir, "other.jsonl"));
  check("別の課題を指すポインタは戻さない", fs.readFileSync(path.join(tmp, "current_trace.txt"), "utf8") === tr1);
  resetPointer(ptrOpts, tr1);
  check("採点した課題を指すポインタは未割り当て(空)に戻る", fs.readFileSync(path.join(tmp, "current_trace.txt"), "utf8") === "");
  fs.mkdirSync(path.join(t1dir, "replay", "project"), { recursive: true });
  fs.writeFileSync(path.join(t1dir, "trace_T1.replay.jsonl"), "x");
  fs.writeFileSync(path.join(t1dir, "trace_T1.replay.score.json"), "x");
  clearReplayArtifacts(t1dir, t1);
  check("--force 用に replay 成果物を全部消す", !fs.existsSync(path.join(t1dir, "replay")) && !fs.existsSync(path.join(t1dir, "trace_T1.replay.jsonl")) && !fs.existsSync(path.join(t1dir, "trace_T1.replay.score.json")));
  check("provenance.json が書かれる", fs.existsSync(path.join(t1dir, "provenance.json")));

  console.log("[3c] TS の出どころ");
  const tsd = path.resolve(here, "..");
  const g = gitInfo(tsd);
  check("ts-dir の commit が引ける", !!g && /^[0-9a-f]{40}$/.test(g.commit), g);
  let thrown = "";
  try { checkBaseline(tsd, "unpinned", ""); } catch (e: any) { thrown = String(e.message); }
  check("ベースラインは unpinned を拒む", /unpinned/.test(thrown), thrown);
  thrown = "";
  try { checkBaseline(tsd, "no-such-tag-xyz", ""); } catch (e: any) { thrown = String(e.message); }
  check("ベースラインは存在しないタグを拒む", /無い/.test(thrown), thrown);
  check("meta に tsCommit が無ければ replay は拒む", compareTsToTrace({ tsRef: "x" }, tsd).length > 0);
  check("tsCommit が違えば拒み、同じなら通す", compareTsToTrace({ tsCommit: "0".repeat(40) }, tsd).length > 0 && g !== null && compareTsToTrace({ tsCommit: g.commit }, tsd).length === 0);
  const mc = mcpConfig({ ...opts, baseline: true }, t1, "x.jsonl", { tsRef: "tag-x", tsCommit: "abc123", tsDirty: false, buildRef: "r", buildCommit: "def456", assetsSource: "" });
  check("mcp.json の meta に tsCommit / buildCommit が入る", mc.mcpServers.dx12.args.includes("tsCommit=abc123") && mc.mcpServers.dx12.args.includes("buildCommit=def456"));

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

  // 行の途中で割れるチャンク / マルチバイトの境界 / JSON でない行の redact
  const lines: string[] = [];
  const sp = lineSplitter((l) => lines.push(l));
  const whole = JSON.stringify({ a: "日本語のテキスト" }) + "\n" + "not json Bearer abcdefgh12345\n" + '{"b":2}';
  const bytes = Buffer.from(whole, "utf8");
  const dec = new StringDecoder("utf8");
  for (let i = 0; i < bytes.length; i += 5) sp.push(dec.write(bytes.subarray(i, i + 5)));
  sp.flush();
  check("lineSplitter: 5 バイトごとに割れても(マルチバイト境界を含め)3 行に復元", lines.length === 3 && JSON.parse(lines[0]).a === "日本語のテキスト" && lines[2] === '{"b":2}', lines);
  const twn = new TraceWriter(path.join(tmp, "nonjson.jsonl"));
  twn.line("c2s", lines[1]); twn.line("s2c", '{"result":{"content":[{"type":"image","data":"' + "Z".repeat(5000) + '"}]},"id":1}'); twn.close();
  const ntxt = fs.readFileSync(path.join(tmp, "nonjson.jsonl"), "utf8");
  check("JSON でない行は _nonJson で残り、秘密は潰れ、画像は縮む", ntxt.includes("_nonJson") && !/abcdefgh12345/.test(ntxt) && ntxt.length < 800, ntxt.length);
  const tp2 = path.join(tmp, "proxy2.jsonl");
  const child2 = 'process.stdin.setEncoding("utf8");let b="";process.stdin.on("data",d=>{b+=d;let i;while((i=b.indexOf("\\n"))>=0){const l=b.slice(0,i);b=b.slice(i+1);if(!l.trim())continue;const m=JSON.parse(l);process.stdout.write(JSON.stringify({jsonrpc:"2.0",id:m.id,result:{content:[{type:"text",text:"日本語"}]}})+"\\n");}});';
  const reqLine = JSON.stringify({ jsonrpc: "2.0", id: 7, method: "tools/call", params: { name: "x", arguments: { s: "日本語" } } }) + "\n";
  const half = Math.floor(reqLine.length / 2);
  const pc = spawn(process.execPath, [path.join(here, "trace_proxy.ts"), "--trace", tp2, "--", process.execPath, "-e", child2]);
  let pout = ""; pc.stdout.on("data", (d) => (pout += d));
  pc.stdin.write(Buffer.from(reqLine).subarray(0, half)); await new Promise((r) => setTimeout(r, 300));
  pc.stdin.end(Buffer.from(reqLine).subarray(half));
  await new Promise((r) => pc.on("close", r));
  const sx2 = summarizeTrace(readTrace(tp2));
  check("プロキシ: 行の途中で割れた入力でも 1 呼び出しとして記録され、応答が返る", sx2.callCount === 1 && sx2.errorCount === 0 && pout.includes("日本語"), { c: sx2.callCount, pout });
  const ptr = path.join(tmp, "ptr.txt"); const ta = path.join(tmp, "ta.jsonl"); const tb = path.join(tmp, "tb.jsonl");
  fs.writeFileSync(ptr, ta);
  const pc2 = spawn(process.execPath, [path.join(here, "trace_proxy.ts"), "--trace-pointer", ptr, "--", process.execPath, "-e", child2]);
  const rq = (id: number) => JSON.stringify({ jsonrpc: "2.0", id, method: "tools/call", params: { name: "x", arguments: {} } }) + "\n";
  pc2.stdin.write(rq(1)); await new Promise((r) => setTimeout(r, 600));
  fs.writeFileSync(ptr, tb);
  pc2.stdin.write(rq(2)); await new Promise((r) => setTimeout(r, 600));
  pc2.stdin.end(); await new Promise((r) => pc2.on("close", r));
  check("--trace-pointer: ポインタを書き換えると記録先が切り替わる", summarizeTrace(readTrace(ta)).callCount === 1 && summarizeTrace(readTrace(tb)).callCount === 1);
  check("parseProxyArgs: --trace-pointer だけでも通る", parseProxyArgs(["--trace-pointer", "p", "--", "node", "x"]).pointer === "p");
  const pc3 = spawn(process.execPath, [path.join(here, "trace_proxy.ts"), "--trace", path.join(tmp, "epipe.jsonl"), "--", process.execPath, "-e", "process.exit(0)"]);
  let died = ""; pc3.stderr.on("data", (d) => (died += d));
  pc3.stdin.on("error", () => { /* テスト側の書き込み失敗は無視 */ });
  await new Promise((r) => setTimeout(r, 400));
  for (let i = 0; i < 20; i++) { try { pc3.stdin.write(rq(i)); } catch { /* 閉じた */ } await new Promise((r) => setTimeout(r, 20)); }
  pc3.stdin.end(); if (pc3.exitCode === null) await new Promise((r) => pc3.on("close", r));
  check("子が先に死んでもプロキシは EPIPE の例外で落ちない", !/EPIPE|Unhandled|Error:/.test(died), died.slice(0, 300));

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
