import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import fs from "node:fs";
import { EngineClient } from "./engineClient.ts";

// DX12 ゲームエンジン用 MCP サーバ。Codex / Claude Code から接続し、
// 起動中のエディタ(TCP 127.0.0.1:<port>)を叩いてゲームを作っていくための入口。
//
// ★遅延同期: create/spawn/delete/duplicate/open_scene/new_scene/play/stop は
//   エンジンがフレーム境界で実処理してから【同じ id】で本物の result を返す。
//   このサーバは id で待つだけなので、ツールは本物の entityId 等を【同期で】返す。
//   旧来の「{queued} が返るので後で name で list して探す」パターンは完全廃止。
//
// ツール名は dx12_ 接頭辞。entity パラメータ(int)はエンジンに合わせてそのまま渡す(変換しない)。
// result のフィールド名(entityId 等)もエンジンの返り値をそのまま通す。

const engine = new EngineClient();
const server = new McpServer({ name: "dx12-engine", version: "0.3.0" });

type ToolResult = {
  content: ({ type: "text"; text: string } | { type: "image"; data: string; mimeType: string })[];
  structuredContent?: Record<string, unknown>;
  isError?: boolean;
};

// 全 JSON ツール共通の outputSchema。エンジンの result は method ごとに形が違い、
// 配列や null も返る(list_scenes 等)。structuredContent は JSON オブジェクト必須なので
// { result: <生の結果> } で一様にラップする(z.any() なので必ず検証を通る)。
// ※Claude Code / Codex は structuredContent を読まないため、本体は content[0].text の JSON 文字列。
const OUT = {
  result: z.any().describe("エンジンからの生の結果。実際の形は各ツールの説明 / dx12_describe_components を参照。text にも同内容を JSON 文字列で格納。"),
};

// エラーを日本語整形(error_code があれば付ける)。isError:true なら outputSchema 検証はスキップされる。
function errResult(e: any): ToolResult {
  const code = e?.code;
  const msg = code != null ? `エラー(code=${code}): ${e.message}` : `エラー: ${e.message}`;
  return { content: [{ type: "text", text: msg }], isError: true };
}

// JSON 結果ツール用ラッパ。result を text(JSON 文字列) + structuredContent({result}) の両方に入れる。
async function run(fn: () => Promise<unknown>): Promise<ToolResult> {
  try {
    const data = await fn();
    const text = typeof data === "string" ? data : JSON.stringify(data, null, 2);
    return {
      content: [{ type: "text", text }],
      structuredContent: { result: data ?? null },
    };
  } catch (e: any) {
    return errResult(e);
  }
}

// 画像結果(PNG)を image ブロック + text(path/サイズ) で返す。
function imageResult(pngPath: string, extra: Record<string, unknown>): ToolResult {
  const data = fs.readFileSync(pngPath).toString("base64");
  return {
    content: [
      { type: "image", data, mimeType: "image/png" },
      { type: "text", text: JSON.stringify({ path: pngPath, ...extra }) },
    ],
  };
}

type Ann = { readOnlyHint?: boolean; destructiveHint?: boolean; idempotentHint?: boolean };

// JSON ツール登録ヘルパ。openWorldHint は常に false(外部世界とやり取りしない閉じたツール群)。
function reg(
  name: string,
  title: string,
  description: string,
  inputSchema: Record<string, z.ZodTypeAny>,
  ann: Ann,
  handler: (args: any) => Promise<ToolResult>,
) {
  server.registerTool(
    name,
    {
      title,
      description,
      inputSchema,
      outputSchema: OUT,
      annotations: { title, openWorldHint: false, ...ann },
    },
    handler,
  );
}

// ── 共通 zod 部品 ────────────────────────────────────────────────
const vec3 = z.array(z.number()).length(3);
const entityId = z.number().int().describe("エンティティ id(int)。dx12_list_entities / dx12_find_entity で取得。");

// ════════════════════════════════════════════════════════════════
//  読み取り系(同期・readOnly)
// ════════════════════════════════════════════════════════════════

