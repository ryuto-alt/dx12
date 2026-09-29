# シーケンサー / タイムライン設計書（Uno Engine：カメラワーク・アニメ・演出を 1 本の時間軸で編集する）

- 対象: 新規 `src/sequencer/`（評価コア）、`src/editor/panels/SequencerPanel.*`（UI）、`src/core/Application*.cpp`（駆動・適用・出力）、`src/core/mcp/*` と `tools/mcp-server`（MCP）
- 作成: 2026-09-30 / 状態: **調査と設計のみ**（エンジンのソース変更・ビルド・エディタ起動・git 操作は一切していない。読んだのはソース・docs・テストだけ）
- 関連: `docs/MCP_ENHANCEMENT_DESIGN.md`（MCP の面・エラー封筒・`apply_scene_spec` の作法を踏襲）/ `docs/ANIMATION.md` / `docs/EFFECT_RECIPES.md` / `docs/MCP.md`
- 表記: `path:line` は 2026-09-30 の作業ツリーで確認した値（パスは `C:\Users\ryuto\Documents\dx12` 起点）。「目標」は設計上の数値で**未実測**。「未確認」は読み切れていない項目で、憶測で埋めていない。
- 用語: 「バインディング」= シーケンス内のトラック群が指す対象（主にエンティティ）への参照。「PreAnimatedState」= シーケンサーが書き換える前の値の退避（UE の同名の仕組みに倣う）。

---

## 0. 要約（1 画面）

**現状（実測）**: 「時間軸で演出をまとめて編集する」機能は**エンジン内に存在しない**。あるのは (1) MCP の TS サーバが台本 JSON から **Lua コンポーネントを生成する** `dx12_sequence_author`（16 種のトラック。`tools/mcp-server/sequence.ts:22-40`）と、それを Play して連写する `dx12_sequence_preview`、(2) カメラを撮影用に動かすだけの `dx12_camera_path`（データは保存されない）、(3) **UI 専用**のタイムライン `AnimationEditorPanel`（`.uianim`。スカラーのキーフレーム＋イージング番号だけ。`src/animation/UiAnimAsset.h`）、(4) 時間依存でバラバラな部品（Animator は `SetClipTime` があるが誰も呼ばない / パーティクルは**シーク不能・乱数が再シードされない** / Lua の `Tween` は状態を持ち時刻指定できない）。イージングの語彙は **3 系統が別物**（Lua `Ease` 9 種・C++ `UiEase` 12 種・`sequence.ts` の 6 種）で、ベジェ／タンジェント付きカーブは**どこにも無い**。

**でも土台は揃っている**（設計の勝ち筋）:
- 純評価コアの雛形が既にある: `UiAnimAsset.h`（GPU/ECS/ImGui 非依存・二分探索・イベント境界規則・`tests/ui_anim_test.cpp`）。同じ流儀で `src/sequencer/` を作れる。
- **エンティティの安定 ID `EntityGuid` がある**（`src/ecs/Components.h:36-53`。保存時に付与・親子とエンティティ参照プロパティが既に guid 参照）。バインディングはこれに乗せる。
- **`entt::meta` によるフィールド反射が約 40 コンポーネントに登録済み**（`src/ecs/ComponentMeta.cpp`）。ポストは X マクロ表 `DX12E_POST_FIELDS`（`src/renderer/PostProcessSettings.h:164-200`）。→「任意コンポーネントの float/色/bool」をトラックにする口がタダで手に入る。
- ポーズ注入 `Animator::SetPoseOverride` と `SetClipTime`（`src/animation/Animator.h:47,65`）、エディタ中も `Scene::Update(0)` が毎フレーム回る（`src/core/Application.cpp:2526-2528`）→ アニメのスクラブが成立する。
- **カメラ視点の別描画が 2 つ前例としてある**: 選択カメラの小窓プレビュー（`RenderView(ViewDesc)`。`src/core/ApplicationRender.cpp:6394-6479`）と、Editor 中だけゲームカメラで 1 枚撮る `gvOverride`（`src/core/Application.cpp:1721-1748`）。
- 決定論の部品: 固定 dt `GameClock::SetFixedDelta`（`src/core/GameClock.h:26-38`）、決定論スクショ（時刻固定・TAA ジッタ固定・履歴破棄。`ApplicationRender.cpp:3814-3822`）、`InvalidateTemporalHistory`（`ApplicationPipeline.cpp:523-539`）。
- UI 基盤: ImGui 1.92.6 の ImDrawList 自前描画でズーム/パン/スクラブ/キードラッグを済ませた実装（`AnimationEditorPanel.cpp:606-841`）、`theme::`／`ui::Px`、コマンド表、`ToolWindows.h`、UI 自動テスト。

**推奨案 = 「純評価コア + 1 本の編集操作層 + 非破壊の適用層」**（§2〜§7）
1. **`.dxseq`（JSON）**: 時刻は**整数ティック（6000/秒）**、トラックは「カーブ系（値の連続）」「クリップ系（区間を持つ）」「イベント系（点）」の 3 族、ID は全要素に**短い安定 ID**（git の行単位マージで壊れない。`EntityGuid` と同じ理由）。バインディングは **guid → 階層パス → 名前** の順で解決し、シーン側の `SequencePlayer` コンポーネントで上書き可能。
2. **評価は純関数** `Evaluate(seq, t) → 値の列`。エンジンへの書き込みは別層（適用層）。エディタのスクラブは PreAnimatedState に元値を退避して**閉じる／Play／保存の前に必ず戻す**。**ポストと DoF は「描画時の一時コピーに上書き」**（既存の `fx:pulse` と同じ作法。`ApplicationRender.cpp:5675-5689`）でシーンを汚さない。Play 中のランタイム再生も**同じコア**を別の時計で回す。
3. **すべての編集は `SeqOp`（逆操作つき）を通る**。UI・MCP・Lua が同じ経路 → Undo が 1 か所で済み、AI 操作は既存の `AiUndoEntry` に自然に乗る。
4. **UI は下部ドックのタブ**（要：ドックの Bottom スロット追加）。ドープシート＋カーブ＋カメラカット帯＋ミニプレビュー（既存のカメラプレビュー RT を再利用）。色はテーマトークンだけを引く（ネオンは別エージェントの案にトークン経由で追従）。
5. **出力は「PNG 連番 → ffmpeg」**。時刻は `n/fps` の整数ティック、TAA 履歴は**開始時に 1 回だけ**捨てて以後は連続（毎フレーム捨てる既存の決定論スクショは流用しない）。音は録らず**トラックデータから ffmpeg で合成**。
6. **MCP**: 新エンジン method 群 `sequence_*`（マニフェスト経由で再起動なしに `dx12_call` で使える）＋ Core 1 本 `dx12_sequence {op}`。AI は宣言的な**仕様 JSON（名前・秒で書く）**を投げ、エンジンが `.dxseq` に落とす。既存 `sequence_author` / `sequence_preview` / `camera_path` は**名前も挙動も残す**（移行は §7）。

**段階（§8。合計 約 48 実働日 + 任意の S7。3〜4 エージェント並列でクリティカルパス約 27 日 ＝ 見積もり誤差 ±40%）**: S0 評価コア(4) → S1 UI 基盤＋バインディング＋適用（Transform/Camera のみ）(8) → **S2 カメラ（カット/FOV/DoF/注視/シェイク/ミニプレビュー）＋決定論レンダー出力(10)** ＝ここで「見て嬉しい」→ S3 カーブエディタ(5) → S4 アニメ/オーディオ/イベント/プロパティ(8) → S5 VFX/ライト/ポスト(6) → S6 MCP・移行・レシピ・Lua(7)。

**撤退条件の要点（§9）**: PreAnimatedState を全保存経路で安全にできないなら「カメラとポストだけ非破壊オーバーライド、Transform は明示の“ライブ編集”トグル」に縮める。決定論が GPU 由来で崩れるなら「静止画は決定論・動画は best-effort」と割り切って出す。

**ユーザーに聞くこと（§10・5 点）**: ①パネルの置き場所（下部ドック新設 vs 浮遊窓）②時刻の保存単位（整数ティック 6000/秒）③ランタイム再生の既定の時計（実時間 vs スケール適用）④既存 `sequence_author` の移行方針（Lua 生成を残して opt-in）⑤レンダー出力の初期スコープ（ビューポート解像度の PNG 連番＋mp4 から）。

---

## 1. 現行の事実（実コード確認）

### 1.1 MCP の演出ツール（TS サーバ側で完結している）
- **`dx12_sequence_author`**（`tools/mcp-server/toolset/sequence.ts:24-157`）: 台本 JSON を検査（`validateSpec`）→ **Lua ソースを生成**（`generateLua`）→ エンジンの `create_lua_component` で `components/<name>.lua` を書き、`SEQ_<name>` という空エンティティ（無ければ作る。同 134-141）に `attach_lua_component`。エンジン側にシーケンスの概念は無い。
  - 台本の型: `SequenceSpec{name, camera?, tracks[], loop?, doneEvent?}`（`sequence.ts:42-53`）。`Track` は **16 種**の判別共用体（`sequence.ts:22-40`）: `camera / fade / post / timeScale / shake / vfx / vfxPlay / vfxStop / shaderParam / sound / move / rotate / light / event / scene / log`。各要素は `t`（秒・絶対時刻）＋`dur`＋`ease`＋型ごとの引数。**対象はエンティティ名の文字列**（`target` / `atName` / `lookAtName`。同 632-637）。
  - **イージング 6 種**: `linear / in / out / inOut / outBack / outBounce`（`sequence.ts:19,266-279`。`in`=k²、`out`=1-(1-k)² で二次）。
  - **時計は `time.realDt()`（タイムスケール非適用）**（`sequence.ts:9-11,573-577`）。スローモを掛けても台本が実時間で流れるのが設計意図。`timeScale` トラックは終了時に 1.0 へ戻す（同 566-572）。
  - **開始値の扱い**: `camera` / `move` / `rotate` / `shaderParam` の `from` 省略時は「再生開始時の現在値を 1 回捕まえる」（`grab`。`sequence.ts:528-533`）。→ 移行では**エディタ時の値を焼く**必要がある（§7）。
  - **生成される Lua の外部インターフェース**: `events:emit("<name>:play")` / `("<name>:stop")` で操作、終了時 `("<name>:done")` を発火（`sequence.ts:553-573`）。**新形式でも同名イベントを維持**する（互換）。
  - 揺れ（`shake`）は `sin` 合成の純関数だが、位置に足して次フレームで引き戻す**書き戻し方式**（`sequence.ts:578-619`）。
  - `camera` トラックの位置は**線形補間のみ**（ease だけで形を付ける）。注視は `lookAt`/`lookAtName` で毎フレーム `aimAt`（同 516-526）。
- **`dx12_sequence_preview`**（`toolset/sequence.ts:160-254`）: `play` → `step_frames{deterministic:true, hold:true}` を挟みながら `screenshot_final` を最大 12 枚 → コンタクトシート。`set_editor_camera{release:true}` を先に撃つ（固定が残ると絵が動かない事故対策。同 189-192）。
- **`dx12_camera_path`**（`toolset/quality.ts:150-…`）: `mode:"line"|"orbit"` の姿勢列 `planCameraPath` を作り、`set_editor_camera` → 撮影を繰り返すだけ。**保存物は無い**（撮影用の一時操作。`catalog.ts:112-114` で `write_setting` 扱い）。
- **カメラ乗っ取り**: `m_mcpCameraOverride`（`Application.cpp:2513`、`core/mcp/ApplicationMcpAsset.cpp:44-115`）。Play/Stop の遷移で必ず解除（`ApplicationScene.cpp:1492,1794`）。
- **決定論ステップ**: `step_frames{deterministic,dt,hold}`（`core/mcp/ApplicationMcpEditor.cpp:735-775`）は `GameClock::SetFixedDelta`（既定 1/60、上限 `kMaxDeltaTime`=0.1）を掛け、完了後に**必ず戻し**、`hold` なら `EditorContext::paused=true` で往復の間を凍らせる（`Application.cpp:1909-1937`）。
- **決定論スクショ**: `screenshot`/`screenshot_final{deterministic,settleFrames}`（`ApplicationMcpEditor.cpp:189-290`）は `InvalidateTemporalHistory()` → `settleFrames`（既定 8、1〜240）回して撮る。描画側で `totalTime = kDeterministicTime(8.0)` に固定し TAA/フォグ/SSGI の位相を 0 へ戻す（`ApplicationRender.cpp:3814-3822`、`Application.h:1442`）。**時間依存の見た目（グレイン/ディザ/ウェーブ）を止める**仕組みで、動画の連続フレームには**そのままでは使えない**（毎枚 TAA 履歴を捨てるため）。
- **決定論ではない部分**: ゲームのシミュレーション（Lua/物理/パーティクル）は `step_frames` の固定 dt でしか再現しない。`m_deterministicCapture` は**パーティクルを dt=0 で止めるだけ**（`Application.cpp:2643`、`ApplicationRender.cpp:5454`）。

