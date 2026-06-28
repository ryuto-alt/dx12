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

server.tool(
  "dx12_create_entity",
  "エンティティを生成する(エディタのみ)。生成はフレーム境界で遅延処理されるため id は即返らない。name を付けて後から dx12_list_entities / dx12_get_entity で引く。",
  {
    type: z.enum(["box", "sphere", "plane", "empty"]).describe("プリミティブ種別。emptyはTransformのみ。"),
    name: z.string().optional().describe("エンティティ名(一意推奨)。省略時は種別名。"),
    position: z.array(z.number()).length(3).optional().describe("[x,y,z]。省略時 [0,0,0]。"),
  },
  ({ type, name, position }) =>
    run(() => engine.call("create_entity", { type, name, position })),
);

server.tool(
  "dx12_delete_entity",
  "エンティティを削除する(子ごと、Undo可)。フレーム境界で遅延処理。",
  { entity: z.number().int().describe("エンティティ id") },
  ({ entity }) => run(() => engine.call("delete_entity", { entity })),
);

server.tool(
  "dx12_set_transform",
  "エンティティの Transform を設定する。指定したフィールドだけ更新(位置/回転(Euler度)/スケール)。",
  {
    entity: z.number().int().describe("エンティティ id"),
    position: z.array(z.number()).length(3).optional().describe("[x,y,z]"),
    rotation: z.array(z.number()).length(3).optional().describe("[x,y,z] Euler度"),
    scale: z.array(z.number()).length(3).optional().describe("[x,y,z]"),
  },
  ({ entity, position, rotation, scale }) =>
    run(() => engine.call("set_transform", { entity, position, rotation, scale })),
);

server.tool(
  "dx12_get_entity",
  "エンティティの全コンポーネントと値を JSON で読む(編集前の状態確認に使う)。",
  { entity: z.number().int().describe("エンティティ id") },
  ({ entity }) => run(() => engine.call("get_entity", { entity })),
);

const transport = new StdioServerTransport();
await server.connect(transport);