reg(
  "dx12_ping",
  "疎通確認",
  "エディタとの疎通確認。mode(Editor/Playing)・entityCount・sceneGeneration・currentScene・protocolVersion を返す。まず最初に叩いて生きてるか確認するのに使う。",
  {},
  { readOnlyHint: true },
  () => run(() => engine.call("ping", {})),
);

reg(
  "dx12_list_entities",
  "エンティティ一覧",
  "今開いてるシーンのエンティティ一覧(entityId, name)を返す。verbose で componentTypes も付く。name_prefix / component_type で絞り込み可。{entities, count, sceneGeneration} が返る。",
  {
    verbose: z.boolean().optional().describe("true で各エンティティの componentTypes も含める。"),
    name_prefix: z.string().optional().describe("名前の前方一致フィルタ。"),
    component_type: z.string().optional().describe("指定 jsonKey を持つものだけに絞る(例 pointLight)。"),
  },
  { readOnlyHint: true },
  ({ verbose, name_prefix, component_type }) =>
    run(() => engine.call("list_entities", { verbose, name_prefix, component_type })),
);

reg(
  "dx12_get_entity",
  "エンティティ詳細",
  "エンティティの全コンポーネントと値を JSON で読む(編集前の状態確認に使う)。返り値は entityId, componentTypes, sceneGeneration と、各コンポーネントの jsonKey をキーにした値。",
  { entity: entityId },
  { readOnlyHint: true },
  ({ entity }) => run(() => engine.call("get_entity", { entity })),
);

reg(
  "dx12_find_entity",
  "名前でエンティティ検索",
  "名前の完全一致でエンティティを1件探す。見つかれば {entityId, name}、無ければ null。",
  { name: z.string().describe("探すエンティティ名(完全一致)。") },
  { readOnlyHint: true },
  ({ name }) => run(() => engine.call("find_entity", { name })),
);

reg(
  "dx12_query_entities",
  "タグ/領域でエンティティ検索",
  "tag か box のどちらかで複数エンティティを探す(どちらか必須)。box は XZ 平面の矩形 [minX,minZ,maxX,maxZ]。{entities:[{entityId,name}], count} を返す。",
  {
    tag: z.string().optional().describe("このタグを持つエンティティを列挙。"),
    box: z.array(z.number()).length(4).optional().describe("[minX,minZ,maxX,maxZ]。この XZ 矩形に入るエンティティを列挙。"),
  },
  { readOnlyHint: true },
  ({ tag, box }) => run(() => engine.call("query_entities", { tag, box })),
);

reg(
  "dx12_list_scenes",
  "シーン一覧",
  "assets/scenes 配下のシーン(.json)一覧 [{path, name}] を返す。dx12_open_scene の path を選ぶのに使う。",
  {},
  { readOnlyHint: true },
  () => run(() => engine.call("list_scenes", {})),
);

reg(
  "dx12_list_assets",
  "アセット一覧",
  "assets 配下のアセット一覧 [{path, type, name}] を返す。type で種別フィルタ(省略で全種別)。spawn_model / spawn_prefab / attach の path 探索に使う。",
  {
    type: z.enum(["model", "texture", "script", "audio", "scene", "prefab"]).optional().describe("種別フィルタ。省略で全種別。"),
  },
  { readOnlyHint: true },
  ({ type }) => run(() => engine.call("list_assets", { type })),
);

reg(
  "dx12_get_mode",
  "モード取得",
  "現在のエンジンモード(Editor / Playing)を返す。",
  {},
  { readOnlyHint: true },
  () => run(() => engine.call("get_mode", {})),
);

reg(
  "dx12_get_log",
  "ログ取得",
  "エンジンログの末尾 N 行を配列で返す。エラーや print() の確認に使う。",
  { lines: z.number().int().optional().describe("取得行数(既定 50)。") },
  { readOnlyHint: true },
  ({ lines }) => run(() => engine.call("get_log", { lines })),
);

reg(
  "dx12_describe_components",
  "コンポーネント辞書",
  "set_component する前にフィールドを知るための辞書。component 省略で全コンポーネント、指定でそれだけ。返り値 components:[{jsonKey, settable, removable, fields:[{name,type,default}], note?}]。dx12_set_component の data を組み立てる前に必ず参照すると確実。",
  { component: z.string().optional().describe("特定 jsonKey の定義だけ欲しい時に指定(例 pointLight)。省略で全件。") },
  { readOnlyHint: true },
  ({ component }) => run(() => engine.call("describe_components", { component })),
);

