// W05-36: MCP-14 E2E のトレース形式(JSONL)。MCP サーバの外側の stdio プロキシ(trace_proxy.ts)が書き、
// score_e2e.ts / replay_e2e.ts が読む。TS 本体には口を足さない(改修前の TS でも同じ条件で撮れる)。
// 1 行 = { seq, t, dir: "c2s" | "s2c", msg } か { seq, t, kind: "meta", meta }。t は開始からの ms。
// トレースに秘密(トークン等)を書かない: 書く前に redact() を必ず通す。
import fs from "node:fs";
import path from "node:path";

export type TraceDir = "c2s" | "s2c";
export type TraceRecord =
  | { seq: number; t: number; dir: TraceDir; msg: any }
  | { seq: number; t: number; kind: "meta"; meta: Record<string, unknown> };

const SECRET_KEY = /token|secret|passw(or)?d|api[_-]?key|authorization|credential|cookie/i;
const SECRET_VALUE = /sk-ant-[A-Za-z0-9_-]+|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|Bearer\s+[A-Za-z0-9._~+/=-]{8,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]+|hf_[A-Za-z0-9]{20,}/g;
export const REDACTED = "[REDACTED]";

/** 秘密らしいキーの値と、秘密らしい文字列を潰した複製を返す(元は変えない)。 */
export function redact(v: unknown): any {
  if (typeof v === "string") return v.replace(SECRET_VALUE, REDACTED);
  if (Array.isArray(v)) return v.map(redact);
  if (v && typeof v === "object") {
    const o: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v as Record<string, unknown>)) {
      o[k] = SECRET_KEY.test(k) && x !== null && x !== undefined && x !== "" ? REDACTED : redact(x);
    }
    return o;
  }
  return v;
}

export class TraceWriter {
  private seq = 0;
  private t0 = Date.now();
  private fd: number;
  constructor(file: string) {
    fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
    this.fd = fs.openSync(file, "a"); // 追記: エージェントが MCP サーバを再起動してもそれまでの記録を失わない(新しい課題は run_e2e が消す)
  }
  meta(meta: Record<string, unknown>) {
    this.write({ seq: this.seq++, t: Date.now() - this.t0, kind: "meta", meta: redact(meta) });
  }
  /** MCP の 1 行(JSON 文字列)を記録する。JSON でない行は _nonJson として残す。 */
  line(dir: TraceDir, raw: string) {
    let msg: any;
    try { msg = JSON.parse(raw); } catch { msg = { _nonJson: redact(raw.slice(0, 2000)) }; }
    this.write({ seq: this.seq++, t: Date.now() - this.t0, dir, msg: redact(msg) });
  }
  /** 既に組み立てた JSON-RPC メッセージを記録する(replay 用)。 */
  message(dir: TraceDir, msg: unknown) {
    this.write({ seq: this.seq++, t: Date.now() - this.t0, dir, msg: redact(msg) });
  }
  private write(r: TraceRecord) { fs.writeSync(this.fd, JSON.stringify(r) + "\n"); }
  close() { try { fs.closeSync(this.fd); } catch { /* 二重 close は無視 */ } }
}

export function parseTrace(text: string): TraceRecord[] {
  const out: TraceRecord[] = [];
  for (const l of text.split(/\r?\n/)) {
    if (!l.trim()) continue;
    try { out.push(JSON.parse(l)); } catch { /* 壊れた行(書き込み途中)は読み飛ばす */ }
  }
  return out;
}
export function readTrace(file: string): TraceRecord[] { return parseTrace(fs.readFileSync(file, "utf8")); }

export type ToolCall = {
  id: unknown; tool: string; name: string; args: any;
  isError: boolean; answered: boolean; t: number; ms: number; isRetry: boolean;
};
export type TraceSummary = {
  calls: ToolCall[]; callCount: number; errorCount: number; retryCount: number;
  byTool: Record<string, number>; meta: Record<string, unknown>;
};

/** dx12_call は中身の name で数える(dx12_call:<name>)。 */
function toolKey(name: string, args: any): string {
  if (name === "dx12_call" && args && typeof args.name === "string") return `dx12_call:${args.name}`;
  return name;
}

function responseIsError(msg: any): boolean {
  return !!msg?.error || msg?.result?.isError === true;
}

/**
 * tools/call の要求と応答を id で突き合わせて数える。
 *  - 呼び出し数: tools/call の要求の数(initialize / tools/list 等は数えない)。
 *  - エラー: JSON-RPC の error、または result.isError。応答が来なかった呼び出しもエラーに数える。
 *  - 再試行: 直近の「同じツール」(dx12_call は中身の名前)の呼び出しがエラーだった後の、同じツールの呼び出し。
 */
export function summarizeTrace(records: TraceRecord[]): TraceSummary {
  const meta: Record<string, unknown> = {};
  const calls: ToolCall[] = [];
  const open = new Map<string, ToolCall>();
  for (const r of records) {
    if ("kind" in r && r.kind === "meta") { Object.assign(meta, r.meta); continue; }
    if (!("dir" in r)) continue;
    const m = r.msg;
    if (r.dir === "c2s" && m?.method === "tools/call" && m.id !== undefined) {
      const name = String(m.params?.name ?? "");
      const args = m.params?.arguments ?? {};
      const c: ToolCall = { id: m.id, tool: toolKey(name, args), name, args, isError: true, answered: false, t: r.t, ms: 0, isRetry: false };
      calls.push(c);
      open.set(JSON.stringify(m.id), c);
    } else if (r.dir === "s2c" && m && m.id !== undefined && (m.result !== undefined || m.error !== undefined)) {
      const key = JSON.stringify(m.id);
      const c = open.get(key);
      if (c && !c.answered) { c.answered = true; c.isError = responseIsError(m); c.ms = r.t - c.t; open.delete(key); }
    }
  }
  const lastErr = new Map<string, boolean>();
  const byTool: Record<string, number> = {};
  let errorCount = 0;
  let retryCount = 0;
  for (const c of calls) {
    byTool[c.tool] = (byTool[c.tool] ?? 0) + 1;
    if (lastErr.get(c.tool) === true) { c.isRetry = true; retryCount++; }
    if (c.isError) errorCount++;
    lastErr.set(c.tool, c.isError);
  }
  return { calls, callCount: calls.length, errorCount, retryCount, byTool, meta };
}
