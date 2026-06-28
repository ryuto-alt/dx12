import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { EngineClient } from "./engineClient.ts";

// DX12 ゲームエンジン用 MCP サーバ。Codex / Claude Code から接続し、
// 起動中のエディタ(TCP 127.0.0.1:8787)を叩いてゲームを作っていくための入口。
// v1: エンティティ/コンポーネント/シーン/アセット/再生制御の計20メソッドを公開。

const engine = new EngineClient();
const server = new McpServer({ name: "dx12-engine", version: "0.2.0" });

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

// ── 既存7ツール(エンティティ基本操作) ─────────────────────────────

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

// ── 新規13ツール ────────────────────────────────────────────────

// シーン入出力 ----------------------------------------------------

server.tool(
  "dx12_save_scene",
  "現在のシーンを保存する。path は assets 相対(例 scenes/title.json)。省略時は現在開いているシーン(currentScenePath)へ上書き。",
  {
    path: z.string().optional().describe("assets 相対パス。例: scenes/title.json。省略で上書き保存。"),
  },
  ({ path }) => run(() => engine.call("save_scene", { path })),
);

server.tool(
  "dx12_open_scene",
  "シーンを開く(現在のシーンを置換)。path は assets 相対。読込はフレーム境界で遅延処理され queued を即返す。",
  {
    path: z.string().describe("assets 相対パス。例: scenes/title.json"),
  },
  ({ path }) => run(() => engine.call("open_scene", { path })),
);

server.tool(
  "dx12_list_scenes",
  "assets/scenes 配下のシーン(.json)一覧(path, name)を返す。dx12_open_scene の path を選ぶのに使う。",
  {},
  () => run(() => engine.call("list_scenes", {})),
);

// アセット --------------------------------------------------------

server.tool(
  "dx12_list_assets",
  "assets 配下のアセット一覧(path, type, name)を返す。type で種別フィルタ(省略で全種別)。spawn_model の path 探索などに使う。",
  {
    type: z
      .enum(["model", "texture", "script", "audio", "scene", "prefab"])
      .optional()
      .describe("種別フィルタ。省略で全種別。"),
  },
  ({ type }) => run(() => engine.call("list_assets", { type })),
);

server.tool(
  "dx12_spawn_model",
  "モデル(.gltf/.glb/.fbx/.obj)を assets 相対パスから生成する。GPU ロードを伴うためフレーム境界で遅延処理され id は即返らない。name を付けて後から dx12_list_entities で引く。",
  {
    path: z.string().describe("assets 相対パス。例: models/player.glb"),
    position: z.array(z.number()).length(3).optional().describe("[x,y,z]。省略時 [0,0,0]。"),
    name: z.string().optional().describe("エンティティ名。省略時はファイル名(拡張子なし)。"),
  },
  ({ path, position, name }) =>
    run(() => engine.call("spawn_model", { path, position, name })),
);

// コンポーネント --------------------------------------------------

server.tool(
  "dx12_set_component",
  "エンティティのコンポーネントを設定(上書き)する。component は jsonKey(例 pointLight/rigidBody/camera/transform)、data は dx12_get_entity が返すのと同じ形。既存があれば置換、無ければ追加。meshRenderer は非対応。",
  {
    entity: z.number().int().describe("エンティティ id"),
    component: z
      .string()
      .describe("jsonKey。例: pointLight, directionalLight, spotLight, camera, rigidBody, boxCollider, transform"),
    data: z.record(z.unknown()).describe("コンポーネントの値。dx12_get_entity の該当 jsonKey の中身と同形。"),
  },
  ({ entity, component, data }) =>
    run(() => engine.call("set_component", { entity, component, data })),
);

server.tool(
  "dx12_remove_component",
  "エンティティからコンポーネントを除去する。component は jsonKey(例 pointLight/rigidBody/boxCollider)。transform/name はコア不変のため除去不可。",
  {
    entity: z.number().int().describe("エンティティ id"),
    component: z
      .string()
      .describe("jsonKey。例: pointLight, rigidBody, boxCollider, sphereCollider, camera, tag"),
  },
  ({ entity, component }) =>
    run(() => engine.call("remove_component", { entity, component })),
);

// 階層 / 命名 -----------------------------------------------------

server.tool(
  "dx12_set_parent",
  "エンティティの親を設定する。parent 省略で親を解除。サイクルになる指定は拒否される。",
  {
    entity: z.number().int().describe("子エンティティ id"),
    parent: z.number().int().optional().describe("親エンティティ id。省略で親解除。"),
  },
  ({ entity, parent }) => run(() => engine.call("set_parent", { entity, parent })),
);

server.tool(
  "dx12_rename_entity",
  "エンティティの名前を変更する。重複名は連番(name_2 など)が付与され、確定した name を返す。",
  {
    entity: z.number().int().describe("エンティティ id"),
    name: z.string().describe("新しい名前"),
  },
  ({ entity, name }) => run(() => engine.call("rename_entity", { entity, name })),
);

// 再生制御 / 状態 -------------------------------------------------

server.tool(
  "dx12_play",
  "Editor -> Playing へ切り替える。切替はフレーム境界で遅延実行(queued を返す)。既に Playing なら即 mode を返す。",
  {},
  () => run(() => engine.call("play", {})),
);

server.tool(
  "dx12_stop",
  "Playing -> Editor へ切り替える(再生前のスナップショットに復元)。フレーム境界で遅延実行。既に Editor なら即 mode を返す。",
  {},
  () => run(() => engine.call("stop", {})),
);

server.tool(
  "dx12_get_mode",
  "現在のエンジンモード(Editor / Playing)を返す。",
  {},
  () => run(() => engine.call("get_mode", {})),
);

server.tool(
  "dx12_get_log",
  "エンジンのログ(dx12_engine.log)の末尾 N 行を返す。エラーや print の確認に使う。",
  {
    lines: z.number().int().optional().describe("取得行数(既定 50)。"),
  },
  ({ lines }) => run(() => engine.call("get_log", { lines })),
);

server.tool(
  "dx12_screenshot",
  "今シーンビューに映っている絵を PNG に書き出し、その絶対パスを返す(result.path/width/height)。" +
    "返ってきた path を画像として開けば、AI が自分の操作結果(配置・見た目)を目で確認して直せる。" +
    "毎回同じファイル(<エンジンCWD>/mcp_screenshot.png)へ上書き。引数なし。",
  {},
  () => run(() => engine.call("screenshot", {})),
);

const transport = new StdioServerTransport();
await server.connect(transport);
