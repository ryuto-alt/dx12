/**
 * ヘッドレスのエンジンへ直接つないでシーンを検証する CI クライアント。
 *
 * なぜ MCP サーバー(index.ts)を経由しないのか:
 *   MCP は「AI が対話しながら 1 手ずつ操作する」ための口で、stdio 越しの 1 クライアント専用。
 *   CI は「窓も AI も無い所で、何十シーンかをまとめて検証して終了コードを返す」用途なので、
 *   エンジンの TCP を直接叩く方が素直（並列にも走らせられる）。
 *   検証ロジック本体（配置検査・到達性・台本）はエンジンと playtest.ts に入っているので、
 *   ここはその呼び出しと集計だけを持つ。
 *
 * 使い方（どちらでもよい）:
 *   ① 自分で起動する
 *      node ciClient.ts --launch <projectDir> --scenes scenes/main.json,scenes/title.json
 *      → 空きポートを選んでヘッドレスで起動し、検査して、終了コードを返して engine も落とす
 *   ② 既に動いているエンジンへ繋ぐ（対話中のエディタでもよい）
 *      node ciClient.ts --port 8850 --scenes scenes/main.json
 *
 * 終了コード: 0 = 全シーン合格 / 1 = どれかに errors か到達不能があった
 */

import net from "node:net";
import { createRequire } from "node:module";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { analyzePath, type MovementCapability } from "./playtest.ts";

// ─── エンジンへの生接続（行 JSON） ───────────────────────────────────────

export interface EngineConn {
  call(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<any>;
  close(): void;
}

export async function connect(port: number, host = "127.0.0.1"): Promise<EngineConn> {
  const sock = net.createConnection(port, host);
  await new Promise<void>((res, rej) => {
    sock.once("connect", () => res());
    sock.once("error", rej);
  });
  sock.setNoDelay(true);

  let buf = "";
  const waiters: ((v: any) => void)[] = [];
  sock.on("data", (d) => {
    buf += d.toString("utf8");
    let i: number;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      const w = waiters.shift();
      if (w) w(JSON.parse(line));
    }
  });

  let seq = 0;
  return {
    call(method, params = {}, timeoutMs = 120_000) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`timeout: ${method}`)), timeoutMs);
        waiters.push((r: any) => {
          clearTimeout(timer);
          if (r.ok === false) {
            const e: any = new Error(`${method}: ${r.error}`);
            e.code = r.error_code;
            reject(e);
          } else resolve(r.result ?? {});
        });
        sock.write(JSON.stringify({ id: ++seq, method, params }) + "\n");
      });
    },
    close() { sock.end(); },
  };
}

/** エンジンが自分で書いたポート（--mcp-port を指定していないインスタンス用）。 */
export function defaultPort(): number {
  return Number(fs.readFileSync(path.join(os.tmpdir(), "dx12_mcp.port"), "utf8").trim());
}

// ─── 検証（純粋な集計部分はテストできるよう分けてある） ──────────────────

export interface SceneReport {
  scene: string;
  errors: number;
  warnings: number;
  checked: number;
  issues: { kind: string; name?: string; text: string; level: string }[];
  reachability?: { pass: boolean; detail: string }[];
  failed: boolean;
}

/** 1 シーンぶんの結果から「CI を落とすか」を決める。errors だけを落とす条件にする。 */
export function shouldFail(reports: SceneReport[]): boolean {
  return reports.some((r) => r.failed);
}

/** 人が読む 1 行にまとめる。 */
export function formatReport(r: SceneReport): string {
  const head = `${r.failed ? "FAIL" : "ok  "} ${r.scene}  ` +
               `検査${r.checked} エラー${r.errors} 注意${r.warnings}`;
  const body = r.issues
    .filter((i) => i.level === "error")
    .slice(0, 10)
    .map((i) => `       [${i.kind}] ${i.text}`);
  const reach = (r.reachability ?? [])
    .filter((x) => !x.pass)
    .map((x) => `       [UNREACHABLE] ${x.detail}`);
  return [head, ...body, ...reach].join("\n");
}

