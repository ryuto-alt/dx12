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
| `dx12_get_contact_shadow` | `{}` | `{enabled, rayLength, thickness, bias, intensity, steps, maxDistance, fadeDistance}` |
| `dx12_get_taa` | `{}` | `{enabled, sampleCount, feedbackMin, feedbackMax, varianceGamma, jitterScale, debugVelocity, active, fxaaSuppressed}` |
| `dx12_read_lua_component` | `{path:string}` | `{path, code}` ※既存 .lua のソースをそのまま読む |
| `dx12_read_shader` | `{path:string(assets/shaders相対)}` | `{path, code, compiled}` ※既存カスタムシェーダーのソースをそのまま読む(compiled は直近の既知のコンパイル成否) |
| `dx12_raycast` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f}` | `{hit, distance?, point?, normal?, entityId?, name?}` ※Playing 中のみ意味のある結果 |
| `dx12_overlap_box` | `{center:[x,y,z], halfExtents:[x,y,z], maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_overlap_sphere` | `{center:[x,y,z], radius:f, maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ |
| `dx12_get_physics_state` | `{entity:int}` | `{entityId, hasRigidBody, velocity:[x,y,z], hasCharacterController, isGrounded}` ※Playing 中のみ |
| `dx12_validate_scene` | `{path?:string}` | `{pass, exitCode, report, scenePath}` ※`--validate` をヘッドレス子プロセスで実行。省略時は現在のシーン |
| `dx12_get_anim_state` | `{entity:int}` | `{hasSkeletalAnimation, clips:[クリップ名...]}` ※`dx12_play_anim` の clipName 選びに |
| `dx12_net_status` | `{}` | `{available, role:"Offline"\|"Host"\|"Client", isConnected, localClientId, tick, syncedEntityCount, players:[{id,rttMs,bytesSent,bytesReceived}], config:{tickRate,snapshotRate,maxPlayers,defaultPort}, testRole, testJoinAddress}` |
| `dx12_screenshot` | `{}` | PNG 画像ブロック + text(`{path(絶対パス), width, height}`) |
| `dx12_ui_screenshot` | `{}` | PNG 画像ブロック ※エディタウィンドウ全体(ImGuiパネル込み)。ゲーム内UI/UIエディタの見た目確認用(scene RT には UI が写らない) |
| `dx12_ui_tree` | `{}` | `{canvases:[{entityId, name, uiCanvas:{refWidth,refHeight,...}, children:[{entityId, name, components, uiRect, resolvedRect:[x,y,w,h](キャンバス空間px), text?, children}]}]}` ※UIレイアウトの数値確認 |
| `dx12_ui_design_brief` | `{genre:"cinematic"\|"tactical"\|"fantasy"\|"horror"\|"arcade"\|"cozy", screen:"title"\|"hud"\|"inventory"\|"settings"\|"result"\|"dialog"\|"other", tone?}` | 画面固有の構図・階層・制約・アンチパターン。UI生成前に呼ぶ |
| `dx12_ui_audit` | `{strictness?:"balanced"\|"strict"}` | 現在のUIを数値監査。`{pass,score,grade,summary,issues[]}`。崩れ/重なり/可読性/入力遮断/過装飾を検出 |
| `dx12_ui_compose` | `{blueprint:{theme,prefix,root}}` | dock/stack/grid と意味的roleからUI一式を制約付き生成。失敗時ロールバック。生成後はaudit→screenshot必須 |
| `dx12_get_editor_camera` | `{}` | `{position, forward, yawDeg, pitchDeg, fovYDeg, orthographic, mode}` ※シーンビューを描いてるカメラの状態 |
| `dx12_get_bounds` | `{entity:int, includeChildren?:bool}` | `{min, max, center, size, hasMesh}` ※ワールド空間 AABB(回転/親子変換込み)。配置座標の計算に |
| `dx12_get_hierarchy` | `{}` | `{roots:[{entityId, name, children:[...]}], count, sceneGeneration}` ※シーンの親子ツリー |
| `dx12_asset_info` | `{path}` | モデル: `{meshCount, totalVertices, totalFaces, materialCount, boneCount, hasSkeleton, animations:[{name,durationSec}], aabbMin/Max(メッシュローカル近似)}`、テクスチャ: `{width, height, mipLevels, format, isCubemap}`、他: `{type, fileSizeBytes}` |
| `dx12_view_texture` | `{path, maxSize?:int=1024}` | PNG 画像ブロック ※dds/tga/hdr も変換して見られる。キューブマップは先頭面のみ |
| `dx12_pick` | `{x?,y?(px) \| u?,v?(0..1), all?:bool, maxHits?:int=16, includeIcons?:bool=true, trianglePrecise?:bool=true, maxCandidates?:int=64}` | `{hits:[{entityId,name,submeshIndex,distance,worldPos,worldNormal,isIcon}], count, totalHits, truncated, screen, viewport, mode}` ※**エディタの左クリック選択と同じ `RaycastScene`**。座標系は `dx12_screenshot` / `dx12_project_world_to_screen` と同じ |
| `dx12_raycast_precise` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f=1000, all?:bool, maxHits?:int=16, trianglePrecise?:bool=true, maxCandidates?:int=256}` | `dx12_pick` と同形式 + `{origin, direction, maxDistance}` ※**描画メッシュの三角形基準**。`dx12_raycast`(物理コライダー基準・Playing 限定)とは別物 |
| `dx12_terrain_sample` | `{entity?/name?, points?:[[x,z]...] (最大512)}` | `{entityId, name, origin, resolution, worldSize, cellSize, boundsXZ, minHeight, maxHeight, samples:[{x,z,height,worldY,normal,slopeDeg,inside}], count}` |
| `dx12_list_lights` | `{limit?:int=50, cursor?:int}` | `{lights:[{entityId,name,type,position,slot,color,intensity,range?,direction?,innerConeDeg?,outerConeDeg?,castShadows?,overBudget,effective}], count, total, cursor, nextCursor, has_more, budget:{point,spot,directional,shadowSpot,shadowPoint}, warnings:[...]}` ※**上限超過は無言で描画されない**ので必ずここで確認する |
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
| `dx12_set_pbr` | `{entity:int, metallic?:f, roughness?:f, uvScaleU?:f, uvScaleV?:f}` | `{entityId, metallic, roughness, uvScaleU, uvScaleV}` |
| `dx12_set_mesh_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, skinnedFallbackWarning}` ※shaderPath省略/空文字で既定Forwardに戻す。alphaBlend省略時は既存値を維持、既定false(不透明固定でPSのalpha出力は無視される)。true でSrcAlpha/InvSrcAlphaブレンド(DepthWrite OFF) |
| `dx12_set_sprite_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, worldSpaceWarning}` ※Sprite2D専用・world-spaceのみ対応。MeshRendererのシェーダーとは頂点/ルートシグネチャの契約が異なる(docs/AUTHORING.md §6.1)。shaderPath省略/空文字で既定Spriteシェーダーに戻す |
| `dx12_set_scene_settings` | `{skybox:{envMapPath?, iblIntensity?, skyboxIntensity?, drawSkybox?}}` | `{applied, envMapRebake}` |
| `dx12_set_post_process` | 約25エフェクトの `<name>On`/パラメータ(指定分のみ適用) | `{applied}` |
| `dx12_set_ssao` | `{enabled?, radius?, bias?, intensity?, power?, sampleCount?, blur?}` | `{applied}` |
| `dx12_set_contact_shadow` | `{enabled?, rayLength?, thickness?, bias?, intensity?, steps?, maxDistance?, fadeDistance?}` | `{applied}` ※太陽(平行光)専用のスクリーン空間近接遮蔽。正射/2Dビューでは自動無効 |
| `dx12_set_taa` | `{enabled?, sampleCount?, feedbackMin?, feedbackMax?, varianceGamma?, jitterScale?, debugVelocity?}` | `{applied}` ※テンポラルAA。ONの間は `fxaaOn` が無視される。深度+速度プリパスが常時走る。正射/2Dビューでは自動無効 |
| `dx12_undo` | `{}` | `{queuedUndo}` |
| `dx12_redo` | `{}` | `{queuedRedo}` |
| `dx12_save_scene` | `{path?:string}` | `{path}` ※省略で現在シーンへ上書き |
| `dx12_create_lua_component` | `{name:string, code:string}` | `{path}` ※書込前に構文検証。既存パスなら上書き更新も兼ねる |
| `dx12_create_shader` | `{name:string, code:string}` | `{path, compiled, error?}` ※assets/shaders/に作成/上書き後、即コンパイルを試す。Luaと違い失敗してもファイルは残る(反復修正前提) |
| `dx12_attach_lua_component` | `{entity:int, script:string(assets相対)}` | `ok` |
| `dx12_create_prefab` | `{entity:int, path?:string}` | `{path, entityId}` ※path省略で assets/prefabs/<name>.prefab |
| `dx12_eval_lua` | `{code:string}` | `{result:string}` ※任意 Lua をその場実行(デバッグ用) |
| `dx12_build_game` | `{}` | `{success, outputDir, error?}` ※ヘッドレスビルド(同期・数十秒かかることあり) |
| `dx12_set_texture` | `{entity:int, path:string(assets相対、空文字で解除), slot?:"albedo"\|"normal"\|"metalRoughness", submesh?:int}` | `{entityId, slot, submesh, path}` ※Inspector のテクスチャ D&D と同じインスタンス単位 override(Material 共有を壊さない) |
| `dx12_play_anim` | `{entity:int, clip?:int, clipName?:string, blend?:f=0.3, loop?:bool}` | `{entityId, clip, clipName, blend}` ※スケルタルアニメのクロスフェード再生(Lua playAnim と同経路) |
| `dx12_net_setup` | `{role:"host"\|"client"\|"offline", address?:string, port?:int}` | `{testRole, address, port}` ※次の `dx12_play` で自動 Host/Join(ツールバーの Play ロールと同じ) |
| `dx12_net_launch_test_client` | `{}` | `{requested}` ※ホスト Playing 中のみ。同エンジンをもう1プロセス起動し 127.0.0.1 へ自動接続(フレーム境界) |
| `dx12_set_editor_camera` | `{position?:[x,y,z], target?:[x,y,z], yawDeg?:f, pitchDeg?:f}` | `{position, forward, yawDeg, pitchDeg}` ※エディタのフライカメラを任意視点へ。target 指定で yaw/pitch 自動逆算。**Editor 限定**(Playing 中は MODE_CONFLICT) |
| `dx12_look_at` | `{entity:int, target?:[x,y,z], targetEntity?:int, targetName?:string, upright?:bool}` | `{entityId, rotation, target}` ※+Z 正面の想定で rotation(Euler) を書く。upright=true でピッチ0。親が回転してると厳密でない |
| `dx12_snap_to_ground` | `{entity:int, offset?:f, precise?:bool=true}` | `{groundY, movedBy, method:"raycast"\|"aabb", position, groundEntityId?}` ※**三角形精密レイキャストで真下の実際の面へ接地**(地形の起伏・斜面・彫った岩に乗る)。真下に三角形が無ければ従来の AABB 天面判定へフォールバック(`method:"aabb"`)。床なしは y=0。Editor 中でも動く |
| `dx12_import_asset` | `{sourcePath:string(絶対パス可), destPath:string(assets相対), overwrite?:bool}` | `{imported:[相対パス...], count}` ※外部ファイル/フォルダを assets へコピー。.gltf はフォルダごと |
| `dx12_move_asset` | `{from, to, overwrite?:bool}` | `{from, to, note}` ※assets 内の移動/リネーム。**シーン内の参照パスは自動更新されない** |
| `dx12_delete_asset` | `{path, recursive?:bool}` | `{deleted, removedCount, wasDirectory}` ※ディレクトリは recursive:true 必須。参照中アセットを消すと壊れる |
| `dx12_terrain_generate` | `{entity?/name?, preset?:"hills"\|"canyon"\|"mountains", seed?:int, frequency?, octaves?, amplitude?, ridged?, baseHeight?, edgeFalloff?, valleyDepth?}` | `{entityId, preset, params:{...}, minHeight, maxHeight, resolution, worldSize}` ※高さ配列を丸ごと作り直す(既存の彫りは消える)。**同じ seed/params なら毎回同じ地形=冪等**。★Editor 限定 |
| `dx12_terrain_sculpt` | `{entity?/name?, brush?:"raise"\|"lower"\|"smooth"\|"flatten"\|"noise", point?:[x,z] \| points?:[[x,z]...](最大512) \| worldPos?:[x,y,z], radius?=12, strength?=5, falloff?=0.5, flattenHeight?, mirrorX?, mirrorZ?, noiseFrequency?, noiseOctaves?, noiseRidged?, seed?}` | `{entityId, brush, points, radius, strength, changed, minHeight, maxHeight}` ※座標は**ワールド XZ**。相対操作(2回撃つと2回ぶん)。`flatten`+`flattenHeight` は絶対値なので冪等寄り。★Editor 限定 |
| `dx12_terrain_erode` | `{entity?/name?, iterations?:int=16, talusDeg?:f=34, region?:[minX,minZ,maxX,maxZ]}` | `{entityId, iterations, talusDeg, changed, minHeight, maxHeight}` ※熱浸食。相対操作。★Editor 限定 |
| `dx12_sculpt_brush` | `{entity?/name?, brush?:"draw"\|"pull"\|"push"\|"smooth"\|"flatten"\|"pinch"\|"noise"\|"grab", position?:[x,y,z](ワールド) \| localPosition?, radius?=0.5, strength?=0.2, falloff?=0.5, direction?, grabDelta?, symmetryX/Y/Z?, noise*?, seed?}` | `{entityId, brush, movedVertices, localCenter, radius, strength, vertexCount, triangleCount, localBounds}` ※radius/strength は**メッシュのローカル単位**(Transform の scale が掛かる前)。相対操作。★Editor 限定 |
| `dx12_set_sun` | `{timeOfDay?:0..24, azimuth?:deg, elevation?:deg, color?:[r,g,b], kelvin?:1000..40000, intensity?, ambient?}` | `{entityId, name, direction, azimuthDeg, elevationDeg, color, intensity, ambient, timeOfDay}` ※最初の DirectionalLight を**絶対指定**で更新(冪等)。方位/高度は「太陽が見える方向」(+Z=0°, +X=90° / 高度 0=地平線) |
| `dx12_apply_lighting_preset` | `{preset:"day"\|"dusk"\|"night"\|"indoor"\|"horror"\|"studio"}` | `{preset, label, tip, sun:{...}\|null, post:{exposure/bloom/vignette/saturation...}}` ※**エディタの「ライティング」窓と同じ表・同じ式**(`src/editor/LightingPresets.h` に 1 本化)。太陽が無ければポストのみ適用 |

### 4-3. 生成・削除・モード遷移(遅延同期 — 本物の値が返る)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_create_entity` | `{type:"box"\|"sphere"\|"plane"\|"empty"\|"camera"\|"light_directional"\|"light_point"\|"light_spot"\|"particle_emitter"\|"trigger"\|"ui_canvas"\|"ui_image"\|"ui_text"\|"ui_button"\|"ui_slider"\|"ui_toggle"\|"ui_scrollview", name?, position?:[x,y,z], parent?:int, parentName?:string, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` ※light_*/camera/particle_emitter/trigger は既定値で生成(dx12_set_component で調整)。ui_* はエディタと同じ部品構成で生成され `entityIds`(自動Canvas/ラベル子含む全id)も返る。parent/parentName は ui_*(ui_canvas 以外)の親指定 |
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
| `dx12_preview_model` | `{path:string(.gltf/.glb/.fbx/.obj)}` | 画像コンテンツ(PNG) ※一時 spawn→撮影→削除。シーンは変更されない |