### 1.2 アニメーション
- **「Animator コンポーネント」という ECS 型は無い**。実体は `SkeletalAnimation`（`unique_ptr<Animator>`・クリップ配列。`ecs/Components.h:311-322`。`ComponentMeta` 未登録＝シーン JSON に保存されない）と `AnimatorController`（`.animfsm` パス等。`Components.h:335-362`）。
- `Animator`（`src/animation/Animator.h:25-107`）: 時刻 `m_currentTime`（秒）。**`SetClipTime(t)`/`GetClipTime()` が既にあるが、どこからも呼ばれていない**（調査時に `src` 全体を grep して確認）。`SetPoseOverride(pose)` は次の `Update` の 1 回だけサンプリングを飛ばす（`Animator.cpp:185-189`）。`Update(dt=0)` でも現在時刻で再評価される（`Animator.cpp:212-257`）→ **`SetClipTime(t)`＋`Update(0)` でスクラブが成立**。
- 更新順（`Scene.cpp:604-651`）: `UpdateAnimGraphs(dt)`（`AnimatorController` があれば `SetPoseOverride` で毎フレーム上書き。同 610）→ 各 `Animator::Update` → `NodeAnimator`。呼び出しは `m_scene->Update(simRunning ? dt : 0.0f)`（`Application.cpp:2526-2528`）。**エディタ中は dt=0 で毎フレーム呼ばれる**。
- `.animfsm`（FSM/ブレンドツリー/レイヤー/BoneMask、`AnimGraphAsset.h`、`AnimGraphRuntime.cpp:364-580`）の状態時刻は `AnimLayerRuntime::stateTime`（`AnimGraphRuntime.h:33-41`）。**「時刻 t で評価」する公開 API は無い**。イベントは `.animfsm` の `clipEvents` 由来で、単一クリップの `Animator` 経路にはイベント収集が無い。
- FootIK は **Play 中のみ**（`Application.cpp:3358-3466`）。`NodeAnimator` は時刻セッターが無く、クロスフェードが行列の要素ごと lerp（`NodeAnimator.cpp:199-211`）。
- **Lua `time.setScale` は Animator に掛からない**（`docs/API_REFERENCE.md:524`）。
- **`AnimationEditorPanel` は UI アニメ（`.uianim`）専用**で、スケルタルアニメ用ではない（`AnimationEditorPanel.cpp:287`）。ただし**部品は再利用価値が高い**: 座標変換 `timeToX/xToTime`（`:621-623`）、目盛りの自動間引き（`:639-650`）、カーソル位置固定ズーム 0.25〜40 倍（`:709-717`）、中ドラッグ横パン（`:719-720`）、スクラブ（`:731-757`）、キー選択/移動/追加/削除、レコード＝実値とクリップ評価値の差からキーを打つ `CaptureRecordedEdits`（`:209-232`）、スナップショット＆復元 `SnapshotTargets/RestoreTargets`（`:234-261`）。**弱点**: スナップショットの復元はパネルを閉じる／対象変更のときだけで、**保存・Play に対しては無防備**。パネル内 Undo は独自スタックで `ctx.undoSystem` に載らない（`AnimationEditorPanel.h:69-75`）。色は `IM_COL32` 直書き。
- `.uianim`（`UiAnimAsset.h:87-117,162-227`）: `UiAnimKey{time,value,easing(int)}`、評価は「左キーのイージングで補間」、`CollectUiAnimEvents` は (prev,cur] の**左開右閉＋開始フレームだけ閉じる**規則。`UiAnimRuntime::ApplyClipAt(reg, root, clip, time)`（`ui/UiAnimRuntime.h:53`）は**再生状態に触れず任意時刻を適用**する既存パターン。
- `.spranim`: コマ番号列（`SpriteSeqIndexAt` は時刻の純関数）。`SpriteSheetEditorPanel` と `TransitionPreviewPanel` は各自の時計（`ImGui::GetIO().DeltaTime`）で進む。TransitionPreviewPanel にはコマ送りスライダーがある（`TransitionPreviewPanel.cpp:333-337`）。
- **既存のキー/カーブ/イージング資産**: `Keyframe<T>`（`AnimationClip.h:11`。線形/slerp 固定）、`UiAnimKey`、`UiEase` 12 種（`ui/UiEase.h:16-95`）、`ShaderTweenEase` 4 種（`scene/ShaderParamTween.h:28-35`）、Lua `Ease` 9 種（`ScriptEngine.cpp:3569-3588`）。**ベジェ/Hermite/タンジェントは存在しない**。
- ※**リポジトリ内に `.animfsm/.uianim/.spranim` のサンプルは 1 つも無い**（`assets/` は components/editor/prefabs/sculpt/shaders のみ）。

### 1.3 VFX / パーティクル / オーディオ
- **パーティクルはシーク不能・非決定論**: `ParticleSystem::Update(dt)` は前進のみ（`dt<=0` は即 return。`ParticleSystem.cpp:695`）。乱数は `std::mt19937 m_rng{1337u}`（`ParticleSystem.h:330`）で、**`Clear()` は再シードしない**（`ParticleSystem.cpp:789-800`）。GPU 版も同様（`GpuParticleSystem.h:104`、`.cpp:334`）。→ 「同じ時刻に同じ絵」は今は再現できない。
- エミッタ駆動（`Application.cpp:2530-2616`）: `!paused` で放出、**エディタ中は `_active` を無視して常時放出**（`:2547`）。`ParticleLayer` に **delay/開始オフセットは無い**（`rate/playOnStart/looping/duration/_age` のみ。`Components.h:1376-1379,1418-1420`）。`fx:play/stop` は `_active=true; _age=0` にするだけ（`ScriptEngine.cpp:2035-2112`）。
- `VfxEditorPanel`（`VfxEditorPanel.cpp:511-775`）は 1 レイヤーのフラットな `VfxAsset`（`assets/vfx/*.json`）と、**別インスタンス**の `ParticleSystem` によるプレビュー（`:74-148`）。タイムライン要素は無い。
- **オーディオ**（`src/audio/AudioSystem.h`）: `Play(PlayParams{path,bus,volume,pitch,loop,spatial,pos,fadeIn,…})`（`:148-165,245`）、`StopVoice/FadeVoice/SetVoiceVolume`、ボイス位置は `GetVoices()[].positionSec/lengthSec`（`:104-105`）。**シークは BGM のみ `SeekBGM`**（`:189`）。任意ボイスの再開始 `RestartVoiceAt` は private（`AudioSystem.cpp:988-1014`）。PCM は `AudioClip::GetPCMData()`（`AudioClip.h:20-22`。全形式 16bit）だが `GetOrLoadClip` が private（`AudioSystem.h:344`）。**波形を描く既存コードは無い**。`.ogg` は `AudioStream`（ワーカーデコード）。`Tick` はモード無関係に回る（`Application.cpp:2735-2741`）＝**エディタ中も鳴らせる**。空間音の定位は Play のみ（`:2661`）。

### 1.4 Lua の演出 API（prelude 純 Lua）
- `Tween(target, prop, to, dur, opts)`（`ScriptEngine.cpp:3560-3665`）: **状態を持つ**（開始値をその場で捕まえ、`dt` で進む）。時刻を外から指定する API は無く、Play 開始で全消去（`:3084`）。`time.setScale` の影響下（スケール済み dt）。`Ease` は 9 種で **`UiEase` とは別語彙**（共通は `linear` のみ）。
- `Flicker`（lightstyle 文字列）、`Lighting.setTimeOfDay/tweenColor/tweenIntensity/lightningFlash/fadeToBlack`（`:3759-3917`。**時刻→太陽の計算は Lua 側**で C++ から呼べない）、`post.get/set/setMany/names`（`:2130-2195`。名前表は `DX12E_POST_FIELDS`）、`shader.get/set`（自由枠 `effect/p1..p4/b1..b3`。`:2210-2270`）、`events:on/emit`（Play 中のみ有効。`:2550-2627`）。
- `Trigger` の 13 種アクション（`Components.h:1521-1603`、`ScriptEngine.cpp:4640-4838`）は**空間イベント起点のワンショット**で、時刻起点・遅延・順序制御・複数プロパティの同期は無い。`AnimShaderParam` だけが時間補間を持つ。

### 1.5 シーン / ID / Undo / 時間管理
- **`EntityGuid{u64}`**（`Components.h:36-53`）: 保存時に未設定のエンティティへ自動付与（`SceneSerializer.cpp:927-946`）＝**生成時には付かない**。JSON は 16 桁 hex。親子は `parentGuid` 優先＋`parent` index 併記（`:956-968,2036-2074`）。`FindEntityByGuid` は**線形走査**（`Components.cpp:171-177`）。**複製・貼り付け・プレハブ新規配置では guid を捨てる**（`SceneSerializer.h:28-40,110-123`）。Undo/Redo と Prefab Apply は `keepGuid=true`。
- **名前は一意でない**。`Scene::FindEntity` は最初の一致（entt の走査順で実質「最後に作られたものが勝つ」。`Scene.cpp:653-664`、`Components.cpp:191-193`）。MCP の `name` 指定も曖昧さを検査しない（`ApplicationInternal.h:566-594`）。`list_entities` は guid を返さない（`ApplicationMcpEntity.cpp:25-52`）。
- **エンティティ参照プロパティ**は `valueGuid`＋名前（派生値）で保存、`ResolveEntityRef` は guid → 名前の順（`SceneSerializer.cpp:879-883`、`Components.cpp:179-187`）。→ バインディングの解決順の**前例**。
- **Play/Stop はシーン全体の JSON 往復**（`EnterPlayMode`: `SaveToString`、`ApplicationScene.cpp:1537`。Stop: `LoadFromString`、`:1919-1921`）。**Stop で entity id は全部変わる**（`++m_sceneGeneration`。`:1965-1968`。`ApplicationInternal.h:552-565`）。
- `EditorContext::paused`（Play 中の一時停止。`Application.cpp:2296-2308`）、`simRunning = Playing && !paused`。**時間ソースは 3 系統**: ゲーム dt（`GameClock`。0.1s クランプ、固定 dt 可）・Lua の dt（`dt*timeScale`。Animator には掛からない）・エディタの `ImGui DeltaTime`（`.uianim`/`.spranim`/VFX/トランジションの各プレビュー）。
- **Undo**（`editor/UndoCore.h`、`UndoSystem.h`）: `IUndoCommand{Undo,Redo,GetName,IsAi}`、`CompositeCommand`、`AiUndoEntry`、上限 100（`kMaxHistory`）。新しい差分は `IUndoCommand` を継承して `ctx.undoSystem.PushCommand(...)` するだけ（`UndoCore.h:101-111`）。任意状態の前例: `LightingPresetCommand`（`panels/LightingPanel.cpp:133-161`）。ドラッグは「開始スナップショット＋終了差分を 1 コマンドにする」流儀（`SceneViewPanel.cpp:360-386`、`InspectorPanel.cpp:744-812`）。MCP 変更は `McpUndoRouter` の `BeginCall/EndCall`（`SetCaptureSink`）が積まれたコマンドを **`AiUndoEntry("AI: <method>")` 1 個に束ねる**（`McpUndoRouter.h:51-72`）。Playing 中は積まない（`ApplicationMcp.cpp:196`）。`EnsureGuid`（`McpUndoTrack.cpp:23-32`）が「今すぐ guid を付ける」既存関数。

### 1.6 カメラ・ポスト・描画
- `CameraComponent`（`Components.h:645-667`）: `fovDegrees/nearClip/farClip/isActive/projection/orthoSize/screenShader*`。**DoF・絞り・フォーカス距離・露出はカメラに無く、シーン単位の `PostProcessSettings`**（`dofOn/dofFocusDist/dofAperture/dofFocalLength/dofFocusName/dofBlurSize`、`exposure`、`motionBlurOn/mbStrength`。`PostProcessSettings.h:25,129-145`）。`dofFocusName` は毎フレーム `FindEntity`（名前引き。`ApplicationRender.cpp:5772-5789`）。
- **Transform の回転は Euler 度（YXZ）**、`quaternion` は非保存の実行時専用（`Components.h:80-101`）。カメラ同期は `-rotation.x` を pitch に使う（`ApplicationScene.cpp:536-569`。「ギズモ（Euler）とカメラは符号が逆」とコメント）。ゲームカメラの同期は **Play 中のみ**（`Application.cpp:2508-2523`。エディタ中は同期しない）。「アクティブカメラ」を `isActive` で走査する箇所は **4 つ**: `Application.cpp:2549`（Play 同期）、`ApplicationScene.cpp:577`、`ApplicationRender.cpp:3960`（投影）、`:5712`（スクリーンシェーダー）。
- **ポストの一時上書きの前例**: 描画時に `PostProcessSettings ppApplied = m_scene->GetPostSettings()` の**コピー**を作り、`fx:pulse` の値を上書き（`ApplicationRender.cpp:5675-5689`）。シーンは無変更。
- カメラ切替で TAA/SSGI/フォグ/DDGI/モーションブラーの履歴を捨てる `InvalidateTemporalHistory()`（`ApplicationPipeline.cpp:523-539`）。モーションブラーは `m_prevViewProjValid` を要する（`ApplicationRender.cpp:5793`）。TAA のジッタは `NextJitterNdc`/`ResetJitter`（`TaaPass.h:87-88`）。
- **描画解像度**: 最終画のコピーは「エディタのシーンビュー矩形」（16:9 レターボックス）。任意解像度のオフスクリーン出力は**未確認**（`RenderView(ViewDesc)` は任意 RT へ描けるが、カメラプレビュー用は機能を削っている。`ApplicationRender.cpp:6435-6466`）。
- 3D 線の描画は `EditorIconRenderer::AddLine`（`editor/EditorIconRenderer.h:55`）。

### 1.7 エディタ UI 基盤とテスト
- **ImGui 1.92.6**（docking-experimental）。**ImPlot / ImSequencer / imgui-node-editor は無い**。ImGuizmo は `src/gui/ImGuizmo.*` に取り込み済み。→ タイムライン/カーブは**自前の ImDrawList**（`DrawTimeline` と `UiEditorPanel.cpp:786-900` が前例）。
- 窓の追加は `ToolWindows.h` の `kAll` に 1 行＋`EditorContext` に bool 1 個（`ToolWindows.h:15-16,27-49`）。**スロットは `RightTab / Floating` のみ＝下部ドックのスロットが無い**。下部は `BuildDefaultLayout`（`EditorLayer.cpp:131-168`。コンソール＋アセットブラウザをタブで入れる）に**ハードコード**。フォーカスパネル判定も窓名直書き（`EditorLayer.cpp:203-221`）。
- ショートカットの唯一の正は `EditorCommandTable.h`（`Def{id,label,labelEn,category,chord,chord2,help,Scope,KeyMode}`。`:41-56`）。パレットは表＋`tools::kAll` から自動構築（`CommandPalette.cpp:236-248`）。キー重複は `tests/editor_ux_test.cpp` が検出。
- テーマは `EditorTheme.h`（`Bg0〜Bg4`/`InputBg*`/`Border*`/`Accent*`/`Selection*`/`Text*`/`Good/Warn/Bad`/`Type*`/寸法 `size::*`）。**ネオン系トークンは現状存在しない**。DPI は `ui::Px()`（論理 px → 物理 px。`EditorTheme.h:122-136`）。
- UI 自動テストは `kTests[]` の `DiagReg{category,name,group,display,body,deep}`（`gui/UiTestHarness.cpp:2967-2977`）。`tools::kAll` の窓は自動で開閉検査される（`:2560-2590`）。単体テストは自前 `main()` と `CHECK`（フレームワーク無し）。登録は `tests/CMakeLists.txt` に 3〜5 行（`UiAnimTests`。`:167-174`）。`component_meta_test.cpp`/`serialize_roundtrip_test.cpp`/`mcp_undo_test.cpp` が純ロジックの前例。
- アセット: **共通のアセット登録機構は無く、拡張子ごとに個別実装**。AssetBrowser の分類（`AssetBrowserPanel.cpp:1097-1120`。`.json` は全部 Scene 扱い）・ダブルクリック（`:712-757`）・D&D（`:658-662`）を都度足す。**アセット GUID/.meta は無く、参照は assets 相対パス文字列**。名前変更・移動の追従は `RewriteAssetPathRefs` の拡張子リスト（`SceneSerializer.cpp:2797-2820`、対象 `.json .prefab .dxmat .animfsm .spranim .terrainlayers .uianim`）に足さないと効かない。`.` 始まりのフォルダは pak から除外（`ApplicationProject.cpp:1808-1830`）。

### 1.8 ドキュメントと実装の食い違い（この調査で見つけたもの）
- `docs/MCP.md:544` は `step_frames` を「決定論ステッパではない（各フレームの dt は実時間）」と書くが、実装は `deterministic/dt/hold` を持つ（`ApplicationMcpEditor.cpp:735-775`）。
- `docs/MCP.md:527` は `camera_path` の引数を `path/keyframes/shots` と書くが、実装は `mode/from/to/target/radius/…`（`toolset/quality.ts`）。
- `docs/AUTHORING.md:245-266` のエンティティ参照は名前のみの旧形式（実装は `valueGuid` 併記）。
→ 本機能の docs を書くときは実装を正とし、上記は S6 の docs 作業で直す。

