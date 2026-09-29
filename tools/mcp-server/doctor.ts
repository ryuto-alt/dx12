// dx12_doctor: エンジンに繋がらない/おかしいときの自己診断。
//
// ★診断は connect だけ(ping は 1 回だけ、接続済みのクライアント経由)。エンジンのブリッジは単一クライアントなので、
//   診断用に 2 本目の接続へ ping を送ると別セッションの接続を奪ったりハングさせたりする。
// ★エンジンの起動は「--background」だけを案内する(窓が前面に出ず、人のカーソル/フォーカスを奪わない)。
//   実マウス/実キーボード/前面化を伴う起動方法は案内しない。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import type { EngineClient } from "./engineClient.ts";
import { discoverPort, portFilePath, probePort } from "./engineClient.ts";
import type { Fix } from "./errors.ts";

const here = path.dirname(fileURLToPath(import.meta.url));

/** エンジン起動コマンドの案内(--background 固定)。exe が見つかれば実パスを入れる。 */
export function launchFixes(o: { port?: number; host?: string } = {}): Fix[] {
  const port = o.port && o.port > 0 ? o.port : 8850;
  const repoExe = path.join(here, "..", "..", "build", "release", "DX12Engine.exe");
  const localExe = process.env.LOCALAPPDATA ? path.join(process.env.LOCALAPPDATA, "DX12Engine", "DX12Engine.exe") : "";
  const exe = fs.existsSync(repoExe) ? path.resolve(repoExe) : localExe && fs.existsSync(localExe) ? localExe : "<DX12Engine.exe のパス>";
  const exeDir = exe.startsWith("<") ? "<DX12Engine.exe のフォルダ>" : path.dirname(exe);
  const fixes: Fix[] = [
    {
      command: `Start-Process -FilePath "${exe}" -ArgumentList '--background','--project','<プロジェクトのフォルダ>','--mcp-port','${port}' -WorkingDirectory "${exeDir}" -WindowStyle Hidden`,
      why: "エンジンを --background で起動する(窓は画面外・前面化しない・人のカーソルを奪わない)。作業ディレクトリは exe のフォルダにする(スクショ等の相対パスが書けるように)",
    },
  ];
  if (!process.env.DX12_MCP_PORT || Number(process.env.DX12_MCP_PORT) !== port) {
    fixes.push({
      command: `$env:DX12_MCP_PORT = '${port}'`,
      why: `--mcp-port を固定して起動した場合は、MCP サーバ側にも DX12_MCP_PORT=${port} を設定する(未設定なら %TEMP%\\dx12_mcp.port の値を自動で使う)`,
    });
  }
  return fixes;
}

export type DoctorIssue = {
  code: string;
  severity: "error" | "warn" | "info";
  message: string;
  fix?: Fix[];
};

export type DoctorDeps = {
  engine: EngineClient;
  toolset: string;
  tsVersion: string;
  toolCounts: { legacy: number; shell: number; total: number; core?: number };
  /** ツール面(full / core / shell / legacy)。toolset の表示に使う。 */
  surface?: string;
  /** マニフェストの expose:"core" による動的登録(list_changed)が有効か。 */
  listChanged?: boolean;
  manifest: { source: string; hash: string | null; snapshotHash: string | null; count: number; lastError: string | null };
  /** マニフェストを取り直す(ping の結果を返す)。 */
  refresh: () => Promise<{ ping?: any; engineTooOld?: boolean; changed?: boolean }>;
  recentErrors: () => { at: number; tool: string; code: string; message: string }[];
  lateResults: () => { method: string; elapsedMs: number; ok: boolean; at: number }[];
  /** テスト用の差し替え口。 */
  now?: () => number;
  processLister?: () => { running: boolean; pids: number[]; commandLines?: string[] };
  portProbe?: (port: number) => Promise<string>;
  logTail?: (cwdHint: string | undefined, lines: number) => { path: string; tail: string[] } | null;
};