/**
 * 1 シーンを開いて検査する。
 * goals が渡されていれば、プレイヤーからそこまでの到達性も見る（ナビメッシュを焼いてから）。
 */
export async function validateScene(
  eng: EngineConn,
  scene: string,
  opts: { goals?: string[]; capability?: MovementCapability } = {},
): Promise<SceneReport> {
  await eng.call("stop", {});                       // 念のため Editor へ
  await eng.call("open_scene", { path: scene });

  const v = await eng.call("validate_layout", {});
  const report: SceneReport = {
    scene,
    errors: v.errors ?? 0,
    warnings: v.warnings ?? 0,
    checked: v.checked ?? 0,
    issues: (v.issues ?? []).map((i: any) => ({
      kind: i.kind, name: i.name, text: i.text, level: i.level,
    })),
    failed: (v.errors ?? 0) > 0,
  };

  if (opts.goals?.length) {
    const cap: MovementCapability = opts.capability ??
      { walkSpeed: 4, jumpHeight: 1.0, jumpDistance: 3.0, stepHeight: 0.3, maxSlopeDeg: 50 };
    report.reachability = [];
    try {
      await eng.call("navmesh_build", {});
      // ★このシーンに存在する目標だけを見る。タイトル画面やクリア画面には
      //   ゴールもプレイヤーも無いのが正しいので、無いことを不合格にしてはいけない
      //   （実際に title.json が「プレイヤーが居ない」で落ちた）。
      const all = await eng.call("list_entities", {});
      const present = new Set((all?.entities ?? []).map((e: any) => e.name));
      const applicable = opts.goals.filter((g) => present.has(g));
      if (applicable.length === 0) {
        report.reachability.push({ pass: true, detail: "このシーンに対象の目標が無いので到達性は見ない" });
        return report;
      }

      const cc = await eng.call("list_entities", { component_type: "characterController" });
      const playerName = cc?.entities?.[0]?.name;
      if (!playerName) {
        report.reachability.push({
          pass: false,
          detail: `目標(${applicable.join(", ")})はあるのにプレイヤー(characterController)が居ない`,
        });
        report.failed = true;
      } else {
        const from = (await eng.call("get_bounds", { name: playerName, includeChildren: true })).center;
        for (const g of applicable) {
          try {
            const to = (await eng.call("get_bounds", { name: g, includeChildren: true })).center;
            const pr = await eng.call("navmesh_path", { from, to });
            const pts = (pr.points ?? []).map((p: number[]) => [p[0], p[1], p[2]] as [number, number, number]);
            if (!pts.length || pr.reached === false) {
              report.reachability.push({ pass: false, detail: `${g} へ到達できない（経路が繋がっていない）` });
              report.failed = true;
              continue;
            }
            const gaps = analyzePath(pts, cap);
            if (gaps.length) {
              report.reachability.push({ pass: false, detail: `${g}: ${gaps[0].reason}` });
              report.failed = true;
            } else {
              report.reachability.push({ pass: true, detail: `${g} へ到達できる（${pr.length?.toFixed?.(1)}m）` });
            }
          } catch (e) {
            report.reachability.push({ pass: false, detail: `${g}: ${(e as Error).message}` });
            report.failed = true;
          }
        }
      }
    } catch (e) {
      report.reachability.push({ pass: false, detail: `ナビメッシュ: ${(e as Error).message}` });
      report.failed = true;
    }
  }
  return report;
}

// ─── ヘッドレス起動 ──────────────────────────────────────────────────────

/** そのポートに誰か居るか。起動待ちに使う。 */
export function portOpen(port: number, timeoutMs = 400): Promise<boolean> {
  return new Promise((resolve) => {
    const sock = new net.Socket();
    let done = false;
    const fin = (v: boolean) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
    sock.setTimeout(timeoutMs);
    sock.once("connect", () => fin(true));
    sock.once("timeout", () => fin(false));
    sock.once("error", () => fin(false));
    sock.connect(port, "127.0.0.1");
  });
}

/** 使われていないポートを探す（複数の CI を同時に走らせても衝突しないように）。 */
export async function findFreePort(start = 8850, tries = 40): Promise<number> {
  for (let p = start; p < start + tries; p++) if (!(await portOpen(p, 200))) return p;
  throw new Error(`空きポートが ${start}..${start + tries} に無い`);
}