### 1.9 事実から出る設計上の制約（以降の節が従う）
1. 生成時に guid が無い（保存時付与）→ バインディング作成時に `EnsureGuid` 相当を**その場で呼ぶ**。プレハブ展開・複製で guid は消えるので**階層パスのフォールバックが要る**。
2. Stop で entity id が全部変わる → 実行時の `entt::entity` を保持しない。`m_sceneGeneration` でキャッシュを無効化する。
3. パーティクルはシーク不能・非決定論 → **VFX は「イベント駆動＋（必要なら）決定論プリロール」**とし、スクラブの完全対応を約束しない。
4. Animator は `SetClipTime` があるが AnimGraph には時刻指定 API が無い → **AnimGraph 付きエンティティは「シーケンサー所有フラグ」でグラフを止めてポーズを注入する**。
5. ポスト/DoF はシーン単位 → **カメラごとの DoF は「カットで選ばれたカメラの値を描画時の上書き」**として表現する。
6. 「毎枚 TAA 履歴を捨てる」既存の決定論スクショは動画に不向き → **レンダー専用の決定論モード**が要る。
7. イージング語彙が 3 系統 → 新形式は**名前付き文字列**で式を固定し、既存 3 系統の式をすべて包含する。

---

## 2. データモデル

### 2.1 資産 `.dxseq`
- 置き場所: `assets/sequences/<name>.dxseq`（JSON、UTF-8、LF、末尾改行）。`.dxseq` を `RewriteAssetPathRefs` の対象拡張子・AssetBrowser の分類/ダブルクリック/D&D に追加する（§1.7 の 5 か所）。pak には自動で入る（`ApplicationProject.cpp:1808-1830`）。
- **時刻は整数ティック**（`ticksPerSecond` 既定 **6000**）。24/25/30/48/50/60/100/120 fps がすべて整数ティックに乗る（30fps=200、24fps=250、60fps=100）。23.976/29.97 は非対応（表示上は 24/30 として扱い、出力時に ffmpeg のレートで指定。§10-②）。float 秒で保存しないのは、フレーム境界の丸め誤差と git 差分のノイズを避けるため。**MCP の仕様 JSON は秒で書き、取り込み時にティックへ丸める**。
- **ID**: バインディング `b_xxxxxxxx`、トラック `t_…`、クリップ `k_…`、マーカー `m_…`、カット `c_…`（8 桁 hex、シーケンス内で一意）。**配列の並び＝UI の並び、識別は ID**。行の移動＝並び替えだけで済み、別ブランチで足された要素が衝突しても行単位でマージできる（`EntityGuid` を導入した理由と同じ。`Components.h:36-53`）。
- **シリアライザは自前で決定的**（キーは固定順、キー配列は「1 キー 1 行」のタプル）。`nlohmann` 既定の整形は数値配列を縦に展開して差分が読めなくなるため使わない。`Serialize(Parse(x)) == x`（バイト一致）を S0 のテストで保証する。

```jsonc
{
  "format": "dxseq", "version": 1,
  "id": "q_7f3a91c2", "name": "BossReveal",
  "ticksPerSecond": 6000, "frameRate": 30,
  "range": [0, 27000],                       // 再生範囲（ティック）。省略＝全キー/クリップの外接
  "render": { "width": 1920, "height": 1080, "shutter": 0.5, "warmup": 8 },   // §5（省略可）
  "cuts": [                                  // カメラカット（マスタートラック）
    { "id": "c_01", "start": 0, "end": 12000, "camera": "b_cam" },
    { "id": "c_02", "start": 12000, "end": 27000, "camera": "b_cam2" }
  ],
  "markers": [ { "id": "m_01", "t": 15600, "name": "impact" } ],
  "bindings": [
    { "id": "b_cam", "name": "CutsceneCam", "kind": "entity",
      "hint": { "guid": "00a1b2c3d4e5f607", "path": "Rig/CutsceneCam", "name": "CutsceneCam" },
      "tracks": [
        { "id": "t_tr", "type": "transform", "rotation": "euler",
          "channels": {
            "position.x": { "post": "hold", "keys": [[0, 0, "a"], [19200, 0, "a"]] },
            "position.y": { "keys": [[0, 6, "a"], [19200, 2.2, "e:inOutQuad"]] },
            "position.z": { "keys": [[0, 14, "a"], [19200, 6, "b", 400, 0, 400, 0]] } } },
        { "id": "t_aim", "type": "aim", "target": { "binding": "b_boss" }, "offset": [0, 1.2, 0], "roll": 0 },
        { "id": "t_fov", "type": "property", "path": "CameraComponent.fovDegrees",
          "channels": { "value": { "keys": [[0, 60, "l"], [19200, 42, "a"]] } } },
        { "id": "t_dof", "type": "cameraDof", "focus": { "binding": "b_boss" },
          "channels": { "aperture": { "keys": [[0, 2.8, "s"]] } } },
        { "id": "t_shk", "type": "shake", "clips": [ { "id": "k_s1", "start": 15600, "dur": 4200, "amp": 0.35, "freq": 26, "seed": 7 } ] }
      ] },
    { "id": "b_boss", "name": "Boss", "kind": "entity", "hint": { "guid": "…", "name": "Boss" },
      "tracks": [ { "id": "t_anim", "type": "animation",
        "clips": [ { "id": "k_a1", "start": 0, "dur": 12000, "clip": "Idle", "offset": 0, "rate": 1.0, "loop": true, "blendIn": 0, "blendOut": 600 } ] } ] },
    { "id": "b_scene", "name": "Scene", "kind": "scene",
      "tracks": [
        { "id": "t_post", "type": "post", "channels": { "bloom": { "keys": [[15600, 0.3, "a"], [18000, 0.9, "a"]] } } },
        { "id": "t_ts", "type": "timeScale", "channels": { "value": { "keys": [[15600, 0.25, "s"], [18600, 1, "e:outQuad"]] } } },
        { "id": "t_ev", "type": "event", "events": [ { "id": "e_1", "t": 26400, "kind": "emit", "name": "bossFightStart", "data": { "value": 1 } } ] } ] }
  ]
}
```

### 2.2 キーとカーブ
- **キー**は `[t, v, ip, …任意の付帯]` のタプル。`ip`（**その区間＝このキーから次のキーまで**の補間）:

| ip | 意味 | 付帯 |
|---|---|---|
| `s` | 定数（ステップ。bool/int/文字列/イベント的な値はこれだけ） | なし |
| `l` | 線形 | なし |
| `a` | **スムーズ（自動タンジェント）**。Hermite。接線は隣接キーから決め、極値ではオーバーシュートを抑える（clamped）。カメラ位置の既定 | なし |
| `b` | **ベジェ**（重み付きハンドル） | `inDt, inDv, outDt, outDv`（ティック/値。ハンドルの相対位置） |
| `e:<name>` | 名前付きイージング（`v0→v1` を 1 本の易しい曲線で結ぶ） | なし |

- **イージング名（`e:`）は既存 3 系統の式を全部包含する**: Lua `Ease` の `linear/inQuad/outQuad/inOutQuad/inCubic/outCubic/inOutSine/outBack/outBounce`（`ScriptEngine.cpp:3569-3588`）＋`UiEase` の `inCubic/outCubic/inOutCubic/outBack/outBounce/outElastic/outExpo/inBack/inOutBack/outQuint/inOutSine`（`ui/UiEase.h:16-95`）＋`sequence.ts` の `in/out/inOut`（= 二次。移行時に `inQuad/outQuad/inOutQuad` へ 1:1）。**式は変えない**（既存作品の見た目を保つ）。S0 のテストで `UiEase(n,p)` と同名の `e:` の値が全 p で一致することを検査する。
- **範囲外（extrapolation）**は各チャンネルの `pre`/`post`: `hold`（既定）/`linear`/`loop`/`pingpong`。
- **チャンネル値の型**: float / bool（`s` 固定）/ int・enum（`s` 固定）/ 色・ベクトルは**成分チャンネル**（`color.r/g/b`、`position.x/y/z`）に分ける（カーブエディタで RGB/XYZ 別々に触れる。既存の `Vec3`＝x/y/z 登録と同じ。`ComponentMeta.cpp:25-29`）。文字列は補間しない（`s` の切替キー）。
- **回転**: 既定は **Euler 度の 3 チャンネル**（Transform が Euler 保存のため。`Components.h:80-101`）。挿入時に前キーとの差が 180° を超えないよう**連続化（unwrap）**する。`"rotation":"quat"` は S3 以降のオプション（4 成分を slerp し、書き込み時に `QuaternionToEulerDegrees` で Euler へ）。
- **評価コスト**: キー配列は時刻昇順（挿入時に安定ソート。同時刻は後ろ勝ち = `UiAnim` と同じ規則）。評価は二分探索＋**前回位置キャッシュ**（プレイヘッドは通常単調）で償却 O(1)。

### 2.3 トラックの 3 族と種別
| 族 | 特徴 | 種別（`type`） |
|---|---|---|
| **カーブ** | チャンネル＝カーブ。シーケンス全体で連続 | `transform` / `property` / `cameraDof` / `post`（scene）/ `timeScale`（scene） |
| **クリップ** | 区間 `{start,dur}` を持つ。同一トラックで重なる＝ブレンド/上書き | `animation` / `audio` / `vfx` / `shake` / `subsequence` |
| **イベント/点** | 時刻 `t` で 1 回 | `event` / `marker`（ルート）/ `cameraCut`（ルート `cuts[]`） |
| **制約** | 毎フレーム計算（キーを持たない） | `aim`（注視。`sequence.ts` の `lookAt/lookAtName` 相当） |

- **`transform`**: `position.{x,y,z}`（ローカル）/`rotation.{x,y,z}`（Euler 度）/`scale.{x,y,z}`。ワールド空間キーは持たない（親付きの扱いは Transform の意味そのまま。`ComputeWorldMatrix` は適用側が使わない）。
- **カメラ**（専用種別は作らない）: FOV/near/far/orthoSize は **`property` トラック**（`CameraComponent.fovDegrees` 等）。**DoF は `cameraDof`**（`aperture`/`focalLength`/`blurSize` のカーブ＋`focus`（バインディング参照。適用側が毎フレーム「カメラ→対象のビュー距離」を計算して `dofFocusDist` に入れる＝**名前引きの `dofFocusName` を使わない**ので guid で安全）＋`focusDist` カーブ（対象なし時））。**カットで選ばれたカメラの `cameraDof` だけが描画時上書きで効く**（§3.4）。
- **`aim`**: 毎フレーム `position` から `target`（バインディング＋オフセット）へ向ける回転を計算して `rotation` を**上書き**（`sequence.ts:516-526` の `aimAt` と同式。符号規約は `ApplicationScene.cpp:543-547` に従う）。評価順は §3.2。
- **`property`（汎用）**: `path = "<コンポーネント型名>.<フィールド>"`。解決は 2 段の**アダプタ**（§3.3）。既存 `entt::meta` 登録（約 40 型）＋**手書きアダプタ**（`MeshRenderer` の色/マテリアル/自由枠 `p1..p4`、`luaScript` の props（num/bool/vec/color）、`Transform`、`SkeletalAnimation` 等 meta 未登録のもの）。
- **`light`**: **トラック型を作らない**。UI が `PointLight/SpotLight/DirectionalLight` の `intensity/color/range/innerConeDeg/outerConeDeg` を「ライトのプリセット」として `property` トラックへ展開するだけ（データは `property`）。
- **`post`（scene バインディング）**: チャンネル名＝`DX12E_POST_FIELDS` の名前（`bloom`, `exposure`, `vignette`, `dofFocusDist`, … 約 90 個。`PostProcessSettings.h:164-200`）。`XxxOn` の bool は**対応する値チャンネルを持つトラックの有無から自動で ON にする**（`sequence.ts` の `post` 生成が `post.set("<k>On", true)` を入れているのと同じ意味。`sequence.ts:343-351`）。
- **`timeScale`（scene）**: Play/Runtime の時だけ Lua の `time.setScale` と同じタイムスケール（`ScriptEngine` が保持。読み出しは `GetTimeScale`、`ScriptEngine.h:83`。書き込み側の C++ 関数名は未確認）。**このトラックがあるシーケンスの時計は強制的に実時間**（自己参照を避ける。§3.5）。終了時に元の値へ戻す（既存 `finish()` の 1.0 復帰と同じ意図）。
- **`animation`**（クリップ）: `{clip 名, offset, rate, loop, blendIn, blendOut}`。クリップ名は `SkeletalAnimation.clips` の名前（`Components.h:311-322`）。同一トラックで重なった区間は**ポーズ空間でクロスフェード**（`SamplePose` + `BlendPoseInPlace` → `Animator::SetPoseOverride`。`Animator.cpp:230-247` と同じ式）。
- **`audio`**（クリップ）: `{path, offset, volume, pitch, bus, fadeIn, fadeOut, spatial（バインディングの位置に追従）}`。波形表示は §4.5。
- **`vfx`**（クリップ）: 2 種の `mode`。`emitter`＝バインディング先の `ParticleEmitter` の `_active` を区間で入切（`fx:play/stop` と同義）、`burst`＝VFX アセット（`assets/vfx/*.json`）をバインディング/オフセット位置に 1 回撒く。**スクラブ時は非対応（警告表示）＋停止位置での「プリロール再現」（§3.6）**。
- **`shake`**（クリップ）: `{amp, freq, decay, seed}`。**カメラの最終ポーズへの加算オフセットを描画側で足す**（Transform を書き換えない＝復元不要）。`sequence.ts` の式（`sin(ph*1.7)` 等。`:605-619`）を `seed` 付きの純関数へ。
- **`subsequence`**（クリップ）: 別 `.dxseq` を `{start, offset, rate}` で入れ子にする。深さ ≤ 8、循環参照は検査で拒否。子のバインディングは親の `bindingMap` で対応付け。**S7**。
- **`event`**: `kind` = `emit`（EventBus。ペイロードは num/string/bool のみという制約 `ScriptEngine.cpp:2616-2618` に合わせる）/ `lua`（`fn` 名と引数。Play 時のみ）/ `loadScene`（`sequence.ts` の `scene` 相当）/ `log`。区間を持たない**点**。`marker` は名前付きの目印で、副作用なし（UI のジャンプ・MCP からの参照用）。
- **`cameraCut`**（ルート `cuts[]`）: `{start,end,camera:<bindingId>,(blend)}`。**カットが無い区間は「現在アクティブなカメラのまま」**。ブレンド（ディゾルブ）は S7。

### 2.4 バインディング
- `kind`: `entity`（既定）/ `scene`（ポスト・時間などグローバル。id は固定で `b_scene`）/ `spawnable`（S7。シーケンスがプレハブを所有して生成）。
- **解決順**: ① `SequencePlayer.bindings[<bindingId>]`（シーンごとの上書き。値は guid の hex）→ ② 資産の `hint.guid` → ③ `hint.path`（`SEQ` と同様の**階層パス**。最寄りの guid 付き祖先からの相対 `A/B/C`。プレハブ展開後は guid が消えるため）→ ④ `hint.name`（名前。最後に作られたものが勝つ既存仕様。**複数一致は警告**を出す。`Scene::FindEntity` の仕様 `Scene.cpp:653-664`）。
- **解決キャッシュ**: `unordered_map<u64 guid, entt::entity>` を `m_sceneGeneration` が変わるたびに 1 回だけ作る（線形走査 `FindEntityByGuid` を毎フレーム呼ばない。`Components.cpp:171-177`）。実行時の `entt::entity` は**保持しない**（Stop で全部変わる。§1.9-2）。
- **作成時の規約**: バインディングを作る瞬間に対象へ `EnsureGuid`（`McpUndoTrack.cpp:23-32`）を呼び、`hint` を埋める。**保存時付与に頼らない**（§1.9-1）。
- **未解決の扱い**: そのバインディングの全トラックを**無効化して警告**（`sequence.ts` の `findE` が `logWarn` して黙って飛ばすのと同じ挙動だが、UI で赤く見せ、`validate` が一覧を返す）。
- **`SequencePlayer` コンポーネント**（シーン JSON キー `sequencePlayer`。追加手順 3 点セットは `SceneSerializer.cpp:128-135`）: `{sequence, autoPlay, loop, rate, clock("real"|"game"), startDelay, restoreOnEnd, bindings{}}`＋ランタイム専有 `_t/_playing/…`（`_` 付きは meta 登録しない規約）。既存の `<name>:play / :stop / :done` イベントに応答・発火する（互換。`sequence.ts:553-573`）。

