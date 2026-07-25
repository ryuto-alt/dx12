# 作業ログ / 引き継ぎメモ

複数マシン（自宅 / 学校）で続きをやるための、直近の実装内容と再開手順のメモ。
詳細な設計は各 `docs/*.md`、MCP は [docs/MCP.md](MCP.md)、ビルドは [README.md](../README.md) を見る。

---

## 別マシンで再開するとき（学校 PC など）

```bash
git clone https://github.com/ryuto-alt/dx12.git
cd dx12
git checkout master                 # 現在の作業ブランチ（feat/engine-mcp から移行済み）
```

1. **依存** … vcpkg（`VCPKG_ROOT`）/ VS2026 + Windows SDK / Node v24+（MCP 用）。
2. **暗号鍵** … 初回だけ `tools/gen_asset_key.ps1` で `src/core/generated/AssetKey.h` を生成（gitignore 対象なのでマシンごとに作る。editor/runtime が同一ツリーなら鍵は一致）。
3. **ビルド** … VS2026 の vcvars を読んでから Ninja。`build/release` は Ninja 構成なので vcvars 無しだと `cstdint` 等が見つからず失敗する。
   ```bat
   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
   cmake --build "build\release"
   ```
   （`build` 自体が無ければ README の configure を先に実行）
4. **MCP を入れる**（AI からエディタを操作する場合）… 手順の正本は [docs/MCP.md](MCP.md)。最短は:
   ```bash
   cd tools/mcp-server && ./install.ps1      # npm install + 自己テスト + 登録コマンド表示
   claude mcp add dx12-engine -- node <REPO>/tools/mcp-server/index.ts
   ```
   `.mcp.json` はトークンが入りうるため gitignore。`tools/mcp-server/.mcp.json.example` をコピーして `<REPO>` を置換する。

---

## 実装履歴

### 2026-07-25 — 精密ピッキング / ギズモ改修 / 地形・スカルプト / ライティング編集 & Lua API / 診断拡張
ブランチ `master`。**この時点では未コミット**（`git status` で上記一式が M / ?? のまま）。
`v1.7.0`（`30edee0`）の次に載る変更。

- **三角形精密ピッキング + 重なりの循環選択**（`src/editor/ScenePick.{h,cpp}`, `RayGeometry.h`,
  `src/editor/panels/SceneViewPanel.cpp`）
  ブロードフェーズに Application の描画リスト（`DrawItem`）を借りてレイ vs バウンディング球 →
  ナローフェーズでサブメッシュ単位の三角形判定。`ComputeWorldMatrix` の回し直しがゼロになった。
  ヒットは距離昇順で返り、**同じ場所を連続クリック（4px / 1.2 秒以内）で手前→奥へ循環選択**。
  `MeshRenderer` 無しの Light/Camera/Empty はアイコンのスクリーン半径 18px で判定する。
- **ギズモ改修**（`SceneViewPanel.cpp`, `EditorContext.h`, `InspectorPanel.cpp`）
  マルチ選択の仮想ピボット（選択群の中心）＋「前フレームとの差分行列」方式にしたので、
  **移動だけでなく回転/スケールも群全体に効く**。スナップ量（移動/回転/スケール・常にスナップ）を
  ハードコードから `EditorContext` へ外出しし、**エンジン設定 > ギズモ**で編集可能に。
  ドラッグ中の増分を数値でオーバーレイ表示、ホバー中の軸を太く/明るく強調。
- **ビューポートツールのフック**（`EditorContext::viewportToolHandlers`, `SceneViewPanel::RunViewportTools`）
  UI 編集・3D ピッキングより前に走る `bool(const ViewportInput&)` の配列。`true` を返した時点で打ち切り。
  地形ブラシ / スカルプトブラシ / ライトハンドルがここに登録している（後から生えるツールの共通受け口）。
- **ハイトフィールド地形**（`src/terrain/HeightField|TerrainBrush|TerrainMeshBuilder|TerrainIO`,
  `src/editor/panels/TerrainPanel.cpp`, `src/scene/Scene.cpp` の `SpawnTerrain`/`RebuildTerrainMesh`）
  ブラシ 6 種 + fBm 一発生成（丘/峡谷/山脈）+ 熱浸食。高さ配列は `assets/terrain/*.hf`（`"DXHF"`）へ
  ストローク終了時に自動保存。Jolt `HeightFieldShape` コライダー。Undo はタイル差分（64x64）。
