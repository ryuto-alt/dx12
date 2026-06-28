import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { EngineClient } from "./engineClient.ts";

// DX12 ゲームエンジン用 MCP サーバ。Codex / Claude Code から接続し、
// 起動中のエディタ(TCP 127.0.0.1:8787)を叩いてゲームを作っていくための入口。
// 初回スライス: Lua コンポーネントの生成 + エンティティへのアタッチ。

const engine = new EngineClient();
const server = new McpServer({ name: "dx12-engine", version: "0.1.0" });

type ToolResult = { content: { type: "text"; text: string }[]; isError?: boolean };

async function run(fn: () => Promise<unknown>): Promise<ToolResult> {
  try {
    const data = await fn();
    const text = typeof data === "string" ? data : JSON.stringify(data, null, 2);
    return { content: [{ type: "text", text }] };
  } catch (e: any) {
    return { content: [{ type: "text", text: `エラー: ${e.message}` }], isError: true };
  }
}

server.tool(
  "dx12_list_entities",
  "起動中エディタで今開いているシーンのエンティティ一覧(id, name)を返す。アタッチ先を知るのに使う。",
  {},
  () => run(() => engine.call("list_entities", {})),
);

server.tool(
  "dx12_create_lua_component",
  "Lua コンポーネント(.lua)を assets/components/ に作成する。構文は作成時に検証され、エラーなら書き込まず error を返す。返り値 path を attach の script に渡す。",
  {
    name: z.string().describe("コンポーネント名(拡張子・パス区切りなし)。例: Health"),
    code: z.string().describe("Lua コード全体。properties / OnStart / OnUpdate を含められる。"),
  },
  ({ name, code }) => run(() => engine.call("create_lua_component", { name, code })),
);

server.tool(
  "dx12_attach_lua_component",
  "Lua コンポーネントをエンティティにアタッチする。エディタ上では貼るだけで、実際の初期化/実行は Play 時(OnStart/OnUpdate)。script は create が返した assets 相対パス(assets 配下限定)。",
  {
    entity: z.number().int().describe("エンティティ id(dx12_list_entities の id)"),
    script: z.string().describe("assets 相対パス。例: components/Health.lua"),
  },
  ({ entity, script }) => run(() => engine.call("attach_lua_component", { entity, script })),
);

const transport = new StdioServerTransport();
await server.connect(transport);