### 2.5 バリデーション（`Validate(seq)`。エディタ・MCP・CI 共通）
未解決バインディング／重複したチャンネル所有（同一 `(binding, path)` を 2 トラックが書く）／`timeScale` と game クロックの併用／`aim` 対象の循環／存在しないクリップ・音声・VFX アセット／時刻昇順違反・範囲外／`cuts` の重なり・穴／`animation` が `AnimatorController` 付きエンティティを指す（グラフ停止の通知）／エンティティ名の重複（一意でない警告）。既存 `validateSpec`（`sequence.ts:96-221`）の警告（カメラの重複・戻し忘れのスローモ・注視点無し）をそのまま継承する。

---

## 3. 評価と再生

### 3.1 三層構造（純評価 / 適用 / 駆動）
```
 SeqClock ──t──▶ Evaluate(CompiledSequence, t) ──▶ EvalResult ──▶ Applier ──▶ Scene / Post 上書き / Audio / EventBus
   ▲ (Preview/Runtime/Render)   純関数・GPU/ECS/ImGui 非依存      (値の列)     (エンジン層。PreAnimatedState)
```
- **`src/sequencer/`（純評価コア。新規ライブラリ `SequencerCore`。依存は `core/Types.h` と nlohmann のみ）**: 型・カーブ評価・`.dxseq` の読み書き・`SeqOp`（編集操作と逆操作）・`Compile`（バインディングと property の文字列パスを配列インデックスに解決したフラット構造）・`Evaluate`・イベント境界の収集。**`UiAnimAsset.h` と同じ規律**（`tests/ui_anim_test.cpp` を雛形にした単体テスト。`tests/CMakeLists.txt:167-174`）。
- **`EvalResult`**: `channelValues[]`（`bindingIdx, trackIdx, channelIdx, f32`）、`activeClips[]`（`binding, track, clip, localTime, weight`）、`cutCamera`（bindingIdx）、`shakes[]`、`aimTargets[]`。**エンジンの型を一切含まない**。
- **適用層（`Application` 側。新規 `ApplicationSequencer.cpp`）**: バインディング解決、`PropertyAdapter` 経由の書き込み、`SetClipTime/SetPoseOverride`、オーディオ、EventBus、描画時上書きの受け渡し。**同じ適用層を Preview（エディタ）と Runtime（Play）と Render（出力）が共有する**。

### 3.2 評価の順序（決定的）
1. 各カーブ/クリップを時刻 `t` で評価（バインディング配列順 → トラック配列順）。
2. **制約**（`aim`）を評価（手順 1 の位置を使って回転を決定）。
3. **カット選択**（`t` を含む `cuts[]`。重なりは後ろ勝ち＋検査で警告）。
4. **加算効果**（`shake`）は描画側の最終カメラポーズへ足す（書き込まない）。
5. 適用は「バインディング順 → トラック順」。**同一 `(binding, path)` を 2 トラックが書く場合は後勝ち**（検査で警告）。

呼び出し順は入力に依存しない: **`Evaluate(seq, t)` は `t` だけの関数**（内部キャッシュは結果に影響しない）。S0 で「10,000 個のランダムな `t` を任意順で呼んだ結果が、昇順で呼んだ結果とビット一致」を検査する。

### 3.3 プロパティアダプタ
- 登録表 `PropertyAdapter{ id, resolve(entity, path) → Accessor{get,set,type,元値のスナップショット} }`。
  1. **meta アダプタ**: `entt::resolve("PointLight"_hs)` で型を引き、`data(name)` で `get/set`（`SceneSerializer.cpp:137-175` の型分岐＝f32/i32/u32/bool/string/XMFLOAT2,3,4/enum と同じ対応）。**meta 登録されたフィールドがそのまま使える**（新コンポーネントは既存の 3 点セット `SceneSerializer.cpp:128-135` を満たせば自動でトラック化できる）。
  2. **手書きアダプタ**: `Transform`（quaternion 併用時の一貫性を保つ）、`MeshRenderer`（`color`/`material`/自由枠 `p1..p4`/`b1..b3`。`shader.get/set` と同じ枠。`ScriptEngine.cpp:2210-2270`）、`luaScript`（`ScriptProp` を名前で。`Components.h:1315-1327`）。`SkeletalAnimation` は `animation` トラック専用。
  3. **scene アダプタ**: `post`（`DX12E_POST_FIELDS` を X マクロで回して get/set を生成。**Lua の `post.*` と MCP の `set_post_process` と同じ名前表**なので名前の三重管理にならない。`PostProcessSettings.h:158-163`）。
- **適用先の分類**（非破壊の方法が違う）:
  | 適用先 | 方法 |
  |---|---|
  | エンティティのコンポーネント（Transform/ライト/カメラ/プロパティ） | **書き込み＋PreAnimatedState**（§3.4） |
  | ポスト/DoF/露出 | **描画時コピーへ上書き**（`ppApplied` 方式。シーン無変更＝復元不要） |
  | カメラの揺れ | 描画側の加算オフセット（無変更） |
  | timeScale | `ScriptEngine` へ書き込み＋終了時復元（Play のみ） |
  | アクティブカメラ | 判定関数 `FindActiveCameraEntity()` が**カットのカメラを優先**（`isActive` を書き換えない。前記 4 か所を置換） |

### 3.4 エディタでの非破壊スクラブ（PreAnimatedState）
- **方式**: 書き込む前に `(entity guid, adapterId, channel)` ごとに**元値を 1 回だけ退避**する（`unordered_map`）。復元は退避の逆順。UE の Sequencer が「アニメート前の状態トークン」で行うのと同じ発想。`AnimationEditorPanel` の `SnapshotTargets/RestoreTargets`（`:234-261`）を**一般化・中央化**したもの。
- **復元のタイミング（すべて 1 か所の関数 `RestorePreAnimated()`）**:
  1. シーケンサーを閉じる／別シーケンスに切り替える／プレイヘッドが範囲外へ出て「解除」した時。
  2. **Play 開始の直前**（`EnterPlayMode` の `SaveToString` より前。`ApplicationScene.cpp:1537`）。
  3. **あらゆる保存の直前**: `BuildSceneJson`（`SceneSerializer.cpp:904-`）の入口に**フック**を 1 つ足し、保存前に復元 → 保存後に次フレームで再適用。**Ctrl+S・オートセーブ・`save_scene`・Play スナップショットが全部ここを通る**ため、経路の網羅漏れが構造的に起きない（要：S1 で全呼び出し元を grep で確認）。
  4. `open_scene/new_scene/open_project`（entity が全部変わるので、退避を捨てるだけ）。
- **ユーザーが制御中のプロパティを触ったとき**: 毎フレーム「最後に書いた値」と現在値を比べ、差が出たら**トースト「このプロパティはシーケンサーが制御中。自動キー（●）を ON にするとキーとして記録される」**を出す（`AnimationEditorPanel` の `CaptureRecordedEdits` と同じ検出。`:209-232`）。自動キー ON なら差分をキーにする。
- **Undo との関係**: PreAnimatedState は Undo に載せない（一時状態）。**エディタの Undo/Redo が `keepGuid=true` でエンティティを作り直しても guid が同じ**なので退避は有効（`SceneSerializer.h:33-37`）。
- **エディタ中の時計**: プレビュー再生は `m_gameClock.GetDeltaTime()` で進める（`Scene::Update(0)` が回り続ける Editor でも Animator は `SetClipTime` で動く。`Application.cpp:2526-2528`）。Lua/物理/AI/EventBus は動かない。**エディタ中はイベントを発火しない**（マーカー表示のみ。設定で「プレビューでイベントを発火」をオプトイン）。
- **フック位置**: `Application::Update` 内の **1 関数 `UpdateSequencers(dt)` を 2 か所から呼ぶ**。(a) Editor/一時停止の分岐（`m_scene->Update` の直前）、(b) Play 分岐の Lua/Trigger/AI の**後**・アクティブカメラ同期（`Application.cpp:2508-2523`）の**前**（＝スクリプトが同フレームに書いた値をシーケンサーが上書きし、その結果をカメラ同期が拾う）。`Scene::Update`（Animator）は必ずその後（`SetClipTime` を反映するため）。

### 3.5 ランタイム再生（Play）と時計
- **`SeqClock`**（3 モード）: **Preview**＝エディタの `GameClock` dt。**Runtime**＝Play 中の dt。`clock:"real"`（既定。`time.realDt` 相当＝**スケール非適用**で、既存 `sequence_author` の意味と一致）/ `"game"`（`dt*timeScale`）。**Render**＝固定（`t_n = n*ticksPerSecond/fps` の整数ティック。dt という概念を持たない）。
- 一時停止（F1）: `simRunning` で止める（`SequencePlayer` は `paused` のとき進まない）。ただし Editor/一時停止中の**シーケンサーパネルからの操作は常に効く**（パネルが時計の持ち主になる）。
- **イベントの発火規則**: `UiAnim` と同じ (prev, cur] の**左開右閉＋開始フレームだけ閉じる**（`UiAnimAsset.h:208-227`）。ジャンプ（シーク）では発火しない。ループのラップは 2 区間に分ける。S0 のテスト: 任意の分割で範囲 `[0,T]` を刻んでも**発火する多重集合が一致**（ちょうど 1 回ずつ）。
- **`timeScale` トラックがあるシーケンスは `real` に強制**。`game` クロックと併用したら `Validate` がエラー。
- **Play 中の終了処理**: `restoreOnEnd`（既定 true）で `timeScale` と**カット前のアクティブカメラ**を戻し、`<name>:done` を発火（`sequence.ts:566-572` 互換）。エンティティのコンポーネント値は**戻さない**（演出の結果を残すのが既存の挙動）。Stop でシーン全体が復元される。

### 3.6 アニメ・オーディオ・VFX の時刻指定
- **アニメ（単一クリップ）**: `SetClip` → `SetClipTime(localTime*tps)` → 必要なら `Update(0)`（`Animator.h:41-47`）。**重なったクリップ**は 2 本を `SamplePose`（`AnimPose.cpp:16-54`）して `BlendPoseInPlace` → `SetPoseOverride`。
- **AnimGraph 付き（`AnimatorController`）**: `UpdateAnimGraphs` が毎フレーム `SetPoseOverride` で上書きする（`Scene.cpp:610`）ため衝突する。**`SkeletalAnimation` に実行時フラグ `_seqOwned` を新設**し、立っている間は `UpdateAnimGraphs` と FootIK（`Application.cpp:3358`）を飛ばす。シーケンサー終了で下ろす。`AnimGraphRuntime` への「時刻 t で評価」API 追加は**しない**（S7 以降の検討事項）。
- **オーディオ**: スクラブ中は鳴らさない（オプションで「音を聞きながらスクラブ」＝短い断片。S4 以降）。再生中はクリップ開始で `AudioSystem::Play`、**ジャンプ/ループ時は全ボイスを止め、現在位置を含むクリップを `offset` 付きで再開始**する。SFX の任意位置再開始のため **`RestartVoiceAt` を公開する小改修**が要る（`AudioSystem.cpp:988-1014`。現状 private）。`fadeIn/Out` は `PlayParams.fadeIn` と `FadeVoice`（`AudioSystem.h:148-165,234-237`）。
- **VFX**: **イベント駆動**（クリップ開始で `_active=true;_age=0;_emitAccum=0`＝`fx:play` と同義。`ScriptEngine.cpp:2077-2080`）。**スクラブ/シーク後の「見た目の再現」は決定論プリロールで行う**: ① `ParticleSystem` に `Reseed(seed)` を新設（`seed = hash(sequenceId, clipId)`。現状は再シードされない。§1.3）、② シーク先が VFX クリップ内なら、クリップ開始から**固定ステップ 1/60 で最大 5 秒ぶん（≤300 ステップ）を 1 フレームで再シミュレート**。ドラッグ中は無効、マウスを離した時だけ実行。③ プリロール中はパーティクル以外のシミュレーションを進めない。**保証範囲**: CPU パーティクルのみ。GPU パーティクル（`gpu=true`）は乱数が別系統のため**プレビュー再現の保証外**（警告表示。S5 で実測して判断）。
- **物理・Lua・AI は評価しない**（§1.1 のとおり決定論再現の対象外。シーケンサーは「物理に任せたい対象」をバインドしない運用とし、`Validate` が `RigidBody` 付き dynamic をトラックの対象にしたときに警告）。

### 3.7 カット時の描画整合
- カットが切り替わったフレームで **`InvalidateTemporalHistory()`**（TAA/SSR/SSGI/フォグ/DDGI と `m_prevViewProjValid=false`。`ApplicationPipeline.cpp:523-539`）を呼ぶ。さもないと切替直後に前のカメラの絵が半透明で残り、モーションブラーの速度がスパイクする（同関数のコメントの罠そのもの）。**カットではない通常のカメラ移動では呼ばない**。
- **アクティブカメラ判定の置換**: `FindActiveCameraEntity()`（カットのカメラ → なければ `isActive` の先頭）を新設し、§1.6 の 4 か所を置き換える。**`isActive` を書き換えないので、シーンは汚れず、シーケンス終了で戻す処理が要らない**。
- **エディタでの「カメラ視点ロック」（パイロット）**: `gvOverride`（`Application.cpp:1721-1748`）と同じ作法で、Update の後にカットのカメラを `m_camera` へ同期し、描画後にエディタカメラを丸ごと復元する。投影の選択は `gameViewShot` と同じ分岐（`ApplicationRender.cpp:3913-3975`）へ「パイロット中」を足す。右ドラッグでフライを始めたらパイロット解除（UE の eject 相当）。

---

## 4. UI / UX 設計

### 4.1 配置
- **下部ドックのタブ**として置く（コンソール/アセットブラウザと同じ帯。`EditorLayer.cpp:154-157`）。`ToolWindows::DockSlot` に **`BottomTab` を追加**し、`BuildDefaultLayout` とフォーカスパネル判定（`EditorLayer.cpp:203-221` の窓名直書き）を拡張（0.5 日）。**タブのダブルクリックで下部を最大化**（ビューポートを縮めて時間軸を広く）。
- 浮遊窓版は作らない（`NoDocking` の窓は時間軸に狭い）。**ツール窓の流儀に従い**、`kAll` に 1 行＋`EditorContext::showSequencer`（`ToolWindows.h:15-16`）。Ctrl+K パレットの `window.sequencer` は自動で出る。UI 自動テストの窓開閉検査にも自動で入る（`UiTestHarness.cpp:2560-2590`）。
- ファイル: `src/editor/panels/SequencerPanel.{h,cpp}`（描画）、`SequencerCurveView.cpp`（カーブ）、`SequencerWidgets.h`（ルーラー/レーン/キー/波形の描画プリミティブ）、`SequencerEditor.cpp`（選択・ドラッグ・Undo。ImGui 非依存の部分は `src/sequencer/` へ）。