- **メッシュ・スカルプト（異形）**（`src/terrain/SculptMesh.{h,cpp}`, `SculptIO.{h,cpp}`,
  `src/editor/panels/SculptPanel.cpp`）
  頂点位置を直接動かす方式（トポロジ不変＝ `Mesh::UploadVertexCache` の部分更新が使える）。
  ブラシ 8 種 + X/Y/Z ミラー。**位置が一致する頂点を溶接**して代表単位で動かすので、
  GLB の UV/法線シームで割れた頂点でも継ぎ目が開かない。素体 4 種（箱/球/板/円柱）または
  「選択中のモデルを編集可能にする」（元の `.glb` は読むだけ）。頂点配列は `assets/sculpt/*.smsh`
  （`"SMSH"`）へ自動保存。Jolt `MeshShape` コライダー（動く剛体のときだけ凸包へフォールバック）。
  Undo は「触った代表頂点の差分」だけを持つ。
- **ライティング編集 UI**（`src/editor/LightHandles.{h,cpp}`, `LightMath.h`,
  `src/editor/panels/LightingPanel.cpp`）
  **`L` 押しっぱなし + マウス移動で太陽を回す**（UE の `Ctrl+L` 相当。`Ctrl+L` は新規スクリプトで
  埋まっているため単独 `L`）。スポットのコーン角 / ポイントの range / 平行光の向きを丸ハンドルで
  ドラッグ。パネルはライト一覧（灯数の上限警告・目玉でミュート）/ 太陽 / 影 / スカイ・IBL /
  プリセット 6 種 / 3点ライト設置。純関数は `LightMath.h` に切り出して `tests/light_math_test.cpp` で検証。
- **ライティング Lua API**（`src/scripting/ScriptEngine.cpp` + prelude）
  `Light` usertype / `post` / `ssao` / `Tween` / `Flicker`（Quake の lightstyle 文字列）/ `Lighting.*`
  （`setTimeOfDay` / `tweenTimeOfDay` / `lightningFlash` / `fadeToBlack` / `pulse` …）。
  サンプルは `assets/components/LightShowDemo.lua`。時刻カーブはライティング・パネルと同式。
- **エンジン診断の拡張**（`src/gui/DeepDiagnostics.{h,cpp}`）
  検査に `lighting` / `terrain` / `picking` / `instancing` / `scripts` を追加して計 10 種に。
  `DeepDiag::RunAll(app, only)` が**機械可読な JSON**（`version`/`engine`/`checks[]`/`summary`）を返すようになり、
  `only` にカンマ区切りの検査 ID を渡して重い検査（textures/models）を外せる。
- **CPU 内訳プロファイラにスコープ追加**（`src/core/CpuScope.h`, `Application.cpp`）
  `picking` / `gizmo` を追加。`dx12_perf_stats` の `cpuScopeMs` に出る（どちらも `editorUi` の内数）。
- **単体テスト**（`tests/CMakeLists.txt` に追加済み）: `ray_pick_test.cpp`（レイ vs 三角形/AABB）、
  `terrain_test.cpp`（ハイトフィールド / ブラシ / `.hf` ラウンドトリップ）、
  `sculpt_test.cpp`（溶接 / ブラシ / `.smsh` ラウンドトリップ）、`light_math_test.cpp`。
  純ロジックは GPU / entt / vfs / ImGui に依存させていないので、`.cpp` を直接ビルドしてテストできる。
- **ドキュメント追従**: `CLAUDE.md`（操作方法 / エディタ実装メモ / 超詳細診断）、
  `docs/AUTHORING.md`（10.5 補足 + **10.6 スカルプト新設** + 「山を作って→彫って→当たり判定を確認」手順）、
  `docs/SCRIPTING.md`（エディタ → Lua の導線）、`docs/index.html`（エディタ節に地形/スカルプト/
  ライティング編集、オーサリング節に `terrain`/`sculpt` の JSON ブロック）。

### 2026-06-29 — テンプレ 2D 追加 / JSON→VSCode / シーンアイコン刷新 / キー固着修正
ブランチ `feat/engine-mcp`。コミット `b00643b`（コード）, `3c99ba2`（アイコン）。

