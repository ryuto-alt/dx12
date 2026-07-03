# MCP / AI Bridge — 完全リファレンス

起動中の DX12 エディタを Claude Code / Codex から操作するための MCP(Model Context Protocol)連携ガイド。
AI がシーンを読み・エンティティを生成し・コンポーネントを設定し・Lua を貼り・Play/Stop まで回せる。

---

## ★ 最重要: 遅延同期の仕組み(旧 `queued:true` は廃止)

`create_entity` / `spawn_model` / `spawn_prefab` / `duplicate_entity` / `delete_entity` /
`open_scene` / `new_scene` / `play` / `stop` は **「遅延同期」** で動く。

- エンジン内部ではフレーム境界(GPU cmdList が使える瞬間)で実処理する。
- **ただし Node 側は id で待つだけでよい。** エンジンは処理完了後に【同じ id】で本物の result を返す。
- 旧仕様の `{queued:true}` は**もう返ってこない**。`entityId` / `sceneGeneration` 等の本物の値が直接返る。
- **「名前で list して探す」旧パターンは完全廃止。** 返ってきた `entityId` を使い続ける。

```
AI → dx12_create_entity(type:"box", name:"Floor")
        ↓ (フレーム境界まで待機)
エンジン → {entityId: 42, name: "Floor", sceneGeneration: 7}
AI → dx12_set_transform(entity: 42, ...)   ← そのまま entityId を使う
```

---

## 1. セットアップ

```bash
cd tools/mcp-server

# Windows:
./install.ps1

# Linux / macOS:
./install.sh
```

スクリプトは Node v24+ を確認し `npm install` と自己テスト(`npm test`、エンジン不要)を実行したあと
絶対パス解決済みの登録コマンドを表示する。手動の場合:

```bash
cd tools/mcp-server
npm install
node test.ts        # 自己テスト(フレーミング/相関/エラー)
```

Node v24+ が `.ts` を直接実行するため `tsc` ビルドは不要。

---

## 2. 接続

`<REPO>` は clone した絶対パスに置換する(Windows でもパスは `/` 区切りで可)。

### Claude Code(CLI)
```bash
claude mcp add dx12-engine -- node <REPO>/tools/mcp-server/index.ts
```

### Claude Code(`.mcp.json`)
`tools/mcp-server/.mcp.json.example` をコピーして `.mcp.json` を作り `<REPO>` を置換する。
`.mcp.json` は `.gitignore` 済み=各自で生成すること。

> 注意: 既定では `env` に `DX12_MCP_PORT` を書かないこと。書くとポート自動探索(`%TEMP%/dx12_mcp.port`)が
> 無効化され、エディタが 8787 を取れず 8788 等に回った時に繋がらなくなる。ポートを固定したい時だけ書く。

```json
{
  "mcpServers": {
    "dx12-engine": {
      "command": "node",
      "args": ["<REPO>/tools/mcp-server/index.ts"]
    }
  }
}
```

### Codex(`~/.codex/config.toml`)
```toml
[mcp_servers.dx12-engine]
command = "node"
args = ["<REPO>/tools/mcp-server/index.ts"]
```

---

## 3. ポート自動探索

エンジンは起動時に空きポートを **8787〜8797** の範囲で自動採番し、確定したポート番号を
`%TEMP%\dx12_mcp.port`(Windows)または `$TMPDIR/dx12_mcp.port` に書き込む。

Node(engineClient)はポートを以下の順で解決する:

| 優先度 | 手段 |
|--------|------|
| 1 | 環境変数 `DX12_MCP_PORT` |
| 2 | ファイル `<os.tmpdir()>/dx12_mcp.port` |
| 3 | 既定値 `8787` |

ホストは `DX12_MCP_HOST`(既定: `127.0.0.1`)で変更可。別マシンのエディタを叩く場合は
SSH ポートフォワード推奨(エンジン側は `127.0.0.1` のみ待受)。

---

## 4. ツール一覧

