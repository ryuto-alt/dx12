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

MCP サーバは **別リポジトリ [ryuto-alt/dx12-mcp](https://github.com/ryuto-alt/dx12-mcp) で配布**する
(エンジン配布物には同梱されない)。エディタの「MCP / AI Bridge」窓がインストールコマンドを表示する。

### 配布エンジン利用者(推奨)

```powershell
git clone https://github.com/ryuto-alt/dx12-mcp "$env:USERPROFILE\dx12-mcp"
cd "$env:USERPROFILE\dx12-mcp"
./install.ps1        # Linux/macOS: ./install.sh
```

`%USERPROFILE%\dx12-mcp` に入れておくと、エディタの「MCP / AI Bridge」窓が自動検出して
登録コマンドをワンクリックコピーできる。

### エンジンをソースから開発している場合

このリポジトリの `tools/mcp-server` がソース・オブ・トゥルース(dx12-mcp リポジトリへは
`tools/mcp-server/publish.ps1` で同期する)。そのまま使える:

```bash
cd tools/mcp-server

# Windows:
./install.ps1

# Linux / macOS:
./install.sh
```

スクリプトは Node v24+ を確認 → `npm install` + 自己テスト(`npm test`、エンジン不要) →
**Claude Code と Codex の両方へ自動登録**（`claude mcp add --scope user` / `codex mcp add`）まで行う。
手で貼るコマンドは無く、CLI が入っていないクライアントの分だけ手順を表示する。
再実行しても壊れない（remove → add で冪等）。登録後はクライアントを再起動すること。

> `--scope user` を使う。Claude Code の既定 `local` スコープは**そのディレクトリでしか有効でない**ため、
> 別のプロジェクトで作業すると「ツールが出ない」になる。

手動の場合:

```bash
cd tools/mcp-server
npm install
node test.ts        # 自己テスト(フレーミング/相関/エラー)
```

Node v24+ が `.ts` を直接実行するため `tsc` ビルドは不要。

---

## 2. 接続（install スクリプトが失敗したとき / 手で入れたいとき）

通常は `install.ps1` / `install.sh` が両クライアントへ自動登録するので、この節は不要。
`<REPO>` は clone した絶対パスに置換する(Windows でもパスは `/` 区切りで可)。

### Claude Code(CLI)
```bash
claude mcp add dx12-engine --scope user -- node <REPO>/tools/mcp-server/index.ts
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

## 4. ツール一覧（全 162 ツール）

MCP ツール名は `dx12_` 接頭辞付き。同期欄: **同期** = 即返り、**遅延同期** = フレーム境界後に本物の値が返る。

### 4-1. 読み取り系(全て同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_ping` | `{}` | `{pong, mode, entityCount, sceneGeneration, currentScene, assetsDir, scriptsDir, baseDir, projectShaderDir, cwd, protocolVersion:4}` ※**`assetsDir` はエンジンが返す正**（`protocolVersion 4` から）。ログの絶対パスから推定する必要はもう無い |
| `dx12_describe_mcp_params` | `{method?:string}` | `{methods:{<method名>:[{key,type}]}, count, globalKeys:["idempotency_key"], note}` ※**エンジンのディスパッチ表そのもの**。`type` は `bool`/`int`/`number`/`string`/`vec3`/`object`/`any`。`"親.子"` は入れ子オブジェクトのキー（例 `skybox.envMapPath`）。TS スキーマとのドリフト検出はこれを正にすること |
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
| `dx12_get_contact_shadow` | `{}` | `{enabled, rayLength, thickness, bias, intensity, steps, maxDistance, fadeDistance}` |
| `dx12_get_taa` | `{}` | `{enabled, sampleCount, feedbackMin, feedbackMax, varianceGamma, jitterScale, debugVelocity, active, fxaaSuppressed}` |
| `dx12_get_render_scale` | `{}` | `{scale, renderResolution:{width,height}, displayResolution:{width,height}, pending, note}` ※**内部解像度スケール**（#16 レンダー解像度と表示解像度の分離） |
| `dx12_get_depth_prepass` | `{}` | `{enabled, note}` ※深度プリパスの単独強制（計画10 A2 の A/B スイッチ） |
| `dx12_get_ssr` | `{}` | `{enabled, intensity, maxDistance, thickness, maxSteps, stride, roughnessCutoff, edgeFade, bias}` |
| `dx12_get_ssgi` | `{}` | `{enabled, intensity, radius, thickness, rayCount, stepCount, clampValue, feedback, iblFallback}` |
| `dx12_get_volumetric_fog` | `{}` | `{enabled, density, albedo, anisotropy, heightFalloff, heightRef, distance, depthDistribution, ambient, sunIntensity, lightScattering, temporal, temporalBlend, extendBeyondRange, debugMode, active}` |
| `dx12_describe_lua_api` | `{}` | binding ごと(entity/transform/Vec3/self/scene/input/camera/physics/audio/nav/ui/fx/events/globals/prelude)の**静的辞書**。★MCP で見えるコンポーネントと Lua から読める API は違う（entity から直接読めるのは transform だけ）。Lua を書く前にこれで確認する |
| `dx12_get_lua_component_state` | `{entity?/name?}` | `{scriptPath, enabled, started, loadError, errorMessage, properties:[{name,type,value,isOverride}]}` ※未上書きの既定値も含む（`get_entity` は保存済みの上書きしか出さない）。`loadError=true` なら `errorMessage` に Lua の traceback がそのまま入る |
| `dx12_get_script_errors` | `{}` | `{count, mode, errors:[{entityId,name,scriptPath,message}]}` ※★`dx12_play` の結果に `scriptErrors>0` が出たら次はこれ。どのエンティティが壊れたか分からない状態で使う |
| `dx12_get_play_session` | `{maxEvents?:int=400, maxSamples?:int=200}` | `{started, recording, durationSec, frames, fpsMin, summary:{...}, events:[{t,kind,detail}], samples:[{t,fps,camPos,...}]}` ※**`dx12_play` を押した時点で自動的に記録が始まる**（開始ツールは無い）。人間に遊んでもらってから取りに来る用。`detail` のキー名は `dx12_key_press` にそのまま渡せる |
| `dx12_read_lua_component` | `{path:string}` | `{path, code}` ※既存 .lua のソースをそのまま読む |
| `dx12_read_shader` | `{path:string(assets/shaders相対)}` | `{path, code, compiled}` ※既存カスタムシェーダーのソースをそのまま読む(compiled は直近の既知のコンパイル成否) |
| `dx12_list_shader_templates` | `{}` | `[{name, title, summary}]` ※同梱のシェーダー雛形(water / ocean / particle_ember 等)を列挙する。★白紙から 200 行の HLSL を書くのは失敗率が高い。まずこれで動くものを起こしてから削る方が確実 |
| `dx12_describe_shader_contract` | `{kind?:"mesh"\|"particle"\|"sprite"\|"screen"(既定 mesh)}` | カスタムシェーダーが使える定数(b0/b1)・テクスチャ・入出力・注意点を kind ごとに返す ※★cbuffer はオフセットで対応が決まるので、1 つでもズレるとコンパイルは通るのに値だけ化ける(エラーが出ないので気付けない)。書き始める前に必ず読むこと |
| `dx12_raycast` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f}` | `{hit, distance?, point?, normal?, entityId?, name?}` ※Playing 中のみ意味のある結果 |
| `dx12_overlap_box` | `{center:[x,y,z], halfExtents:[x,y,z], maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_overlap_sphere` | `{center:[x,y,z], radius:f, maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_get_physics_state` | `{entity:int}` | `{entityId, hasRigidBody, velocity:[x,y,z], hasCharacterController, isGrounded}` ※Playing 中のみ |
| `dx12_validate_scene` | `{path?:string}` | `{pass, exitCode, report, scenePath}` ※`--validate` をヘッドレス子プロセスで実行。省略時は現在のシーン |
| `dx12_get_anim_state` | `{entity:int}` | `{hasSkeletalAnimation, clips:[クリップ名...], boneCount, currentClip, clipTime, speed, looping, blending, hasController, graphPath, graphLoaded, layers:[{name,weight,state,normalizedTime,transitioning,transitionTo,transitionProgress,masked}], parameters:{名前:値}, footIK:{enabled,weight,resolved,bones,boneNames,leftContact,rightContact,leftLift,rightLift,pelvisOffset,leftNormal,rightNormal}}` ※`dx12_play_anim` の clipName 選びと、接地の破綻をスクショ無しで検知するのに使う |
| `dx12_describe_anim_graph` | `{entity:int}` または `{path:string}` | `{source, graph:{version,parameters,clipEvents,extraClips,layers:[{name,weight,blend,mask,defaultState,states,transitions}]}}` ※`.animfsm` の構造を返す。ステート名/パラメータ名の確認に。**TS 側未定義**（B12 と同様） |
| `dx12_net_status` | `{}` | `{available, role:"Offline"\|"Host"\|"Client", isConnected, localClientId, tick, syncedEntityCount, players:[{id,rttMs,bytesSent,bytesReceived}], config:{tickRate,snapshotRate,maxPlayers,defaultPort}, testRole, testJoinAddress}` |
| `dx12_screenshot` | `{path?:string, deterministic?:bool=false, settleFrames?:int=8(1..240), gizmos?:bool=true}` | PNG 画像ブロック + text(`{path(絶対パス), width, height, source:"sceneRT(pre-post)", note}`) ※**ポストプロセス前の `m_sceneRT`**。グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド / **TAA の解決結果が一切写らない**。見た目を判断するなら `dx12_screenshot_final` を使うこと。★`gizmos:false` はこの経路では `deterministic:true` のときだけ効く（既定は直前フレームの読み戻しで撮り直さないため） |
| `dx12_screenshot_final` | `{path?:string, deterministic?:bool=false, settleFrames?:int=8(1..240), gizmos?:bool=true}` | **遅延同期**。`{path, width, height, source:"backbuffer", postApplied, deterministic, gizmos, taa, mode, note}` ※**バックバッファ（＝ポスト適用後の最終画）のビューポート矩形**。ImGui を描く前にコピーするので**エディタのパネル / ギズモは写らない**＝ゲームと同じ絵。サイズはウィンドウ全体ではなく**シーンビューの矩形**。★`gizmos:false` で**この 1 枚だけ**エディタのデバッグ描画（カメラの視錐台の水色の線 / 選択枠 / ライト・カメラのアイコン / 物理・ナビのワイヤ / 床グリッド）を止めて撮る。選択を外しても消えない「アクティブなカメラの視錐台」もこれで消える。**戻す呼び出しは不要 ── 撮影状態と一緒に破棄されるので次の 1 枚では必ず元どおり**（`dx12_render_debug` と同じ作法） |
| `dx12_screenshot_game_view` | `{}` | PNG 画像ブロック ※**アクティブな `CameraComponent`（ゲームカメラ）視点**で 1 フレーム描いて返す。Editor 中でも Play せずに画角・構図を確認できる。アクティブなカメラが無いとエラー |
| `dx12_ui_screenshot` | `{}` | PNG 画像ブロック ※エディタウィンドウ全体(ImGuiパネル込み)。ゲーム内UI/UIエディタの見た目確認用(scene RT には UI が写らない) |
| `dx12_render_debug` | `{mode:string, frames?:int=3(1..120), gain?:number=1, depthRange?:number=100, exposure?:number=1}` | `{path(絶対パス), mode, width, height, toneMapped:bool, warnings:[string], mode_engine:"Editor"\|"Playing"}` ※**中間バッファの可視化**（「なぜ変に見えるか」の切り分け用）。`frames` フレーム描いてからスクショを撮り、**必ず元の設定へ戻す**。必要な機能（TAA/SSAO/コンタクトシャドウ/SSR/SSGI）は一時的に自動で ON にし、その旨を `warnings` に返す。返り値の `path` を画像として読むこと |
| `dx12_ui_tree` | `{}` | `{canvases:[{entityId, name, uiCanvas:{refWidth,refHeight,...}, children:[{entityId, name, components, uiRect, resolvedRect:[x,y,w,h](キャンバス空間px), text?, children}]}]}` ※UIレイアウトの数値確認 |
| `dx12_ui_click` | `{name?/entity?, x?:0..1, y?:0..1, move?:bool}` | `{target, viewportLocal:{x,y}, viewport:{width,height}, moveOnly, stepped, mode, note}` ※**ゲーム内 UI を合成ポインタで押す**。テストプレイで AI がメニューを操作する口。★実マウスとまったく同じ経路（レイキャスト→最前面判定→押下キャプチャ→release-inside で確定）へ流すので、**前面に別の要素が被っているボタンは押せない**のが正しく再現される（名前で onClick を直接呼ぶ方式ではない）。name/entity でその要素の中心、x,y（ビューポート基準 0..1）で任意の座標。**何も無い所を押してフォーカスを外す**のにも使える。押す→離す→配送の 3 フレームを回してから返る(遅延同期)。Lua の onClick が走るのは Play 中だけ |
| `dx12_ui_design_brief` | `{genre:"cinematic"\|"tactical"\|"fantasy"\|"horror"\|"arcade"\|"cozy", screen:"title"\|"hud"\|"inventory"\|"settings"\|"result"\|"dialog"\|"other", tone?}` | 画面固有の構図・階層・制約・アンチパターン。UI生成前に呼ぶ |
| `dx12_ui_audit` | `{strictness?:"balanced"\|"strict"}` | 現在のUIを数値監査。`{pass,score,grade,summary,issues[]}`。崩れ/重なり/可読性/入力遮断/過装飾を検出 |
| `dx12_ui_compose` | `{blueprint:{theme,prefix,root}}` | dock/stack/grid と意味的roleからUI一式を制約付き生成。失敗時ロールバック。生成後はaudit→screenshot必須 |
| `dx12_get_editor_camera` | `{targetDistance?:number=10}` | `{position, forward, target, targetDistance, yawDeg, pitchDeg, fovYDeg, aspect, nearZ, farZ, orthographic, overridden, mode}` ※シーンビューを描いてるカメラの状態。**`target` は `position + forward * targetDistance`**。そのまま `dx12_set_editor_camera {position, target}` へ渡すと同じ yaw/pitch に戻る＝読み返し検証ができる |
| `dx12_get_bounds` | `{entity:int, includeChildren?:bool, perSubmesh?:bool=false}` | `{min, max, center, size, hasMesh}` ※ワールド空間 AABB(回転/親子変換込み)。配置座標の計算に。★`perSubmesh:true` で `{submeshes:[{index, name, materialName, triangles, localMin/localMax/localSize, worldMin/worldMax/worldCenter/worldSize}], submeshCount, largestSubmesh}` も返る＝**「モデルの一部だけ変な位置に飛んでいる。どの部品か」が 1 回で分かる**。`index` は `dx12_pick` の `submeshIndex` と同じ並び。★glTF/FBX の JSON 内の並びとは一致しない（ローダがノード単位に展開するため）ので照合は `name` / `materialName` で |
| `dx12_get_hierarchy` | `{}` | `{roots:[{entityId, name, children:[...]}], count, sceneGeneration}` ※シーンの親子ツリー |
| `dx12_asset_info` | `{path}` | モデル: `{meshCount, totalVertices, totalFaces, materialCount, boneCount, hasSkeleton, animations:[{name,durationSec}], aabbMin/Max(ノード変換込みのワールド AABB)}`、テクスチャ: `{width, height, mipLevels, format, isCubemap}`、他: `{type, fileSizeBytes}` |
| `dx12_view_texture` | `{path, maxSize?:int=1024}` | PNG 画像ブロック ※dds/tga/hdr も変換して見られる。キューブマップは先頭面のみ |
| `dx12_pick` | `{x?,y?(px) \| u?,v?(0..1), all?:bool, maxHits?:int=16, includeIcons?:bool=true, trianglePrecise?:bool=true, maxCandidates?:int=64}` | `{hits:[{entityId,name,submeshIndex,distance,worldPos,worldNormal,isIcon}], count, totalHits, truncated, screen, viewport, mode}` ※**エディタの左クリック選択と同じ `RaycastScene`**。座標系は `dx12_screenshot` / `dx12_project_world_to_screen` と同じ |
| `dx12_raycast_precise` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f=1000, all?:bool, maxHits?:int=16, trianglePrecise?:bool=true, maxCandidates?:int=256}` | `dx12_pick` と同形式 + `{origin, direction, maxDistance}` ※**描画メッシュの三角形基準**。`dx12_raycast`(物理コライダー基準・Playing 限定)とは別物 |
| `dx12_project_world_to_screen` | `{entity?/name?}` | `{x, y, visible, depth, w, width, height, mode}` ※エンティティのワールド座標を、今シーンビューを描いているカメラで画面ピクセルへ投影する(`dx12_screenshot` と同じカメラ。Playing 中はアクティブなゲームカメラ)。★`w<=0` はカメラ背面 |
| `dx12_terrain_sample` | `{entity?/name?, points?:[[x,z]...] (最大512)}` | `{entityId, name, origin, resolution, worldSize, cellSize, boundsXZ, minHeight, maxHeight, samples:[{x,z,height,worldY,normal,slopeDeg,inside}], count}` |
| `dx12_list_lights` | `{limit?:int=50, cursor?:int}` | `{lights:[{entityId,name,type,position,slot,color,intensity,range?,direction?,innerConeDeg?,outerConeDeg?,castShadows?,overBudget,effective}], count, total, cursor, nextCursor, has_more, budget:{total,perCluster,point,spot,directional,shadowSpot,shadowPoint}, warnings:[...]}` ※**上限超過は無言で描画されない**ので必ずここで確認する。クラスタードライティング(Forward+)で点/スポットの個別上限は撤廃され、**合計 1024 灯 / 1 クラスタ 128 灯**が上限。**影は spot 4 / point 2 のまま** |
| `dx12_diagnose` | `{only?:string[], fast?:bool}` | `DeepDiag::RunAll` の JSON(`{version, engine, checks:[{id,title,checked,errors,warnings,infos,issues,omitted,skipped}], summary:{checks,errors,warnings,infos,ok,unknownIds}, checkIds, note}`) ※`summary.errors > 0` だけが失敗 |

### 4-2. 編集系(同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_set_transform` | `{entity:int, position?:[x,y,z], rotation?:[x,y,z](Euler度), quaternion?:[x,y,z,w], scale?:[x,y,z]}` | `ok` |
| `dx12_set_component` | `{entity:int, component:string(jsonKey), data:object\|array}` | `{entityId, component}` |
| `dx12_remove_component` | `{entity:int, component:string}` | `{entityId, removed}` |
| `dx12_set_parent` | `{entity:int, parent?:int}` | `ok` ※parent 省略で親解除 |
| `dx12_group_entities` | `{entities?:int[], names?:string[], name?:string}` | `{groupId, name, count}` ※空の親にまとめる（原点・単位スケール＝見た目は不変）。入れ子の子は自動除外、Undo 可 |
| `dx12_rename_entity` | `{entity:int, name:string}` | `{name}` ※重複は連番付与 |
| `dx12_select_entity` | `{entity:int}` | `{selected}` |
| `dx12_focus_camera` | `{entity:int}` | `{cameraPos:[x,y,z], target, distance}` |
| `dx12_set_pbr` | `{entity:int, metallic?:f, roughness?:f, uvScaleU?:f, uvScaleV?:f, alphaMode?:"auto"|"opaque"|"mask"|"blend", alphaCutoff?:f, opacity?:f, emissiveIntensity?:f, emissiveColor?:[r,g,b]}` | `{entityId, metallic, roughness, uvScaleU, uvScaleV, alphaMode, alphaCutoff, opacity, emissiveIntensity, emissiveColor}` ※**自己発光**は `emissiveIntensity`（0..64、0 で消灯、負でマテリアルに従う）。色を省くと白。ライティングも影も通さず最終色へ加算するので 1 を超えるとブルームが乗る（看板 2..5 / 天井照明 4..10 / 非常口サイン 3..6 が目安）。発光の形をテクスチャで指定するなら `dx12_set_texture` の `slot:"emissive"` と併用。 ※**透明**は `alphaMode`。既定の `auto` はモデル側（glTF の `alphaMode`）に従う。`mask` は `baseColor.a < alphaCutoff` を discard（葉・フェンス・角膜。**影も同じ形に抜ける**）、`blend` は半透明（不透明の後にカメラから遠い順で描く。深度を書かず影も落とさない）。`opacity` は 1 未満なら `alphaMode` を省いても半透明になる（ガラス・水面）。詳しくは docs/AUTHORING.md §7.5 |
| `dx12_set_mesh_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, skinnedFallbackWarning}` ※shaderPath省略/空文字で既定Forwardに戻す。alphaBlend省略時は既存値を維持、既定false(不透明固定でPSのalpha出力は無視される)。true でSrcAlpha/InvSrcAlphaブレンド(DepthWrite OFF) |
| `dx12_set_mesh_shader_params` | `{entity?/name?, effect?:f, params?:f[](最大4), paramsB?:f[](最大3)}` | 適用後の現在値一式 ※カスタムシェーダーの自由枠(b0 の `effectValue`/`shaderParams`/`shaderParamsB`)へ値を書く。★これが無いとシェーダーは【貼れるが動かない】(割当直後は全パラメータ 0)。各値の意味はシェーダー自身のヘッダコメント(`dx12_read_shader` で読める)。ルート定数なので毎フレーム撃っても安い。時間で動かす(徐々に溶ける等)なら Trigger の AnimShaderParam を使うこと(Lua からの口はまだ無い) |
| `dx12_set_sprite_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, worldSpaceWarning}` ※Sprite2D専用・world-spaceのみ対応。MeshRendererのシェーダーとは頂点/ルートシグネチャの契約が異なる(docs/AUTHORING.md §6.1)。shaderPath省略/空文字で既定Spriteシェーダーに戻す |
| `dx12_set_scene_settings` | `{skybox:{envMapPath?, iblIntensity?, skyboxIntensity?, drawSkybox?}}` | `{applied, envMapRebake}` |
| `dx12_set_post_process` | 約25エフェクトの `<name>On`/パラメータ(指定分のみ適用) | `{applied}` |
| `dx12_set_post_process`（被写界深度） | `{dofOn?, dofFocusDist?, dofFocusName?, dofAperture?, dofFocalLength?, dofBlurSize?, dofFocusRange?}` | `{applied}` ※**DoF は絞り基準（物理モード）が既定**。`dofAperture`＝F 値（既定 2.8）と `dofFocalLength`（mm。**0 でカメラの画角から導出**）から薄レンズの錯乱円 `CoC = |z-zf|/z * f²/(N*(zf-f))` を出し、35mm 判のセンサ高 24mm で px 化する。＝**画面解像度に依存する `dofBlurSize` は「暴走防止の上限」へ後退**し、絵作りは F 値だけで決まる。`dofAperture:0` で旧来の `dofFocusRange` 方式（範囲基準）へ戻る。`dofFocusName` にエンティティ名を入れると**毎フレームそのエンティティまでのビュー距離を合焦距離に使う**（＝被写体に合焦したまま寄る/回るカメラワークが Lua 無しで書ける。空なら `dofFocusDist`）|
| `dx12_set_ssao` | `{enabled?, radius?, bias?, intensity?, power?, sampleCount?, blur?}` | `{applied}` |
| `dx12_set_contact_shadow` | `{enabled?, rayLength?, thickness?, bias?, intensity?, steps?, maxDistance?, fadeDistance?}` | `{applied}` ※太陽(平行光)専用のスクリーン空間近接遮蔽。正射/2Dビューでは自動無効 |
| `dx12_get_normal_filter` | `{}` | `{enabled, strength, varianceClamp, geometricBlend}` |
| `dx12_set_normal_filter` | `{enabled?, strength?, varianceClamp?, geometricBlend?}` | `{applied}` ※**法線マップフィルタリング**。1 画素の中に法線マップの山谷が何個も入る状況（高いタイリング / 遠景 / 面を舐める角度）で ①鏡面がちらつく ②**光源が面を舐める角度で N·L の符号が裏返り、面が黒い斑点で埋まる** の 2 つが起きる。スクリーン空間の法線微分から画素内の法線分散を見積もり、分散を GGX のラフネス(α) へ足し込み（Toksvig / Kaplanyan 相当）、分散が大きい画素は法線を幾何法線へ寄せる（＝平均法線の復元）。**既定 ON**（strength=2 / varianceClamp=0.25 / geometricBlend=2）。`enabled:false` または `strength:0` で完全に従来どおり。シーン JSON の `normalFilter` に保存される |
| `dx12_set_taa` | `{enabled?, sampleCount?, feedbackMin?, feedbackMax?, varianceGamma?, jitterScale?, debugVelocity?}` | `{applied}` ※テンポラルAA。ONの間は `fxaaOn` が無視される。深度+速度プリパスが常時走る。正射/2Dビューでは自動無効 |
| `dx12_set_render_scale` | `{scale:0.25..1.0}` | `{...get と同じ...}` ※**内部解像度スケール**。3D シーン系の RT（sceneRT / 深度 / SSAO / コンタクトシャドウ / TAA 履歴・速度 / G-Buffer / SSR・SSGI / ブルーム / DoF / ゴッドレイ / 歪み）だけを `scale` 倍で確保し、最終(uber)パスで表示解像度へ引き伸ばす。**UI / ImGui / エディタのアイコンとギズモは常に表示解像度のまま**＝文字はボケない。`settings.json` の `"render_scale"` に保存される（起動時とプロジェクト切替時に読む）。★変更は**次のフレーム先頭**で反映される（内部で `WaitIdle` するのでフレーム外でしか作り直せない）。反映後は TAA / SSR / SSGI / ボリュメトリックフォグの**時間履歴が全部捨てられる**（座標系が変わるため。持ち越すとゴーストする）。★`dx12_screenshot` は**レンダー解像度**、`dx12_screenshot_final` は**表示解像度**で返る。`dx12_pick` / `dx12_project_world_to_screen` の座標系もレンダー解像度 |
| `dx12_set_depth_prepass` | `{enabled:bool}` | `{enabled, note}` ※**深度プリパスを単独で ON にする**。通常は SSAO / コンタクトシャドウ / TAA / SSR / SSGI / DXR のどれかが要求したときだけ走るが、これを立てると単独で走る（正射 / 2D ビューでは自動的に無効）。`dx12_perf_stats` / `dx12_benchmark` の `gpuPassMs.depthPrepass` が**プリパスの描画だけ**の実測値で、`prepassSsao` はそれを含む「プリパス一式（SSAO / コンタクトシャドウ / SSR・SSGI 生成込み）」。**そのシーンでオーバードローがどれだけあるか＝オクルージョンカリングの余地**を測るための道具。`settings.json` の `"render_depth_prepass"` に保存される |
| `dx12_get_shadow_pcss` | `{}` | `{enabled, lightTanAngle, maxPenumbraTexels, blockerSearchTexels, temporalDither, active, temporalDitherActive, applied:false, note}` ※`active` は「ON でも実際に走る条件（影 ON かつ透視カメラ）」を満たしているか |
| `dx12_set_shadow_pcss` | `{enabled?:bool, lightTanAngle?:0.001..0.5, maxPenumbraTexels?:1..64, blockerSearchTexels?:1..64, temporalDither?:bool}` | `{...get と同じ..., applied:true}` ※**PCSS（ソフトシャドウ）**。CSM の固定幅 PCF を「ブロッカー探索 → 可変ペナンブラ」へ置き換え、接地部は鋭く・離れるほど柔らかい影にする。**OFF で従来の 3x3 PCF に戻る（絵はビット一致）**。`lightTanAngle` は太陽の角半径の tan（実際の太陽は 0.0044＝ほぼ硬い影。既定 0.05 は誇張値）。`temporalDither` は **TAA 有効時のみ**効く（無効時に回してもチラつくだけなのでエンジンが自動で切る）。シーン JSON の `shadowPcss` に保存される |
| `dx12_get_dxr` | `{}` | `{supported, raytracingTier:"1.2" or "none", highestShaderModel:"6.8", shadowEnabled, shadowSunAngle, shadowNormalBias, shadowMaxDistance, shadowIntensity, aoEnabled, aoRadius, aoRayCount, aoIntensity, aoPower, aoCombineWithSsao, maxInstances, forceBuildTlas, shadowActive, tlasReady, stats:{instances, blasCount, blasBytes, blasTriangles, tlasBytes, scratchBytes, instanceDescBytes, skippedSkinned, skippedTransparent, droppedOverLimit, tlasReuseFrames, bytesPerTriangle}, applied:false, note}` ※`shadowActive` は「ON でも実際に走ったか」。`stats` は直近フレームの加速構造の実測値 |
| `dx12_set_dxr` | `{shadowEnabled?:bool, shadowSunAngle?:0..20(度), shadowNormalBias?:0..1(m), shadowMaxDistance?:0..100000(m, 0=無限), shadowIntensity?:0..1, aoEnabled?:bool, aoRadius?:0.01..100(m), aoRayCount?:1..8, aoIntensity?:0..1, aoPower?:0.01..8, aoCombineWithSsao?:bool, maxInstances?:0..32768, forceBuildTlas?:bool}` | `{...get と同じ..., applied:true}` ※**DXR 1.1 inline raytracing（RayQuery）**。RT サン影は既存のコンタクトシャドウ枠(t11)、RT-AO は既存の SSAO 枠(t8) へ書くので**ルートシグネチャは 1 DWORD も増えない**。**非対応 GPU では `error_code` を返す**（要 DXR Tier 1.1 かつ SM 6.5）。★スキンドと半透明は加速構造に入らないので従来どおり CSM が担当し、フォワードの `min()` で合成される（RT 影が有効なフレームは CSM が「RT の担当ぶん」を描かなくなる＝排他）。シーン JSON の `raytracing` に保存される |
| `dx12_set_ssr` | `{enabled?, intensity?, maxDistance?, thickness?, maxSteps?, stride?, roughnessCutoff?, edgeFade?, bias?}` | `{applied}` ※スクリーン空間反射。深度プリパスのG-Bufferと前フレームカラーをレイマーチして IBL の鏡面を置き換える。反射は1フレーム遅れる。ONの間は深度+速度プリパスが常時走る。正射/2Dビューでは自動無効 |
| `dx12_set_ssgi` | `{enabled?, intensity?, radius?, thickness?, rayCount?, stepCount?, clampValue?, feedback?, iblFallback?}` | `{applied}` ※スクリーン空間GI。前フレームカラーを間接光源にして IBL の拡散(irradiance)を置き換える。`iblFallback` を切るとカメラ回転で明るさが変動する。正射/2Dビューでは自動無効 |
| `dx12_set_volumetric_fog` | `{enabled?, density?, albedo?, anisotropy?, heightFalloff?, heightRef?, distance?, depthDistribution?, ambient?, sunIntensity?, lightScattering?, temporal?, temporalBlend?, extendBeyondRange?, debugMode?}` | `{applied}` ※froxel ボリュメトリックフォグ。視錐台に沿った 3D テクスチャ(160x90x64)へ散乱を焼いて合成する＝光の筋が立体的に見える。有効にすると VRAM を 28MB 確保する。GodRays と同時に有効にすると太陽の散乱が二重計上される。正射/2Dビューでは自動無効 |
| `dx12_get_occlusion` / `dx12_set_occlusion` | `{enabled:bool}` | `{enabled, active, ready, pyramid{width,height,mips}}` ※**Hi-Z オクルージョンカリング**。深度プリパスの深度から階層 Z ピラミッド（max 縮約）を作り、壁の裏に完全に隠れた描画を GPU 側で落とす。判定結果は D3D12 の**プレディケーション**へ直接渡すので読み戻しゼロ・遅延ゼロ（前フレームの結果を使う方式で起きる「速く振り向くと物が数フレーム消える」は構造的に起きない）。★**ON にすると深度プリパスも強制的に走る**。TAA/SSAO/SSR/DXR のどれかが有効なシーンではプリパスは元々走っているので追加コストは `gpuPassMs.hiZ`（実測 0.04ms）だけだが、**どれも無効なシーンで ON にするとプリパスぶんの描画コールが増えて遅くなることがある**。GPU 律速のときに効く機能で、CPU 律速のシーンでは fps は改善しない。既定 OFF、`settings.json` の `"render_occlusion_culling"` に保存。実際に何体隠れたかは `dx12_perf_stats` の `occlusion` ブロック（`occluded`/`tested`/`ratio`/`predicatedDraws`/`batches`）を見ること。正射/2Dビューでは自動無効 |
| `dx12_set_occlusion` | `{enabled:bool}` | `{enabled, active, ready, pyramid{width,height,mips}}` ※Hi-Z オクルージョンカリングの ON/OFF。★ON にすると深度プリパスも強制的に走る。TAA/SSAO/SSR/DXR のどれかが有効なシーンでは追加コストは Hi-Z ぶん(実測 0.04ms)だけだが、どれも無効なシーンで ON にするとプリパスぶんの描画コールが増えて逆に遅くなることがある。GPU 律速のときに効く機能で、CPU 律速のシーンでは fps は改善しない。既定 OFF、`settings.json` の `render_occlusion_culling` に保存される |

> **TAA の効果確認は `dx12_ui_screenshot` を使うこと。** `dx12_screenshot` はポスト前の `m_sceneRT` を読むので、TAA の解決結果も `debugVelocity` の可視化も写らない（どちらもその後段で出力される）。

### 4-2-1. `dx12_render_debug` の mode 一覧

「絵が変」の原因を切り分けるための唯一の入口。**呼ぶ前と後でシーンの設定は完全に同じ**（一時的に ON にした機能は必ず戻す）。
可視化はポスト前の `m_sceneRT` へ描くので、返ってくる PNG には**必ず写る**（`dx12_screenshot` の B5 の罠を踏まない）。
`toneMapped:false` のモードはトーンマップ/露出を掛けずに 8bit へ落とすので、**PNG のピクセル値がそのままバッファの値**として読める。

| mode | 出るもの | 実装 | 備考 |
|---|---|---|---|
| `normal` | ワールド法線 `0.5+0.5*N`（+X=赤 +Y=緑 +Z=青） | 専用パス | **G-Buffer は幾何法線**。法線マップは載っていない（SSR/SSGI が見ているのもこれ） |
| `roughness` | G-Buffer の roughness（白 = 1） | 専用パス | **スカラー値のみ**。ORM テクスチャは載っていない |
| `metallic` | G-Buffer の metallic（白 = 1） | 専用パス | 同上 |
| `depth` | ビュー空間 Z をヒートマップ（青=近 → 赤=遠、空は黒） | 専用パス | `depthRange`(m) で正規化。既定 100 |
| `ao` | SSAO（白 = 遮蔽なし） | 専用パス | SSAO を一時 ON |
| `contactShadow` | コンタクトシャドウ（白 = 遮蔽なし） | 専用パス | コンタクトシャドウを一時 ON |
| `velocity` | 速度バッファ（R=+X G=+画面下、**静止していれば一様な (0.5,0.5,0.5)**） | 専用パス | `gain` で強調（既定 1、20 くらいが見やすい） |
| `ssr` | SSR の結果（リニア HDR にガンマのみ） | 専用パス | SSR を一時 ON。時間蓄積があるので `frames` を 8〜16 に |
| `ssgi` | SSGI の結果 | 専用パス | 同上 |
| `rt` | **DXR のプライマリレイのヒット距離**をヒートマップ（空/ミスは黒） | 専用パス | TLAS が正しく建っているかの目視確認。`depthRange`(m) で正規化。RT 影 / RT-AO が OFF でも TLAS を一時的に建てる |
| `rtDiff` | **&#124;RT のヒット距離 − ラスタの距離&#124;** をヒートマップ（**黒 = 完全一致**、マゼンタ = 片方だけヒット） | 専用パス | ★加速構造の検証はこれが本命。行列の転置ミス / ノード変換の付け忘れを一発で炙り出す。`gain` を 20 くらいにすると 5cm でフルスケール。**スキンドと半透明は TLAS に入らない仕様なのでマゼンタになる**。BLAS は LOD0 固定なので、遠くて低 LOD で描かれている物は数 cm の差が出るのが正常 |
| `shadowCascade` | CSM のカスケードを色分け（赤/緑/青/黄の色掛け） | 既存 `shadowParams.w` | フォワード PS の既存実装をそのまま使う |
| `lightComplexity` | クラスタごとのライト数ヒートマップ（青0 → 緑 → 赤、**白 = 128 灯で切り捨て中**） | 既存 `clusterExtra.z=1` | 正射/2D ではクラスタード自体が無効 |
| `clusterGrid` | クラスタ境界の市松 | 既存 `clusterExtra.z=2` | |
| `decalCount` | クラスタごとのデカール枚数ヒートマップ（**白 = 16 枚で切り捨て中**） | 既存 `clusterExtra.z=3` | デカール 0 枚のクラスタはほぼ黒 |
| `fogScattering` / `fogTransmittance` / `fogSlice` | ボリュメトリックフォグの散乱 / 透過率 / froxel スライス | 既存 `FogParams.gMisc.z` | フォグが無効なら何も出ない（`warnings` で通知） |
| `off` | 何もせず全部を戻すだけ（スクショも撮らない） | — | 途中で失敗したときのリセット用 |

**非対応（作っていない。理由つき）**
- `albedo` … 前方レンダラなのでアルベドの G-Buffer が存在しない。作るには深度プリパスに RT をもう 1 枚足す必要があり、
  速度 PSO の RTV 本数（＝00-COORDINATION §5.5 の契約）に手を入れることになるので見送った。
- `overdraw` … 加算カウント用の専用パス（全メッシュを再描画してブレンド加算）が要る。既存のどのバッファにも無い。

**⚠️ `normal` / `roughness` / `metallic` / `velocity` は「深度+速度プリパス」でしか書かれない**ので、TAA も SSR も SSGI も
OFF のときは TAA を一時的に ON にして撮る（`warnings` に出る）。この 4 モードが「ジオメトリだけの粗い絵」に見えるのは仕様。
| `dx12_undo` | `{}` | `{queuedUndo, undoable, willUndo}` ※MCP の編集は積まれない。`willUndo` で何が戻るか確認してから使う |
| `dx12_redo` | `{}` | `{queuedRedo, redoable, willRedo}` |
| `dx12_save_scene` | `{path?:string}` | `{path}` ※省略で現在シーンへ上書き |
| `dx12_create_lua_component` | `{name:string, code:string}` | `{path}` ※書込前に構文検証。既存パスなら上書き更新も兼ねる |
| `dx12_create_shader` | `{name:string, code:string}` | `{path, compiled, error?}` ※assets/shaders/に作成/上書き後、即コンパイルを試す。Luaと違い失敗してもファイルは残る(反復修正前提) |
| `dx12_attach_lua_component` | `{entity:int, script:string(assets相対)}` | `ok` |
| `dx12_set_lua_property` | `{entity?/name?, key:string, value:any}` | `{entityId, key, value}` ※スクリプトの `properties` 宣言にあるものだけ。Playing 中は即再注入（`OnStart` 再実行）、Editor 中は保存だけで次 Play から反映 |
| `dx12_reload_scripts` | `{path?:string}` | `{reloaded, cleared}` ※実行時エラーで死んだスクリプトを **Play を止めずに**復帰させる。ファイルを書き換えた場合は 0.5 秒で自動リロードされるので不要 |
| `dx12_reload_assets` | `{path?:string(assets相対のファイル or フォルダ), force?:bool=false}` | `{path, force, reloaded, textures[], models[], reboundEntities, checkedTextures, checkedModels, skipped[], warnings[], note}` ※**DCC ツールと行き来するときの必須ツール**。エンジンはテクスチャもモデルも**プロセス起動から一生キャッシュする**ので、Blender や画像編集ソフトから同じパスへ書き出し直しても絵は変わらない（これが無いと確認のたびにエディタ再起動＝1 往復 20 秒以上）。★**シーンは開き直さない**: エンティティ / Transform / 選択状態はそのまま、いま置かれている `MeshRenderer` の参照だけが新しい実体へ張り替わる（`reboundEntities` がその体数）。テクスチャは**同じ `Texture` オブジェクト・同じ SRV 番号**のまま中身だけ差し替わるので、スプライト / UI / マテリアルの参照は 1 つも直さずに済む。★既定はディスクの更新時刻を見て**変わったものだけ**。書き出し直したのに `reloaded` が 0 なら `force:true`。★キューブマップ / 配列テクスチャ（スカイボックス・地形レイヤー）は張り直せず `skipped` に載る（シーンを開き直すこと）。モデル内の埋め込みテクスチャはモデル側を読み直せば一緒に更新される |
| `dx12_set_color` | `{entity?/name?, color:[r,g,b]}` | `{entityId, color}` ※メッシュの基本色（頂点色の乗算）。金属感は `dx12_set_pbr` と併用 |
| `dx12_install_font` | `{family:string, weight?:int=400}` | `{fontPath, family, weight}` ※Google Fonts から `.ttf` を `assets/fonts/` へ取り込む。★日本語 UI には日本語対応フォント（Noto Sans JP 等）を選ぶこと（欧文フォントは豆腐になる） |
| `dx12_create_prefab` | `{entity:int, path?:string}` | `{path, entityId}` ※path省略で assets/prefabs/<name>.prefab |
| `dx12_eval_lua` | `{code:string}` | `{result:string}` ※任意 Lua をその場実行(デバッグ用) |
| `dx12_build_game` | `{}` | `{success, outputDir, error?}` ※ヘッドレスビルド(同期・数十秒かかることあり) |
| `dx12_set_texture` | `{entity:int, path:string(assets相対、空文字で解除), slot?:"albedo"\|"normal"\|"metalRoughness"\|"emissive", submesh?:int}` | `{entityId, slot, submesh, path}` ※Inspector のテクスチャ D&D と同じインスタンス単位 override(Material 共有を壊さない)。`emissive` は貼っただけでは光らない（色×強度が既定 0）ので `dx12_set_pbr` の `emissiveIntensity` を一緒に上げること |
| `dx12_play_anim` | `{entity:int, clip?:int, clipName?:string, blend?:f=0.3, loop?:bool, speed?:f, state?:string, layer?:int=0}` | `{entityId, clip, clipName, blend, speed}` または `{entityId, state, layer, blend}` ※スケルタルアニメのクロスフェード再生(Lua playAnim と同経路)。**`state` を渡すと `.animfsm` のステート遷移**になる(`AnimatorController` が要る)。渡さなければ従来どおり clip 経路で完全後方互換。`state`/`layer` は **TS 側未定義** |
| `dx12_set_anim_param` | `{entity?:int / name?:string(エンティティ名), param:string(FSM パラメータ名), value?:number\|bool, trigger?:bool}` | `{entityId, param, name(=param。後方互換), value}` ※アニメ FSM のパラメータを外から叩いて遷移を検証する。**★パラメータ名は `param`**。`name` は他ツールと同じく「エンティティ名」。`param` を省略したときだけ `name` をパラメータ名として読む後方互換がある（`{entity, name:"Speed"}` も通る）が、**新しいコードは必ず `param` を使うこと** |
| `dx12_net_setup` | `{role:"host"\|"client"\|"offline", address?:string, port?:int}` | `{testRole, address, port}` ※次の `dx12_play` で自動 Host/Join(ツールバーの Play ロールと同じ) |
| `dx12_net_launch_test_client` | `{}` | `{requested}` ※ホスト Playing 中のみ。同エンジンをもう1プロセス起動し 127.0.0.1 へ自動接続(フレーム境界) |
| `dx12_set_editor_camera` | `{position?:[x,y,z], target?:[x,y,z], yawDeg?:f, pitchDeg?:f, release?:bool}` | `{position, forward, yawDeg, pitchDeg, overridden, mode, note}` ※シーンビューのカメラを任意視点へ。target 指定で yaw/pitch 自動逆算。**Play 中も使える**: Playing 中に呼ぶとアクティブ `CameraComponent` の毎フレーム同期を止めて視点を固定する（`overridden:true`）。`{"release":true}` でゲームカメラへ返す。**Play/Stop の遷移でも自動解除**。Play 中の絵で `look_compare` / `camera_path` を回すための機能 |
| `dx12_look_at` | `{entity:int, target?:[x,y,z], targetEntity?:int, targetName?:string, upright?:bool}` | `{entityId, rotation, target}` ※+Z 正面の想定で rotation(Euler) を書く。upright=true でピッチ0。親が回転してると厳密でない |
| `dx12_snap_to_ground` | `{entity:int, offset?:f, precise?:bool=true}` | `{groundY, movedBy, method:"raycast"\|"aabb", position, groundEntityId?}` ※**三角形精密レイキャストで真下の実際の面へ接地**(地形の起伏・斜面・彫った岩に乗る)。真下に三角形が無ければ従来の AABB 天面判定へフォールバック(`method:"aabb"`)。床なしは y=0。Editor 中でも動く |
| `dx12_import_asset` | `{sourcePath:string(絶対パス可), destPath:string(assets相対), overwrite?:bool}` | `{imported:[相対パス...], count}` ※外部ファイル/フォルダを assets へコピー。.gltf はフォルダごと |
| `dx12_move_asset` | `{from, to, overwrite?:bool}` | `{from, to, note}` ※assets 内の移動/リネーム。**シーン内の参照パスは自動更新されない** |
| `dx12_delete_asset` | `{path, recursive?:bool}` | `{deleted, removedCount, wasDirectory}` ※ディレクトリは recursive:true 必須。参照中アセットを消すと壊れる |
| `dx12_terrain_generate` | `{entity?/name?, preset?:"hills"\|"canyon"\|"mountains", seed?:int, frequency?, octaves?, amplitude?, ridged?, baseHeight?, edgeFalloff?, valleyDepth?}` | `{entityId, preset, params:{...}, minHeight, maxHeight, resolution, worldSize}` ※高さ配列を丸ごと作り直す(既存の彫りは消える)。**同じ seed/params なら毎回同じ地形=冪等**。★Editor 限定 |
| `dx12_terrain_sculpt` | `{entity?/name?, brush?:"raise"\|"lower"\|"smooth"\|"flatten"\|"noise", point?:[x,z] \| points?:[[x,z]...](最大512) \| worldPos?:[x,y,z], radius?=12, strength?=5, falloff?=0.5, flattenHeight?, mirrorX?, mirrorZ?, noiseFrequency?, noiseOctaves?, noiseRidged?, seed?}` | `{entityId, brush, points, radius, strength, changed, minHeight, maxHeight}` ※座標は**ワールド XZ**。相対操作(2回撃つと2回ぶん)。`flatten`+`flattenHeight` は絶対値なので冪等寄り。★Editor 限定 |
| `dx12_terrain_erode` | `{entity?/name?, iterations?:int=16, talusDeg?:f=34, region?:[minX,minZ,maxX,maxZ]}` | `{entityId, iterations, talusDeg, changed, minHeight, maxHeight}` ※熱浸食。相対操作。★Editor 限定 |
| `dx12_terrain_paint` | `{entity?/name?, layer?:0..3, point?:[x,z] \| points?:[[x,z]...](最大512) \| worldPos?:[x,y,z], radius?=12, strength?=0.7, falloff?=0.5}` | `{entityId, layer, points, radius, strength, changed, splatSize}` ※**テクスチャレイヤーの重み**を円ブラシで塗る。座標は**ワールド XZ**。相対操作。`terrain.layerSetPath` 未設定だと `INVALID_PARAM`。★Editor 限定 |
| `dx12_terrain_autopaint` | `{entity?/name?, rockSlopeStart?, rockSlopeEnd?, dirtSlopeStart?, dirtSlopeEnd?, snowHeightStart?, snowHeightEnd?, noiseStrength?}` | `{entityId, splatSize}` ※傾斜と標高から 4 層を焼き直す（**冪等**。手で塗った内容は消える）。傾斜は 0=平ら〜1=垂直、標高はワールド Y(m)。★Editor 限定 |
| `dx12_terrain_set_layers` | `{entity?/name?, layerSetPath:string(assets 相対 .terrainlayers。**空文字で割当解除**), splatResolution?:int=512(32..2048), autopaint?:bool=true, uvScale?, heightBlendDepth?:0.01..1, triplanarSharpness?:1..16, normalStrength?:0..2, macroScale?:10..400, macroStrength?:0..1, distTilingStart?:5..200, distTilingFarScale?:2..16, pomHeightScale?:0..0.3, pomFadeStart?:0..40, pomFadeEnd?:1..120, triplanar?:bool, pom?:bool, macro?:bool, distTiling?:bool}` | `{entityId, layerSetPath, previousLayerSetPath, layerCount, layerNames:[...], splatPath, splatSize, splatCreated, uvScale, terrainMatFlags, sceneGeneration, note}` ※**地形にテクスチャレイヤーを割り当てる唯一の MCP 経路**（#27）。初回割当時にスプラットを作り、`autopaint:true`（既定）なら傾斜/標高から自動で塗る。省略したパラメータは触らない（冪等）。`layerSetPath:""` で外すと従来の頂点色描画へ戻る。★Editor 限定 |
| `dx12_terrain_splat_info` | `{entity?/name?, gridSize?:int=8(0..32。0 で grid を返さない), point?:[x,z] \| points?:[[x,z]...](最大256)}` | `{entityId, layerSetPath, splatPath, hasSplat, unsavedSplat, splatSize, coverage:[4](層ごとの平均重み 0..1), dominantRatio:[4](その層が最大だったテクセルの割合), gridSize, grid:[gridSize 本の文字列。`grid[z][x]` が `'0'..'3'` でそのセルの支配レイヤー。z が増えると +Z、x が増えると +X], samples:[{world:[x,z], texel:[tx,tz], weights:[4], dominant:int}], note}` ※**読み取り専用**。`terrain_paint` / `autopaint` の結果を絵を見ずに検証する。スプラット未作成なら `hasSplat:false` と案内だけ返す。Playing 中も可 |
| `dx12_sculpt_brush` | `{entity?/name?, brush?:"draw"\|"pull"\|"push"\|"smooth"\|"flatten"\|"pinch"\|"noise"\|"grab", position?:[x,y,z](ワールド) \| localPosition?, radius?=0.5, strength?=0.2, falloff?=0.5, direction?, grabDelta?, symmetryX/Y/Z?, noise*?, seed?}` | `{entityId, brush, movedVertices, localCenter, radius, strength, vertexCount, triangleCount, localBounds}` ※radius/strength は**メッシュのローカル単位**(Transform の scale が掛かる前)。相対操作。★Editor 限定 |
| `dx12_set_sun` | `{timeOfDay?:0..24, azimuth?:deg, elevation?:deg, color?:[r,g,b], kelvin?:1000..40000, intensity?, ambient?}` | `{entityId, name, direction, azimuthDeg, elevationDeg, color, intensity, ambient, timeOfDay}` ※最初の DirectionalLight を**絶対指定**で更新(冪等)。方位/高度は「太陽が見える方向」(+Z=0°, +X=90° / 高度 0=地平線) |
| `dx12_navmesh_build` | `{cellSize?, cellHeight?, agentHeight?, agentRadius?, agentMaxClimb?, agentMaxSlope?, minRegionArea?, mergeRegionArea?, maxEdgeLen?, maxSimplificationErr?, maxVertsPerPoly?:int, monotonePartition?:bool, filterLedgeSpans?:bool, filterLowHanging?:bool, useBounds?:bool, boundsMin?:[x,y,z], boundsMax?:[x,y,z]}` | `{ok, settingsChanged, stats:{...}, config:{...}, stageLog}` ※シーンのメッシュを**実際の三角形のまま**ボクセル化して歩ける面を取り出す。引数はシーンの生成設定を上書きしてから焼く。★Editor 限定。焼いた実体は隣の `.nav`（`dx12_save_scene` で書かれる） |
| `dx12_navmesh_settings` | build と同じキー | `{applied, config, note}` ※焼き直さずに設定だけ変える。引数なしで現在値を読める |
| `dx12_navmesh_info` | `{}` | `{config, stats:{built, polyCount, vertCount, sampleCount, gridW, gridH, walkableArea, buildMs, memoryBytes, boundsMin, boundsMax}, debugDraw}` |
| `dx12_navmesh_path` | `{from:[x,y,z], to:[x,y,z], searchRadius?, searchHeight?}` | `{pointCount, points:[[x,y,z]...], length, reached}` ※A* + ファネル。`reached:false` は「到達できないので一番近い所まで」 |
| `dx12_navmesh_sample` | `{point:[x,y,z], searchRadius?, searchHeight?}` | `{onNavMesh, poly?, point?, distance?}` ※位置を歩行面へ落とす（高さは坂道でもボクセル分解能で正確） |
| `dx12_navmesh_raycast` | `{from:[x,y,z], to:[x,y,z], searchRadius?, searchHeight?}` | `{hit, t, point, normal}` ※壁（隣のポリゴンが無い辺）との精密な交差。「真っ直ぐ行けるか」の判定 |
| `dx12_navmesh_debug` | `{enabled?:bool}` | `{enabled}` ※シーンビューにワイヤを重ねる（明るい線=壁 / 暗い線=ポータル）。MCP のスクショにも写る |
| `dx12_navmesh_clear` | `{}` | `{cleared}` ※★Editor 限定 |
| `dx12_apply_lighting_preset` | `{preset:"day"\|"dusk"\|"night"\|"indoor"\|"horror"\|"studio"}` | `{preset, label, tip, sun:{...}\|null, post:{exposure/bloom/vignette/saturation...}}` ※**エディタの「ライティング」窓と同じ表・同じ式**(`src/editor/LightingPresets.h` に 1 本化)。太陽が無ければポストのみ適用 |

### 4-3. 生成・削除・モード遷移(遅延同期 — 本物の値が返る)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_create_entity` | `{type:"box"\|"sphere"\|"plane"\|"empty"\|"camera"\|"light_directional"\|"light_point"\|"light_spot"\|"particle_emitter"\|"trigger"\|"ui_canvas"\|"ui_image"\|"ui_text"\|"ui_button"\|"ui_slider"\|"ui_toggle"\|"ui_scrollview", name?, position?:[x,y,z], parent?:int, parentName?:string, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` ※light_*/camera/particle_emitter/trigger は既定値で生成(dx12_set_component で調整)。ui_* はエディタと同じ部品構成で生成され `entityIds`(自動Canvas/ラベル子含む全id)も返る。parent/parentName は ui_*(ui_canvas 以外)の親指定 |
| `dx12_spawn_box` | `{name?, position?, scale?, rotation?, color?, metallic?, roughness?}` | `{entityId, name, sceneGeneration}` ※足場/壁/床用。内部で create_entity → set_transform → set_pbr → set_color を順に実行する |
| `dx12_spawn_sphere` | `{name?, position?, scale?, rotation?, color?, metallic?, roughness?}` | `{entityId, name, sceneGeneration}` |
| `dx12_spawn_coin` | `{name?, position?}` | `{entityId, name, sceneGeneration}` ※金色の薄い円盤 + tag `coin`。回転やスコア加算は Lua / Trigger で付ける |
| `dx12_spawn_model` | `{path:string(.gltf/.glb/.fbx/.obj), position?:[x,y,z], name?, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` |
| `dx12_spawn_prefab` | `{path:string(.prefab), position?, name?}` | `{entityId, rootEntityId, entityIds:[...], name, sceneGeneration}` |
| `dx12_duplicate_entity` | `{entity:int}` | `{entityId, name, sceneGeneration}` |
| `dx12_delete_entity` | `{entity:int}` | `{deletedEntityId, deletedCount, sceneGeneration}` |
| `dx12_open_scene` | `{path:string(assets相対)}` | `{sceneName, path, entityCount, sceneGeneration}` |
| `dx12_open_project` | `{path:string(プロジェクトルート絶対パス)}` | `{name, rootDir, defaultScene, loading:true}` ※ロードは非同期に数フレーム進む。完了は `dx12_ping` の currentScene で確認 |
| `dx12_new_scene` | `{savePath?:string}` | `{applied}` |
| `dx12_play` | `{}` | `{mode:"Playing", sceneGeneration}` |
| `dx12_stop` | `{}` | `{mode:"Editor", sceneGeneration}` |
| `dx12_terrain_create` | `{name?="Terrain", resolution?:int=128(16..512), worldSize?:f=200, maxHeight?:f=200, position?:[x,y,z], uvScale?, color?:[r,g,b]}` | `{entityId, name, created, resolution, worldSize, maxHeight, sceneGeneration}` ※**同名があれば作り直さず設定更新**(冪等)。resolution/worldSize を変えた時だけ高さがリセットされ `heightsReset:true` |
| `dx12_sculpt_create` | `{name?="Sculpt", primitive?:"box"\|"sphere"\|"plane"\|"cylinder", subdivisions?:int=16(1..64), size?:f=2, position?, uvScale?, color?, collision?}` | `{entityId, name, created, vertexCount, triangleCount, sceneGeneration}` ※**同名があれば素体を作り直さない**(彫った形を失わないため) |
| `dx12_sculpt_make_editable` | `{entity?/name?(元モデル), name?(出力名。既定 "<元>_Sculpt")}` | `{entityId, name, created, sourceEntityId, vertexCount, triangleCount, sceneGeneration}` ※元の .glb 等は読むだけ。CPU 頂点キャッシュが無いモデルは不可 |

### 4-4. Node 合成ツール(エンジン非依存。Node が複数 call を順に実行)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_batch` | `{ops:[{method:string, params:object}], stopOnError?:bool}` | `{results:[{index, ok, result?, error?, error_code?}]}` |
| `dx12_focus_and_screenshot` | `{entity:int}` | 画像コンテンツ(PNG) |

| `dx12_scatter` | `{type\|model\|prefab(どれか1つ), count:int(1..200), area:[minX,minZ,maxX,maxZ], y?:f, placement?:"random"\|"grid", seed?:int, randomYaw?:bool, scaleRange?:[min,max], snapToGround?:bool, namePrefix?:string}` | `{entities:[{entityId,name}], count, seed, placement, errors?}` |
| `dx12_screenshot_from` | `{position:[x,y,z], target?:[x,y,z]}` | 画像コンテンツ(PNG) ※Editor 限定 |
| `dx12_material_apply` | `{entity?/name?, dir?:string(assets相対), baseColor?, normal?, orm?, height?, uvScale?}` | `{entityId, applied:{...}, ignored:[{file,reason}]}` ※PBR の 4 点セットを 1 回で割当（`set_texture`×3 + `set_pbr` を畳んだもの）。`dir` を渡すとファイル名から用途を推定する（Poly Haven の `diff`/`nor_gl`/`arm`/`disp`、`albedo`/`basecolor`/`ORM`/`RMA` 等）。推定できなかったものは `ignored` に理由付きで返る |
| `dx12_scene_write` | `{path:string, scene:object, open?:bool}` | `{path, entityCount, opened}` ※**シーン JSON を直接書く**。MCP の spawn は 1 体につき 1 フレームかかる（遅延同期）ので、数十体以上を一気に並べるならこちらが桁違いに速い。書く前に `meshRenderer.modelPath` / `luaScript.scriptPath` の実在を `dx12_list_assets` と突き合わせて検証する |
| `dx12_look_compare` | `{referencePath:string, bins?:int=24, ...}` | 横並び PNG + **測光の数値**（対数輝度ヒストグラムと EMD / 平均・中央輝度 / コントラスト / 相関色温度 CCT / 平均彩度 / 黒潰れ率 / 白飛び率）と「どのノブをどっちへ何倍動かすか」の指示 ※リアル系ライティングを詰める本体 |
| `dx12_ui_compare` | `{referencePath:string, grid?:bool}` | 横並び PNG（左=参照 / 右=現在）+ `diffRatio(%)` ※`grid=true` で右側に 8px グリッドを重畳。**「参照と違う点を 3 つ」挙げてから直す**ループを回す用 |
| `dx12_camera_path` | `{path:[[x,y,z],...] または keyframes, shots?:int, source?:"backbuffer"\|"sceneRT", ...}` | 連写をタイル化した 1 枚（コンタクトシート）※静止画 1 枚では分からない TAA のゴースト / LOD ポップ / 影のちらつき / カリング抜けを探す用。既定は `screenshot_final`（TAA の解決結果はポスト前の RT に出ない） |
| `dx12_preview_model` | `{path:string(.gltf/.glb/.fbx/.obj)}` | 画像コンテンツ(PNG) ※一時 spawn→撮影→削除。シーンは変更されない |

**`dx12_batch` 実装**: 各 op を順に await。`stopOnError=true` なら最初の失敗以降を skip 記録。往復削減用。
**`dx12_focus_and_screenshot` 実装**: `focus_camera` → (1フレーム描画) → `screenshot` → 画像読み込み → 画像コンテンツ返却。
**`dx12_scatter` 実装**: seed 付き乱数(mulberry32)で位置を決め、`create_entity`/`spawn_model`/`spawn_prefab` を1体ずつ実行(+必要なら `set_transform`/`snap_to_ground`)。同じ seed なら同じ配置になる(リトライで再現)。失敗3件で打ち切り。
**`dx12_screenshot_from` 実装**: `set_editor_camera` → (1フレーム描画) → `screenshot`。
**`dx12_preview_model` 実装**: `spawn_model`(y=-10000 の遠方) → `focus_camera` → `screenshot` → `delete_entity`。失敗時も一時エンティティは削除する。

### 4-6. 実行・入力シミュレーション・計測

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_key_down` / `dx12_key_up` | `{key:int(VK) \| string("W","SPACE","UP","F1"…)}` | `{key}` ※押しっぱなしの挙動確認。Lua の `input:isKeyDown` / `keyDown()` に効く（`GetAsyncKeyState` を直接読む経路には効かない）。ウィンドウがフォーカスを失うと合成キーはクリアされる |
| `dx12_key_up` | `{key:int(VK) \| string}` | `{key}` ※`dx12_key_down` で押したキーを離す |
| `dx12_key_press` | `{key}` | `{key}` ※1 フレームだけ押して離す（`isKeyPressed` / `keyPressed()` が 1 回立つ）|
| `dx12_mouse_move` | `{dx?:f, dy?:f}` | `{dx, dy, mode, note}` ※合成マウス移動を次の 1 フレームぶんだけ注入する。一人称の視点操作はこれが唯一の口(★`camera:setYaw()` では向きを変えられない。エンジン標準の FpsController が yaw を Lua のローカル変数で持ち毎フレーム上書きするため)。押しっぱなしの概念は無いので、回し続けるには `dx12_step_frames` と交互に撃つこと。目標角度へ向けたいなら `dx12_play_script` の yaw や `dx12_autoplay` を使う方が確実(実測して比例で詰める閉ループになっている) |
| `dx12_step_frames` | `{frames?:int=1(1..600)}` | `{frames}` ※**N フレーム進んでから応答する同期バリア**。入力がシミュレーションに効いてから観測するために挟む。※決定論ステッパではない（各フレームの dt は実時間）|
| `dx12_perf_stats` | `{window?:int=60(..240)}` | `fps` / `frameMs{avg,min,max,p95}` / `cpu{workMs,fenceWaitMs,presentMs}` / `gpuPassMs{total,shadows,depthPrepass,prepassSsao,clusterCull,raytracing,rtScreen,ddgi,screenSpaceGi,volFog,hiZ,mainScene,particles,postFx,ui}` / `drawCalls` / `culled` / `triangles` / `occlusion{...}` / `analysis{verdict:"gpu-bound"\|"cpu-bound"\|"fps-limit-capped"…, notes}` ※**FPS が出ないときはまずこれで犯人を特定する** |
| `dx12_benchmark` | `{...}` | 規模の梯子を測るベンチハーネス（同一シーンを条件を変えて回し、どこで折れるかを出す）|

**入力テストの型**:

```
dx12_play
dx12_key_down(key:"D") → dx12_step_frames(frames:30) → dx12_get_entity(name:"Player")   # 右へ動いたか
                        → dx12_project_world_to_screen(name:"Player")                    # 画面内に居るか
dx12_key_up(key:"D") → dx12_get_script_errors()    # Lua が死んでいないか
dx12_stop
```

★合成入力より**人間に遊ばせて `dx12_get_play_session` を読む方が正確**（Play を押した時点で記録は始まっている）。

---

### 4-7. Git / GitHub（同期）

エディタの「Git」窓と同じ操作。対象は**プロジェクトフォルダの git リポジトリ**で、
エンジンの状態は変えない。認証はユーザーの既存の git/gh 資格情報に委ねる。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_git_status` | `{}` | `{branch, remoteUrl, changedFiles[{path,status,staged}], mergeInProgress, conflicts[], githubUser}` ※status は `A`追加/未追跡 `M`変更 `D`削除 `R`リネーム `U`競合 |
| `dx12_git_branches` | `{}` | `{current, branches[{name,current}]}` |
| `dx12_git_checkout` | `{name:string, create?:bool=false}` | `{succeeded, exitCode, output, branch, created}` |
| `dx12_git_merge` | `{name:string, noFastForward?:bool=true}` | `{succeeded, ..., mergedFrom, into, mergeInProgress, conflicts[]}` |
| `dx12_git_merge_abort` | `{}` | `{succeeded, exitCode, output}` |
| `dx12_git_commit` | `{message:string}` | `{succeeded, ..., branch}` |
| `dx12_git_push` | `{}` | `{succeeded, ..., branch}` |
| `dx12_git_pull` | `{}` | `{succeeded, ..., mergeInProgress, conflicts[]}` |
| `dx12_git_fetch` | `{}` | `{succeeded, exitCode, output}` |

**約束事**:

- **まず `dx12_git_status` を読む。** 未コミットの変更があると `dx12_git_merge` は実行前に弾かれるし、
  `mergeInProgress:true` の間にできるのは「マージを終わらせるコミット」だけ。
- ブランチ名は `dx12_git_branches` から取る（推測しない）。
- **`dx12_git_checkout` / `dx12_git_pull` はプロジェクトの assets を入れ替える。**
  シーンや `.hlsl` が消える/増えるので、実行後は `dx12_list_scenes` と `dx12_git_status` を読み直す。
  ★エディタで開いているシーンのファイルが消えたら、そのまま編集を続けないこと。
- コンフリクトは `succeeded:false` + `conflicts[]` で返る（エラーではない＝作業ツリーが止まった状態）。
  直して `dx12_git_commit` で完了させるか、`dx12_git_merge_abort` で取り消す。
- **`dx12_git_push` は外向きの操作**（他人から見える場所へ出る）。ユーザーが明示的に頼んでいないなら撃たない。

```
dx12_git_status                                  # 汚れていないか / マージ中でないか
dx12_git_branches                                # 取り込み元の正確な名前
dx12_git_merge(name:"feature/x")                 # conflicts[] が空なら完了
  └ 競合したら → 各ファイルを直す → dx12_git_commit(message:"...")
  └ 諦めるなら → dx12_git_merge_abort
```

---

### 4-8. VFX(パーティクル / トレイル)

エンジンのパーティクルは 1 レイヤー 40 フィールド近くあり、生の数値を並べても「それらしく」ならない。
①`dx12_vfx_library`(何が作れるか)→②`dx12_vfx_apply`(レシピ+倍率で複数レイヤーまとめて置く)→
③`dx12_vfx_preview`(時間を進めながら連写して本当に出ているか確認)の順に使う。下 3 本の生レイヤー操作は
レシピから外れた微調整用。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_list_particle_layers` | `{entity?/name?}` | `{layers:[{index, name, kind, rate, looping, offset, vfxPath}], count}` ※放出器のレイヤー一覧。`dx12_set_component` の `layer` 引数(添字/名前)や Trigger の PlayEffect/StopEffect に渡すレイヤー名はここで分かる |
| `dx12_add_particle_layer` | `{entity?/name?, layerName?:string}` | 追加後のレイヤー情報 ※放出器にレイヤーを 1 枚足す(上限 16 枚)。★これが無いと 1 枚目しか触れない=「炎に煙を重ねる」ができない。レシピから一気に組むなら `dx12_vfx_apply` の方が速い(内部でこれを呼ぶ) |
| `dx12_remove_particle_layer` | `{entity?/name?, layer:int\|string}` | 削除結果 ※放出器のレイヤーを 1 枚消す。★最後の 1 枚は消せない。放出器ごと消すなら `dx12_remove_component(component:'particleEmitter')` |
| `dx12_vfx_library` | `{tag?:string, id?:string}` | `id` 省略時 `{presets:[...], count, tags}`、`id` 指定時はそのレシピの全レイヤー実値と注意書き ※松明/焚き火/爆発/魔法陣/雨/雪/剣閃など。★どれも複数レイヤーの重ね合わせで作ってある(炎+煙+火の粉) |
| `dx12_vfx_apply` | `{preset:string, entity?/name?, position?:[x,y,z], entityName?:string, parentName?:string, scale?:f, rate?:f, intensity?:f, color?:[r,g,b], oneShot?:bool, duration?:f, life?:f, light?:bool, replaceLayers?:bool, dryRun?:bool}` | `{applied, preset, title, entityId, name, created, layersApplied[], layersRemoved, trailApplied, current, estimatedLiveParticles, notes, lookHint, warnings, next}` ※レシピから複数レイヤーの放出器を 1 コールで組み立てる(新規作成 or 既存へ付与)。★Editor 限定(新規生成を伴うため)。置いた後は必ず `dx12_vfx_preview` で確認すること |
| `dx12_vfx_preview` | `{entity?/name?, seconds?:f=1.5, frames?:int=6(2..12), distance?:f=3, height?:f=0.5, fire?:bool, columns?:int=3, additive?:bool=true, baseline?:bool=true}` | PNG 画像ブロック + text(計測値と助言) ※放出器に寄って時間を進めながら連写し、格子画像+計測値を返す。★静止画 1 枚では「たまたま写っていない」のか「そもそも出ていない」のか区別できない。ワンショットは `fire:true` で試し撃ちしてから撮る |

### 4-9. ルック(絵作り)

ポストは約 90 フィールドあり、1 つずつ触っても bloom と exposure だけ上げて終わりがち。
光+空気+グレーディングを組み合わせで渡す。`dx12_apply_lighting_preset`(太陽+ごく一部のポスト)は土台、
ここは土台の上に乗せる仕上げ(フォグ・トーンマッパー・ビネット・粒子・色収差まで)。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_look_library` | `{tag?:string, id?:string}` | `id` 省略時 `{looks:[{id,title,summary,tags,touches,pairsWith}], count, tags}`、`id` 指定時は全フィールドの実値と注意書き ※ゴールデンアワー/ネオンノワール/ホラー/白黒/水中など |
| `dx12_look_apply` | `{preset:string, strength?:f=1, parts?:("sun"\|"fog"\|"post"\|"sky")[], dryRun?:bool}` | `{applied, preset, title, parts, strength, sun, fog, sky, postRequested, currentPost, mismatched?, hint?, notes, pairsWith, warnings, next}` ※太陽+ボリュメトリックフォグ+背景の強さ+ポスト約 20 項目を 1 コールでまとめて当てる(冪等)。★`strength` はポストにだけ効く(0 で無味無臭の値に寄る)。`parts:['post']` で今の光を壊さずグレーディングだけ乗せる。★暗いルックは光源を置いてから当てること(真っ暗なシーンに当てても真っ黒になるだけ) |

### 4-10. デカール(投影テクスチャ: 弾痕・焦げ・血・水たまり・苔・汚れ)

「そこで何かが起きた」を語る唯一の安い手段。1 つも無い床はどれだけ光を凝ってもショールームに見える。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_decal_library` | `{}` | `{decals:[{id,title,summary,defaultSize,surface,changes,notes}], count, atlas, next}` ※貼れる汚れ・傷の一覧。★水たまり/血だまり/油/雪はほぼ水平面専用(角度フェードが小さい)。壁には dirt / leak / blood_splatter を使う |
| `dx12_decal_apply` | `{preset:string, position:[x,y,z], normal?:[x,y,z](既定[0,1,0]), size?:f, depth?:f, rotationDeg?:f, opacity?:f, tint?:[r,g,b], sortOrder?:int, count?:int(1..24), spread?:f, seed?:int, entityName?:string, parentName?:string, dryRun?:bool}` | 貼ったデカールの姿勢と値 ※面へ投影して DecalComponent 付きエンティティを作る。★初回はアトラス画像(`assets/textures/decals/atlas.png`)を手続き生成してシーンに設定する(無いと無言で何も出ない)。位置と法線は `dx12_raycast_precise`/`dx12_pick` の `worldPos`/`worldNormal` をそのまま渡すのが正確。★Editor 限定。`count>1` で散らして複数枚貼れる |

### 4-11. 演出(カットシーン / シーケンス)

カメラ・ポスト・時間・エフェクト・音が同じ時間軸で噛み合って初めて演出になる。宣言的な台本→生成コードに固定し、
時間の扱いと後始末を 1 箇所で正しくする(AI に Lua を直接書かせると毎回ちがう自己流の状態機械が生える)。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_sequence_author` | `{name:string, tracks:object[], camera?:string, loop?:bool, attachTo?:string, doneEvent?:string, activateCamera?:bool, dryRun?:bool}` | `{applied, name, path, camera, attachedTo, createdEntity, duration, trackCount, playEvent, stopEvent, doneEvent, referenced, vfx, warnings, next}` ※時間軸の台本(JSON)から Lua コンポーネントを生成して貼る。track の type: camera(位置移動+注視) / fade / post / timeScale(スローモ) / shake / vfx / sound / move・rotate / light / event / scene / log。★時計は実時間(タイムスケール非適用)で進むので、スローモを掛けても台本は実時間で流れる。終了時にタイムスケールを 1.0 へ戻す。★カメラを動かすには `camera` に CameraComponent 持ちのエンティティ名が要る(グローバルカメラは毎フレーム上書きされるため)。他スクリプトから `events:emit('<name>:play'/'stop')` で操作できる |
| `dx12_sequence_preview` | `{seconds?:f=5, frames?:int=6(2..12), startDelay?:f=0, columns?:int=3, name?:string}` | PNG 画像ブロック + text(`{path, name, frames, secondsTotal, secondsPerFrame, frameDiffs, moved, recentLog, hint}`) ※Play して演出を実際に流し、ゲーム画面を時間で連写した格子画像を返す。撮影後は必ず Stop する。★撮る前に `dx12_set_editor_camera` の固定を自動解除する(残っていると演出が動いても絵が変わらない事故になる) |

### 4-12. シーンの整理 / 命名規約

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_scene_scaffold` | `{only?:("ENV"\|"LVL"\|"LGT"\|"GP"\|"FX"\|"UI"\|"CAM")[]}` | `{groups:[{key, root, entityId, created}], convention}` ※共同開発用のグループ骨格(空エンティティ)を作る。既にあるものは作り直さない(何度撃っても安全)。★ルートは必ず原点・無回転・スケール 1(`set_parent` はワールド座標を保持しないため、単位変換でないルートにぶら下げると物がワープする) |
| `dx12_organize_scene` | `{dryRun?:bool=true, rename?:bool=true}` | `{applied, moves:[{entityId, oldName, newName, group, reparent, locked}], untouched, protectedNames, luaFilesScanned, convention}` ※既存シーンを命名規約に沿って整理(コンポーネントで分類→規約グループへ親付け→`<PREFIX>_<Kind>_<NN>` に改名)。既定 `dryRun:true`=計画のみ。★プロジェクトの `.lua` を全部読み、文字列として出てくる名前は改名しない(`scene:findEntity` は名前で引くので改名すると『エラーも出ずに OnUpdate の残りが動かない』壊れ方をする) |
| `dx12_validate_naming` | `{}` | `{pass, checked, issues:[{entityId, name, kind, text}], counts, convention}` ※命名とグループ分けの崩れ(DEFAULT_NAME/NO_PREFIX/DUPLICATE_NAME/BAD_CHARS/NOT_IN_GROUP)を数える。直すのは `dx12_organize_scene` |

### 4-13. 品質検査(アセット欠落 / 配置 / 仕上がり)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_asset_gap` | `{includePlaceholders?:bool=true}` | `{missing:[{entityId,name,modelPath}], placeholders:[{entityId,name,primitive,sizeHint}], count, next}` ※参照切れ(modelPath があるのにファイルが無い)と、プリミティブで代用しているだけの仮置きを集める。Blender で作り始める前の入口 |
| `dx12_validate_layout` | `{fix?:"none"\|"safe"\|"all"(既定 none), tolerance?:f=0.001}` | `{pass, checked, errors, warnings, fixed, issues:[{kind, level, entityId, name, otherEntityId?, text, fixed}]}` ※埋まり(BURIED)/浮き(FLOATING)/ちらつき(Z_FIGHT)/深いめり込み(OVERLAP)/二重配置(DUPLICATE)/当たり判定欠落(NO_COLLIDER・COLLIDER_WITHOUT_BODY)/スケール異常(SCALE_ANOMALY・NAN_TRANSFORM)を数値で拾う。Editor 限定(Playing 中は MODE_CONFLICT)。★`COLLIDER_WITHOUT_BODY` はこのエンジン固有の罠(boxCollider だけでは Jolt に載らず床をすり抜ける)。`fix:'safe'` で BURIED/FLOATING・Z_FIGHT・COLLIDER_WITHOUT_BODY を自動修正。DUPLICATE は取り返しがつかないので報告のみ |
| `dx12_polish_audit` | `{screenshot?:bool=true, only?:("light"\|"air"\|"grade"\|"motion"\|"material"\|"contact"\|"image")[], sampleMeshes?:int=24}` | `{score, verdict, findings:[{category, severity, what, why, fix}], facts}` ※高品質な絵に必ず入っている要素が揃っているかを測り、足りないものを効く順(光→空気→階調→動き→素材→接地)で返す。各指摘に「なぜ安っぽく見えるか」と「次に撃つコマンド」が付く。★`dx12_diagnose` は壊れているか、`dx12_look_compare` は参照画像との差を見る道具で、これは参照画像なしに「作りかけに見える理由」を言うためのもの |

### 4-14. Blender 連携(自動起動 → PBR素材/仕上げ → 規約どおり書き出し → 実寸検証)

アドオンの既定は全部 OFF なので、放っておくと AI はプリミティブだけで真っ白でのっぺりしたモデルを組む。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_model_brief` | `{kind?:string(character/prop/level/家具等)}` | `{rules, materials, gotchas}` ※dx12 へ持ってくるモデルの作り方(Blender で作り始める前に読む)。★単色マテリアル禁止(glTF の baseColorFactor を読まないので真っ白になる)/アルファ抜きが無いので葉・枝カードは Blender で消してから出す/単位はメートル・原点は底面中心・+Y が正面/テクセル密度 512〜1024texel/m/ORM は G=roughness B=metallic/シェイプキーは捨てる |
| `dx12_blender_ensure` | `{blenderPath?:string, timeoutMs?:f=60000}` | `{running, started, port, blenderPath?, waitedMs?, ...assetSources}` ※BlenderMCP アドオンのソケット(127.0.0.1:9876)が生きているか確かめ、死んでいたら Blender を起動してポートが開くまで待つ。PolyHaven も自動で有効化する。★モデリングを頼まれたらまずこれを撃つ(手で Blender を開いてもらう必要は無い) |
| `dx12_blender_polish` | `{objects?:string[], bevelWidth?:f=0.003, bevelSegments?:int=2, smoothAngle?:f=30, uvMeters?:f=1.0, minThickness?:f=0.004}` | Blender 実行結果(処理内容の要約) ※「うすぺらい」を消す一括処理: ①スケール適用(★ベベルより先。非一様スケールのままだと角が歪む) ②厚みゼロの板に Solidify ③UV を実寸で切り直す ④スムーズ+自動スムーズ ⑤ベベル ⑥加重法線。書き出す前に必ず通すこと |
| `dx12_blender_material` | `{objects?:string[], assetId?:string, keyword?:string, resolution?:"1k"\|"2k"\|"4k"(既定2k), uvMeters?:f=2.0}` | Blender 実行結果(貼った素材の情報) ※PolyHaven(CC0・API キー不要)から PBR 素材を落として貼る。★エンジンは glTF の baseColorFactor を読まないのでテクスチャ無しのマテリアルは真っ白になる。ORM は arm マップがあればそのまま、無ければ Rough から B=0 で合成(単体のまま出すと木や布が金属として描かれる) |
| `dx12_blender_export` | `{objects?:string[], destPath:string, clearShapeKeys?:bool=true, applyModifiers?:bool=true}` | `{destPath, absPath, exported[], bytes, renamedImages[], warnings[], assetInfo, next}` ※Blender の選択物を dx12 の規約どおり glTF で書き出し assets へ取り込み実寸まで検証する。★シェイプキーを捨てる/画像テクスチャが 1 枚も無いマテリアルを警告/`tmpXXXX.jpg` 名の画像を意味のある名前へ直し uri も書き換える。取り込み後 `dx12_asset_info` で実寸を読むので cm/m の取り違えもその場で分かる |
| `dx12_scene_env` | `{assetId?:string, keyword?:string, resolution?:"1k"\|"2k"\|"4k"(既定2k), iblIntensity?:f=1.0, skyboxIntensity?:f=0.35, drawSkybox?:bool=true}` | `{assetId, path, bytes, skybox, note}` ※PolyHaven の HDRI(CC0・API キー不要)を落としてシーンの環境マップにする。★既定の手続き空のままだと全部に青が乗って彩度が落ちる。金属と光沢は環境に映るものが無いと質感が出ない。★屋内シーンで環境光を効かせたくない場合は使わないこと(envMapPath があると DirectionalLight.ambient が無視される) |

### 4-15. テストプレイ(決定論台本 / 移動能力の実測 / 到達性 / 回帰テスト)

コードは ctest で守られているのに遊びは誰も守っていない、という穴を埋めるための一群。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_play_script` | `{steps:[{t:f, down?:string\|string[], up?:string\|string[], press?:string\|string[], yaw?:f, note?:string}], expect?:[{at?:f, by?:f, near?:[x,y,z], radius?:f, yAbove?:f, yBelow?:f, grounded?:bool, movedAtLeast?:f, label?:string}], player?:string, until?:f, sampleHz?:f, dt?:f, autoPlay?:bool=true}` | `{pass, player, durationSec, dt, results[], trace[], samples, mouseDegPerPixel?, yawWarnings?, next?}` ※入力タイムラインと合否条件を 1 コールで走らせる(dt を 1/60 に固定=同じ台本なら毎回同じ結果になる)。yaw(度) はマウス移動注入の閉ループで合わせる(★`camera:setYaw()` では向けられない)。落ちた条件は「どれだけ足りなかったか」を返す |
| `dx12_measure_player` | `{player?:string, forwardKey?:string=W, jumpKey?:string=SPACE, save?:bool=true}` | `{walkSpeed, jumpHeight, jumpDistance, stepHeight, maxSlopeDeg, measuredAt, warnings, savedTo?, startPos}` ※プレイヤーを実際に歩かせ・跳ばせて【歩行速度/ジャンプ高/ジャンプ距離】を実測する(宣言値ではなく Lua の実装値)。結果は `<project>/.dx12/movement.json` に保存され、`dx12_check_reachable` の判定根拠になる |
| `dx12_check_reachable` | `{from?:[x,y,z], fromName?:string, to?:[x,y,z], toName?:string}` | `{reachable, warnings, capability, start?, goal?, pathPoints?, pathLength?, issues?, estimatedWalkSec?, reason?, next?}` ※ナビメッシュの経路と実測した移動能力で「そこへ行けるか」を判定する(Play しないので何度でも撃てる)。★先に `dx12_measure_player` を撃つこと(実測値が無ければ保存値→保守的な既定値の順にフォールバックし warning を出す) |
| `dx12_autoplay` | `{goal?:[x,y,z], goalName?:string, player?:string, forwardKey?:string=W, jumpKey?:string=SPACE, arriveRadius?:f=1.5, timeoutSec?:f=60}` | `{cleared, player, goal, finalPos?, notes, remainingDistance?, waypointsReached?, waypoints?, trace, elapsedSec, stuckAt?, stuckAtWaypoint?, reason?, next?}` ※ナビメッシュの経路を実際の入力(マウス+WASD+ジャンプ)でなぞって本当にゴールへ行けるか確かめる。★`dx12_check_reachable`(静的判定)の実証版。詰まったら座標付きで返す |
| `dx12_record_playtest` | `{name:string, endTolerance?:f=1.0, pathTolerance?:f=2.0, note?:string}` | `{saved, name, scene, durationSec, inputs, samples, humanDrift, warnings, next}` ※直前の 1 プレイ(`dx12_get_play_session`)を `.playtest` として保存する回帰テスト化。手順: `dx12_play`→人に遊んでもらう→`dx12_stop`→これ。★人の軌跡はそのまま基準にせず、1 回再生した結果をゴールデンランとして焼き込む(人のプレイは実時間・再生は固定 dt なので構造的にずれるため) |
| `dx12_run_playtests` | `{name?:string}` | `{ran, passed, failed, results:[{name, scene, pass, endDistance, maxDeviation, maxDeviationAt, reasons}], next?}` ※保存済み `.playtest` を再生して記録どおり動くか確かめる。落ちたときは「いつ・どれだけ」ずれたかを返す。`name` 省略で全部走らせる |

---

### 4-5. 精密ピック / 地形 / スカルプトの約束事

**2 種類のレイキャストを取り違えないこと。**

| | `dx12_raycast` | `dx12_raycast_precise` / `dx12_pick` |
|---|---|---|
| 何に当たるか | Jolt の**物理コライダー** | **描画メッシュの三角形** |
| いつ使えるか | **Playing 中のみ**(body は Play 開始時に登録される) | Editor / Playing どちらでも |
| 精度 | コライダー形状(箱/カプセル/凸包の近似) | 実際の三角形。法線も面の法線 |
| 典型用途 | ゲームロジックの当たり確認・接地判定の再現 | 「スクショのここに何がある？」「地面の実際の高さは？」配置の自動化 |
| 制限 | コライダー形状ぶんの丸め（法線はコライダー面の真の法線） | スキンドメッシュはバインドポーズの AABB 止まり |

`dx12_pick` はエディタの左クリック選択と**同じ `RaycastScene` 実装**を通る（`src/editor/ScenePick.h`）。
MCP で見えるものとエディタで選ばれるものが食い違わないのが、この 2 つを共有している理由。

> ブロードフェーズは**直近に描かれたフレームの描画リスト**を借りる（10 万体でも速いのはこのため）。
> `dx12_set_transform` で動かした直後に撃つと 1 フレームぶん古い位置で判定されることがある。
> 移動 → ピックを続けてやるときは間に `dx12_step_frames(frames:1)` を挟むこと。

**ナビメッシュ（追いかける AI の経路探索）**

- 生成は「ラスタライズ → フィルタ（またぎ/崖/頭上）→ コンパクト化 → エージェント半径ぶん侵食 →
  領域分割（分水嶺 or monotone）→ 輪郭抽出と単純化 → 凸ポリゴン化 → 高さサンプル格子」の自作パイプライン。
  入力は **AABB ではなくメッシュの実三角形**なので、坂道・階段・斜めの壁がそのままの形で反映される。
- **設定はシーン JSON の `navmesh`、焼いた実体はシーンの隣の `<シーン>.nav`**（バイナリ）。
  `dx12_navmesh_build` はメモリ上に焼くだけなので、**残すには `dx12_save_scene` が要る**。
  シーンを開くと `.nav` があれば自動で読む。Play/Stop をまたいでも消えない。
- 除外したいメッシュには `navMeshIgnore` タグを付ける。スキンメッシュ（動くキャラ）は自動で除外。
- **箱や柱のような閉じた立体は、底面と天面の間に頭上クリアランスが空くと内部の床も歩行面として残る**
  （どこからも行けない孤立島になる）。`minRegionArea` を 8〜20 m² に上げると消える。
- Lua からは `nav:ready()` / `nav:sample(pos)` / `nav:findPath(from,to)` / `nav:raycast(from,to)` /
  `nav:moveAlong(from,to)`。`findPath` を毎フレーム全員ぶん呼ばないこと。

**地形（ハイトフィールド）**

- 座標は常に**ワールド XZ**（`point:[x,z]`）。`dx12_pick` の `worldPos:[x,y,z]` をそのまま渡してもよい（y は無視）。
- 高さ配列はシーン JSON に入らない。`assets/terrain/<name>.hf` へ**自動保存**される（彫った次のフレームで書き出す）。
- コリジョンは Jolt の `HeightFieldShape` が**同じ高さ配列を読む**＝彫れば当たり判定も一緒に動く。
- 回転・スケールは効かない（XZ グリッドの前提）。位置だけが意味を持つ。
- 手順は「① `dx12_terrain_create` → ② `dx12_terrain_generate`（土台）→ ③ `dx12_terrain_sculpt`/`dx12_terrain_erode`（詰め）」。
  ②は高さを丸ごと作り直すので、**必ず③より先**にやること。
- **テクスチャレイヤー**（4 層スプラット）は `terrain.layerSetPath` に `.terrainlayers` を割り当てた地形だけ。
  割り当ては **`dx12_terrain_set_layers`**（MCP）/ シーン JSON / 地形ツール窓のどれでもよい。
  割当後は `dx12_terrain_autopaint`（傾斜と標高から焼き直す・冪等）と
  `dx12_terrain_paint`（円ブラシで 1 層を塗る・相対）が使え、結果は
  `dx12_terrain_splat_info` で数値検証できる。詳細は
  [`AUTHORING.md` §10.5.1](AUTHORING.md)。**高さを彫り直したら autopaint をやり直すこと**
  （重みは高さに自動追従しない）。
  手順の例:
  ```
  dx12_terrain_create      {name:"Terrain", resolution:256, worldSize:400}
  dx12_terrain_generate    {name:"Terrain", preset:"mountains", seed:1}
  dx12_terrain_set_layers  {name:"Terrain", layerSetPath:"terrain/alpine.terrainlayers"}   ← ここが無くて詰んでいた
  dx12_terrain_paint       {name:"Terrain", point:[0,0], layer:2, radius:60, strength:1}
  dx12_terrain_splat_info  {name:"Terrain", point:[0,0]}   → weights:[0,0,1,0] で確認
  ```

**スカルプト（異形メッシュ）**

- ハイトフィールドで作れないもの（洞窟・アーチ・せり出した岩）担当。トポロジは変えず頂点だけ動かす。
- `position` は**ワールド座標**で渡すが、`radius` / `strength` は**メッシュのローカル単位**（Transform の scale が掛かる前）。
- 頂点配列は `assets/sculpt/<name>.smsh` へ自動保存。コライダー（`MeshShape`）も彫った形に追従する。

**共通**

- 地形・スカルプトの生成 / 編集系は**すべて Editor 限定**（Playing 中は `MODE_CONFLICT(3)`）。
  Play→Stop はシーンを作り直すので、Playing 中に彫っても巻き戻る。
- メッシュ・コリジョン・`.hf`/`.smsh` の保存が反映されるのは**次のフレーム**（エディタのブラシと同じ経路）。
  彫った直後に見た目を確認するなら `dx12_step_frames(frames:2)` を挟んでから撮ること
  （`dx12_screenshot` は直近に描かれたフレームを返すため）。`dx12_step_frames` は Editor でも使える。

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

`create_entity` / `spawn_model` / `spawn_prefab` は `idempotency_key` を受け付ける。
同じキーで2回送った場合、2回目は処理をスキップして1回目の `entityId` を
`{"idempotentReplay": true}` 付きで返す。AI がリトライしたときに重複エンティティを防ぐ用途。

```json
{"method":"create_entity", "params":{"type":"box","idempotency_key":"floor-001"}}
```

- `spawn_prefab` は**リプレイ時も `rootEntityId` / `entityIds`（サブツリー全部）を返す**。
- キーはシーンをまたがない（`open_scene` / `new_scene` で表ごと捨てる）。
- 記録された entity が削除済みなら「無かったこと」にして普通に生成する。
- ⚠️ **`spawn_prefab` は 2026-07-26 まで記録だけしてリプレイ判定を持っておらず、
  再送で毎回サブツリーが増えていた**（#20-4 の実バグ。修正済み）。

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

### error_hint / error_values（「次の一手」と有効値）

エラー応答には **任意で** 次の 2 フィールドが乗る（無いこともある。旧来の形は変えていない）。

| フィールド | 内容 |
|---|---|
| `error_hint` | 次に何をすればいいかを 1 文で（例: 「先に `dx12_stop` で Editor へ戻してくれ」）|
| `error_values` | 列挙型の引数が不正だったときの**有効値の全部**（例: `["raise","lower","smooth","flatten","noise"]`）|

Node 側は `Error.hint` / `Error.valid_values` として受け取り、ツールのエラーメッセージへ

```
エラー(code=2): unknown brush: dig
ヒント: 有効値のどれかを指定してくれ
有効な値: raise, lower, smooth, flatten, noise
```

の形で整形して返す。**エラー本文を読めばリトライの引数が決まる**のが狙いなので、
新しいツールを足すときは範囲外・列挙ミスに必ず hint（と可能なら valid_values）を添えること。

---

## 9. 検証ループ

変更をかけた後は以下のループで目視確認できる:

```
1. dx12_set_transform / dx12_set_component で変更
2. dx12_focus_and_screenshot(entity: <entityId>) → PNG で確認
3. dx12_get_log(lines: 30) → エンジンのエラー/警告を確認
4. 問題があれば**同じ set_* を反対の値で呼び直す**
```

★ここで `dx12_undo` を使ってはいけない。**MCP の編集ツールはほぼ Undo に積まれない**
（積むのは `dx12_group_entities` だけ）。`set_transform` を取り消すつもりで `dx12_undo` を
呼ぶと、スタックの一番上にある別の操作（エディタでの編集や entity 生成）が戻る。
`dx12_undo` は戻り値に `willUndo`（次に戻る操作の名前）と `undoable` を返すので、
どうしても使うなら**自分の操作かどうかを確かめてから**呼ぶこと。

Play/Stop テスト:
```
dx12_play → (ゲームロジック動作) → dx12_stop
→ dx12_focus_and_screenshot でシーン確認
→ dx12_get_log でランタイムエラー確認
```

### 9-1. どのスクショを使うか（絵を判断するときの必読）

| ツール | 撮る先 | 写るもの | 用途 |
|---|---|---|---|
| `dx12_screenshot` | `m_sceneRT`（**ポスト前**） | シーン本体だけ | 幾何 / ライティングの素の値を見たいとき |
| `dx12_screenshot_final` | **バックバッファ**（ポスト後・ImGui 前） | グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド / **TAA 解決結果** | **見た目の判断は必ずこちら** |
| `dx12_ui_screenshot` | ウィンドウ全体（`PrintWindow`） | 上に加えて ImGui のパネル / ギズモ | エディタ UI・ゲーム内 UI の確認 |

### 9-2. `deterministic` — ピクセル差分で A/B を取るとき（#31）

**同じ設定で 2 回撮っても絵は一致しない。** 実測した原因は 3 つ:

| 原因 | 効く先 | 実測（1920x1032・同一設定 2 枚） |
|---|---|---|
| ポストの **deband ディザ / フィルムグレイン**（`time` 依存の TPDF ノイズ） | `screenshot_final` のみ | 画面の **66%** のピクセルが ±1〜2 LSB |
| **TAA のジッタ**（毎フレーム位相が回るのでラスタ結果そのものが動く） | 両方 | `screenshot` で **9.4%** / max 140 |
| **SSGI・ボリュメトリックフォグの時間ジッタ + 履歴蓄積** | 両方 | SSGI 1.5% / フォグ 5.9% |

`{"deterministic": true}` を付けると:
1. `time` を固定（deband / grain / wave / glitch / パーティクル / カスタムシェーダの time が全部止まる）
2. TAA・フォグ・SSGI の**時間ジッタ位相を毎フレーム 0 に固定**
3. パーティクルの前進を止める（dt=0）
4. **時間蓄積の履歴を捨ててから** `settleFrames`（既定 8）フレーム回し、そこで撮る

→ 実測: 上の全構成（TAA / SSGI / フォグを個別 ON・全部 ON）で **2 枚が完全一致（diff 0.00%）**。

> ⚠️ 止まるのは**レンダラの時間依存だけ**。Play 中のゲームシミュレーション（移動 / 物理 /
> アニメーション）は止まらないので、厳密に比べたいときは `dx12_stop` してから撮ること。
> `settleFrames` を増やすと TAA / SSGI の収束が進む（決定性は 8 でも得られる）。

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
- **レスポンス(失敗)**: `{"id":<同id>, "ok":false, "error":<string>, "error_code":<int>,
  "error_hint":<string 任意>, "error_values":<string[] 任意>}\n`

遅延同期 method は受信時は何も返さず、フレーム境界で実処理後に同じ id でレスポンスを送る。

### 12-1. エンジン側に method を足す（実装者向け）

`Application::HandleMcpCommand` は **`std::unordered_map<std::string, McpMethodEntry>` の表引き**。
以前は `else if (method == "...")` の 118 本連鎖で、MSVC の
**「ブロックの入れ子のレベルが深すぎます (C1061)」上限に張り付いていた**（1 本足すとコンパイルが落ちた）。
表引きなので **method を何本足しても入れ子は 1 段も深くならない**。

足し方は 3 つだけ:

1. `src/core/mcp/ApplicationMcp*.cpp` の `Register***McpMethods()` のどれか（テーマで選ぶ。
   順序に意味は無い）へ。ファイルはテーマ 1 対 1 で分かれている
   （`Entity` / `Editor` / `Render` / `Tooling` / `Asset` / `Terrain` / `Lighting`。
   表の土台と共有ヘルパは `mcp/ApplicationMcp.cpp` と `core/ApplicationInternal.h`）
   ```cpp
   McpDefine("名前", "キー:型,キー:型", DX12E_MCP_HANDLER
       {
           // params / resp / method / deferred / isDeferred / busyPlaying がそのまま使える
           resp["ok"] = true;
           resp["result"] = { ... };
       });
   ```
   `"a|b"` と書くと 1 本のハンドラで 2 つの method を受ける（本文で `method ==` を見て分ける）。
2. 第 2 引数のキー表は **`dx12_describe_mcp_params` がそのまま返す**。型は
   `bool` / `int` / `number` / `string` / `vec3` / `object` / `any`、入れ子は `"親.子"`。
   **本文で読むキーと必ず一致させること。**
   ポスト / SSAO だけは `DX12E_POST_FIELDS` / `DX12E_SSAO_FIELDS`（X マクロ）から自動生成している。
3. `tools/mcp-server/index.ts` の zod スキーマとこのドキュメントにも同じキーを足す
   （**足し忘れると zod が黙って引数を捨てる**。`schemaDrift.test.ts` が見張っている）。

テーマ別ファイルを新設したときだけ、`src/core/CMakeLists.txt` のソース一覧と
`tests/CMakeLists.txt` の `DX12E_MCP_SOURCES`（`McpParamSpecTests` が走査する対象）にも足すこと。

- 例外は `throw McpError(McpErr::…, msg, hint, validValues)`。呼び出し側の try/catch が拾う。
- 遅延応答は `deferred` を保存して `isDeferred = true`。
- ハンドラの中で `return;` してよい（旧 else-if 連鎖では `HandleMcpCommand` ごと抜けてしまい
  `RecordCommand` を飛ばしていたので使えなかった）。

---

関連: `tools/mcp-server/AGENTS.md`(AI エージェント運用ガイド)、`tools/mcp-server/README.md`(サーバ構成)、
`docs/AUTHORING.md`、`docs/SCRIPT_COMPONENTS.md`(Lua)。
