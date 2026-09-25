// ヘッドレスのエンジンを 1 つ起動して、行 JSON の TCP で 1 本だけ繋ぐための小さな共通部品。
//
// なぜ tools/mcp-server/ciClient.ts を使わないのか:
//   あちらは MCP サーバ側（別の担当が並行で触る）に属していて、既定の exe が build/release 固定。
//   ベンチ/ゴールデンは「どの exe を撮ったか」を自分で決めたいし、MCP サーバの変更に
//   巻き込まれたくないので、必要な 3 つ（起動・接続・確実な停止）だけをここに持つ。
//
// ★ブリッジは単一クライアント + listen backlog 1。接続は 1 本だけ張って全部そこで済ませること。
// ★止めるときは自分が起動した PID だけを taskkill /PID <pid> /T /F で落とす。
//   イメージ名で殺すと、人が開いているエディタまで落ちる。
import net from "node:net";
import path from "node:path";
import fs from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
export const DEFAULT_EXE = path.join(REPO_ROOT, "build", "dev", "DX12Engine.exe");

/** そのポートで誰かが listen しているか。 */
export function portOpen(port, timeoutMs = 300) {
  return new Promise((resolve) => {
    const s = new net.Socket();
    let done = false;
    const fin = (v) => { if (!done) { done = true; s.destroy(); resolve(v); } };
    s.setTimeout(timeoutMs);
    s.once("connect", () => fin(true));
    s.once("timeout", () => fin(false));
    s.once("error", () => fin(false));
    s.connect(port, "127.0.0.1");
  });
}

/**
 * 8850..8899 から空きポートを探す（人のエディタは 8787〜 を使う）。
 * 環境変数 DX12_BENCH_PORT_START / DX12_BENCH_PORT_END で範囲を変えられる
 * （複数の worktree が同時にゴールデンを回すとき、担当ごとに範囲を分けて取り合いを避ける）。
 */
export async function findFreePort(start = Number(process.env.DX12_BENCH_PORT_START) || 8850,
                                   end = Number(process.env.DX12_BENCH_PORT_END) || 8899) {
  for (let p = start; p <= end; p++) if (!(await portOpen(p, 150))) return p;
  throw new Error(`空きポートが ${start}..${end} に無い`);
}

/**
 * ヘッドレスで起動して MCP ポートが開くまで待つ。
 * CWD は exe のフォルダ（ログ / クラッシュダンプ / 既定スクショの置き場。リポジトリ直下を汚さない）。
 * --mcp-port を明示するので %TEMP%/dx12_mcp.port は書き換わらない＝人のエディタの接続を奪わない。
 */
export async function launchEngine({ exe = DEFAULT_EXE, project, scene, port, timeoutMs = 120_000 } = {}) {
  if (!fs.existsSync(exe)) throw new Error(`エンジンが無い: ${exe}（build/dev をビルドしたか）`);
  const usePort = port ?? (await findFreePort());
  const args = ["--headless", "--project", project, "--mcp-port", String(usePort)];
  if (scene) args.push("--scene", scene);
  const child = spawn(exe, args, { cwd: path.dirname(exe), detached: false, stdio: "ignore" });
  let exited = false;
  child.on("exit", () => { exited = true; });

  const kill = () => {
    if (exited || child.pid == null) return;
    // GUI サブシステムのプロセスは SIGTERM で死なないので taskkill（自分の PID だけ）。
    spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
    exited = true;
  };
  // 異常終了でも孤児を残さない
  const onExit = () => kill();
  process.once("exit", onExit);
  process.once("SIGINT", () => { kill(); process.exit(130); });

  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (exited) throw new Error(`エンジンが起動直後に終了した（code=${child.exitCode}）`);
    if (await portOpen(usePort)) return { port: usePort, pid: child.pid, kill };
    await new Promise((r) => setTimeout(r, 400));
  }
  kill();
  throw new Error(`ヘッドレス起動が ${timeoutMs}ms でポート ${usePort} を開けなかった`);
}

/**
 * 行 JSON で 1 本繋ぐ。応答は id で突き合わせる（遅延応答が混ざっても取り違えない）。
 * call() は ok:false なら例外（error / error_hint を載せる）。
 */
export async function connect(port, host = "127.0.0.1") {
  const sock = net.createConnection(port, host);
  await new Promise((res, rej) => { sock.once("connect", res); sock.once("error", rej); });
  sock.setNoDelay(true);
  let buf = "";
  const pending = new Map();
  sock.on("data", (d) => {
    buf += d.toString("utf8");
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch { continue; }
      const p = pending.get(msg.id);
      if (p) { pending.delete(msg.id); p(msg); }
    }
  });
  let seq = 0;
  return {
    call(method, params = {}, timeoutMs = 180_000) {
      const id = ++seq;
      return new Promise((resolve, reject) => {
        const t = setTimeout(() => { pending.delete(id); reject(new Error(`timeout: ${method}`)); }, timeoutMs);
        pending.set(id, (m) => {
          clearTimeout(t);
          if (m.ok === false) {
            const e = new Error(`${method}: ${m.error}${m.error_hint ? `（${m.error_hint}）` : ""}`);
            e.code = m.error_code;
            reject(e);
          } else resolve(m.result ?? {});
        });
        sock.write(JSON.stringify({ id, method, params }) + "\n");
      });
    },
    close() { sock.end(); },
  };
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
