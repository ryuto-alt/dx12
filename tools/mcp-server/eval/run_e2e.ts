// W05-36: MCP-14 E2E ランナー。★LLM を呼ばない。課題ごとに新しいプロジェクトとエンジンを用意し、
// 課題文 task_<id>.md と MCP 設定(trace_proxy 経由)を書いて待つ。課題そのものは、メインが起動する
// Claude Code のサブエージェント(サブスクリプション)が実行する。終わったら score_e2e で採点し、エンジンを止める。
//   node run_e2e.ts --out <dir> --task T1|all [--phase prepare|prepare-replay|finish|stop|all] [--port 8856] [--build-dir <dir>]
//                   [--ts-dir <MCP サーバ(index.ts)のあるフォルダ>] [--ts-ref <タグ名>] [--baseline] [--force] [--pointer]
//                   [--timeout-min 20] [--no-engine]
//   prepare       : プロジェクト作成 + エンジン起動 + task_<id>.md / mcp.json / provenance.json を書く。
//                   既に trace / score / probes がある課題は --force なしでは拒む(ベースラインを消さない)
//   prepare-replay: replay_e2e 用の新しいエンジンを <task>/replay/project に用意する(元の trace / project には触れない)
//   finish        : probe → 採点 → score.json → エンジン停止      stop: エンジンだけ止める
//   ★prepare / prepare-replay / finish は --task を 1 課題ずつ指定する(all は拒む)。finish は既存の score / probes があれば --force なしで拒み、
//     接続できない probe(E_ENGINE_UNREACHABLE)が 1 つでもあれば何も書かずに失敗する。finish / stop は --pointer のポインタを未割り当てに戻す
//   all           : prepare → DONE ファイル(エージェントが作る)かタイムアウトまで待つ → finish
// ★比較に使う TS の版はタグで固定する: --baseline では --ts-ref(タグ名)必須で、--ts-dir の HEAD がそのタグと一致し、
//   作業ツリーが汚れていないことを検査する。実際の commit と --build-dir の nightly.json の ref/commit を provenance.json と
//   mcp.json の meta(=トレースの meta)に残す。ポートは E2E 専用 8856 だけ。
// ★エージェントの起動(サブスクリプション。課題ごとに mcp.json を strict で渡す。親セッションの MCP は使わせない):
//     claude -p "<task_<id>.md を読んで課題をやる>" --mcp-config <task>/mcp.json --strict-mcp-config
//   親セッションの Agent ツールは課題ごとの mcp.json を付けられない。その場合は --pointer を付けて prepare し、
//   <out>/mcp.pointer.json(プロキシが記録先を <out>/current_trace.txt から決める。prepare が課題ごとに書き換える)を 1 回だけ登録する。
// ★API キーが環境にあれば起動を拒む(assertNoPaidApiKey)。エンジンは --background / --headless だけ(窓を出さない)。
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertNoPaidApiKey, loadTasks, runProbes, scoreTask } from "./score_e2e.ts";
import type { Task, Score, ProbeResult } from "./score_e2e.ts";
import { readTrace, summarizeTrace } from "./trace.ts";
import { checkBaseline, collectProvenance } from "./provenance.ts";
import type { Provenance } from "./provenance.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..", "..");