reg(
  "dx12_get_scene_settings",
  "シーン設定取得",
  "シーンのスカイボックス/IBL 設定を返す。{skybox:{envMapPath,iblIntensity,skyboxIntensity,drawSkybox}, note}。dx12_set_scene_settings で変える前の確認に使う。",
  {},
  { readOnlyHint: true },
  () => run(() => engine.call("get_scene_settings", {})),
);

// ════════════════════════════════════════════════════════════════
//  編集系(同期)
// ════════════════════════════════════════════════════════════════

reg(
  "dx12_set_transform",
  "Transform 設定",
  "エンティティの Transform を設定する。指定したフィールドだけ更新。回転は rotation(Euler 度) か quaternion([x,y,z,w]) のどちらか。即時反映で ok を返す。",
  {
    entity: entityId,
    position: vec3.optional().describe("[x,y,z]"),
    rotation: vec3.optional().describe("[x,y,z] Euler 度。quaternion と併用しない。"),
    quaternion: z.array(z.number()).length(4).optional().describe("[x,y,z,w] クォータニオン。rotation と併用しない。"),
    scale: vec3.optional().describe("[x,y,z]"),
  },
  { idempotentHint: true },
  ({ entity, position, rotation, quaternion, scale }) =>
    run(() => engine.call("set_transform", { entity, position, rotation, quaternion, scale })),
);

reg(
  "dx12_set_component",
  "コンポーネント設定",
  "コンポーネントを設定(無ければ追加・あれば置換)。component は jsonKey、data は dx12_describe_components の形。tags は data=文字列配列、DataComponent(data) は {key:{t,v}} オブジェクト。即時反映で {entityId, component} を返す。形が不安なら先に dx12_describe_components を見るとええ。",
  {
    entity: entityId,
    component: z.string().describe("jsonKey。例: pointLight, directionalLight, spotLight, camera, rigidBody, boxCollider, transform, tags, data"),
    data: z.union([z.record(z.any()), z.array(z.any())]).describe("コンポーネントの値。オブジェクト or 配列(tags は文字列配列)。dx12_describe_components の fields に合わせる。"),
  },
  { idempotentHint: true },
  ({ entity, component, data }) =>
    run(() => engine.call("set_component", { entity, component, data })),
);

reg(
  "dx12_remove_component",
  "コンポーネント除去",
  "エンティティからコンポーネントを除去する。component は jsonKey。transform/name などコア不変のものは除去不可。即時反映で {entityId, removed} を返す。",
  {
    entity: entityId,
    component: z.string().describe("除去する jsonKey。例: pointLight, rigidBody, boxCollider, sphereCollider, camera, tags"),
  },
  { idempotentHint: true },
  ({ entity, component }) =>
    run(() => engine.call("remove_component", { entity, component })),
);

reg(
  "dx12_set_parent",
  "親子設定",
  "エンティティの親を設定する。parent 省略で親を解除。サイクルになる指定は拒否。即時反映で ok を返す。",
  {
    entity: entityId,
    parent: z.number().int().optional().describe("親エンティティ id。省略で親解除。"),
  },
  { idempotentHint: true },
  ({ entity, parent }) => run(() => engine.call("set_parent", { entity, parent })),
);

reg(
  "dx12_rename_entity",
  "リネーム",
  "エンティティ名を変更する。重複名は連番(name_2 など)が付与され、確定した {name} を返す。",
  {
    entity: entityId,
    name: z.string().describe("新しい名前。"),
  },
  { idempotentHint: true },
  ({ entity, name }) => run(() => engine.call("rename_entity", { entity, name })),
);

reg(
  "dx12_select_entity",
  "選択",
  "エディタ上で対象エンティティを選択状態にする(Inspector 表示が切り替わる)。{selected} を返す。",
  { entity: entityId },
  { idempotentHint: true },
  ({ entity }) => run(() => engine.call("select_entity", { entity })),
);

