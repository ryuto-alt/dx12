# Lua スクリプトのエンティティアタッチ機能 設計

- 日付: 2026-04-15
- ブランチ: `feature/audio`
- 関連: `src/scripting/ScriptEngine.{h,cpp}`, `src/scene/`, `src/editor/panels/`

## 目的 / 背景

現状 `ScriptEngine` はプロジェクト全体で 1 つの `sol::state` を持ち、グローバル関数 `OnStart` / `OnUpdate` だけが走る。エンティティに個別の振る舞いを持たせる手段がなく、「ゲームエンジン」として必須の仕組みが欠けている。

AssetBrowser から `.lua` ファイルを Hierarchy / Inspector にドラッグ&ドロップすることで、そのエンティティ専用のスクリプトを紐付けて挙動を与えられるようにする。Unity の MonoBehaviour に相当するワークフロー。

## 要件サマリー

| 項目 | 決定 |
|---|---|
| アタッチ数 | 1 エンティティ 1 スクリプト |
| スクリプト書式 | `function OnStart(self)` / `function OnUpdate(self, dt)` の self スタイル |
| D&D ドロップ先 | Hierarchy パネル行 + Inspector パネル |
| 実行タイミング | Play モード中のみ |
| シーン保存 | scriptPath + enabled をシーンファイルに含める |
| 既存グローバル Lua API | 維持（後方互換） |
| Lua 分離方式 | 単一 `sol::state` + per-component `sol::environment` |
| Undo/Redo | 対応（Attach/Detach） |

## アーキテクチャ

### 1. `LuaScript` コンポーネント

`src/scene/Components.h` に追加する。

```cpp
struct LuaScript
{
    // シリアライズ対象
    std::string scriptPath;          // assets 相対パス (例: "scripts/player.lua")
    bool        enabled = true;

    // ランタイム専有（非シリアライズ）
    std::shared_ptr<sol::environment> env;   // このコンポーネント専用 Lua 環境
    std::shared_ptr<sol::table>       self;  // self テーブル
    bool started   = false;
    bool loadError = false;
};
```

- sol2 のオブジェクトをコンポーネントに直接埋めるとコピー/ムーブ周りが厄介なため `shared_ptr` で保持。
- Editor モードでは env / self は構築されない。Play 開始時に初期化、Play 停止時に破棄。

### 2. ScriptEngine 拡張

`ScriptEngine.h` に以下を追加:

```cpp
void AttachScriptToEntity(entt::entity e, const std::string& scriptPath);
void DetachScriptFromEntity(entt::entity e);
void OnPlayStart();                  // 全 LuaScript の env/self 構築 + OnStart
void OnPlayStop();                   // 全 env 破棄、started リセット
void UpdateAttachedScripts(f32 dt);  // 毎フレーム OnUpdate
void ReloadScript(entt::entity e);   // Inspector の Reload ボタン用
```

### 3. Lua 環境の構築戦略

Play 開始時、各 `LuaScript` コンポーネントに対して:

1. `sol::environment env(*m_lua, sol::create, m_lua->globals())` で `_G` を基底にした新環境を生成
2. `env["self"] = sol::table` を構築し `{ entity, name, transform (ref), enabled }` をセット
3. `m_lua->safe_script_file(path, env, sol::script_pass_on_error)` でスクリプトをロード
   - この env 内で `local` が閉じる → 他エンティティの local と衝突しない
4. env の `OnStart` 関数があれば `env["OnStart"](env["self"])` を呼び出す
5. `started = true`

`self.transform` は Transform の参照またはプロキシとして `sol::usertype<Transform>` で登録。読み書き両方可能。

### 4. ライフサイクル

**Editor モード:**
- LuaScript コンポーネントは `scriptPath` のみ保持、env は未構築
- D&D による Attach は `scriptPath` をセットするだけ

**Play 入り (`OnPlayStart`):**
- 全 `LuaScript` を走査して env/self を構築、`OnStart` を呼び出し

**Play 中フレーム (`UpdateAttachedScripts`):**
- 全 `LuaScript` を走査
- `registry.valid(entity) == false` → env/self を破棄してスキップ
- `!enabled || loadError` → スキップ
- `!started` → env 構築 + `OnStart` 呼び出し（Play 中に Attach されたケース）
- `env["OnUpdate"](self, dt)` を呼び出し、例外は `loadError = true` + `Logger::Error`

