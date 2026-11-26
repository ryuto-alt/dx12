// W05-36: MCP サーバの外側に立つ stdio プロキシ。エージェントの MCP 設定で
//   command: node  args: [trace_proxy.ts, --trace <file>, --, node, index.ts]
// のように指すと、エージェント <-> MCP サーバの行(改行区切り JSON-RPC)をそのまま中継しつつ JSONL に記録する。
// TS 本体には何も足さない(改修前のタグの TS でも同じ条件で撮れる)。秘密は trace.ts の redact を通して書く。
//   node trace_proxy.ts (--trace <file> | --trace-pointer <file>) [--meta k=v ...] [-- <cmd> <args...>]
// --trace-pointer: 記録先を「そのファイルの中身(トレースのパス)」で決める。親のセッションにプロキシを 1 回だけ登録し、
//   課題を替えるたびに run_e2e の prepare がポインタの中身を書き換える使い方のため(行ごとに読み直して切り替える)。
// -- を省くと、一つ上のフォルダの index.ts を node で起動する。
import fs from "node:fs";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { TraceWriter } from "./trace.ts";

const here = path.dirname(fileURLToPath(import.meta.url));

export type ProxyArgs = { trace: string; pointer: string; meta: Record<string, string>; cmd: string; cmdArgs: string[] };

export function parseProxyArgs(argv: string[]): ProxyArgs {
  const meta: Record<string, string> = {};
  let trace = "";
  let pointer = "";
  let i = 0;
  for (; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--") { i++; break; }
    if (a === "--trace") trace = argv[++i] ?? "";
    else if (a === "--trace-pointer") pointer = argv[++i] ?? "";
    else if (a === "--meta") {
      const kv = argv[++i] ?? "";
      const e = kv.indexOf("=");
      if (e > 0) meta[kv.slice(0, e)] = kv.slice(e + 1);
    } else throw new Error(`不明な引数: ${a}`);
  }
  if (!trace && !pointer) throw new Error("--trace <file> か --trace-pointer <file> が必要");
  const rest = argv.slice(i);
  if (rest.length === 0) return { trace, pointer, meta, cmd: process.execPath, cmdArgs: [path.join(here, "..", "index.ts")] };
  return { trace, pointer, meta, cmd: rest[0], cmdArgs: rest.slice(1) };
}

/** 文字列の塊を行に切って onLine へ渡す。末尾の改行なし断片は flush まで保持する。 */
export function lineSplitter(onLine: (line: string) => void) {
  let buf = "";
  return {
    push(chunk: string) {
      buf += chunk;
      let i: number;
      while ((i = buf.indexOf("\n")) >= 0) {
        const l = buf.slice(0, i).replace(/\r$/, "");
        buf = buf.slice(i + 1);
        if (l.trim()) onLine(l);
      }
    },
    flush() { if (buf.trim()) onLine(buf.replace(/\r$/, "")); buf = ""; },
  };
}

/** 記録先の管理。ポインタ指定なら、中身のパスが変わったときに閉じて開き直す。 */
export function makeSink(a: ProxyArgs) {
  let tw: TraceWriter | null = null;
  let cur = "";
  const metaBase = () => ({ proxy: "trace_proxy", startedAt: new Date().toISOString(), ...a.meta });
  const resolve = (): string => {
    if (a.pointer) {
      try { const p = fs.readFileSync(a.pointer, "utf8").trim(); if (p) return p; } catch { /* まだ無い */ }
      return a.trace || a.pointer + ".unassigned.jsonl";
    }
    return a.trace;
  };
  const get = (): TraceWriter => {
    const p = resolve();
    if (!tw || p !== cur) { tw?.close(); tw = new TraceWriter(p); cur = p; tw.meta(metaBase()); }
    return tw;
  };
  return {
    line: (dir: "c2s" | "s2c", l: string) => { try { get().line(dir, l); } catch { /* 記録の失敗で中継を止めない */ } },
    meta: (m: Record<string, unknown>) => { try { get().meta(m); } catch { /* 同上 */ } },
    close: () => { tw?.close(); tw = null; },
  };
}

export function runProxy(a: ProxyArgs): Promise<number> {
  const sink = makeSink(a);
  sink.meta({});
  const child = spawn(a.cmd, a.cmdArgs, { stdio: ["pipe", "pipe", "inherit"], env: process.env });
  const up = lineSplitter((l) => sink.line("c2s", l));
  const down = lineSplitter((l) => sink.line("s2c", l));
  process.stdin.setEncoding("utf8");
  child.stdout.setEncoding("utf8");
  // 子や親が先に閉じたときの EPIPE などで落ちない(記録に残して続ける)
  child.stdin.on("error", (e) => sink.meta({ childStdinError: String((e as Error).message ?? e) }));
  process.stdout.on("error", (e) => sink.meta({ stdoutError: String((e as Error).message ?? e) }));
  process.stdin.on("error", (e) => sink.meta({ stdinError: String((e as Error).message ?? e) }));
  process.stdin.on("data", (d: string) => { up.push(d); if (!child.stdin.destroyed) child.stdin.write(d); });
  process.stdin.on("end", () => { up.flush(); if (!child.stdin.destroyed) child.stdin.end(); });
  child.stdout.on("data", (d: string) => { down.push(d); if (!process.stdout.destroyed) process.stdout.write(d); });
  return new Promise((resolve) => {
    child.on("close", (code) => { up.flush(); down.flush(); sink.close(); resolve(code ?? 0); });
    child.on("error", (e) => { sink.meta({ spawnError: String(e) }); sink.close(); resolve(1); });
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  runProxy(parseProxyArgs(process.argv.slice(2))).then((c) => process.exit(c));
}