reg(
  "dx12_focus_camera",
  "カメラフォーカス",
  "エディタのフライカメラを対象エンティティに寄せる。{cameraPos, target, distance} を返す。撮影前に画角を合わせるのに使う(dx12_focus_and_screenshot もある)。",
  { entity: entityId },
  { idempotentHint: true },
  ({ entity }) => run(() => engine.call("focus_camera", { entity })),
);

reg(
  "dx12_set_pbr",
  "PBR マテリアル設定",
  "エンティティの PBR パラメータ(metallic/roughness/UV スケール)を設定する。指定分のみ更新。即時反映で {entityId, metallic, roughness, uvScaleU, uvScaleV} を返す。",
  {
    entity: entityId,
    metallic: z.number().optional().describe("金属度 0..1"),
    roughness: z.number().optional().describe("粗さ 0..1"),
    uvScaleU: z.number().optional().describe("UV の U 方向スケール(タイリング)"),
    uvScaleV: z.number().optional().describe("UV の V 方向スケール(タイリング)"),
  },
  { idempotentHint: true },
  ({ entity, metallic, roughness, uvScaleU, uvScaleV }) =>
    run(() => engine.call("set_pbr", { entity, metallic, roughness, uvScaleU, uvScaleV })),
);

reg(
  "dx12_set_scene_settings",
  "シーン設定変更",
  "シーンのスカイボックス/IBL を設定する。skybox 内の指定フィールドだけ適用。envMapPath を変えると {applied, envMapRebake} を返し再ベイクが走ることがある。",
  {
    skybox: z.object({
      envMapPath: z.string().optional().describe("環境マップ(HDR/EXR 等)の assets 相対パス。"),
      iblIntensity: z.number().optional().describe("IBL(間接光)の強さ。"),
      skyboxIntensity: z.number().optional().describe("スカイボックス描画の明るさ。"),
      drawSkybox: z.boolean().optional().describe("スカイボックスを描画するか。"),
    }).describe("スカイボックス設定。指定したフィールドのみ適用。"),
  },
  { idempotentHint: true },
  ({ skybox }) => run(() => engine.call("set_scene_settings", { skybox })),
);

reg(
  "dx12_undo",
  "Undo",
  "直前の編集操作を取り消す。フレーム境界で適用され {queuedUndo} を返す(取り消し自体は次フレームで反映)。",
  {},
  {},
  () => run(() => engine.call("undo", {})),
);

reg(
  "dx12_redo",
  "Redo",
  "取り消した操作をやり直す。フレーム境界で適用され {queuedRedo} を返す。",
  {},
  {},
  () => run(() => engine.call("redo", {})),
);

reg(
  "dx12_save_scene",
  "シーン保存",
  "現在のシーンを保存する。path は assets 相対(例 scenes/title.json)。省略時は現在開いてるシーンへ上書き。{path} を返す。",
  { path: z.string().optional().describe("assets 相対パス。例: scenes/title.json。省略で上書き保存。") },
  { idempotentHint: true },
  ({ path }) => run(() => engine.call("save_scene", { path })),
);

reg(
  "dx12_create_lua_component",
  "Luaコンポーネント作成",
  "Lua コンポーネント(.lua)を assets/components/ に作成する。書き込み前に構文検証され、エラーなら書かず error を返す。返り値 {path} を dx12_attach_lua_component の script に渡す。",
  {
    name: z.string().describe("コンポーネント名(拡張子・パス区切りなし)。例: Health"),
    code: z.string().describe("Lua コード全体。properties / OnStart / OnUpdate を含められる。"),
  },
  {},
  ({ name, code }) => run(() => engine.call("create_lua_component", { name, code })),
);

reg(
  "dx12_attach_lua_component",
  "Luaコンポーネントアタッチ",
  "Lua コンポーネントをエンティティにアタッチする。エディタ上では貼るだけで、実際の初期化/実行は Play 時(OnStart/OnUpdate)。script は assets 相対(assets 配下限定)。即時反映で ok を返す。",
  {
    entity: entityId,
    script: z.string().describe("assets 相対パス。例: components/Health.lua"),
  },
  {},
  ({ entity, script }) => run(() => engine.call("attach_lua_component", { entity, script })),
);

// ════════════════════════════════════════════════════════════════
//  編集系(遅延同期)— 本物の結果が【同期で】返る。{queued} は返らへん。
// ════════════════════════════════════════════════════════════════