**Play 抜け (`OnPlayStop`):**
- 全 env/self クリア、started = false にリセット

### 5. エラーハンドリング

- ロード失敗 (`safe_script_file` エラー) → `loadError = true`、Logger::Error にメッセージ、`m_lastError` に保存 → Inspector に赤字表示
- `OnStart` / `OnUpdate` 呼び出し中の例外 → `loadError = true`、以後そのエンティティだけスキップ
- ファイル不在でも他エンティティの処理は続行

### 6. D&D 実装

**AssetType に `Script` を追加:**
- `AssetBrowserPanel` の拡張子判定で `.lua` を `AssetType::Script` にマッピング

**ドラッグソース (AssetBrowserPanel):**

```cpp
if (entry.type == AssetType::Script)
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        std::string pathStr = entry.path.string();
        ImGui::SetDragDropPayload("DND_SCRIPT", pathStr.c_str(), pathStr.size() + 1);
        ImGui::Text("%s", entry.name.c_str());
        ImGui::EndDragDropSource();
    }
}
```

**ドロップターゲット1: Hierarchy 行:**
- 各エンティティ行の `Selectable` / `TreeNode` 直後に `BeginDragDropTarget`
- `AcceptDragDropPayload("DND_SCRIPT")` を受けたら `EditorContext::pendingScriptAttachments` に `{ entity, scriptPath }` を積む
- `Application::Update` 冒頭の遅延処理で実際に `AttachScriptToEntity` を呼ぶ（既存 pendingSpawns と同じパターン）

**ドロップターゲット2: Inspector パネル全体:**
- `InspectorPanel` のウィンドウ全体を `DND_SCRIPT` 受け皿に
- 選択中エンティティ（複数選択時は全て）に対して `pendingScriptAttachments` に積む

**既存アタッチ上書き:**
- 新規 D&D で既に LuaScript を持つエンティティには `scriptPath` を置き換え、env/self を破棄（Play 中なら再初期化）

### 7. Inspector UI

LuaScript コンポーネントを持つエンティティ選択時:

```
┌─ Lua Script ───────────────────────── [ X ] ┐
│  Script:  scripts/player.lua  [ Browse... ] │
│  ☑ Enabled                                  │
│  [ Reload ]  [ Open in Editor ]             │
│  ⚠ Load error: ...                          │  loadError 時のみ
└─────────────────────────────────────────────┘
```

- **Script 欄**: `InputText` read-only + Browse ボタン（FileDialog で .lua 選択）。D&D 受け皿も兼ねる
- **Enabled チェック**: `enabled` 切り替え、OnUpdate をスキップ
- **X**: コンポーネント削除 (`DetachScriptFromEntity`)
- **Reload**: Play 中に .lua を書き換えたときに env 再構築 + OnStart 再実行
- **Open in Editor**: `ShellExecute` で OS の関連付けアプリを起動
- **エラー表示**: `loadError` 時に赤字で `m_lastError`

### 8. シーン保存 / ロード

**SceneSerializer::Save** に追加:

```json
{
  "entities": [
    {
      "name": "Player",
      "transform": { ... },
      "luaScript": {
        "scriptPath": "scripts/player.lua",
        "enabled": true
      }
    }
  ]
}
```

**SceneSerializer::Load:**
- `luaScript` キーがあれば `LuaScript` コンポーネントを追加（env は未構築）
- ファイル存在チェックはしない（ロード時点では env を作らないので安全）
- Play 開始時に `safe_script_file` が失敗すれば loadError が立つだけ

**後方互換:** 既存シーンに `luaScript` キーは無い → 何もしないだけで壊れない。

**エディタスナップショット** (`m_editorSnapshots`):
- `scriptPath` と `enabled` をスナップショットに含める
- Play→Editor 復帰時は env を破棄、scriptPath / enabled だけ戻す

### 9. Undo/Redo

`UndoSystem` に以下のコマンドを追加:

- `AttachScriptCommand(entity, oldPath, newPath)` - 既存パスと新パス両方保持、`Undo()` で戻す
- `DetachScriptCommand(entity, oldPath, oldEnabled)` - 削除時の状態を保存、`Undo()` で再 Attach