### 4.2 レイアウト
```
┌ ツールバー ─────────────────────────────────────────────────────────────────────────────┐
│ ⏮ ◀ ▶/⏸ ▶| ⏭ ⟲  00:03:12 (frame 96) │ 30fps▾ │ 範囲 0..13:00 │ スナップ▾ │ ● 自動キー │ 🎥視点 │ ⏺ レンダー │ +トラック▾ │
├ トラック一覧 ───────┬ ルーラー(タイムコード/マーカー/カット帯) ──────────────────────────────────────┤
│ ▾ CutsceneCam  [🎥]   │ ▮▮▮ Cut1 ▮▮▮▮▮▮▮▮ | ▮▮ Cut2 ▮▮▮▮▮▮  ← カメラカット帯(サムネイル)              │
│   ▾ Transform         │   ◆──◆───────────◆        ← ドープシート(キーはひし形)                     │
│      position.x/y/z   │                                                                                │
│   ▸ Aim → Boss        │ [ドープシート | カーブ]                                        ┌ミニプレビュー┐ │
│   ▸ FOV               │                                                                │  (カット中の  │ │
│ ▾ Boss                │   ▁▂▄▆▄▂▁▂▃▅▇  ← 音声波形                                       │   カメラ視点) │ │
│   ▸ Animation         │                                                                └──────────────┘ │
└───────────────────────┴───────────────────────────────────────────────────────────────────────────────┘
```
- **左（トラック一覧）**: バインディング→トラック→チャンネルのツリー（`ImGuiListClipper` 相当の自前クリッピング。`HierarchyPanel` と同じ理由）。各行に ミュート/ソロ/ロック（目・鍵アイコン。Lucide の `ICON_*`。`EditorIcons.h`）。種別色は既存トークンの**再利用のみ**（カメラ `TypeCamera`、ライト `TypeLight`、音 `TypeAudio`、メッシュ `TypeMesh`、物理 `TypePhysics`、スクリプト/イベント `TypeScript`、VFX `TypeUi` など。新しい直書き色を作らない）。`+トラック` は**選択中エンティティ（ヒエラルキー）に効く候補だけ**を出す文脈メニュー。ヒエラルキーからのドラッグ＆ドロップでバインディング追加（`AssetDrop.h` と同じ受け口の作法）。
- **右（時間軸）**: ルーラー（タイムコード＝`MM:SS:FF`、ティック目盛りは `AnimationEditorPanel` の間引きロジック `:639-650` を一般化）、マーカー、**カメラカット帯**（カットごとの色付きブロック＋カメラ名。S2 でミニサムネイル）、その下にレーン。
- **表示モードタブ「ドープシート | カーブ」**（UE Sequencer と同じ二本立て）。
- **ミニプレビュー**: 既存の**カメラプレビュー RT**（`m_cameraPreviewRT`／`RenderView(ViewDesc)`。`ApplicationRender.cpp:6394-6479`）を流用し、対象を「選択カメラ」から「現在のカットのカメラ」へ切り替える（S2）。機能は削ってある（影・SSAO・ポスト・DoF なし＝ポストはトーンマップだけ）ので、**最終の見た目はビューポートのパイロットで確認**する分担。サイズは自由（既定 320×180）。
- **ビューポートのオーバーレイ**（3D ビューポートに何も重ねない規約 `dx12-editor-ui` に従い、**線だけ**を 3D 空間へ描く）: カメラ位置キーの軌跡（`EditorIconRenderer::AddLine`。`EditorIconRenderer.h:55`）、キー位置の点、セーフフレーム（パイロット中のみ）。`dx12_screenshot_final{gizmos:false}` で消える（`McpHidingGizmos`）。

### 4.3 見た目（ダーク＋ネオン。別エージェントの 3 案に**トークン経由**で追従）
- **色は `theme::` の意味トークンだけを引く**（`Bg0〜4`、`Border*`、`Accent*`、`Selection*`、`Text*`、`Good/Warn/Bad`、`Type*`）。**16 進の直書き禁止**（`AnimationEditorPanel` の `IM_COL32(28,28,32,255)` 直書きは移行しない。`UiWidgets.h:10` の方針）。ネオン案が採用されて `EditorTheme.h` に新トークン（グロー色・グロー強度など）が入った場合は、`SequencerWidgets.h` の**1 か所のエイリアス表**（例 `PlayheadColor = theme::Accent`、`KeySelectedColor = theme::AccentHover`）だけを差し替える。新トークンが無い間は既存の `Accent` で代用する。
- **グローは描画ヘルパ 1 つに閉じ込める**（`ui::GlowLine/GlowRect`＝アルファを落とした 2〜3 層の `AddLine/AddRect`）。使う場所を限定する: **プレイヘッド、選択中のキー、カメラカット帯の現在ブロック、録画（自動キー）中のトラック枠**の 4 つだけ（乱用しない）。グローの層数は定数で持ち、`0` にすれば完全にフラット表示に戻せる（低スペック時・スクショ比較の安定化用）。
- **DPI**: 寸法は論理 px で書き `ui::Px()` を通す（線の太さ・半径は `PxF`）。キー掴み距離・ルーラー高・行高もトークン化。**ヒット領域と描画を同じ定数で揃える**（DPI 報告 §3 の教訓）。`--dpi-scale 100/150/200` の UI テストで論理サイズ一致を検査（既存 `dpi_scale_layout` に倣う）。

### 4.4 操作
| 操作 | 内容 |
|---|---|
| 再生 | `Space` 再生/一時停止、`Home/End` 範囲の先頭/末尾、`←/→` 1 フレーム、`Shift+←/→` 前後のキーへ、`Ctrl+←/→` 前後のマーカーへ、ループ ⟲ |
| キー | 二重クリックで追加、**`S` = 選択チャンネルに「現在の値でキーを打つ」**（UE の Set Key と同じ）、ドラッグ移動（`Shift`=軸固定、`Alt`=スナップ無効）、矩形選択、`Ctrl/Shift` クリックで複数選択、`Del`、`Ctrl+D` 複製、`Ctrl+C/V`（貼り付けはプレイヘッド基準。**クリップボードは JSON テキスト**に `dxseq-keys` ヘッダ付き＝別シーケンス・AI とも受け渡せる） |
| 一括 | 範囲選択して `Ctrl+ドラッグ` = **リップル**（後続を一緒に動かす。トグル「リップル編集」もツールバーに）、`Ctrl+Shift+R` = 範囲削除してリップル詰め |
| 録画 | ● 自動キー ON: 制御中のプロパティをユーザーが動かした差分をキーにする（`CaptureRecordedEdits` の一般化）。`S` は ON/OFF に関係なく明示キー |
| スナップ | フレーム／キー／マーカー／カット境界／グリッド（ツールバーの ▾ で個別 ON/OFF。既定はフレーム＋キー） |
| ズーム/パン | ホイール＝カーソル位置固定ズーム、中ドラッグ＝パン、`F` = 範囲にフィット、`Shift+F` = 選択キーにフィット（`AnimationEditorPanel.cpp:709-720` の流儀） |
| カメラ | 🎥 ＝パイロット ON/OFF、選択カメラを「カットに追加」（現在のプレイヘッドで分割 = `Ctrl+B`） |
| 3D で編集 | カメラ位置を**ギズモで動かすと、プレイヘッドのキーを更新/作成**（自動キー ON のとき）。軌跡の点を直接ドラッグ（S3 以降） |

- ショートカットは **`EditorCommandTable.h` の `kCommands` に `sequencer.*` として追加**（`Scope::Panel`＋`KeyMode::Panel`。重複は `editor_ux_test` が落とす）。実処理は `EditorCommands.cpp` の `Execute`。**パレットから「シーケンサー: キーを打つ」等が呼べる**。
- **Undo/Redo は 1 本の `SeqOp` 経路**（§4.6）。ドラッグ中は `開始スナップショット＋終了差分を 1 コマンド`（`InspectorPanel.cpp:744-812` の流儀）。

### 4.5 波形付きオーディオトラック
- PCM の取得は `AudioSystem` に **`GetClipPcm(path)`（公開ラッパー）**を足す（`GetOrLoadClip` が private。`AudioSystem.h:344`）。16bit PCM（`AudioClip.cpp:79-82,115-118,149-152`）から**ピークのミップ列**（最小/最大の対を 2 の冪ずつ畳む。1 ピクセル列 = 1 対）を作り、`assets/.cache/waves/<hash>.peaks` に保存（`.` 始まりは pak から除外＝配布物に混ざらない）。**`.ogg` は `stb_vorbis` をワーカースレッドで全デコード**して同じ形式へ（読み込み中はプレースホルダ）。
- 描画は表示範囲のピクセル列ぶんだけ縦線を 1 本ずつ `AddLine`（≤ 2000 本/レーン）。クリップの `offset/rate/volume` を反映。フェードは半透明の三角で表示。

### 4.6 編集操作層 `SeqOp` と Undo
- **すべての変更は `SeqOp`**: `AddBinding/RemoveBinding/AddTrack/RemoveTrack/MoveTrack/SetKeys/RemoveKeys/MoveKeys/SetClip/…/SetMeta`。各 op は**逆 op を返す**（`Apply(seq, op) → inverse`）。**UI・MCP・Lua・Undo が全部これを通る**（別経路で `.dxseq` を書き換えない）。
- Undo は `SequenceEditCommand : IUndoCommand`（op 列と逆 op 列を保持。`UndoCore.h:19-30`）。**全体スナップショット方式は採らない**（`AnimationEditorPanel` は `UiAnimClip` 丸ごと 64 個を持つが、数百キー×数十トラックでは重い）。ドラッグは終了時に 1 コマンド。複数対象は `CompositeCommand`。MCP 経由の編集は `McpUndoRouter` が `AiUndoEntry("AI: sequence_edit")` に束ねる（`McpUndoRouter.h:51-72`。**Playing 中は積まれない**規約 `ApplicationMcp.cpp:196` に従い、Play 中の編集は Undo 対象外）。
- **未保存**: シーケンス資産は独立した dirty フラグ（タブ名に `*`）。`Ctrl+S`（パネルにフォーカス時）は**アトミック保存**（一時ファイル → リネーム）。シーンの `EditSeq()`（`UndoCore.h:135`）とは別管理（シーン JSON に `.dxseq` の中身は入らない）。

### 4.7 性能（目標。実測は S1 で採る）
- **60fps 維持**: 「数百キー × 数十トラック」で、パネル 1 フレームの CPU ≤ **1.5 ms**（目標）。手段: ① 行の**可視範囲だけ**描画（自前クリッパー）、② 各レーンは**可視時間範囲を二分探索**で切り出してから描く、③ ヒットテストは**ホバー中の 1 行**＋矩形選択時の可視キーのみ、④ カーブ折れ線は**画面 3px あたり最大 1 セグメント**へ適応分割、⑤ キーが 2px 未満の間隔で詰まる場合は 1 つに間引いて描く（LOD）、⑥ レーンごとの `PushClipRect`。`cpuScopeMs` に `sequencerUi`（と評価側の `sequencerEval`）のスコープを追加し、`dx12_perf_stats` で測れるようにする（`docs/MCP.md` の perf 系。`dx12-perf-tools` の作法）。
- 評価側: 100 トラック × 各 300 キー × 3 チャンネルで `Evaluate` ≤ **0.3 ms/フレーム**（目標）。前回位置キャッシュで償却 O(1)。アロケーションなし（`EvalResult` は再利用）。

---

## 5. 出力（レンダー）

### 5.1 目的と方式
- **PNG 連番 → ffmpeg で mp4/webm/ProRes**。**決定論**（同じ入力から同じフレーム）・**固定タイムステップ**・**TAA と整合**・（任意）モーションブラー。
- エンジン側にレンダージョブ（新規 `ApplicationSequencerRender.cpp`。`m_mcpFinalShot` と同じ「遅延応答＋状態機械」の作法）。1 フレームごと: ① `t_n = n*tps/fps`（整数ティック）へ適用層を進める ② 固定 dt（`GameClock::SetFixedDelta(1/fps)`。`GameClock.h:26-38`）でシミュレーション（パーティクル・物理・Lua）を 1 ステップ ③ レンダー ④ 最終画を読み戻して PNG へ（書き出しは別スレッド）。

### 5.2 決定論モードの設計（既存の決定論スクショを**流用しない**理由）
既存モードは毎枚 `InvalidateTemporalHistory` し、`totalTime=8.0` 固定、TAA ジッタ 0 固定（`ApplicationRender.cpp:3814-3822`、`ApplicationMcpEditor.cpp:212,263`）。これは「1 枚を収束させて再現する」ためで、**動画では毎フレーム TAA の履歴が無い＝時間方向の平滑化が効かず、エッジがちらつく**。動画用に新しい**レンダー決定論モード**を足す:
- **開始時に 1 回だけ** `InvalidateTemporalHistory` → `warmup` フレーム（既定 8。DDGI は hysteresis 0.97 なので収束に**数十〜百フレーム**かかる → DDGI 使用時は `warmup` を上げる）を `t=開始` で回してから本番。**以後は履歴を捨てない**（連続フレームで TAA/SSGI/DDGI が正常に蓄積）。
- **カットの瞬間だけ** `InvalidateTemporalHistory`（§3.7）。
- `totalTime` は固定値ではなく **`t_n` の秒**（グレイン/ディザ/ウェーブが時刻の関数として動く＝フレーム番号から一意に決まる）。TAA ジッタ・`denoiseFrame`・`ddgi.frameIndex` は **`perfTotalFrames` ではなくレンダー内のフレーム連番**から決める（`ApplicationRender.cpp:4926,5171` が `m_perfTotalFrames` 由来のため、ここを差し替える）。
- **再現性の検査**（S2b の合否）: 同じシーケンスを 2 回レンダーして、**全フレームの画素差分率（`contactSheet.ts` の `frameDiffs` と同じ指標）が 0.05% 以下**（閾値 2/255 超の画素の割合。目標。GPU 由来の非決定＝アトミック順・RT デノイザ・GPU パーティクルで 0 にならない可能性があるため、**ビット一致ではなく許容つき**で合否を決める。既存の決定論スクショも「240 フレーム回しても 1% 残った」と実測している＝`ApplicationMcpEditor.cpp:206-211` のコメント）。
- **部分再レンダー（再開・区間指定）は近似**: 履歴を持たないため、`preroll`（既定 = `warmup`+1 秒）を頭に足して回し、境界フレームの差分は許容する旨を仕様に書く。

