/**
 * 品質プリセットのツール(dx12_get_quality_preset / dx12_set_quality_preset)のテスト(FU-E-40)。エンジン不要(偽エンジン)。
 *   1) tools/list に 2 本が出る・$ref が無い・preset は小文字の enum(5 値)
 *   2) get は引数なしでエンジンの get_quality_preset をそのまま通す
 *   3) set は preset をエンジンへ渡す。大文字・未知の名前は偽エンジンに届く前にスキーマで弾く
 *   4) 例(dx12_tool_describe)と searchHints(dx12_tool_search)から見つかる
 * 実行: node qualityPresetTools.test.ts
 */
import assert from "node:assert/strict";
import net from "node:net";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const INDEX_TS = path.join(here, "index.ts");
let passed = 0;
const pass = (label: string) => { passed++; console.log(`  OK  ${label}`); };

type EngineHandler = (method: string, params: any) => { result?: any; error?: { code: number; message: string; hint?: string } };

// ── 偽エンジン(TCP) ────────────────────────────────────────────
// 改行区切り JSON。received に届いた method を溜めて「何回撃ったか」を検証できるようにする。

async function startFakeEngine(handler: EngineHandler) {
  const received: { method: string; params: any }[] = [];
  const server = net.createServer((sock) => {
    sock.setEncoding("utf8");
    let buf = "";
    sock.on("error", () => { /* テスト終了時の切断は無視 */ });
    sock.on("data", (chunk: string) => {
      buf += chunk;
      let nl: number;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line) continue;
        const req = JSON.parse(line);
        received.push({ method: req.method, params: req.params });
        const r = handler(req.method, req.params ?? {});
        const resp = r.error
          ? { id: req.id, ok: false, error_code: r.error.code, error: r.error.message, error_hint: r.error.hint }
          : { id: req.id, ok: true, result: r.result ?? null };
        sock.write(JSON.stringify(resp) + "\n");
      }
    });
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  return { server, port: (server.address() as net.AddressInfo).port, received };
}

// ── MCP クライアント(stdio・最小実装) ──────────────────────────

class McpStdio {
  private proc: ChildProcessWithoutNullStreams;
  private buf = "";
  private nextId = 1;
  private pending = new Map<number, (m: any) => void>();
  stderr = "";

  constructor(port: number) {
    this.proc = spawn(process.execPath, [INDEX_TS], {
      env: { ...process.env, DX12_MCP_PORT: String(port), DX12_MCP_HOST: "127.0.0.1" },
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.proc.stdout.setEncoding("utf8");
    this.proc.stderr.setEncoding("utf8");
    this.proc.stderr.on("data", (d: string) => { this.stderr += d; });
    this.proc.stdout.on("data", (d: string) => {
      this.buf += d;
      let nl: number;
      while ((nl = this.buf.indexOf("\n")) >= 0) {
        const line = this.buf.slice(0, nl).trim();
        this.buf = this.buf.slice(nl + 1);
        if (!line) continue;
        const msg = JSON.parse(line);
        const p = msg.id != null ? this.pending.get(msg.id) : undefined;
        if (p) { this.pending.delete(msg.id); p(msg); }
      }
    });
  }

  private send(method: string, params: any): Promise<any> {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`MCP timeout: ${method}\n${this.stderr}`)), 20000);
      this.pending.set(id, (m) => { clearTimeout(timer); resolve(m); });
      this.proc.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    });
  }

  notify(method: string, params: any) {
    this.proc.stdin.write(JSON.stringify({ jsonrpc: "2.0", method, params }) + "\n");
  }

  async init() {
    const r = await this.send("initialize", {
      protocolVersion: "2025-06-18",
      capabilities: {},
      clientInfo: { name: "qualityPresetTools.test", version: "0" },
    });
    this.notify("notifications/initialized", {});
    return r;
  }

  listTools = () => this.send("tools/list", {});
  callTool = (name: string, args: any) => this.send("tools/call", { name, arguments: args });
  kill() { this.proc.kill(); }
}

/** ツール結果の content[0].text を JSON として読む。 */
function payload(res: any): any {
  const text = res?.result?.content?.[0]?.text;
  assert.ok(typeof text === "string", `content[0].text が無い: ${JSON.stringify(res)}`);
  return JSON.parse(text);
}


let current = "high";
const eng = await startFakeEngine((method, params) => {
  if (method === "get_quality_preset") return { result: { preset: current, applied: false } };
  if (method === "set_quality_preset") { current = params.preset; return { result: { preset: current, applied: false, note: "記録のみ" } }; }
  return { result: {} };
});
const mcp = new McpStdio(eng.port);
try {
  await mcp.init();
  const list = await mcp.listTools();
  const tools: any[] = list.result.tools;
  const get = tools.find((t) => t.name === "dx12_get_quality_preset");
  const set = tools.find((t) => t.name === "dx12_set_quality_preset");
  assert.ok(get && set, "2 本が tools/list にある");
  assert.ok(!JSON.stringify([get, set]).includes("$ref"), "$ref が無い");
  assert.deepEqual(set.inputSchema.properties.preset.enum, ["low", "medium", "high", "epic", "cinematic"]);
  assert.deepEqual(set.inputSchema.required, ["preset"]);
  assert.equal(get.annotations?.readOnlyHint, true);
  pass("tools/list に 2 本・$ref なし・preset は小文字 enum で必須・get は readOnly");

  const g = payload(await mcp.callTool("dx12_get_quality_preset", {}));
  assert.equal(g.preset, "high");
  assert.equal(eng.received.filter((r) => r.method === "get_quality_preset").length, 1);
  pass("get はエンジンの値をそのまま返す");

  const s = payload(await mcp.callTool("dx12_set_quality_preset", { preset: "epic" }));
  assert.equal(s.preset, "epic");
  assert.deepEqual(eng.received.find((r) => r.method === "set_quality_preset")?.params, { preset: "epic" });
  pass("set は preset をエンジンへ渡す");

  const before = eng.received.length;
  for (const bad of [{ preset: "Epic" }, { preset: "ultra" }, {}]) {
    const r = await mcp.callTool("dx12_set_quality_preset", bad);
    assert.equal(r.result?.isError, true, JSON.stringify(bad));
  }
  assert.equal(eng.received.length, before, "不正な引数はエンジンに届かない");
  pass("大文字・未知の名前・省略はスキーマで弾かれエンジンに届かない");

  const d = await mcp.callTool("dx12_tool_describe", { name: "dx12_set_quality_preset" });
  assert.ok(JSON.stringify(d).includes("epic"), "describe に例(epic)がある");
  const q = await mcp.callTool("dx12_tool_search", { query: "品質プリセット 切替" });
  assert.ok(JSON.stringify(q).includes("dx12_set_quality_preset"), "日本語の検索で見つかる");
  const q2 = await mcp.callTool("dx12_tool_search", { query: "quality preset" });
  assert.ok(JSON.stringify(q2).includes("dx12_get_quality_preset"), "英語の検索で見つかる");
  pass("tool_describe の例と tool_search(日英)から見つかる");
} finally {
  mcp.kill();
  eng.server.close();
}
console.log(`
OK: 品質プリセットのツール ${passed} 項目すべて通過`);