reg(
  "dx12_create_entity",
  "エンティティ生成",
  "エンティティを生成する(エディタ専用)。フレーム境界で実処理されるが、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】({queued} は返らへん)。idempotency_key を付けると、再試行で同じキーが来ても二重生成されず同じ結果が返る。",
  {
    type: z.enum(["box", "sphere", "plane", "empty"]).describe("プリミティブ種別。empty は Transform のみ。"),
    name: z.string().optional().describe("エンティティ名(一意推奨)。省略時は種別名。"),
    position: vec3.optional().describe("[x,y,z]。省略時 [0,0,0]。"),
    idempotency_key: z.string().optional().describe("再試行の重複防止キー。同じキーの再送は二重生成されない。"),
  },
  {},
  ({ type, name, position, idempotency_key }) =>
    run(() => engine.call("create_entity", { type, name, position, idempotency_key })),
);

reg(
  "dx12_spawn_model",
  "モデル生成",
  "モデル(.gltf/.glb/.fbx/.obj)を assets 相対パスから生成する。GPU ロードを伴いフレーム境界で実処理されるが、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】。idempotency_key で再試行の二重生成を防げる。",
  {
    path: z.string().describe("assets 相対パス。例: models/player.glb"),
    position: vec3.optional().describe("[x,y,z]。省略時 [0,0,0]。"),
    name: z.string().optional().describe("エンティティ名。省略時はファイル名(拡張子なし)。"),
    idempotency_key: z.string().optional().describe("再試行の重複防止キー。同じキーの再送は二重生成されない。"),
  },
  {},
  ({ path, position, name, idempotency_key }) =>
    run(() => engine.call("spawn_model", { path, position, name, idempotency_key })),
);

reg(
  "dx12_spawn_prefab",
  "プレハブ生成",
  "プレハブ(.prefab)を assets 相対パスから生成する。フレーム境界で実処理され、Node が完了を待って【本物の {entityId, rootEntityId, entityIds:[...], name, sceneGeneration} を同期で返す】。",
  {
    path: z.string().describe("assets 相対パス。例: prefabs/enemy.prefab"),
    position: vec3.optional().describe("[x,y,z]。省略時 [0,0,0]。"),
    name: z.string().optional().describe("ルートエンティティ名。省略時はプレハブ名。"),
  },
  {},
  ({ path, position, name }) =>
    run(() => engine.call("spawn_prefab", { path, position, name })),
);

reg(
  "dx12_duplicate_entity",
  "複製",
  "エンティティを子ごとディープ複製する。フレーム境界で実処理され、Node が完了を待って【本物の {entityId, name, sceneGeneration} を同期で返す】。",
  { entity: entityId },
  {},
  ({ entity }) => run(() => engine.call("duplicate_entity", { entity })),
);

reg(
  "dx12_delete_entity",
  "削除",
  "エンティティを子ごと削除する(Undo 可)。フレーム境界で実処理され、Node が完了を待って【本物の {deletedEntityId, deletedCount, sceneGeneration} を同期で返す】。",
  { entity: entityId },
  { destructiveHint: true },
  ({ entity }) => run(() => engine.call("delete_entity", { entity })),
);

reg(
  "dx12_open_scene",
  "シーンを開く",
  "シーンを開く(現在のシーンを置換)。path は assets 相対。重い遷移をフレーム境界で実処理し、Node が完了を待って【本物の {sceneName, path, entityCount, sceneGeneration} を同期で返す】。開いた後は古い entityId は無効になる(sceneGeneration が変わる)ので list し直すこと。",
  { path: z.string().describe("assets 相対パス。例: scenes/title.json") },
  {},
  ({ path }) => run(() => engine.call("open_scene", { path })),
);

reg(
  "dx12_new_scene",
  "新規シーン",
  "新規シーンを作る(現在のシーンを破棄)。savePath を渡すとそのパスに紐づけて作る。フレーム境界で実処理され {applied} を同期で返す。現在の編集内容は失われるので注意。",
  { savePath: z.string().optional().describe("新シーンの保存先 assets 相対パス(任意)。") },
  { destructiveHint: true },
  ({ savePath }) => run(() => engine.call("new_scene", { savePath })),
);