### 5.3 モーションブラー・TAA
- **既定＝既存のポスト MB**（`MotionBlurPass`。TAA 有効時は速度バッファでオブジェクト毎、無効時は深度再構成でカメラのみ。`MotionBlurPass.h:13-19`）。`shutter`（0〜1）を `mbStrength`（シャッター係数。`PostProcessSettings.h:145-147`）へ写す。`render.shutter` を `.dxseq` に保存。
- **任意（S7）**: サブフレーム累積（`t_n` を中心に `shutter/fps` の幅で N 個の時刻を評価し、TAA を切ってリニア HDR で平均）。**時刻→状態が純関数**（§3）なのでカメラ・Transform・プロパティは無料で対応できるが、**パーティクル/物理はサブステップを持てない**ため対象外（警告）。

### 5.4 解像度・読み戻し
- v1: **シーンビュー矩形（16:9）のサイズ**で書く（既存の最終画コピーと同じ。`Application.h` の `McpFinalShot`）。**推奨運用**は `--background` の窓（論理 1920×1080）で回す（DPI 報告 §1。窓サイズ＝出力サイズが安定する）。
- v2（S7 の spike、1 日）: **任意解像度のオフスクリーン**（`RenderView(ViewDesc)` の `primary` パスを `outputToBackBuffer=false` で使えるか）。**未確認**（カメラプレビュー用は機能を削っている。`ApplicationRender.cpp:6435-6466`）。
- 読み戻しは既存の同期コピー（`WaitIdle` 前提）で始め、S2b で **1080p/フレームの実測**を採る（目標 ≤ 0.5 秒/フレーム＝1000 フレーム ≈ 8 分）。遅ければ読み戻しバッファのリング化（フレーム遅延 2〜3）を S7 で。

### 5.5 ffmpeg 連携・音
- ffmpeg は PATH から探す（別スキルで既に使っている前提。**未確認**：エンジン同梱ではない）。エンジンが `CreateProcess`＋パイプでコマンドを実行（進捗はフレーム数で表示）:
  `ffmpeg -y -framerate <fps> -i frame_%05d.png [-i audio.wav] -c:v libx264 -crf 16 -pix_fmt yuv420p -movflags +faststart out.mp4`（プリセット: mp4(H.264) / webm(VP9) / ProRes(mov) / GIF）。`fps` は 23.976/29.97 を選べるようにし、**ティックは 24/30 相当のまま ffmpeg のレート指定だけ変える**（§10-②）。
- **音は録音しない**。`audio` トラックのクリップ列から **ffmpeg のフィルタで合成**（`atrim`/`adelay`/`volume`/`afade`/`amix`）。音量カーブは v1 では**定数＋フェードのみ**。空間音・ピッチ・バス効果は再現しない旨を明記（S7 で拡張）。
- 出力先: `<project>/renders/<seq>/<timestamp>/`（`.gitignore` 対象。`.dxseq` の `render` ブロックに既定値）。

---

## 6. MCP

### 6.1 方針
- **エンジン側 method（`sequence_*`）を主、TS の Core ツール 1 本を従**とする。`MCP_ENHANCEMENT_DESIGN.md` の 3 層（shell 5 本＋Core＋長尾）に従い、新 method は `McpDefine(names, McpMeta{…}, handler)` で登録すれば**再起動なしで `dx12_tool_describe/dx12_call` から使える**（`core/mcp/McpMeta.h:16`）。Core への昇格は **`dx12_sequence` 1 本のみ**（`op` で振り分け。`coreSpec.ts` の consolidated の作法）。
- **エンジンにシーケンスの実体があるので、TS で Lua を生成する必要が無くなる**。TS は「仕様 JSON → `.dxseq`」の**純関数コンパイラ**（`specToDxseq`。エンジン非接続で単体テスト可）と、コンタクトシート合成（既存 `contactSheet.ts`）を持つ。
- 効果分類（既存の分類名。`catalog.ts:104-114`）: `list/get/validate` = **read**、`create/edit/apply/bind` = **write_scene**（資産＋シーン）、`scrub/shots/play/stop` = **runtime**、`render` = **runtime（ジョブ）**。エラーは M2 の封筒（`cause/fix/didYouMean`）に従い、**`fix` はそのまま撃ち直せる `dx12_sequence` 呼び出し**にする。

### 6.2 method / op 一覧
| op（method） | 内容 | 主な引数 → 返り値 |
|---|---|---|
| `list` | プロジェクトのシーケンス一覧 | → `[{name,path,duration,fps,bindings,unresolved}]` |
| `get` | 内容（要約 or 全文） | `{name, detail:"summary"|"full"}` |
| `apply` | **宣言的な仕様 JSON から作成/更新**（§6.3）。冪等 | `{spec, mode:"create"|"merge"|"replace", dryRun, attachTo}` → `{plan, warnings, path, duration, unresolved}` |
| `edit` | `SeqOp` 列の適用（キー追加/移動/削除・トラック追加…） | `{name, ops:[…], txn?}` → `{applied, inverse?}`（Undo 可） |
| `bind` | バインディングの付け替え・`EnsureGuid`・`SequencePlayer` の上書き | `{name, binding, entity}` |
| `validate` | §2.5 の検査 | → `{errors,warnings,unresolved:[…],fix:[…]}` |
| `scrub` | エディタでその時刻を**評価して適用**（非 Play）。`hold` でプレイヘッド保持 | `{name, t}` → `{t, cut, values:[要約]}` |
| `shots` | **指定時刻を決定論で撮影**してコンタクトシート | `{name, times:[…]|frames:n, columns, source:"final"}` → 画像＋`frameDiffs` |
| `play` / `stop` | Play してシーケンスを流す／止める（`SequencePlayer` 経由） | 既存 `sequence_preview` と等価の連写も可（`shots{mode:"play"}`） |
| `render` | §5 のレンダージョブ | `{name, range, fps, size, codec, out, warmup, audio}` → ジョブ ID（`dx12_job` 相当で進捗/キャンセル） |
| `migrate` | 旧 `sequence_author` の台本 → `.dxseq`（§7） | `{spec}` |

- **`shots` が最重要**: Play せず**エディタ中に時刻を評価して `screenshot_final{deterministic:true}` を時刻ぶん撃つ**（1 枚 ≈ 決定論スクショ 1 回。Play/Stop の往復が無く、`sequence_preview` の 3〜5 倍速い見込み＝**未実測**）。`gizmos:false`・パイロット ON を自動で付ける。VFX/物理を含む場合は警告して `mode:"play"`（既存 `sequence_preview` の仕組み）へ切替。
- `dx12_call` 経由: すべての method は `dx12_call{name:"sequence_apply", args, dryRun:true}` で撃てる（`dryRun` はネイティブ対応にして**計画と検査だけ返し、何も書かない**。`docs/MCP.md:89` の native dryRun の作法）。

### 6.3 AI が宣言的に作る仕様 JSON（`apply`）
名前・秒・人間の言葉で書ける（ティック・ID・guid を AI に触らせない）。エンジンが解決して `.dxseq` に落とす。
```jsonc
{
  "name": "BossReveal", "fps": 30, "length": 6.0,
  "bindings": {
    "cam":  { "entity": "CutsceneCam", "create": { "type": "camera", "position": [0, 6, 14] } },
    "boss": { "entity": "Boss" }
  },
  "cuts": [ { "t": 0, "camera": "cam" } ],
  "tracks": [
    { "on": "cam", "type": "transform",
      "position": [ { "t": 0, "v": [0, 6, 14] }, { "t": 3.2, "v": [0, 2.2, 6], "ease": "inOutQuad" } ] },
    { "on": "cam", "type": "aim", "target": "boss", "offset": [0, 1.2, 0] },
    { "on": "cam", "type": "property", "path": "CameraComponent.fovDegrees",
      "keys": [ { "t": 0, "v": 60 }, { "t": 3.2, "v": 42, "ease": "outQuad" } ] },
    { "on": "cam", "type": "cameraDof", "focus": "boss", "aperture": 2.0 },
    { "on": "cam", "type": "shake", "t": 2.6, "dur": 0.7, "amp": 0.35, "freq": 26 },
    { "on": "boss", "type": "animation", "clip": "Roar", "t": 2.6 },
    { "type": "audio", "path": "audio/boss_theme.wav", "t": 0.4, "bus": "music" },
    { "on": "scene", "type": "post", "set": { "saturation": [ { "t": 3.2, "v": 1.0 }, { "t": 4.2, "v": 1.35 } ] } },
    { "on": "scene", "type": "timeScale", "keys": [ { "t": 2.6, "v": 0.25, "ease": "s" }, { "t": 3.1, "v": 1.0, "ease": "outQuad" } ] },
    { "type": "event", "t": 4.4, "emit": "bossFightStart" }
  ]
}
```
- **上位互換の作り**: 旧 `SequenceSpec`（`{t,type,to,dur,ease,…}` の平坦な列）も `apply` にそのまま渡せる（`apply` は「新形式」と「旧形式」を判別し、旧形式は §7 の写像で変換）。
- **自己修正できるフィードバック**: `warnings` に「Boss が 2 体見つかった（最後に作られた方を採用）」「`Roar` クリップは Boss に無い（有るのは Idle/Walk）」「カメラの `aim` 対象が画面外へ出る区間あり」等を**日本語で具体的に**返し、`specPatch` の形の修正案（`MCP_ENHANCEMENT_DESIGN.md` §4.4.3 の作法）を付ける。
- **AI が Lua を直接書かなくなる**＝時間の扱い（実時間/スケール）・後始末（timeScale 復帰・カメラ復帰）が 1 か所（エンジン）で正しくなる（`sequence.ts:1-13` の設計意図を継承）。

### 6.4 Lua API（Play 中のゲームから）
`Sequence.play(name|entity, {rate, loop, from})` / `Sequence.stop(name)` / `Sequence.seek(name, sec)` / `Sequence.isPlaying(name)` / `Sequence.duration(name)`。イベント `<name>:play|:stop|:done` は互換で継続。**Lua API を作ったら MCP 辞書＋リファレンス 3 点（`docs/API_REFERENCE.md` / `docs/SCRIPTING.md` / `docs/index.html`）＋予測変換まで一括で更新する**（memory の Lua API checklist。`tests/lua_api_doc_test.cpp` が辞書との整合を検査）。

---

## 7. 統合・移行

### 7.1 既存ツールの扱い（**壊さない・名前も挙動も残す**）
| 既存 | 方針 |
|---|---|
| `dx12_sequence_author`（Lua 生成） | **そのまま残す**。S6 で `engine:"dxseq"` のオプトインを足し、`.dxseq`＋`SequencePlayer` を生成（`SEQ_<name>` に貼る。`playEvent/stopEvent/doneEvent` は同名で返す）。**既定の切替は 1 リリース使ってから**（§10-④）。`dryRun` は両モードで維持（`MCP.md:89`）。 |
| `dx12_sequence_preview` | **無変更**（Play して `step_frames` で連写するだけなので Lua でも `SequencePlayer` でも動く）。`shots`（§6.2）が上位互換の高速版。 |
| `dx12_camera_path` | **無変更**（撮影専用ツール）。新規に `sequence.from_path{mode:"line"|"orbit",…}`（`planCameraPath` の姿勢列を**カメラトラックの `a` キー列**にして保存）を足し、「撮ってみて良ければ残す」導線を作る。 |
| Lua の `Tween/Flicker/Lighting.*` | **無変更**（ゲームロジックの一発演出には引き続き使う）。シーケンサーは「時間軸で編集して残したい演出」の担当。ドキュメントに使い分けを 1 表で書く（§7.3）。 |
| `UiAnimPlayer`/`.uianim` | **無変更**。`UIAnimPlayer` を `animation` トラックの一種として**参照**する拡張（`.uianim` クリップを区間で鳴らす）は S4 の任意項目。 |

### 7.2 旧台本 → `.dxseq` の写像（`specToDxseq`。TS の純関数。S6）
| 旧 `type` | 新 |
|---|---|
| `camera`（`to/from/lookAt/lookAtName/dur/ease`） | `transform` の `position.*`（2 キー。`from` 省略時は**エディタ時の現在値を `get_entity` で読んで焼く**。読めなければ警告）＋（注視があれば）`aim` トラック（`lookAt` 点は `aim.target` の固定位置、`lookAtName` はバインディング）＋`cuts[]` に 1 件 |
| `fade` | `post` の `exposure`（黒 0.0 / 白 8.0 / clear 1.0。`sequence.ts:326-341` と同じ値。先行 fade が無い `clear` は開始で 0 に落とす旧仕様を再現） |
| `post` | `post` チャンネル（値の対）。`XxxOn` は自動 |
| `timeScale`（`dur>0`） | `timeScale` トラック（2 キー）。`dur==0` は `s` キー |
| `shake` | `shake` クリップ |
| `vfx`（プリセット） | `vfx`（`mode:"burst"`）。`vfxPlay/vfxStop` は `mode:"emitter"` |
| `shaderParam` | `property`（`MeshRenderer.p1` 等の手書きアダプタ） |
| `sound` | `audio`（`bgm/loop` は `bus`/`loop`）。長さはクリップ長 |
| `move/rotate` | `transform`（2 キー。`from` 省略は焼く） |
| `light` | `property`（`PointLight.intensity` 等。`color` は 3 チャンネル） |
| `event/log/scene` | `event`（`emit/log/loadScene`） |
| イージング | `in/out/inOut/outBack/outBounce` → `inQuad/outQuad/inOutQuad/outBack/outBounce`（**式が同一**）。省略時の `inOut` → `inOutQuad` |
- **往復は保証しない**（新→旧は非対応）。旧 Lua を残す限り、旧台本の再生成は従来どおり動く。
- **時計の写像**: 旧生成物は `real`（`time.realDt`）。移行で作る `SequencePlayer` は `clock:"real"`。
- **検証**: 既存の `sequence.test.ts` の台本（`SEQUENCE_EXAMPLE` ほか）を新形式へ変換 → エンジンで両方を Play → 同じ時刻の `screenshot_final` を比較（差分率の閾値内）を S6 の合否にする。

### 7.3 既存の編集パネルとの役割分担（置換ではなく参照）
| 既存パネル | 役割 | シーケンサーとの関係 |
|---|---|---|
| `AnimationEditorPanel`（`.uianim`） | UI 要素のアニメを作る | 独立のまま。シーケンサーの UI 用トラックとして**クリップを参照**（S4 任意）。タイムライン描画コード（ズーム/目盛り）は `SequencerWidgets.h` へ**汎用部品として抽出**し、両者で共有する（逆に `AnimationEditorPanel` も theme トークンへ移行する副次効果） |
| `VfxEditorPanel`（`assets/vfx/*.json`） | エフェクト**自体**を作る | シーケンサーの `vfx` トラックが**アセットを参照**して撃つ（中身は編集しない） |
| Animator / `.animfsm` | ゲーム中のアニメ状態機械 | シーケンサーは `animation` トラックで**クリップ名**を指す。グラフ付きは `_seqOwned` で止める（§3.6）。FSM のエディタは作らない（既存方針 `docs/ANIMATION.md:60-64`） |
| `TransitionPreviewPanel` / `SpriteSheetEditorPanel` | 画面遷移/スプライトの確認 | 無関係。**時計だけ**が別（`ImGui DeltaTime`）である点を記録（統合は不要） |
| `AudioMixerPanel` | バス/リバーブ | `audio` トラックの `bus` が参照 |

