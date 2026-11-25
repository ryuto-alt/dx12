// W05-36: MCP-14 E2E ランナー。★LLM を呼ばない。課題ごとに新しいプロジェクトとエンジンを用意し、
// 課題文 task_<id>.md と MCP 設定(trace_proxy 経由)を書いて待つ。課題そのものは、メインが起動する
// Claude Code のサブエージェント(サブスクリプション)が実行する。終わったら score_e2e で採点し、エンジンを止める。
//   node run_e2e.ts --out <dir> --task T1|all [--phase prepare|finish|all] [--port 8856] [--build-dir <dir>]
//                   [--ts-dir <MCP サーバ(index.ts)のあるフォルダ>] [--ts-ref <タグ名>] [--timeout-min 20] [--no-engine]
//   prepare: プロジェクト作成 + エンジン起動 + task_<id>.md / mcp.json を書く
//   finish : probe → 採点 → score.json → エンジン停止
//   all    : prepare → DONE ファイル(エージェントが作る)かタイムアウトまで待つ → finish
// ★比較に使う TS の版はタグで固定する(--ts-ref をトレースの meta に残す)。ポートは E2E 専用 8856 だけ。
// ★API キーが環境にあれば起動を拒む(assertNoPaidApiKey)。エンジンは --background / --headless だけ(窓を出さない)。
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertNoPaidApiKey, loadTasks, runProbes, scoreTask } from "./score_e2e.ts";
import type { Task, Score } from "./score_e2e.ts";
import { readTrace, summarizeTrace } from "./trace.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..", "..");

export type RunOpts = {
  out: string; port: number; buildDir: string; tsDir: string; tsRef: string; timeoutMin: number; noEngine: boolean;
};

export const E2E_PORT = 8856;

/** 既定の出力先(リポジトリの外)。 */
export function defaultOut(): string { return path.join(os.homedir(), "dx12-scorecard", "w0", "mcp14"); }

export function taskDir(o: RunOpts, t: Task): string { return path.join(o.out, `task_${t.id}`); }

/** 新しい空のプロジェクト(.dx12proj と空の assets/scripts)を作る。既にあれば消してから作る。 */
export function createProject(dir: string): string {
  const proj = path.join(dir, "project");
  fs.rmSync(proj, { recursive: true, force: true });
  fs.mkdirSync(path.join(proj, "assets"), { recursive: true });
  fs.mkdirSync(path.join(proj, "scripts"), { recursive: true });
  fs.writeFileSync(path.join(proj, "e2e.dx12proj"), JSON.stringify({ assetsDir: "assets", defaultScene: "", lastOpenedScene: "", name: "e2e", scriptsDir: "scripts", version: "0.1.0" }, null, 2));
  return proj;
}

export function mcpConfig(o: RunOpts, t: Task, tracePath: string) {
  return {
    mcpServers: {
      dx12: {
        command: "node",
        args: [path.join(here, "trace_proxy.ts"), "--trace", tracePath, "--meta", `task=${t.id}`, "--meta", `tsRef=${o.tsRef}`, "--",
          "node", path.join(o.tsDir, "index.ts")],
        env: { DX12_MCP_PORT: String(o.port) },
      },
    },
  };
}

export function taskMarkdown(o: RunOpts, t: Task, dir: string): string {
  return [
    `# 課題 ${t.id}: ${t.title}`,
    "",
    "## やること",
    t.prompt,
    "",
    "## 条件",
    `- 使うのは MCP サーバ dx12(同じフォルダの mcp.json)のツールだけ。エンジンは MCP ポート ${o.port} で起動済み(窓は出ない)。`,
    "- 実マウス・実キーボード・フォーカスを動かす操作はしない。ファイルを直接書き換えてシーンを作らない。",
    "- 終わったら、このフォルダに空のファイル `DONE` を作る。課題が不可能だと判断した場合も `DONE` を作り、理由を `NOTE.txt` に書く。",
    `- 呼び出しの上限の目安は ${t.maxCalls ?? "なし"} 回(超えても失格ではない)。合否はエンジンの状態だけで決まる。`,
    "",
    `作業フォルダ: ${dir}`,
    "",
  ].join("\n");
}

function engineScript(): string { return path.join(repoRoot, "tools", "engine_instance.ps1"); }
export const engineName = "mcp14-e2e";