/**
 * ヘッドレスでエンジンを起動し、MCP ポートが開くまで待つ。
 * ★--mcp-port を明示するので %TEMP%/dx12_mcp.port は書き換わらない
 *   ＝人が開いているエディタの接続を奪わない（CI を回しながら作業できる）。
 */
export async function launchHeadless(opts: {
  projectDir: string; enginePath?: string; scene?: string; port?: number; timeoutMs?: number;
}): Promise<{ port: number; kill: () => void }> {
  const { spawn } = await import("node:child_process");
  const exe = opts.enginePath ??
    path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\//, "")),
                 "../../build/release/DX12Engine.exe");
  const port = opts.port ?? (await findFreePort());
  const args = ["--headless", "--project", opts.projectDir, "--mcp-port", String(port)];
  if (opts.scene) args.push("--scene", opts.scene);

  const child = spawn(exe, args, {
    cwd: path.dirname(exe),      // ★CWD がここでないとスクショが WIC で開けない（既知の罠）
    detached: false,
    stdio: "ignore",
  });
  // ★Windows の GUI プロセス（隠し窓でも GUI サブシステム）は SIGTERM で死なない。
  //   child.kill() だけだと CI が終わってもエンジンが残り続ける（実測で残った）。
  //   taskkill /T /F で子ごと確実に落とす。
  const kill = () => {
    if (child.exitCode != null) return;
    try {
      if (process.platform === "win32" && child.pid != null) {
        const { spawnSync } = createRequire(import.meta.url)("node:child_process");
        spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
      } else child.kill("SIGKILL");
    } catch { /* もう死んでいる */ }
  };

  const limit = opts.timeoutMs ?? 90_000;
  const t0 = Date.now();
  while (Date.now() - t0 < limit) {
    if (await portOpen(port)) return { port, kill };
    if (child.exitCode != null) throw new Error(`エンジンが起動直後に終了した（code=${child.exitCode}）`);
    await new Promise((r) => setTimeout(r, 500));
  }
  kill();
  throw new Error(`ヘッドレス起動が ${limit}ms でポート ${port} を開けなかった`);
}

// ─── CLI ────────────────────────────────────────────────────────────────

function parseArgs(argv: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) out[a.slice(2)] = argv[i + 1]?.startsWith("--") || argv[i + 1] == null
      ? "true" : argv[++i];
  }
  return out;
}

// import されたときは走らせない（テストから純関数だけ使えるように）
const isMain = process.argv[1] && import.meta.url.endsWith(path.basename(process.argv[1]));
if (isMain) {
  const args = parseArgs(process.argv.slice(2));
  const scenes = (args.scenes ?? "").split(",").map((s) => s.trim()).filter(Boolean);
  const goals = (args.goals ?? "").split(",").map((s) => s.trim()).filter(Boolean);

  let port: number;
  let kill: (() => void) | null = null;
  if (args.launch && args.launch !== "true") {
    const started = await launchHeadless({
      projectDir: path.resolve(args.launch),
      enginePath: args.engine,
      scene: scenes[0],
    });
    port = started.port;
    kill = started.kill;
    console.log(`ヘッドレス起動: port ${port}`);
  } else {
    port = args.port ? Number(args.port) : defaultPort();
  }

  const reports: SceneReport[] = [];
  try {
    const eng = await connect(port);
    const ping = await eng.call("ping", {});
    console.log(`接続: ${ping.currentScene}（${ping.entityCount} エンティティ / port ${port}）`);

    const targets = scenes.length ? scenes : [ping.currentScene];
    for (const s of targets) {
      const r = await validateScene(eng, s, { goals });
      reports.push(r);
      console.log(formatReport(r));
    }
    eng.close();
  } finally {
    if (kill) kill();
  }

  const failed = shouldFail(reports);
  console.log(`\n${reports.length} シーン中 ${reports.filter((r) => r.failed).length} 件が不合格`);
  process.exit(failed ? 1 : 0);
}