**使い分けの 1 表（docs に載せる）**: 一発の反応（被弾で揺れる等）＝ Lua `Tween`/`Trigger`。**時間軸で作り込んで残す演出（カットシーン・ムービー・ライティングの時間変化）＝ シーケンサー**。**空間に置く常時エフェクト＝ `ParticleEmitter`**。

---

## 8. 段階計画

**原則**: 各段階は ①単体でマージできる（既存機能・既存テストを壊さない。新機能は窓を開かない限り無影響）②**機械で合否判定できる**（ctest・UI 自動テスト `--background --ui-tests-run-all`・スクショ）③別エージェントへ並列に振れる。工数は「1 人（1 エージェント）が集中した実働日」で、**±40%**。**全段階共通のマージ条件**: `ctest` 全緑（現状 61/61〜62/62）、UI 自動テスト全緑（現状 52〜54）、`npm run test:offline`（TS を触る段階）、**UI 検証は仮想入力＋`--background` のみ**（実マウス・実キーボード・フォーカス操作は禁止＝CLAUDE.md の規約）。

### 8.1 一覧（1 行 / 段階）
| ID | 段階 | 目的 | 依存 | 工数(日) | C++ | 並列レーン |
|---|---|---|---|---:|:-:|---|
| **S0** | データモデル＋評価コア（UI 無し） | `.dxseq`・カーブ・`Evaluate`・`SeqOp`・イベント境界・単体テスト | なし | 4 | ○（純） | 直列（先頭） |
| **S1a** | タイムライン UI 基盤 | 下部ドックのパネル・ルーラー・プレイヘッド・ドープシート・キー編集・Undo（**インメモリのシーケンス**で動く） | S0 | 4 | ○ | A |
| **S1b** | バインディング＋適用＋Play 統合（Transform のみ） | guid 解決・PreAnimatedState・保存フック・`SequencePlayer`・`.dxseq` の開閉・Play で再生 | S0 | 4 | ○ | B |
| **S2a** | カメラ（カット/FOV/注視/DoF/シェイク）＋ミニプレビュー | 「カメラワークが作れる」。パイロット・アクティブカメラ判定の置換・軌跡表示 | S1a, S1b | 5 | ○ | A |
| **S2b** | 決定論レンダー出力（PNG 連番＋ffmpeg） | 「動画にできる」。レンダー決定論モード・ジョブ・ffmpeg | S1b | 5 | ○ | B |
| **S3** | カーブエディタ＋補間 UI | ベジェハンドル・タンジェント・プリセット・カーブ表示・Euler 連続化 | S1a | 5 | ○ | C |
| **S4a** | アニメ/プロパティトラック | `animation` クリップ（ブレンド）・`_seqOwned`・`property`（meta＋手書きアダプタ）・ライトの展開 UI | S1b, S1a | 4 | ○ | A |
| **S4b** | オーディオ/イベント/timeScale | `audio` クリップ＋波形・`event/marker`・`timeScale`・`SeekVoice` 公開 | S1b, S1a | 4 | ○ | B |
| **S5** | VFX/ライト/ポスト | ポスト上書き層（X マクロ）・`vfx`（emitter/burst）・`Reseed`＋プリロール | S4a | 6 | ○ | A |
| **S6** | MCP 拡張・移行・レシピ・Lua | `sequence_*` method・`dx12_sequence`・`apply` 仕様・`specToDxseq`・`Sequence.*`・docs | S0（TS 部）/ S1b（method）/ S2a〜S5（完全版） | 7 | ○ | C（TS 部は S0 直後から着手可） |
| S7（任意） | 拡張 | `subsequence`・カットのブレンド・サブフレーム MB・任意解像度出力・スポーナブル・カメラレール | S2〜S5 | 8 | ○ | — |
| | **合計（S0〜S6）** | | | **48** | | |

**クリティカルパス（3〜4 エージェント）**: S0(4) → S1a∥S1b(4) → S2a∥S2b∥S3∥S6-TS 部(5) → S4a∥S4b(4) → S5(6) → S6 残り(4) ≒ **27 実働日**。**「見て嬉しい」最短点は S1+S2a**（≒ 13 日目。カメラをタイムラインで動かし、カットを切り、ミニプレビューで確認できる）、動画書き出しは S2b（同時期）。

```
S0 ─┬─> S1a ─┬─> S2a ─────────────┐
    │        ├─> S3               │
    │        └─> S4a ─> S5 ───────┼─> S6(完全版)
    ├─> S1b ─┬─> S2b              │
    │        └─> S4b ─────────────┘
    └─> S6-TS 部(specToDxseq・仕様スキーマ・単体テスト)
```

### 8.2 各段階の詳細（(a) 成果物 / (b) 依存 / (c) 工数 / (d) 合否基準 / (e) 並列に振る単位）

#### S0 データモデル＋評価コア（4 日）
- **(a)** `src/sequencer/`（`SeqTypes.h` 型と ID、`SeqCurve.h` 評価（`s/l/a/b/e:`・extrapolation）、`SeqEase.h`（既存 3 系統の式を包含）、`SeqIO.cpp`（決定的シリアライザ＋パーサ。未知フィールドは無視・欠落は既定値＝`UiAnim` と同じ）、`SeqOps.h/.cpp`（`Apply→inverse`）、`SeqCompile.cpp`、`SeqEval.cpp`（`Evaluate`・イベント境界収集）、`SeqValidate.cpp`）。`CMakeLists.txt` 新規ライブラリ `SequencerCore`。`tests/sequencer_core_test.cpp`（`SequencerCoreTests`）と `tests/golden/*.dxseq`。
- **(b)** なし。
- **(c)** 4 日。
- **(d)** ctest 新規 1 本が緑。**チェック ≥ 80 項目**: ①各補間の解析値との一致（`a` は区間端の値・単調区間でオーバーシュート無し、`b` は Hermite 相当ハンドルで `a` と一致、`e:` は `UiEase`/Lua 式と全 p で一致）、②`Serialize(Parse(golden)) == golden` を**バイト一致**、③**評価の順序独立性**（10,000 個のランダム `t` を任意順で評価した結果が昇順評価とビット一致）、④イベントの**分割不変性**（任意の刻みで `[0,T]` を走査しても発火の多重集合が同じ・ループのラップ・シークでは発火しない）、⑤`SeqOp` の**往復**（ランダム op 列を Apply → inverse を逆順 Apply で元とバイト一致）、⑥`Validate` の各エラーの再現、⑦同時刻キーの後ろ勝ち、⑧`SequencerCore` が ECS/GPU/ImGui にリンクしないこと（ターゲットのリンク先を CMake で検査）。性能はログ出力（100 トラック×300 キー×3 で 1 万回評価の平均。目標 ≤ 0.3ms）。
- **(e)** 1 エージェント（純ロジックで単独完結）。**同時に別エージェントが `specToDxseq` の仕様スキーマだけ先行して書ける**（S6-TS 部。S0 の型が固まれば）。

#### S1a タイムライン UI 基盤（4 日）
- **(a)** `DockSlot::BottomTab` の追加（`ToolWindows.h`＋`EditorLayer.cpp:131-168,203-221`）、`SequencerPanel`（ツールバー・トラック一覧・ルーラー・プレイヘッド・ドープシート・ズーム/パン・スナップ・キー選択/移動/追加/削除/コピペ/リップル・`S` キー・タイムコード表示）、`SequenceEditCommand`（Undo）、`sequencer.*` コマンド（`EditorCommandTable.h`）、theme エイリアス表、`GlowLine` ヘルパ、`cpuScopeMs` の `sequencerUi`。**シーンには一切触れない**（インメモリのシーケンスをパネル内でだけ編集。開発用に「サンプルを作る」ボタン）。
- **(b)** S0。
- **(c)** 4 日。
- **(d)** UI 自動テスト新カテゴリ `sequencer`（`--background`）: 窓の開閉・トラック 3 本追加・キー追加/ドラッグ/削除・**Undo/Redo 往復で S0 の `SeqOp` がバイト一致**・ズーム/パン・`--dpi-scale 100/150/200` で論理サイズ一致。**性能**: 合成データ 50 トラック×300 キー（チャンネル展開で約 45,000 キー）でパネルの `sequencerUi` **平均 ≤ 1.5ms（120 フレーム。目標）**、超えたら §4.7 の LOD を入れて再測定。**スクショ**（`dx12_imgui_screenshot`）: 100%/150% で文字切れ・重なり無し、ネオン ON/OFF（グロー層数 0）の 2 枚。
- **(e)** UI 担当 1 エージェント。S1b と**ファイルが被らない**（S1a は `panels/`・`ToolWindows.h`・`EditorLayer.cpp`、S1b は `Application*.cpp`・`Components.h`・`SceneSerializer.cpp`）。共通の `EditorContext` の bool 追加だけ衝突するので S1a が先に入れる。

#### S1b バインディング＋適用＋Play 統合（Transform のみ）（4 日）
- **(a)** `SequencePlayer` コンポーネント（`Components.h`＋`ComponentMeta.cpp`＋`RegisterCoreComponentSerializers` の 3 点セット）、`ApplicationSequencer.cpp`（`UpdateSequencers(dt)`・バインディング解決＋`m_sceneGeneration` キャッシュ・`PropertyAdapter`（Transform のみ）・**PreAnimatedState**・`RestorePreAnimated()` と 4 つの復元点（§3.4）・`BuildSceneJson` フック）、`.dxseq` の開閉（AssetBrowser の分類/ダブルクリック/D&D、`RewriteAssetPathRefs` の拡張子）、Play で `SequencePlayer` 再生（`clock:"real"`）、`<name>:play|stop|done` 互換イベント。
- **(b)** S0（S1a とは独立）。
- **(c)** 4 日。
- **(d)** ctest `SequencerBindTests`（`Scene` をリンクする型。`tests/CMakeLists.txt` の `if(TARGET Scene)` 形式）: ①**100 エンティティを scrub → 閉じる**で全 Transform が**元値とビット一致**、②**scrub 中に保存**した JSON が scrub 前の保存と**バイト一致**（ハッシュ比較）、③Play→Stop の 20 往復で復元、④guid のバインディングが「保存→開き直し→Play→Stop」の **100 サイクルで 100% 解決**、⑤複製・プレハブ展開したエンティティで `path` フォールバックが効く、⑥名前重複時に警告。UI テスト（S1a の後にマージ）: 実 UI から Transform のキーを打って Play で動く。
- **(e)** エンジン統合担当 1 エージェント。

#### S2a カメラ（カット/FOV/注視/DoF/シェイク）＋ミニプレビュー（5 日）
- **(a)** `cuts[]`・カメラカット帯（UI）・`aim` 制約・`property`（`CameraComponent.*` のみ）・`cameraDof`（描画時上書き。`ppApplied` へ。`dofFocusDist` を毎フレーム計算）・`shake`（描画側加算オフセット）・**`FindActiveCameraEntity()`（§1.6 の 4 か所を置換）**・パイロット（`gvOverride` 方式）・カット時の `InvalidateTemporalHistory`・ミニプレビュー（`m_cameraPreviewRT` を「カットのカメラ」に切替）・軌跡描画（`AddLine`）・セーフフレーム。
- **(b)** S1a, S1b。
- **(c)** 5 日。
- **(d)** ①**決定論スクショ 5 枚**（カット境界の前後 2 枚ずつ＋中間）を `shots` 相当の内部呼び出しで撮り、**境界で別カメラの絵に切り替わる**（隣接フレームの画素差分率が閾値超）・**カット内は連続**（差分が小さい）・**カット直後にゴーストが無い**（履歴破棄あり/なしの比較で、あり側が同一構図の決定論スクショと差分 1% 未満）、②`FindActiveCameraEntity` 置換後に**既存のカメラ系 UI テスト・ctest が全緑**（Play 同期・投影・スクリーンシェーダーの回帰）、③DoF: `focus` を動く対象にして距離が追従する（数値検査）、④`isActive` が書き換わっていない（シーン JSON がバイト一致）。**スクショ**: 3 カット構成のシーケンスで、パイロット ON のビューポート・ミニプレビュー・カット帯が 1 枚に収まる画（人間レビュー用）。
- **(e)** S1b の成果に依存するカメラ判定と、S1a の UI（カット帯）に分けられる。**カット帯 UI と判定関数の置換は別エージェント可**（前者は `panels/`、後者は `Application*.cpp`）。

#### S2b 決定論レンダー出力（5 日）
- **(a)** レンダー決定論モード（§5.2: 開始時 1 回の履歴破棄＋`warmup`、`totalTime=t_n`、ジッタ/`denoiseFrame`/`ddgi.frameIndex` をレンダー内連番へ）、レンダージョブ（`ApplicationSequencerRender.cpp`。遅延応答・キャンセル・進捗）、PNG 書き出しスレッド、ffmpeg 起動（プリセット 4 種）、音の ffmpeg 合成（v1: 定数音量＋フェード）、UI の ⏺ ボタンとダイアログ、`render` ブロックの保存。
- **(b)** S1b（評価と適用）。カメラが無くても動く（現在のビューをそのまま撮る）。S2a と統合すると完全。
- **(c)** 5 日。
- **(d)** ①**再現性**: 90 フレーム（3 秒 @30）を 2 回レンダーし、全フレームで**閾値 2/255 超の画素の割合が 0.05% 以下**、②枚数＝`round(長さ×fps)`、ファイル名連番の欠けなし、③`ffprobe` で mp4 のフレーム数＝N・長さ＝N/fps ±1 フレーム、④**TAA 連続性**: 連続フレームの差分が「毎枚履歴破棄」の対照実験より**有意に小さい**（エッジのちらつき低減の数値裏付け）、⑤`step_frames`/`screenshot_final` の既存動作が変わらない（回帰）、⑥1080p/フレームの所要時間を記録（目標 ≤ 0.5 秒。超えたら S7 の読み戻しリング化を計画に入れる）。**スクショ**: 出力 mp4 から先頭・中間・末尾の 3 フレームを抜いてコンタクトシート化。
- **(e)** レンダー担当 1 エージェント。`Application*.cpp` の描画側を触るため、S2a の判定関数置換と**ファイルが近い**（`ApplicationRender.cpp`）→ 同時マージ時に衝突しやすいので、**S2b は新規ファイル中心＋描画側の変更を数行に留める**（時刻/連番の注入点だけ）。

#### S3 カーブエディタ＋補間 UI（5 日）
- **(a)** カーブビュー（値軸のズーム/パン・自動フィット・チャンネル色 XYZ=RGB）、ベジェハンドル編集（重み付き。`Shift`＝対称/`Alt`＝折れ）、タンジェントモード（自動/スムーズ/線形/定数/ユーザー）、イージングのプリセットメニュー（プレビュー曲線付き）、複数キーの値の数値入力、Euler の連続化ツール、**`rotation:"quat"` オプション**、extrapolation の UI、キーの補間種別の一括変更。
- **(b)** S1a（コアは S0 で完成済み）。
- **(c)** 5 日。
- **(d)** UI テスト: ハンドルをドラッグして `SeqOp` 経由で S0 の値が期待どおり変わる、Undo 往復、ズーム/パン、100k 評価点の折れ線描画が**適応分割で ≤ 1.0ms**（目標）。**スクショ**: XYZ の 3 チャンネルカーブ・ハンドル・プリセットメニューが読める（100%/150%）。
- **(e)** UI 担当 1 エージェント（S2a/S4 と並列）。`SequencerCurveView.cpp` は新規ファイルで衝突しない。