function listProcesses(deep: boolean): { running: boolean; pids: number[]; commandLines?: string[]; note?: string } {
  try {
    if (process.platform === "win32") {
      const r = spawnSync("tasklist", ["/FI", "IMAGENAME eq DX12Engine.exe", "/FO", "CSV", "/NH"], { encoding: "utf8", timeout: 4000, windowsHide: true });
      const pids: number[] = [];
      for (const line of (r.stdout ?? "").split(/\r?\n/)) {
        const m = /^"DX12Engine\.exe","(\d+)"/i.exec(line.trim());
        if (m) pids.push(Number(m[1]));
      }
      const out: { running: boolean; pids: number[]; commandLines?: string[] } = { running: pids.length > 0, pids };
      if (deep && pids.length) {
        const ps = spawnSync("powershell", ["-NoProfile", "-NonInteractive", "-Command",
          "Get-CimInstance Win32_Process -Filter \"Name='DX12Engine.exe'\" | ForEach-Object { $_.CommandLine }"],
          { encoding: "utf8", timeout: 8000, windowsHide: true });
        out.commandLines = (ps.stdout ?? "").split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
      }
      return out;
    }
    const r = spawnSync("pgrep", ["-f", "DX12Engine"], { encoding: "utf8", timeout: 3000 });
    const pids = (r.stdout ?? "").split(/\s+/).filter(Boolean).map(Number).filter(Number.isFinite);
    return { running: pids.length > 0, pids };
  } catch (e: any) {
    return { running: false, pids: [], note: `プロセス一覧を取得できなかった: ${e?.message ?? e}` };
  }
}

function tailFile(file: string, lines: number): string[] | null {
  try {
    const st = fs.statSync(file);
    const fd = fs.openSync(file, "r");
    try {
      const size = Math.min(st.size, 64 * 1024);
      const buf = Buffer.alloc(size);
      fs.readSync(fd, buf, 0, size, st.size - size);
      return buf.toString("utf8").split(/\r?\n/).filter(Boolean).slice(-lines);
    } finally { fs.closeSync(fd); }
  } catch { return null; }
}

function defaultLogTail(cwdHint: string | undefined, lines: number): { path: string; tail: string[] } | null {
  const candidates = [
    cwdHint ? path.join(cwdHint, "dx12_engine.log") : "",
    path.join(here, "..", "..", "build", "release", "dx12_engine.log"),
    path.join(process.cwd(), "dx12_engine.log"),
  ].filter(Boolean);
  for (const c of candidates) {
    const t = tailFile(c, lines);
    if (t) return { path: path.resolve(c), tail: t };
  }
  return null;
}