function startEngine(o: RunOpts, project: string) {
  const args = ["-NoProfile", "-File", engineScript(), "-Name", engineName, "-Port", String(o.port), "-Project", project, "-Mode", "headless", "-Refresh"];
  if (o.buildDir) args.push("-BuildDir", o.buildDir);
  const r = spawnSync("pwsh", args, { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`エンジンを起動できない: ${(r.stderr || r.stdout || "").slice(0, 400)}`);
}
function stopEngine() { spawnSync("pwsh", ["-NoProfile", "-File", engineScript(), "-Name", engineName, "-Stop"], { encoding: "utf8" }); }

async function waitEngine(port: number, timeoutMs = 60000) {
  const { EngineClient } = await import("../engineClient.ts");
  const t0 = Date.now();
  for (;;) {
    const c = new EngineClient("127.0.0.1", port, 3000, { backoffMs: [] });
    try { await c.call("ping", {}, { retry: false }); c.close(); return; } catch { c.close(); }
    if (Date.now() - t0 > timeoutMs) throw new Error("エンジンが応答しない");
    await new Promise((r) => setTimeout(r, 1000));
  }
}

export async function prepare(o: RunOpts, t: Task): Promise<{ dir: string; trace: string }> {
  assertNoPaidApiKey();
  const dir = taskDir(o, t);
  fs.mkdirSync(dir, { recursive: true });
  fs.rmSync(path.join(dir, "DONE"), { force: true });
  fs.rmSync(path.join(dir, `trace_${t.id}.jsonl`), { force: true });
  const project = createProject(dir);
  const trace = path.join(dir, `trace_${t.id}.jsonl`);
  fs.writeFileSync(path.join(dir, `task_${t.id}.md`), taskMarkdown(o, t, dir));
  fs.writeFileSync(path.join(dir, "mcp.json"), JSON.stringify(mcpConfig(o, t, trace), null, 2));
  if (!o.noEngine) { startEngine(o, project); await waitEngine(o.port); }
  return { dir, trace };
}

export async function finish(o: RunOpts, t: Task): Promise<Score> {
  const dir = taskDir(o, t);
  const trace = path.join(dir, `trace_${t.id}.jsonl`);
  const { EngineClient } = await import("../engineClient.ts");
  const client = new EngineClient("127.0.0.1", o.port, 15000);
  let probes;
  try { probes = await runProbes(t, client); } finally { client.close(); }
  fs.writeFileSync(path.join(dir, `probes_${t.id}.json`), JSON.stringify(probes, null, 2));
  const summary = summarizeTrace(fs.existsSync(trace) ? readTrace(trace) : []);
  const score = scoreTask(t, probes, summary);
  fs.writeFileSync(path.join(dir, `score_${t.id}.json`), JSON.stringify(score, null, 2));
  if (!o.noEngine) stopEngine();
  return score;
}

async function waitDone(o: RunOpts, t: Task) {
  const done = path.join(taskDir(o, t), "DONE");
  const t0 = Date.now();
  while (!fs.existsSync(done)) {
    if (Date.now() - t0 > o.timeoutMin * 60000) return false;
    await new Promise((r) => setTimeout(r, 2000));
  }
  return true;
}

function arg(argv: string[], name: string, def?: string): string | undefined {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : def;
}

async function main(argv: string[]) {
  assertNoPaidApiKey();
  const o: RunOpts = {
    out: arg(argv, "--out", defaultOut())!,
    port: Number(arg(argv, "--port", String(E2E_PORT))),
    buildDir: arg(argv, "--build-dir", "")!,
    tsDir: arg(argv, "--ts-dir", path.resolve(here, ".."))!,
    tsRef: arg(argv, "--ts-ref", "unpinned")!,
    timeoutMin: Number(arg(argv, "--timeout-min", "20")),
    noEngine: argv.includes("--no-engine"),
  };
  if (o.port !== E2E_PORT && !argv.includes("--allow-port")) throw new Error(`E2E 専用ポートは ${E2E_PORT} だけ(別のポートは --allow-port)`);
  const phase = arg(argv, "--phase", "all")!;
  const which = arg(argv, "--task", "all")!;
  const tasks = loadTasks().filter((t) => which === "all" || t.id === which);
  if (tasks.length === 0) throw new Error(`課題が無い: ${which}`);
  const results: Score[] = [];
  for (const t of tasks) {
    if (phase === "prepare" || phase === "all") {
      const p = await prepare(o, t);
      console.log(`[run_e2e] ${t.id} 準備完了: ${p.dir}`);
    }
    if (phase === "all") {
      const ok = await waitDone(o, t);
      if (!ok) console.log(`[run_e2e] ${t.id} タイムアウト(${o.timeoutMin} 分)。このまま採点する`);
    }
    if (phase === "finish" || phase === "all") {
      const s = await finish(o, t);
      results.push(s);
      console.log(`[run_e2e] ${t.id} ${s.pass ? "合格" : "不合格"} 呼び出し=${s.metrics.calls} 再試行=${s.metrics.retries} エラー=${s.metrics.errors}`);
    }
  }
  if (results.length > 0) console.log(`[run_e2e] 合格 ${results.filter((r) => r.pass).length}/${results.length}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  main(process.argv.slice(2)).catch((e) => { console.error(String(e?.message ?? e)); process.exit(2); });
}