#### S4a アニメ/プロパティトラック（4 日）
- **(a)** `animation` クリップ（単一・重なりのクロスフェード）、`SkeletalAnimation::_seqOwned`（`UpdateAnimGraphs` と FootIK を飛ばす）、`property` の **meta アダプタ＋手書きアダプタ**（`MeshRenderer` 自由枠/色、`luaScript` props）、ライトのプリセット展開 UI、エディタでのスクラブ（`SetClipTime`＋`Update(0)`）、トラック追加メニューの文脈化（選択エンティティが持つコンポーネントの meta 登録フィールドから候補を出す）。
- **(b)** S1a, S1b。
- **(c)** 4 日。
- **(d)** ctest: `animation` クリップの**任意時刻評価が `SamplePose` の直接呼び出しとビット一致**、重なりクロスフェードが `Animator` の従来ブレンド式と一致（`animator_crossfade_test` の期待値を流用）。meta アダプタ: `PointLight.intensity/color`・`CameraComponent.fovDegrees` の往復。UI/決定論スクショ: **歩行クリップを 3 時刻で撮って姿勢が時刻に応じて変わり、同じ時刻は同じ絵**、`AnimatorController` 付きで `_seqOwned` が効きグラフが上書きしない。
- **(e)** S4b と別エージェント（`Scene.cpp`/`Animator` 周辺 vs `AudioSystem`）。

#### S4b オーディオ/イベント/timeScale（4 日）
- **(a)** `audio` クリップ（Play 中の再生・ジャンプ時の再開始）、`AudioSystem::GetClipPcm` 公開＋波形ピークキャッシュ（`.cache/waves`）＋波形描画、`RestartVoiceAt`/`SeekVoice` 公開、`event/marker`（`emit/lua/loadScene/log`）、`timeScale` トラック（Play のみ・終了時復元・`real` 強制の検査）、エディタのプレビューでのイベント発火オプトイン。
- **(b)** S1a, S1b。
- **(c)** 4 日。
- **(d)** ctest（`audio_math_test` 等の流儀）: ピーク列が PCM の最小/最大と一致（合成波形）。UI テスト: 波形が描画される・クリップ移動で `offset` が変わる。**Play 統合テスト**: 3 つのイベント（`emit`）が**ちょうど 1 回ずつ**（`step_frames deterministic` で 1 フレームずつ進めて数える）、ジャンプ後の音が `offset` 付きで再開（`GetVoices().positionSec` で検査）、`timeScale=0.25` のあいだも台本が実時間で進む（既存 `sequence_author` の意味と一致）。
- **(e)** オーディオ担当 1 エージェント。

#### S5 VFX/ライト/ポスト（6 日）
- **(a)** ポスト上書き層（`DX12E_POST_FIELDS` を X マクロで回した scene アダプタ。`ppApplied` へ。`XxxOn` の自動 ON）、`post` トラック UI（カテゴリ別のフィールド選択・検索）、`vfx`（`emitter`/`burst`）、`ParticleSystem::Reseed`＋プリロール（§3.6）、ライトのプリセット拡充、`fade` ショートカット（露出キーの生成）。
- **(b)** S4a（アダプタ基盤）。
- **(c)** 6 日。
- **(d)** ctest: `post` チャンネルの名前表が `post.names()` と**完全一致**（X マクロの二重管理が無い）。**決定論**: 同じシーケンスで CPU パーティクルの `burst` を**Reseed 済みで 2 回再生し粒子状態がビット一致**、プリロールで「停止位置に直接ジャンプ」と「頭から再生してその時刻で停止」の粒子数・位置が許容内で一致。ポスト上書きで**シーンの `postProcess` JSON が不変**。スクショ: 露出フェード・ブルームの時間変化・バーストの 5 枚。GPU パーティクルは非保証のまま警告が出ること。
- **(e)** S4a の後に 1 エージェント（内部で VFX 担当とポスト担当に**さらに分割可**）。

#### S6 MCP 拡張・移行・レシピ・Lua（7 日）
- **(a)** エンジン method `sequence_list/get/apply/edit/bind/validate/scrub/shots/play/stop/render/migrate`（新規 `ApplicationMcpSequence.cpp`。`McpMeta` を付けてマニフェストへ）、TS: `toolset/sequencer.ts`（`dx12_sequence {op}` の Core 昇格、`coreSpec.ts` と `searchHints.ts` に同義語）、`specToDxseq.ts`（純関数）＋`sequenceSpec.test.ts`、`sequence_author` の `engine:"dxseq"` オプトイン、`sequence.from_path`、Lua `Sequence.*`（辞書・リファレンス 3 点・予測変換を一括）、`docs/MCP.md`（§1.8 の食い違いも直す）、`docs/EFFECT_RECIPES.md` に**シネマティックのレシピ**（ボス登場・ドリー・クレーン・オービット・フェード・スローモ・被写界深度の送り）、eval タスク（`eval/discovery_tasks.json` に追加）。
- **(b)** TS 部（仕様スキーマ＋コンパイラ＋単体テスト）は **S0 直後から**。エンジン method は S1b 以降で段階的、完全版は S2a〜S5 の後。
- **(c)** 7 日（TS 部 3 日を S0 直後に前倒し可）。
- **(d)** `npm run test:offline`・`schemaDrift`・`toolSurface`（core 面のサイズ/lint。`dx12_sequence` 1 本の追加で `tools/list` が M0 基準を超えない）緑、ctest（`mcp_manifest_test` に新 method を追加）。**AI 自動テスト**: 仕様 JSON（§6.3 の例）を `apply{dryRun:true}` → `apply` → `validate`（未解決 0）→ `shots`（3 枚）が**エンジン起動 1 回・`--background` で通る**。**互換**: 既存 `sequence.test.ts` の台本を新形式へ変換して両方を Play し、同時刻の `screenshot_final` の差分率が閾値内。`dx12_sequence_author`/`preview`/`camera_path` の**既存呼び出しの返り値が変わらない**（`legacy-tools.snapshot.json` 比較）。**エラー再現テスト**（M2 の作法）: 不正なトラック種別・未解決バインディング・範囲外時刻で `didYouMean/fix` が出て、`fix[0]` を機械適用して直る。
- **(e)** MCP 担当（TS＋エンジン method）と docs/レシピ担当に分割可。TS のコンパイラは S0 完了直後に別エージェントで並列着手。

#### S7（任意）拡張（8 日）
`subsequence`（入れ子・`bindingMap`・循環検査）、カットのブレンド（ディゾルブ）、サブフレーム MB（§5.3）、任意解像度オフスクリーン出力（spike 1 日→実装）、読み戻しリング化、`spawnable`（プレハブ所有）、カメラレール（3D スプライン＋弧長パラメータ）、`AnimGraph` の時刻指定評価。**合否は着手時に個別設計**（本書のスコープ外）。

---

## 9. リスクと撤退条件

| # | リスク | 根拠 | 対策 | 撤退条件（合否ゲート） |
|---|---|---|---|---|
| R1 | **バインディングの不安定**（guid は保存時付与・プレハブ/複製で消える・Stop で entity id が全部変わる・名前は一意でない） | §1.5 / §1.9 | guid→パス→名前の解決順・作成時 `EnsureGuid`・`m_sceneGeneration` キャッシュ・未解決の可視化・`validate` | S1b の「100 サイクルで 100% 解決」が満たせない場合、**プレハブ内エンティティへのバインドを禁止**（警告）して guid 付きシーンオブジェクトのみに絞る。それでも不安定なら、バインディングを**必ず SequencePlayer 側の上書き表で管理**する方式へ寄せる。 |
| R2 | **評価の決定論が GPU 由来で崩れる**（アトミック順・RT デノイザ・GPU パーティクル・DDGI の収束） | §1.1 の実測（決定論スクショでも 240 フレームで 1% 残った） | 純評価は CPU で完全決定論（S0 で保証）。レンダーは許容つきの合否（0.05%）＋`warmup`。GPU パーティクルは警告。 | S2b で許容を満たせない場合、**「静止画（`shots`）は決定論、動画は best-effort」と割り切って出荷**し、原因（どのパスが非決定か）を `render_debug` で切り分けて S7 へ。**動画出力自体は止めない**（価値の中心は編集体験）。 |
| R3 | **UI が重い**（数百キー×数十トラックで 60fps を割る） | §4.7 | 自前クリッパー・可視範囲の二分探索・適応分割・LOD | S1a の実測で ≤ 1.5ms を満たせない場合、**チャンネル展開行の遅延生成**と**レーンごとの描画キャッシュ（頂点列の再利用）**を入れる。それでも駄目なら**1 画面 200 行を上限**にして残りを折りたたむ仕様に縮める。 |
| R4 | **非破壊スクラブの漏れ**（保存・オートセーブ・Play・MCP の書き込みとの競合。ユーザーが制御中の値を編集） | §3.4 | 復元点の集約関数・`BuildSceneJson` フック・差分検知トースト | S1b の「scrub 中の保存がバイト一致」「Play↔Stop 20 往復」が満たせない場合、**エンティティへの書き込みを Transform だけ**に絞り、それ以外（ライト・プロパティ）は「ライブ編集モード」の明示トグル（既定 OFF）へ後退。**ポスト/DoF/カメラは元から非破壊（描画時上書き）なので影響しない**。 |
| R5 | **AnimGraph・FootIK との衝突**（毎フレーム `SetPoseOverride` される） | §1.2 | `_seqOwned` フラグで停止 | 停止で足りない副作用（IK の戻り遅れ等）が出る場合、`animation` トラックの対象を「`AnimatorController` を持たないエンティティ」に制限して警告。 |
| R6 | **オーディオのシーク/波形の実装コスト**（SFX の再開始・`.ogg` ストリームの波形） | §1.3 | 公開ラッパー追加・ワーカーデコード・ピークキャッシュ | 不足なら**スクラブ中は無音**を恒久仕様にし、再生時のジャンプは「次のクリップ開始から鳴らす」に縮める。波形は WAV/MP3（全デコード可）のみ先行。 |
| R7 | **`entt::meta` の網羅不足**（`MeshRenderer` 等は未登録。§1.7） | §2.3 | 手書きアダプタで補う・新コンポーネントは既存の 3 点セットで自動対応 | 手書きが増えすぎる場合、**meta 登録の拡充を別タスク化**（`MeshRenderer` の永続フィールド登録は `SceneSerializer` 全体に効くので慎重に）。 |
| R8 | **カメラ判定の置換で既存動作を壊す**（`isActive` を走査する 4 か所） | §1.6 | 関数化＋既存テスト全緑を合否に | 回帰が出たら S2a を**パイロット/ミニプレビューのみ**に縮め、Play 側の同期はシーケンサーが `m_mcpCameraOverride` と同型のフラグで担う。 |
| R9 | **スコープ肥大（UE との完全な同等性を目指して終わらない）** | 要望が「UE のシーケンサー相当」 | **非目標を明記**: Take Recorder・Movie Render Queue のパス/AOV・Control Rig・Niagara 連携・複数ユーザー編集・スポーナブル（S7）・字幕/トランスポートの外部同期。 | 各段階の合否を満たした時点で**その段階でマージ**し、次段階を待たない。S3 以降は S2 の使用感を見てから優先度を並べ替える。 |
| R10 | **時間ソースの分裂**（GameClock・Lua の timeScale・ImGui DeltaTime） | §1.5 | `SeqClock` の 3 モード＋`timeScale` 併用検査 | Lua の `timeScale` が Animator に掛からない既存仕様は**変えない**。シーケンサーの `animation` は自分の時計で進むので影響を受けない。 |
| R11 | **エンジン内 ffmpeg 起動の環境依存**（PATH・コーデック） | §5.5 | 検出と丁寧なエラー・PNG 連番だけでも成功扱い | ffmpeg 不在なら PNG 連番までで完了し、**実行すべきコマンドをそのままコピーできる形で表示**する。 |
| R12 | **ドキュメントの食い違い**（§1.8）が AI の誤操作を招く | §1.8 | S6 で修正 | — |

**全体の撤退線**: S1b の R1/R4 が両方とも合格しない場合は、**「カメラとポスト専用のシーケンサー」**（Transform/プロパティのエンティティ書き込みを持たず、描画時上書きだけ）へ縮めても価値が出る（カメラワーク・カット・DoF・露出は非破壊で成立する）。この縮小版でも S2 までの「見て嬉しい」は届く。

---

## 10. 未決事項（ユーザーに聞くこと・最大 5 点、推奨案つき）

1. **パネルの置き場所**: 下部ドックに**新しいタブ**を作る（コンソール/アセットブラウザの隣。ドックのスロットを 1 つ追加。ダブルクリックで最大化）か、浮遊窓にするか。
   **推奨**: 下部ドックのタブ＋最大化。時間軸は横に広く取りたく、浮遊窓（`NoDocking`）だと画面が狭い。追加コストは 0.5 日（`ToolWindows.h`＋`EditorLayer.cpp`）。
2. **時刻の保存単位**: **整数ティック 6000/秒**（24/25/30/48/50/60/100/120fps が整数に乗る。23.976/29.97 は非対応）か、float 秒か、フレーム番号か。
   **推奨**: 整数ティック 6000/秒。差分が安定し、フレーム境界の誤差が出ない。23.976/29.97 は「24/30 で編集し、出力時の ffmpeg レートだけ変える」で実用上足りる。放送用の厳密な非整数レートが要るなら 6006000 系へ変更が必要なので今決めたい。
3. **ランタイム再生の既定の時計**: **実時間（タイムスケール非適用）**か、**ゲーム時間（スケール適用）**か。
   **推奨**: 既定は実時間（既存 `sequence_author` の意味と同じ。スローモを掛けても台本が壊れない）。`clock:"game"` をオプトインで用意し、`timeScale` トラックを持つシーケンスは実時間へ強制する。
4. **既存 `sequence_author`（Lua 生成）の移行方針**: 新形式へ**即切替**するか、Lua 生成を**残して opt-in** にするか。
   **推奨**: Lua 生成をそのまま残し、S6 で `engine:"dxseq"` を opt-in で追加。実際に 1 リリース使い、同時刻の絵が一致することを確認してから既定を切り替える（旧ツールの名前・引数・返り値は恒久的に維持。`MCP_ENHANCEMENT_DESIGN.md` の alias 方針と同じ）。
5. **レンダー出力の初期スコープ**: 最初は**エディタのビューポートサイズの PNG 連番＋ffmpeg で mp4**（`--background` の 1920×1080 窓で回す運用）から始めるか、最初から**任意解像度のオフスクリーン出力＋サブフレームのモーションブラー**まで作るか。
   **推奨**: 前者（S2b）。エンジンの描画パスはビューポート矩形を前提としており（§5.4）、任意解像度は `RenderView` の主パス拡張を要する大きめの改修で**未確認**。まず動画になる価値を最短で出し、解像度/サブフレーム MB は S7 の spike（1 日）の結果で判断する。

（補足: ネオン系の配色は別エージェントの 3 案に依存するため質問に含めていない。**トークン経由で追従する**設計（§4.3）なので、案が決まってから `EditorTheme.h` に足されるトークンをエイリアス表 1 か所へ繋ぐだけで良い。）