**`dx12_batch` 実装**: 各 op を順に await。`stopOnError=true` なら最初の失敗以降を skip 記録。往復削減用。
**`dx12_focus_and_screenshot` 実装**: `focus_camera` → (1フレーム描画) → `screenshot` → 画像読み込み → 画像コンテンツ返却。
**`dx12_scatter` 実装**: seed 付き乱数(mulberry32)で位置を決め、`create_entity`/`spawn_model`/`spawn_prefab` を1体ずつ実行(+必要なら `set_transform`/`snap_to_ground`)。同じ seed なら同じ配置になる(リトライで再現)。失敗3件で打ち切り。
**`dx12_screenshot_from` 実装**: `set_editor_camera` → (1フレーム描画) → `screenshot`。
**`dx12_preview_model` 実装**: `spawn_model`(y=-10000 の遠方) → `focus_camera` → `screenshot` → `delete_entity`。失敗時も一時エンティティは削除する。

### 4-5. 精密ピック / 地形 / スカルプトの約束事

**2 種類のレイキャストを取り違えないこと。**

| | `dx12_raycast` | `dx12_raycast_precise` / `dx12_pick` |
|---|---|---|
| 何に当たるか | Jolt の**物理コライダー** | **描画メッシュの三角形** |
| いつ使えるか | **Playing 中のみ**(body は Play 開始時に登録される) | Editor / Playing どちらでも |
| 精度 | コライダー形状(箱/カプセル/凸包の近似) | 実際の三角形。法線も面の法線 |
| 典型用途 | ゲームロジックの当たり確認・接地判定の再現 | 「スクショのここに何がある？」「地面の実際の高さは？」配置の自動化 |
| 制限 | 法線は up 向きの近似 | スキンドメッシュはバインドポーズの AABB 止まり |