reg(
  "dx12_play",
  "再生開始",
  "Editor → Playing へ切り替える。フレーム境界で実処理され {mode:'Playing', sceneGeneration} を同期で返す。カメラ無し等で再生不可なら error(code=3 MODE_CONFLICT)。",
  {},
  {},
  () => run(() => engine.call("play", {})),
);

reg(
  "dx12_stop",
  "再生停止",
  "Playing → Editor へ切り替える(再生前のスナップショットに復元)。フレーム境界で実処理され {mode:'Editor', sceneGeneration} を同期で返す。",
  {},
  {},
  () => run(() => engine.call("stop", {})),
);

// ════════════════════════════════════════════════════════════════
//  合成ツール(エンジンには無い。Node 内で複数 call を順に行う)
// ════════════════════════════════════════════════════════════════

reg(
  "dx12_batch",
  "一括実行",
  "複数のエンジン操作を順番に実行して往復を減らす。各 op は engine の method 名(dx12_ 接頭辞なし。例 create_entity)と params。結果は {results:[{index, ok, result?|error?, error_code?, skipped?}]}。stopOnError=true なら最初の失敗で打ち切り、残りは skipped 記録。各 op は同期結果なので確実(ただし1フレーム原子性は無い)。",
  {
    ops: z.array(z.object({
      method: z.string().describe("エンジン method 名(dx12_ 接頭辞なし)。例: create_entity, set_component"),
      params: z.record(z.any()).optional().describe("その method の params。省略で {}。"),
    })).describe("順に実行する操作の配列。"),
    stopOnError: z.boolean().optional().describe("true なら最初の失敗で打ち切り、残りを skipped 記録。"),
  },
  {},
  ({ ops, stopOnError }) => run(async () => {
    const results: any[] = [];
    let aborted = false;
    for (let i = 0; i < ops.length; i++) {
      if (aborted) { results.push({ index: i, ok: false, skipped: true }); continue; }
      const op = ops[i];
      try {
        const r = await engine.call(op.method, op.params ?? {});
        results.push({ index: i, ok: true, result: r });
      } catch (e: any) {
        const entry: any = { index: i, ok: false, error: e.message };
        if (e.code != null) entry.error_code = e.code;
        results.push(entry);
        if (stopOnError) aborted = true;
      }
    }
    return { results };
  }),
);

// 画像を返す合成ツール(focus → 1フレーム描画 → 撮影)。outputSchema は宣言しない(構造化結果ではなく image)。
server.registerTool(
  "dx12_focus_and_screenshot",
  {
    title: "寄せて撮影",
    description: "カメラを対象エンティティに寄せてから(1フレーム描画を挟んで)スクショを撮り、PNG 画像で返す。配置や見た目を自分の目で確認するのに使う。image ブロック + text(path/サイズ)を返す。",
    inputSchema: { entity: entityId },
    annotations: { title: "寄せて撮影", openWorldHint: false, idempotentHint: true },
  },
  async ({ entity }) => {
    try {
      await engine.call("focus_camera", { entity });
      const shot = await engine.call("screenshot", {});
      if (!shot || !shot.path) throw new Error("screenshot が path を返さんかった");
      return imageResult(shot.path, { entity, width: shot.width, height: shot.height });
    } catch (e: any) {
      return errResult(e);
    }
  },
);

// スクショ単体も画像ブロックで返す。
server.registerTool(
  "dx12_screenshot",
  {
    title: "スクリーンショット",
    description: "今シーンビューに映ってる絵を PNG に書き出して画像で返す(+text に path/width/height)。AI が自分の操作結果(配置・見た目)を目で確認して直すのに使う。引数なし。",
    inputSchema: {},
    annotations: { title: "スクリーンショット", openWorldHint: false, readOnlyHint: true },
  },
  async () => {
    try {
      const shot = await engine.call("screenshot", {});
      if (!shot || !shot.path) throw new Error("screenshot が path を返さんかった");
      return imageResult(shot.path, { width: shot.width, height: shot.height });
    } catch (e: any) {
      return errResult(e);
    }
  },
);

const transport = new StdioServerTransport();
await server.connect(transport);
