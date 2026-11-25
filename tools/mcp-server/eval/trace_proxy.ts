// W05-36: MCP サーバの外側に立つ stdio プロキシ。エージェントの MCP 設定で
//   command: node  args: [trace_proxy.ts, --trace <file>, --, node, index.ts]
// のように指すと、エージェント <-> MCP サーバの行(改行区切り JSON-RPC)をそのまま中継しつつ JSONL に記録する。
// TS 本体には何も足さない(改修前のタグの TS でも同じ条件で撮れる)。秘密は trace.ts の redact を通して書く。
//   node trace_proxy.ts --trace <file> [--meta k=v ...] [-- <cmd> <args...>]
// -- を省くと、一つ上のフォルダの index.ts を node で起動する。
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { TraceWriter } from "./trace.ts";

const here = path.dirname(fileURLToPath(import.meta.url));

export type ProxyArgs = { trace: string; meta: Record<string, string>; cmd: string; cmdArgs: string[] };

export function parseProxyArgs(argv: string[]): ProxyArgs {
  const meta: Record<string, string> = {};
  let trace = "";
  let i = 0;
  for (; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--") { i++; break; }
    if (a === "--trace") trace = argv[++i] ?? "";
    else if (a === "--meta") {
      const kv = argv[++i] ?? "";
      const e = kv.indexOf("=");
      if (e > 0) meta[kv.slice(0, e)] = kv.slice(e + 1);
    } else throw new Error(`不明な引数: ${a}`);
  }
  if (!trace) throw new Error("--trace <file> が必要");
  const rest = argv.slice(i);
  if (rest.length === 0) return { trace, meta, cmd: process.execPath, cmdArgs: [path.join(here, "..", "index.ts")] };
  return { trace, meta, cmd: rest[0], cmdArgs: rest.slice(1) };
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

export function runProxy(a: ProxyArgs): Promise<number> {
  const tw = new TraceWriter(a.trace);
  tw.meta({ proxy: "trace_proxy", startedAt: new Date().toISOString(), ...a.meta });
  const child = spawn(a.cmd, a.cmdArgs, { stdio: ["pipe", "pipe", "inherit"], env: process.env });
  const up = lineSplitter((l) => tw.line("c2s", l));
  const down = lineSplitter((l) => tw.line("s2c", l));
  process.stdin.setEncoding("utf8");
  child.stdout.setEncoding("utf8");
  process.stdin.on("data", (d: string) => { up.push(d); child.stdin.write(d); });
  process.stdin.on("end", () => { up.flush(); child.stdin.end(); });
  child.stdout.on("data", (d: string) => { down.push(d); process.stdout.write(d); });
  return new Promise((resolve) => {
    child.on("close", (code) => { up.flush(); down.flush(); tw.close(); resolve(code ?? 0); });
    child.on("error", (e) => { tw.meta({ spawnError: String(e) }); tw.close(); resolve(1); });
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  runProxy(parseProxyArgs(process.argv.slice(2))).then((c) => process.exit(c));
}