`dx12_pick` はエディタの左クリック選択と**同じ `RaycastScene` 実装**を通る（`src/editor/ScenePick.h`）。
MCP で見えるものとエディタで選ばれるものが食い違わないのが、この 2 つを共有している理由。

> ブロードフェーズは**直近に描かれたフレームの描画リスト**を借りる（10 万体でも速いのはこのため）。
> `dx12_set_transform` で動かした直後に撃つと 1 フレームぶん古い位置で判定されることがある。
> 移動 → ピックを続けてやるときは間に `dx12_step_frames(frames:1)` を挟むこと。

**地形（ハイトフィールド）**

- 座標は常に**ワールド XZ**（`point:[x,z]`）。`dx12_pick` の `worldPos:[x,y,z]` をそのまま渡してもよい（y は無視）。
- 高さ配列はシーン JSON に入らない。`assets/terrain/<name>.hf` へ**自動保存**される（彫った次のフレームで書き出す）。
- コリジョンは Jolt の `HeightFieldShape` が**同じ高さ配列を読む**＝彫れば当たり判定も一緒に動く。
- 回転・スケールは効かない（XZ グリッドの前提）。位置だけが意味を持つ。
- 手順は「① `dx12_terrain_create` → ② `dx12_terrain_generate`（土台）→ ③ `dx12_terrain_sculpt`/`dx12_terrain_erode`（詰め）」。
  ②は高さを丸ごと作り直すので、**必ず③より先**にやること。

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
- **レスポンス(失敗)**: `{"id":<同id>, "ok":false, "error":<string>, "error_code":<int>,
  "error_hint":<string 任意>, "error_values":<string[] 任意>}\n`

遅延同期 method は受信時は何も返さず、フレーム境界で実処理後に同じ id でレスポンスを送る。

---

関連: `tools/mcp-server/AGENTS.md`(AI エージェント運用ガイド)、`tools/mcp-server/README.md`(サーバ構成)、
`docs/AUTHORING.md`、`docs/SCRIPT_COMPONENTS.md`(Lua)。