export async function runDoctor(d: DoctorDeps, opts: { deep?: boolean } = {}): Promise<Record<string, unknown>> {
  const now = d.now ?? Date.now;
  const deep = !!opts.deep;
  const issues: DoctorIssue[] = [];
  const host = d.engine.getHost();

  // ── ポート ──────────────────────────────────────────────
  const envPort = process.env.DX12_MCP_PORT ? Number(process.env.DX12_MCP_PORT) : null;
  const pf = portFilePath();
  let portFileValue: number | null = null;
  let portFileAgeSec: number | null = null;
  try {
    const txt = fs.readFileSync(pf, "utf8").trim();
    const n = Number(txt);
    if (Number.isFinite(n) && n > 0) portFileValue = n;
    portFileAgeSec = Math.round((now() - fs.statSync(pf).mtimeMs) / 1000);
  } catch { /* ファイル無し */ }
  const targetPort = d.engine.getPort();
  const scan: number[] = [];
  const addPort = (p: number | null | undefined) => { if (p && p > 0 && p <= 65535 && !scan.includes(p)) scan.push(p); };
  addPort(targetPort); addPort(envPort); addPort(portFileValue); addPort(discoverPort()); addPort(8787);
  for (let p = 8850; p <= (deep ? 8899 : 8853); p++) addPort(p);
  if (deep) for (let p = 8788; p <= 8797; p++) addPort(p);
  const probe = d.portProbe ?? ((p: number) => probePort(host, p, 300));
  const states = await Promise.all(scan.map(async (p) => ({ port: p, connect: await probe(p) })));
  const openPorts = states.filter((s) => s.connect === "open").map((s) => s.port);
  const candidates = deep ? states : states.filter((s) => s.connect !== "refused" || s.port === targetPort || s.port === envPort || s.port === portFileValue);

  // ── プロセス ────────────────────────────────────────────
  const proc = d.processLister ? d.processLister() : listProcesses(deep);

  // ── ping(接続済みのクライアント経由。1 回だけ) ──────────────
  let engineInfo: Record<string, unknown> = { connected: false, host, port: targetPort };
  let pingMs: number | null = null;
  let pong: any = null;
  let pingError: any = null;
  const t0 = now();
  try {
    pong = await d.engine.call("ping", {}, { timeout: 2500, retry: false });
    pingMs = now() - t0;
  } catch (e: any) { pingError = e; }

  if (pong) {
    let refreshed: { ping?: any; engineTooOld?: boolean; changed?: boolean } = {};
    try { refreshed = await d.refresh(); } catch { /* マニフェスト取得失敗は診断を止めない */ }
    engineInfo = {
      connected: true, host, port: d.engine.getPort(), pingMs,
      version: pong.engineVersion ?? null,
      protocolVersion: pong.protocolVersion ?? null,
      manifestProtocol: pong.manifestProtocol ?? null,
      manifestHash: pong.manifestHash ?? null,
      methodCount: pong.methodCount ?? null,
      mode: pong.mode, project: pong.baseDir ?? null, scene: pong.currentScene ?? null,
      sceneDirty: pong.sceneDirty, virtualInput: pong.virtualInput, background: pong.background,
      dpiScale: pong.dpiScale, cwd: pong.cwd, startedAtMs: pong.engineStartedAtMs ?? null,
    };
    if (!pong.manifestHash || refreshed.engineTooOld) {
      issues.push({
        code: "E_ENGINE_TOO_OLD", severity: "warn",
        message: "エンジンがマニフェスト(describe_mcp_manifest)を持たない古い版。dx12_tool_search/describe は型だけの情報になる",
        fix: [{ why: "エンジンを更新(または最新をビルド)して再起動する。MCP サーバ(Node)の再起動は要らない" }],
      });
    }
    if (d.manifest.snapshotHash && pong.manifestHash && d.manifest.snapshotHash !== pong.manifestHash) {
      issues.push({
        code: "MANIFEST_SNAPSHOT_STALE", severity: "info",
        message: "同梱の manifest.snapshot.json とエンジンのマニフェストが違う(エンジンが新しい/古い)。実行中はエンジン側を正として取り込み済み",
        fix: [{ command: "node scripts/gen_manifest_snapshot.mjs", why: "エンジンに繋いだ状態でスナップショットを更新する(リポジトリ側の作業)" }],
      });
    }
    if (pong.sceneDirty) {
      issues.push({ code: "SCENE_DIRTY", severity: "info", message: "未保存の変更がある。dx12_open_scene / dx12_new_scene / dx12_open_project は現在のシーンを閉じる(MCP 接続中は自動保存されるが、外部でシーン JSON を書く前は sceneDirty:false を確認する)", fix: [{ tool: "dx12_save_scene", args: {}, why: "先に保存する" }] });
    }
    if (pong.virtualInput === false && pong.background) {
      issues.push({ code: "VIRTUAL_INPUT_OFF", severity: "info", message: "--background で起動しているが仮想入力モードが OFF。エディタ UI を操作するなら先に ON にする(実入力は使わない)", fix: [{ tool: "dx12_imgui_virtual_input", args: { enable: true }, why: "仮想入力モードを ON" }] });
    }
    if (d.engine.getConnectEpoch() > 1) {
      issues.push({ code: "ENGINE_RECONNECTED", severity: "info", message: `このセッション中に ${d.engine.getConnectEpoch() - 1} 回再接続した(エンジンの再起動/切断)。古い entityId は失効している。sceneGeneration を確認する`, fix: [{ tool: "dx12_list_entities", args: {}, why: "entityId を引き直す" }] });
    }
  } else {
    const timedOut = /timeout/i.test(String(pingError?.message ?? "")) && openPorts.includes(targetPort);
    if (timedOut) {
      issues.push({
        code: "E_ENGINE_BUSY", severity: "error",
        message: `ポート ${targetPort} は開いているが ping に応答が無い。別のクライアント(別の Claude Code セッションなど)が単一ブリッジを握っているか、エンジンが処理中`,
        fix: [{ why: "他の MCP セッション/スクリプトを閉じる。エンジンが重い処理中なら少し待って dx12_doctor をもう一度撃つ" }],
      });
    } else {
      const otherOpen = openPorts.filter((p) => p !== targetPort);
      if (otherOpen.length) {
        issues.push({
          code: "PORT_MISMATCH", severity: "error",
          message: `MCP サーバの接続先ポート ${targetPort} は閉じているが、ポート ${otherOpen.join(", ")} でエンジンらしきものが待ち受けている`,
          fix: [{ command: `$env:DX12_MCP_PORT = '${otherOpen[0]}'`, why: `DX12_MCP_PORT=${otherOpen[0]} を MCP サーバの環境に設定して再接続する(自動探索は %TEMP%\\dx12_mcp.port を読む)` }],
        });
      } else {
        issues.push({
          code: "E_ENGINE_UNREACHABLE", severity: "error",
          message: proc.running
            ? `エンジン(DX12Engine.exe, pid ${proc.pids.join(",")})は動いているが、ポート ${targetPort} に繋がらない(起動直後か、--mcp-port が違う)`
            : `ポート ${targetPort} に応答が無く、DX12Engine.exe のプロセスも見つからない(エンジンが起動していない)`,
          fix: launchFixes({ port: targetPort }),
        });
      }
      if (portFileValue != null && !openPorts.includes(portFileValue)) {
        issues.push({ code: "STALE_PORT_FILE", severity: "warn", message: `ポートファイル ${pf} は ${portFileValue} を指すが、そのポートは閉じている(前回のエンジンの残り)`, fix: [{ why: "エンジンを起動し直せば更新される。固定するなら DX12_MCP_PORT を設定する" }] });
      }
    }
  }

  // ── ログ ────────────────────────────────────────────────
  const logRes = (d.logTail ?? defaultLogTail)(typeof (pong as any)?.cwd === "string" ? (pong as any).cwd : undefined, deep ? 20 : 5);

  const late = d.lateResults();
  if (late.length) issues.push({ code: "LATE_RESULTS", severity: "info", message: `タイムアウト後に完了した呼び出しが ${late.length} 件ある(${late.slice(-3).map((l) => l.method).join(", ")})`, fix: [{ why: "結果は次の dx12_call の meta.lateResults に出る。生成/削除系は list_entities で実際の状態を確認する" }] });

  const errors = issues.filter((i) => i.severity === "error").length;
  const report: Record<string, unknown> = {
    ok: errors === 0,
    summary: errors === 0
      ? `エンジンに接続できている${pong?.mode ? `(mode=${pong.mode})` : ""}`
      : issues.find((i) => i.severity === "error")!.message,
    engine: engineInfo,
    ports: {
      target: targetPort, env: envPort, source: d.engine.getPort() === envPort ? "env" : d.engine.getPort() === portFileValue ? "portfile" : "default",
      portFile: { path: pf, value: portFileValue, ageSec: portFileAgeSec },
      candidates, open: openPorts,
    },
    process: proc,
    versions: {
      ts: d.tsVersion, toolset: d.toolset,
      manifestSource: d.manifest.source, manifestMethods: d.manifest.count,
      engineManifestHash: (pong as any)?.manifestHash ?? null, tsManifestHash: d.manifest.hash, snapshotHash: d.manifest.snapshotHash,
      hashMatch: d.manifest.hash != null && (pong as any)?.manifestHash != null ? d.manifest.hash === (pong as any).manifestHash : null,
      node: process.version,
    },
    tsServer: {
      version: d.tsVersion, toolset: d.toolset, ...(d.surface ? { surface: d.surface } : {}), tools: d.toolCounts,
      ...(d.listChanged !== undefined ? { listChanged: d.listChanged } : {}), manifestSource: d.manifest.source, manifestError: d.manifest.lastError,
    },
    recentErrors: d.recentErrors().slice(-10),
    issues,
  };
  if (logRes) report.log = logRes;
  if (late.length) report.lateResults = late;
  if (!pong && pingError) report.connectError = String(pingError.message ?? pingError);
  report.next = errors === 0
    ? "問題なし。dx12_tool_search で目的のツールを探す/dx12_guide で手順を確認する"
    : "issues[].fix の command / tool を実行してから、もう一度 dx12_doctor を撃つ";
  return report;
}