export type RunOpts = {
  out: string; port: number; buildDir: string; tsDir: string; tsRef: string; timeoutMin: number; noEngine: boolean;
  baseline?: boolean; force?: boolean; pointer?: boolean;
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

export function pointerFile(o: RunOpts): string { return path.join(o.out, "current_trace.txt"); }

export function mcpConfig(o: RunOpts, t: Task, tracePath: string, prov?: Provenance) {
  const metaArgs: string[] = [];
  const add = (k: string, v: string) => { if (v) metaArgs.push("--meta", `${k}=${v}`); };
  if (!o.pointer) add("task", t.id);
  add("tsRef", o.tsRef); add("tsCommit", prov?.tsCommit ?? ""); add("buildRef", prov?.buildRef ?? ""); add("buildCommit", prov?.buildCommit ?? "");
  return {
    mcpServers: {
      dx12: {
        command: "node",
        args: [path.join(here, "trace_proxy.ts"), ...(o.pointer ? ["--trace-pointer", pointerFile(o)] : ["--trace", tracePath]), ...metaArgs, "--",
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
    `- 使うのは MCP サーバ dx12(同じフォルダの mcp.json。--strict-mcp-config で渡される)のツールだけ。エンジンは MCP ポート ${o.port} で起動済み(窓は出ない)。`,
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
export function stopEngine() { spawnSync("pwsh", ["-NoProfile", "-File", engineScript(), "-Name", engineName, "-Stop"], { encoding: "utf8" }); }

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

/** 既に結果がある課題を黙って作り直さない(ベースラインのトレースと保存物を守る)。 */
export function existingArtifacts(dir: string, t: Task): string[] {
  return [`trace_${t.id}.jsonl`, `score_${t.id}.json`, `probes_${t.id}.json`].filter((f) => fs.existsSync(path.join(dir, f)));
}

/** 直前の replay の成果物(replay/ 一式、.replay.jsonl、.replay.score.json)を消す(--force で作り直すとき)。 */
export function clearReplayArtifacts(dir: string, t: Task) {
  fs.rmSync(path.join(dir, "replay"), { recursive: true, force: true });
  fs.rmSync(path.join(dir, `trace_${t.id}.replay.jsonl`), { force: true });
  fs.rmSync(path.join(dir, `trace_${t.id}.replay.score.json`), { force: true });
}

/** 接続できなかった probe(数値 code の無い E_ENGINE_UNREACHABLE)の id。1 つでもあれば採点は信用できない。 */
export function unreachableProbes(probes: ProbeResult[]): string[] {
  return probes.filter((p) => !p.ok && p.errName === "E_ENGINE_UNREACHABLE" && p.code === undefined).map((p) => p.id);
}

/** ポインタが指しているのが trace なら(または trace 指定なしなら)未割り当てに戻す。採点済みのトレースに追記されないように。 */
export function resetPointer(o: RunOpts, trace?: string) {
  try {
    const f = pointerFile(o);
    if (!fs.existsSync(f)) return;
    if (trace === undefined || fs.readFileSync(f, "utf8").trim() === trace) fs.writeFileSync(f, "");
  } catch { /* ポインタが書けなくても採点は止めない */ }
}

export async function prepare(o: RunOpts, t: Task, replay = false): Promise<{ dir: string; trace: string }> {
  assertNoPaidApiKey();
  const taskRoot = taskDir(o, t);
  if (!replay) {
    const have = existingArtifacts(taskRoot, t);
    if (have.length > 0 && !o.force) throw new Error(`${t.id}: 既に結果がある(${have.join(", ")})。消さずに拒否する。再生用のエンジンは --phase prepare-replay、作り直すなら --force`);
    if (o.force) clearReplayArtifacts(taskRoot, t); // 古い再生結果を新しいトレースの横に残さない
  }
  if (o.baseline) checkBaseline(o.tsDir, o.tsRef, o.buildDir);
  const dir = replay ? path.join(taskRoot, "replay") : taskRoot;
  fs.mkdirSync(dir, { recursive: true });
  if (!o.noEngine) stopEngine(); // project/ を消す前に古いエンジンを止める(開いたままだと EBUSY)
  const project = createProject(dir);
  const trace = path.join(taskRoot, `trace_${t.id}.jsonl`);
  if (!replay) {
    const prov = collectProvenance(o.tsDir, o.tsRef, o.buildDir);
    fs.rmSync(path.join(dir, "DONE"), { force: true });
    fs.rmSync(trace, { force: true });
    fs.writeFileSync(path.join(dir, "provenance.json"), JSON.stringify(prov, null, 2));
    fs.writeFileSync(path.join(dir, `task_${t.id}.md`), taskMarkdown(o, t, dir));
    fs.writeFileSync(path.join(dir, "mcp.json"), JSON.stringify(mcpConfig(o, t, trace, prov), null, 2));
    if (o.pointer) {
      fs.writeFileSync(pointerFile(o), trace);
      fs.writeFileSync(path.join(o.out, "mcp.pointer.json"), JSON.stringify(mcpConfig(o, t, trace, prov), null, 2));
    }
    if (o.buildDir) console.log("[run_e2e] 注意: -BuildDir でも assets / shaders-src は現行のチェックアウトから写される(engine_instance.ps1)。比較の条件に書くこと");
    if (!prov.tsCommit) console.log("[run_e2e] 注意: --ts-dir の commit を引けない(比較の再現性が弱い)");
  }
  if (!o.noEngine) {
    try { startEngine(o, project); await waitEngine(o.port); } catch (e) { stopEngine(); throw e; }
  }
  return { dir, trace };
}

export async function finish(o: RunOpts, t: Task): Promise<Score> {
  const dir = taskDir(o, t);
  const trace = path.join(dir, `trace_${t.id}.jsonl`);
  // 既存の採点(ベースライン)を黙って上書きしない
  const have = [`score_${t.id}.json`, `probes_${t.id}.json`].filter((f) => fs.existsSync(path.join(dir, f)));
  if (have.length > 0 && !o.force) throw new Error(`${t.id}: 既に採点がある(${have.join(", ")})。上書きを拒否する。やり直すなら --force`);
  const { EngineClient } = await import("../engineClient.ts");
  const client = new EngineClient("127.0.0.1", o.port, 15000);
  try {
    let probes;
    try { probes = await runProbes(t, client, path.join(dir, "project")); } finally { client.close(); }
    const down = unreachableProbes(probes);
    if (down.length > 0) throw new Error(`${t.id}: エンジンに接続できない probe がある(${down.join(", ")})。score / probes は書かない(エンジンの状態が失われている可能性)`);
    fs.writeFileSync(path.join(dir, `probes_${t.id}.json`), JSON.stringify(probes, null, 2));
    const summary = summarizeTrace(fs.existsSync(trace) ? readTrace(trace) : []);
    const score = scoreTask(t, probes, summary);
    fs.writeFileSync(path.join(dir, `score_${t.id}.json`), JSON.stringify(score, null, 2));
    return score;
  } finally { if (!o.noEngine) stopEngine(); resetPointer(o, trace); }
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
    baseline: argv.includes("--baseline"),
    force: argv.includes("--force"),
    pointer: argv.includes("--pointer"),
  };
  if (o.port !== E2E_PORT && !argv.includes("--allow-port")) throw new Error(`E2E 専用ポートは ${E2E_PORT} だけ(別のポートは --allow-port)`);
  const phase = arg(argv, "--phase", "all")!;
  const which = arg(argv, "--task", "all")!;
  const tasks = loadTasks().filter((t) => which === "all" || t.id === which);
  if (tasks.length === 0) throw new Error(`課題が無い: ${which}`);
  const results: Score[] = [];
  if (phase === "stop") { stopEngine(); resetPointer(o); return; }
  if (phase !== "all" && which === "all") throw new Error(`--phase ${phase} は課題を 1 つずつ指定する(--task T1 など)。--task all は全課題を順に準備・採点する --phase all だけ`);
  try {
  for (const t of tasks) {
    if (phase === "prepare" || phase === "all") {
      const p = await prepare(o, t);
      console.log(`[run_e2e] ${t.id} 準備完了: ${p.dir}`);
      console.log(`[run_e2e] 起動例: claude -p "<task_${t.id}.md を読んで課題をやる>" --mcp-config "${path.join(p.dir, "mcp.json")}" --strict-mcp-config`);
    }
    if (phase === "prepare-replay") {
      const p = await prepare(o, t, true);
      console.log(`[run_e2e] ${t.id} 再生用のエンジンを用意: ${path.join(p.dir, "project")} (replay_e2e の --project に渡す)`);
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
  } catch (e) { if (phase === "all" && !o.noEngine) stopEngine(); throw e; }
  if (results.length > 0) console.log(`[run_e2e] 合格 ${results.filter((r) => r.pass).length}/${results.length}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  main(process.argv.slice(2)).catch((e) => { console.error(String(e?.message ?? e)); process.exit(2); });
}