- **2D テンプレート追加**（`src/project/Project.cpp`, `ProjectManager.cpp` ほか）
  ランチャーの新規作成テンプレに「2D (横スクロール)」を追加（FPS / TPS / **2D** / 空）。
  中身は正射投影カメラ（`camera.projection=1, orthoSize=6`）＋地面＋足場＋プレイヤー＋コイン。
  `game.lua` は A/D・←→で横移動、Space で重力ジャンプ。アクションは XY 平面・Z 固定。
  アイコンは `tmpl_2d`（エメラルド地に白の太字「2D」、128×128）。生成器は `tools/gen_icons.ps1`。
- **.json ダブルクリック → VS Code**（`src/editor/panels/AssetBrowserPanel.cpp`）
  Lua と同様に VS Code で開くよう変更。シーンの読み込みは右クリック →「シーンを読み込み」に残してある。
- **シーン（.json）アイコン作り直し**（同ファイル `DrawAssetGlyph` case 3）
  分かりづらかったレイヤースタック → カチンコ（クラップボード）に変更。
- **フォーカス喪失でのキー押しっぱなし修正**（`src/input/InputSystem.*`, `src/core/Window.cpp`）
  他ウィンドウ/タブをクリックすると以降の `WM_KEYUP` が届かず、最後のキーが押下のまま残って動けなくなる不具合。
  `WM_KILLFOCUS` で `InputSystem::OnFocusLost()` を呼び、全キー状態＋マウス差分をクリアして解消。

## 2026-07-26 アニメーション基盤の刷新（計画05 Step 1〜6）

- **クロスフェードが骨を縮めていたバグ（B1）を修正**（src/animation/Animator.cpp）
  最終スキニング行列を要素ごとに XMVectorLerp していた。行列の線形補間は回転を保存しない。
  実測: 90 度離れた 2 姿勢を t=0.5 で混ぜると骨長 1.000 → **0.707**（29.3% 収縮）。
  中間表現 AnimPose（ローカル TRS 配列）を挟み、位置=lerp / 回転=slerp / スケール=lerp に。
  	ests/anim_pose_test.cpp が旧実装も一緒に計算して差を固定している。
  単一クリップ再生の結果は旧実装と**ビット一致**（後方互換の機械的保証）。
- 同時に B2（CrossFadeTo(clip, 0) の 0 除算 → NaN 固着）/ B3（先クリップのラップに
  現クリップの loop フラグを流用）/ B5（duration<=0 のポーズクリップが無視される）を修正。
- **.animfsm（アニメーションステートマシン）** を新設。ステート/遷移/条件/1D ブレンドツリー/
  レイヤー/ボーンマスク/クリップイベントを JSON で組む。Lua はパラメータだけ触る。
  → docs/ANIMATION.md
- **フット IK**（2 ボーン解析 IK + 地面レイキャスト + 腰下げ + 面法線）。
  前提として PhysicsSystem::RaycastEx（真の面法線を返す版）を追加した。
- Lua API を 14 本追加。setAnimSpeed が辞書 5 箇所中 2 箇所にしか無かったドリフトも解消
  （dx12e.lua / API_REFERENCE.md / index.html / SCRIPTING.md / McpLuaApi）。

### 既知の残課題（このタスクでは触っていない）

- **PhysicsSystem::Raycast（法線が {0,1,0} 固定のフェイク）はそのまま**。
  Lua の physics:raycast の挙動を変えると既存ゲームに影響しうるため、
  法線が要る用途は RaycastEx を使う形にした。いつか統一するなら別タスクで。
- **glTF のインポート向きが Y-up モデルに対して過剰回転する**（Fox.glb が縦に立つ）。
  スケルタルの計算とは無関係で、ModelLoader 側の座標変換の問題。別タスク。

---

## 次にやる候補 / TODO

- **2026-07-25 の一式はまだビルド検証していない**。`build/release` でビルドして PerfTest プロジェクトで
  実機確認すること（地形/スカルプトを彫る → Play → 当たり判定、ライトハンドル、循環選択）。
- 地形 / スカルプトを MCP から触るツールは未整備（`DeepDiag::RunAll` の JSON 化は済み）。
- スカルプトのレイキャストは三角形総当たり（`SculptPanel.cpp` の `RaycastSculptLocal`）。
  十万三角形級で重くなったら BVH を積む。今は「重い」と言われるまで入れない方針。
- FPS/TPS/empty の既存アイコンは据え置き。全部の作り直し要望が来たら `tools/gen_icons.ps1` で対応。
- `WM_SYSKEYDOWN`/`WM_SYSKEYUP`（Alt 系）は未配線。Alt 入力を拾いたくなったら `Window.cpp` に追加。