MCP ツール名は `dx12_` 接頭辞付き。同期欄: **同期** = 即返り、**遅延同期** = フレーム境界後に本物の値が返る。

### 4-1. 読み取り系(全て同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_ping` | `{}` | `{pong, mode, entityCount, sceneGeneration, currentScene, protocolVersion}` |
| `dx12_list_entities` | `{verbose?:bool, name_prefix?:string, component_type?:string}` | `{entities:[{entityId,id,name,componentTypes?}], count, sceneGeneration}` |
| `dx12_get_entity` | `{entity:int}` | `{entityId, componentTypes:[...], sceneGeneration, ...(全コンポーネント値)...}` |
| `dx12_find_entity` | `{name:string}` | `{entityId, name}` または `null` |
| `dx12_query_entities` | `{tag?:string, box?:[minX,minZ,maxX,maxZ]}` | `{entities:[{entityId,name}], count}` ※tag か box のどちらか必須 |
| `dx12_list_scenes` | `{}` | `[{path, name}]` |
| `dx12_list_assets` | `{type?:"model"\|"texture"\|"script"\|"audio"\|"scene"\|"prefab"\|"shader"}` | `[{path, type, name}]` |
| `dx12_get_mode` | `{}` | `{mode:"Editor"\|"Playing"}` |
| `dx12_get_log` | `{lines?:int=50}` | `["ログ行", ...]`(末尾N行) |
| `dx12_describe_components` | `{component?:string}` | `{components:[{jsonKey, settable, removable, fields:[{name,type,default}], note?}]}` |
| `dx12_get_scene_settings` | `{}` | `{skybox:{envMapPath, iblIntensity, skyboxIntensity, drawSkybox}, note}` |
| `dx12_get_post_process` | `{}` | ポストプロセス全フィールド(約25エフェクトの `<name>On`/パラメータ) |
| `dx12_get_ssao` | `{}` | `{enabled, radius, bias, intensity, power, sampleCount, blur}` |
| `dx12_read_lua_component` | `{path:string}` | `{path, code}` ※既存 .lua のソースをそのまま読む |
| `dx12_read_shader` | `{path:string(assets/shaders相対)}` | `{path, code, compiled}` ※既存カスタムシェーダーのソースをそのまま読む(compiled は直近の既知のコンパイル成否) |
| `dx12_raycast` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f}` | `{hit, distance?, point?, normal?, entityId?, name?}` ※Playing 中のみ意味のある結果 |
| `dx12_overlap_box` | `{center:[x,y,z], halfExtents:[x,y,z], maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_overlap_sphere` | `{center:[x,y,z], radius:f, maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_get_physics_state` | `{entity:int}` | `{entityId, hasRigidBody, velocity:[x,y,z], hasCharacterController, isGrounded}` ※Playing 中のみ |
| `dx12_validate_scene` | `{path?:string}` | `{pass, exitCode, report, scenePath}` ※`--validate` をヘッドレス子プロセスで実行。省略時は現在のシーン |
| `dx12_screenshot` | `{}` | PNG 画像ブロック + text(`{path(絶対パス), width, height}`) |

### 4-2. 編集系(同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_set_transform` | `{entity:int, position?:[x,y,z], rotation?:[x,y,z](Euler度), quaternion?:[x,y,z,w], scale?:[x,y,z]}` | `ok` |
| `dx12_set_component` | `{entity:int, component:string(jsonKey), data:object\|array}` | `{entityId, component}` |
| `dx12_remove_component` | `{entity:int, component:string}` | `{entityId, removed}` |
| `dx12_set_parent` | `{entity:int, parent?:int}` | `ok` ※parent 省略で親解除 |
| `dx12_rename_entity` | `{entity:int, name:string}` | `{name}` ※重複は連番付与 |
| `dx12_select_entity` | `{entity:int}` | `{selected}` |
| `dx12_focus_camera` | `{entity:int}` | `{cameraPos:[x,y,z], target, distance}` |
| `dx12_set_pbr` | `{entity:int, metallic?:f, roughness?:f, uvScaleU?:f, uvScaleV?:f}` | `{entityId, metallic, roughness, uvScaleU, uvScaleV}` |
| `dx12_set_mesh_shader` | `{entity:int, shaderPath?:string(assets/shaders相対)}` | `{entityId, shaderPath, skinnedFallbackWarning}` ※shaderPath省略/空文字で既定Forwardに戻す |
| `dx12_set_scene_settings` | `{skybox:{envMapPath?, iblIntensity?, skyboxIntensity?, drawSkybox?}}` | `{applied, envMapRebake}` |
| `dx12_set_post_process` | 約25エフェクトの `<name>On`/パラメータ(指定分のみ適用) | `{applied}` |
| `dx12_set_ssao` | `{enabled?, radius?, bias?, intensity?, power?, sampleCount?, blur?}` | `{applied}` |
| `dx12_undo` | `{}` | `{queuedUndo}` |
| `dx12_redo` | `{}` | `{queuedRedo}` |
| `dx12_save_scene` | `{path?:string}` | `{path}` ※省略で現在シーンへ上書き |
| `dx12_create_lua_component` | `{name:string, code:string}` | `{path}` ※書込前に構文検証。既存パスなら上書き更新も兼ねる |
| `dx12_create_shader` | `{name:string, code:string}` | `{path, compiled, error?}` ※assets/shaders/に作成/上書き後、即コンパイルを試す。Luaと違い失敗してもファイルは残る(反復修正前提) |
| `dx12_attach_lua_component` | `{entity:int, script:string(assets相対)}` | `ok` |
| `dx12_create_prefab` | `{entity:int, path?:string}` | `{path, entityId}` ※path省略で assets/prefabs/<name>.prefab |
| `dx12_eval_lua` | `{code:string}` | `{result:string}` ※任意 Lua をその場実行(デバッグ用) |
| `dx12_build_game` | `{}` | `{success, outputDir, error?}` ※ヘッドレスビルド(同期・数十秒かかることあり) |

### 4-3. 生成・削除・モード遷移(遅延同期 — 本物の値が返る)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_create_entity` | `{type:"box"\|"sphere"\|"plane"\|"empty"\|"camera"\|"light_directional"\|"light_point"\|"light_spot"\|"particle_emitter"\|"trigger", name?, position?:[x,y,z], idempotency_key?:string}` | `{entityId, name, sceneGeneration}` ※light_*/camera/particle_emitter/trigger は既定値で生成(dx12_set_component で調整) |
| `dx12_spawn_model` | `{path:string(.gltf/.glb/.fbx/.obj), position?:[x,y,z], name?, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` |
| `dx12_spawn_prefab` | `{path:string(.prefab), position?, name?}` | `{entityId, rootEntityId, entityIds:[...], name, sceneGeneration}` |
| `dx12_duplicate_entity` | `{entity:int}` | `{entityId, name, sceneGeneration}` |
| `dx12_delete_entity` | `{entity:int}` | `{deletedEntityId, deletedCount, sceneGeneration}` |
| `dx12_open_scene` | `{path:string(assets相対)}` | `{sceneName, path, entityCount, sceneGeneration}` |
| `dx12_new_scene` | `{savePath?:string}` | `{applied}` |
| `dx12_play` | `{}` | `{mode:"Playing", sceneGeneration}` |
| `dx12_stop` | `{}` | `{mode:"Editor", sceneGeneration}` |

### 4-4. Node 合成ツール(エンジン非依存。Node が複数 call を順に実行)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_batch` | `{ops:[{method:string, params:object}], stopOnError?:bool}` | `{results:[{index, ok, result?, error?, error_code?}]}` |
| `dx12_focus_and_screenshot` | `{entity:int}` | 画像コンテンツ(PNG) |

**`dx12_batch` 実装**: 各 op を順に await。`stopOnError=true` なら最初の失敗以降を skip 記録。往復削減用。
**`dx12_focus_and_screenshot` 実装**: `focus_camera` → (1フレーム描画) → `screenshot` → 画像読み込み → 画像コンテンツ返却。

---

## 5. describe_components → set_component の流れ

`set_component` を使う前に `dx12_describe_components` でフィールド定義を確認する。

```
# 1. 使えるコンポーネント一覧を取得
dx12_describe_components({})

# 2. 特定コンポーネントのフィールドを確認
dx12_describe_components({component: "pointLight"})
# → {fields: [{name:"color", type:"vec3", default:[1,1,1]}, {name:"intensity", type:"float", default:1.0}, ...]}

# 3. フィールドに合わせて set_component を実行
dx12_set_component({entity: 42, component: "pointLight", data: {color:[1,0.8,0.6], intensity:3.0, range:10.0}})
```

**tags コンポーネントの例外**: `data` は文字列配列(`["enemy","dynamic"]`)。
**dataComponent**: `data` は `{key: {t:"string", v:"値"}}` 形式のオブジェクト。

---

## 6. idempotency_key

`create_entity` と `spawn_model` は `idempotency_key` を受け付ける。
同じキーで2回送った場合、2回目は処理をスキップして1回目の `entityId` を返す。
AI がリトライしたときに重複エンティティを防ぐ用途。

```json
{"method":"create_entity", "params":{"type":"box","idempotency_key":"floor-001"}}
```

---

## 7. sceneGeneration（古い entityId の検出）

`sceneGeneration` は `open_scene` / `new_scene` のたびに +1 される整数で、ほぼ全レスポンスに含まれる。
シーンを開き直すと entt レジストリが作り直され、以前の `entityId` は無効になる。

- 無効な `entityId` を使うと `error_code=1(NOT_FOUND)` が返る。
- 解決策: レスポンスの `sceneGeneration` が変わったら、`dx12_ping` で現世代を確認し
  `dx12_list_entities` でエンティティを引き直す。
- `error_code=4(STALE_SCENE)` は将来用に予約済みだが**現状は未送出**（今は `NOT_FOUND` + `sceneGeneration` の変化で判断）。

---

## 8. error_code 一覧

| コード | 定数 | 意味と対処 |
|--------|------|-----------|
| 1 | `NOT_FOUND` | 指定エンティティ/アセット/コンポーネントが存在しない。ID や path を確認。 |
| 2 | `INVALID_PARAM` | パラメータ型・値が不正。`describe_components` でフィールド型を確認。 |
| 3 | `MODE_CONFLICT` | Playing 中に生成系を呼んだ、またはカメラ無しで Play しようとした。先に `dx12_stop` → 再試行。 |
| 4 | `STALE_SCENE` | （予約・現状未送出）シーン再読込での entityId 失効。実際は `NOT_FOUND(1)` が返るので `sceneGeneration` 変化で判断。 |
| 6 | `UNKNOWN_COMPONENT` | `component` の jsonKey が不明。`dx12_describe_components` で有効な jsonKey を確認。 |
| 7 | `INTERNAL` | エンジン内部エラー。`dx12_get_log` でエンジンログを確認。 |

---

## 9. 検証ループ

変更をかけた後は以下のループで目視確認できる:

```
1. dx12_set_transform / dx12_set_component で変更
2. dx12_focus_and_screenshot(entity: <entityId>) → PNG で確認
3. dx12_get_log(lines: 30) → エンジンのエラー/警告を確認
4. 問題があれば dx12_undo で戻す
```

Play/Stop テスト:
```
dx12_play → (ゲームロジック動作) → dx12_stop
→ dx12_focus_and_screenshot でシーン確認
→ dx12_get_log でランタイムエラー確認
```

---

## 10. セキュリティモデル

ブリッジは **エディタ専用**(ゲーム=封印ランタイムでは起動しない)。

- 受けるのは **`127.0.0.1`** のみ。外部ホストからは到達不可。
- 最初の1行が JSON オブジェクト(`{`)で始まらない接続は即切断
  → ブラウザの HTTP/WebSocket ドライブバイ(localhost CSRF)を遮断。
- パス系ツールは **assets 相対のみ**。絶対パス・`..`・`\`・`:` を拒否。
- `create_lua_component` の Lua は書き込み前に構文チェック(コンパイルのみ・非実行)。
- `create_shader` は書き込み前の静的検証ができない(DXC はファイルからしかコンパイルできない)ため、
  書いた後にコンパイルを試し成否を返す方式。失敗してもファイルは書き込まれたまま残る
  (無効なカスタムシェーダーは既定 Forward へ安全にフォールバックするだけで実害は無い)。
- **認証なし(localhost 開発機前提)**。同一マシンの別ユーザプロセスは接続可能なため、
  共有開発機では注意。アップグレード経路: ポートのトークン認証。
- **`dx12_eval_lua` は任意 Lua コードをその場実行する**(意図的な設計。デバッグ効率を優先)。
  上記の認証なしモデルと同水準のリスク(localhost の他プロセスから叩かれれば任意 Lua 実行が可能)。
  ファイルシステムへの直接アクセスは Lua 標準の `io`/`os` ライブラリを sol2 側で公開していない限り
  できないが、エンジンが公開する全バインディング(scene/physics/audio 等)は呼べる。
- `dx12_validate_scene` はエンジン自身を `--validate` 付きで子プロセス起動する。この経路は
  main.cpp で GPU/ウィンドウ/MCP ブリッジの初期化より前に return するため、実行中のエディタと
  ポート等が衝突することはない。

---

## 11. トラブルシュート

| 症状 | 対処 |
|------|------|
| `エディタに繋がらない` | エディタが起動しているか・シーンを開いているか確認。ゲームモードではブリッジ起動しない。 |
| `engine timeout` | エディタがフレームを回していない(別モーダル等)。エディタを前面にしてリトライ。 |
| ポート競合 | `%TEMP%\dx12_mcp.port` を読む、または `DX12_MCP_PORT` 環境変数を合わせる。 |
| `node が見つからない` / `.ts` 実行不可 | Node **v24+** を入れる(`node --version` で確認)。 |
| ツールが AI 側に出ない | `claude mcp add` 済みか、`.mcp.json` の `args` パスが正しいか確認。登録後はクライアント再起動。 |
| 古い entityId で `NOT_FOUND(1)` | シーンを開き直した。`dx12_ping` で `sceneGeneration` 確認 → `dx12_list_entities` で引き直す。 |
| `MODE_CONFLICT(3)` | Playing 中に生成系ツールを呼んだ。`dx12_stop` してから再試行。 |
| 生成したのに entityId が見つからない | `entityId` をそのまま使う。`name` で検索し直す必要はない(遅延同期で本物の id が返る)。 |
| 別マシンから繋ぎたい | SSH ポートフォワードで localhost に橋渡しし、`DX12_MCP_HOST` + `DX12_MCP_PORT` を合わせる。 |

---

## 12. エンジン内部プロトコル(Node↔エンジン間。参考)

改行区切り JSON、単一 TCP クライアント。

- **リクエスト**: `{"id":<正整数>, "method":<string>, "params":<object>}\n`
- **レスポンス(成功)**: `{"id":<同id>, "ok":true, "result":<any>}\n`
- **レスポンス(失敗)**: `{"id":<同id>, "ok":false, "error":<string>, "error_code":<int>}\n`

遅延同期 method は受信時は何も返さず、フレーム境界で実処理後に同じ id でレスポンスを送る。

---

関連: `tools/mcp-server/AGENTS.md`(AI エージェント運用ガイド)、`tools/mcp-server/README.md`(サーバ構成)、
`docs/AUTHORING.md`、`docs/SCRIPT_COMPONENTS.md`(Lua)。