### 10. Lua 側 API (self テーブル)

Play 中に各コンポーネントの self に提供されるフィールド:

```lua
self.entity     -- entt::entity handle (不透明値)
self.name       -- string (NameTag.name のコピー)
self.transform  -- Transform usertype: .position / .rotation / .scale 読み書き可
self.enabled    -- bool (self.enabled = false で自分の OnUpdate をスキップ)
```

グローバル bindings (`input`, `audio`, `physics`, `camera`, `scene`) は env の基底 `_G` から継承されるのでそのまま参照可能。

**将来拡張（本スコープ外）:** `self:GetComponent("MeshRenderer")`, `OnDestroy(self)`, `OnCollide(self, other)` 等。YAGNI で今は入れない。

## 呼び出しポイント

`Application::Update` 内:

```cpp
// Play 入り
if (stateChanged && newMode == EngineMode::Playing) {
    m_scriptEngine->OnPlayStart();
}
// Play 中
if (m_engineMode == EngineMode::Playing) {
    m_scriptEngine->CallOnUpdate(dt);          // 既存グローバル (互換)
    m_scriptEngine->UpdateAttachedScripts(dt); // 新規
}
// Play 抜け
if (stateChanged && newMode == EngineMode::Editor) {
    m_scriptEngine->OnPlayStop();
}
```

`Application::Update` 冒頭の遅延処理:

```cpp
if (!m_editorCtx->pendingScriptAttachments.empty()) {
    for (auto& req : m_editorCtx->pendingScriptAttachments) {
        m_scriptEngine->AttachScriptToEntity(req.entity, req.scriptPath);
        // Undo コマンド積む
    }
    m_editorCtx->pendingScriptAttachments.clear();
}
```

## 影響範囲

| ファイル | 変更内容 |
|---|---|
| `src/scene/Components.h` | `LuaScript` 構造体追加 |
| `src/scripting/ScriptEngine.{h,cpp}` | Attach/Detach/OnPlayStart/OnPlayStop/UpdateAttachedScripts/ReloadScript 実装、Transform usertype 登録 |
| `src/scene/SceneSerializer.cpp` | LuaScript の save/load |
| `src/editor/EditorContext.h` | `pendingScriptAttachments` 追加、スナップショット拡張 |
| `src/editor/UndoSystem` | AttachScript/DetachScript コマンド |
| `src/editor/panels/AssetBrowserPanel.{h,cpp}` | AssetType::Script 追加、`.lua` の D&D ソース |
| `src/editor/panels/HierarchyPanel.cpp` | エンティティ行にドロップターゲット |
| `src/editor/panels/InspectorPanel.cpp` | LuaScript セクション UI、ドロップターゲット |
| `src/core/Application.cpp` | OnPlayStart/OnPlayStop 呼び出し、UpdateAttachedScripts、pendingScriptAttachments 処理 |

## テスト観点 (手動)

- [ ] `.lua` を Hierarchy エンティティ行にドロップ → Inspector に Lua Script コンポーネント表示
- [ ] `.lua` を Inspector にドロップ → 同上
- [ ] Play 押下 → `OnStart` が各エンティティで 1 度だけ呼ばれる
- [ ] Play 中、各エンティティの `OnUpdate(self, dt)` が毎フレーム呼ばれる、self は別インスタンス
- [ ] Stop 押下 → env 破棄、Editor の位置に戻る
- [ ] シーン保存 → 再ロードで scriptPath/enabled が復元される
- [ ] ロードエラーのあるスクリプト → Inspector に赤字表示、他エンティティは動き続ける
- [ ] Undo で Attach/Detach が元に戻る
- [ ] Play 中に D&D → 即座に OnStart が呼ばれて動く
- [ ] 既存のグローバル Lua スクリプトが引き続き動作する（後方互換）

## 非スコープ

- 複数スクリプトのアタッチ
- Editor モードでのスクリプト実行（ExecuteInEditMode 相当）
- `OnDestroy` / `OnCollide` 等の追加ライフサイクル
- Inspector 上でのスクリプト公開変数の自動表示（Unity の `[SerializeField]` 相当）
- ホットリロード（ファイル監視による自動 Reload） — Reload ボタンだけ提供
