# Lua スクリプトのエンティティアタッチ実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** AssetBrowser から `.lua` ファイルを Hierarchy / Inspector に D&D してエンティティに紐付け、Play モード中に `OnStart(self)` / `OnUpdate(self, dt)` を実行する機能を追加する。

**Architecture:** `LuaScript` コンポーネントを ECS に追加。`ScriptEngine` の単一 `sol::state` を維持しつつ、Play 開始時に各コンポーネントへ `sol::environment` を構築して self スタイルで呼び出す。Scene 保存・Undo・Inspector UI・ホットリロードに対応。

**Tech Stack:** C++20, EnTT, sol2 (Lua), ImGui, nlohmann::json, DirectXMath

**Spec:** `docs/superpowers/specs/2026-04-15-lua-script-attach-design.md`

**Build/Run:**
- ビルド: `cmake --build build/debug --config Debug`
- 実行: `build/debug/Debug/DX12Engine.exe` (Working Dir: プロジェクトルート)

**注意:** このプロジェクトには自動テストが無い。各タスクの検証はコンパイル成功 + 実機での手動動作確認で行う。

---

## ファイル構成（変更/新規）

| ファイル | 変更種 | 役割 |
|---|---|---|
| `src/ecs/Components.h` | 修正 | `LuaScript` 構造体追加 |
| `src/ecs/Components.cpp` | 修正 (新規の可能性) | `LuaScript` ムーブコンストラクタ |
| `src/editor/EditorContext.h` | 修正 | `PendingScriptAttach` 追加、`pendingScriptAttachments` キュー |
| `src/editor/UndoSystem.h` | 修正 | `AttachScriptCommand` / `DetachScriptCommand` 追加 |
| `src/scripting/ScriptEngine.h` | 修正 | Attach/Detach/OnPlayStart/OnPlayStop/UpdateAttachedScripts/ReloadScript 宣言 |
| `src/scripting/ScriptEngine.cpp` | 修正 | 上記の実装 |
| `src/scene/SceneSerializer.cpp` | 修正 | `luaScript` の save/load |
| `src/editor/panels/AssetBrowserPanel.cpp` | 修正 | `.lua` ドラッグソース (`DND_SCRIPT`) |
| `src/editor/panels/HierarchyPanel.cpp` | 修正 | エンティティ行に `DND_SCRIPT` 受け皿 |
| `src/editor/panels/InspectorPanel.cpp` | 修正 | Lua Script セクション UI + パネル全体受け皿 |
| `src/core/Application.h` | 修正 | `EntitySnapshot` に `luaScriptPath` / `luaEnabled` 追加 |
| `src/core/Application.cpp` | 修正 | Play 遷移で OnPlayStart/Stop、毎フレーム UpdateAttachedScripts、pendingScriptAttachments 処理、スナップショット拡張 |

---

## 共通スタイル

- すべて `namespace dx12e` 内
- コミットメッセージは既存の日本語 + prefix スタイル (`feat:`, `fix:` ...)
- `/W4 /WX` でビルドするので未使用パラメータは `/*p*/`
- Lua の wchar_t リテラル NG、日本語ログは `Logger::Info` 等で UTF-8

---

## Task 1: `LuaScript` コンポーネント追加

**Files:**
- Modify: `src/ecs/Components.h` (struct 群の末尾 `ConvexHullCollider` の直後、`} // namespace dx12e` の前に追加)

- [ ] **Step 1: sol2 の前方宣言を追加**

`src/ecs/Components.h` の先頭の前方宣言ブロック（`class NodeAnimator;` の直後）に追加:

```cpp
} // sol 名前空間は global で扱いにくいので shared_ptr<void> で隠蔽する
```

実装上は sol のヘッダを Components.h に引き込みたくない（コンパイル時間）。`std::shared_ptr<void>` でランタイム専有状態を保持し、ScriptEngine.cpp 側で `static_pointer_cast` する方針に倒す。前方宣言は不要。

- [ ] **Step 2: `LuaScript` 構造体を追加**

`src/ecs/Components.h` の `ConvexHullCollider` 構造体（160 行付近）の直後、`} // namespace dx12e` の直前に挿入:

```cpp
struct LuaScript
{
    // シリアライズ対象
    std::string scriptPath;   // assets 相対パス（例 "scripts/player.lua"）
    bool        enabled = true;

    // ランタイム専有（非シリアライズ）
    // sol::environment / sol::table を直接持つとヘッダ依存が膨らむため void で隠蔽
    std::shared_ptr<void> env;    // sol::environment
    std::shared_ptr<void> self;   // sol::table
    bool started   = false;
    bool loadError = false;
};
```

- [ ] **Step 3: ビルドして通ることを確認**

Run: `cmake --build build/debug --config Debug --target Ecs 2>&1 | tail -20`

Expected: エラーなし。警告も出ない（`LuaScript` はまだどこからも使われない）。

- [ ] **Step 4: コミット**

```bash
cd /c/Users/ryuto/Documents/github/dx12
git add src/ecs/Components.h
git commit -m "$(cat <<'EOF'
feat: LuaScript コンポーネントを ECS に追加

scriptPath / enabled のシリアライズ対象に加え、env / self /
started / loadError のランタイム専有フィールドを持つ。
sol の型はヘッダ露出を避けて shared_ptr<void> で隠蔽。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `EditorContext` に pendingScriptAttachments を追加

**Files:**
- Modify: `src/editor/EditorContext.h`

- [ ] **Step 1: PendingScriptAttach 構造体を追加**

`src/editor/EditorContext.h` の `PendingSpawnRequest` 構造体（17 行目）の直後に追加:

```cpp
struct PendingScriptAttach
{
    entt::entity entity = entt::null;
    std::string  scriptPath;   // assets 相対パス
};
```

- [ ] **Step 2: EditorContext にキューフィールドを追加**

`src/editor/EditorContext.h` の `pendingDeletions` 宣言の直後（101 行目付近）に追加:

```cpp
std::vector<PendingScriptAttach> pendingScriptAttachments;
```

- [ ] **Step 3: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Editor 2>&1 | tail -20`

Expected: エラーなし。

- [ ] **Step 4: コミット**

```bash
git add src/editor/EditorContext.h
git commit -m "$(cat <<'EOF'
feat: EditorContext に pendingScriptAttachments キューを追加

D&D でアタッチ要求を発行 → Application::Update 冒頭で遅延処理する
ため、エンティティハンドル + scriptPath をペアで積む構造を用意。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: SceneSerializer で `luaScript` を save / load

**Files:**
- Modify: `src/scene/SceneSerializer.cpp`

- [ ] **Step 1: Save に LuaScript 書き出しを追加**

`src/scene/SceneSerializer.cpp` の `ConvexHullCollider` 保存ブロック（194 行目付近 `ej["convexHullCollider"] = true;` の直後、`root["entities"].push_back(ej);` の前）に追加:

```cpp
// --- LuaScript ---
if (reg.all_of<LuaScript>(entity))
{
    const auto& ls = reg.get<LuaScript>(entity);
    if (!ls.scriptPath.empty())
    {
        ej["luaScript"] = {
            {"scriptPath", ls.scriptPath},
            {"enabled",    ls.enabled}
        };
    }
}
```

- [ ] **Step 2: Load に LuaScript 復元を追加**

`src/scene/SceneSerializer.cpp` の Load 関数内、UV タイリング復元ブロック（434 行目付近）の閉じカッコ `}` の直後、`}` (for ループの閉じ) の前に追加:

```cpp
// LuaScript 復元（env は構築しない。Play 開始時に初期化される）
if (ej.contains("luaScript"))
{
    const auto& lsj = ej["luaScript"];
    LuaScript ls;
    ls.scriptPath = lsj.value("scriptPath", "");
    ls.enabled    = lsj.value("enabled", true);
    if (!ls.scriptPath.empty() && !reg.all_of<LuaScript>(e))
        reg.emplace<LuaScript>(e, std::move(ls));
}
```

- [ ] **Step 3: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Scene 2>&1 | tail -20`

Expected: エラーなし。

- [ ] **Step 4: コミット**

```bash
git add src/scene/SceneSerializer.cpp
git commit -m "$(cat <<'EOF'
feat: SceneSerializer で LuaScript の save/load 対応

シーンファイルに luaScript: { scriptPath, enabled } を保存。
Load 時は env を構築せず、scriptPath と enabled だけ復元する。
Play 開始時に ScriptEngine が env を初期化する。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: ScriptEngine に新 API を宣言

**Files:**
- Modify: `src/scripting/ScriptEngine.h`

- [ ] **Step 1: include / 前方宣言を追加**

`src/scripting/ScriptEngine.h` の先頭、`#include "core/Types.h"` の直後に追加:

```cpp
#include <entt/entt.hpp>
```

- [ ] **Step 2: public メソッドを追加**

`src/scripting/ScriptEngine.h` の `void Shutdown();` の直前（38 行目付近）に追加:

```cpp
// エンティティアタッチ版 API
void AttachScriptToEntity(entt::entity e, const std::string& scriptPath);
void DetachScriptFromEntity(entt::entity e);
void OnPlayStart();                   // 全 LuaScript 初期化 + OnStart
void OnPlayStop();                    // 全 env 破棄、started リセット
void UpdateAttachedScripts(f32 dt);   // 毎フレーム OnUpdate
void ReloadScript(entt::entity e);    // Inspector Reload ボタン用
```

- [ ] **Step 3: ビルド確認（宣言だけ）**

Run: `cmake --build build/debug --config Debug --target Scripting 2>&1 | tail -20`

Expected: `unresolved external` か、未実装でリンクエラーが出る想定（まだ実装してないので）。ただし `.h` 単体コンパイルが通る個所までは OK。リンクは Task 5/6 完了後に成立する。

- [ ] **Step 4: コミット**

```bash
git add src/scripting/ScriptEngine.h
git commit -m "$(cat <<'EOF'
feat: ScriptEngine にエンティティアタッチ版 API を追加 (宣言)

AttachScriptToEntity / DetachScriptFromEntity / OnPlayStart /
OnPlayStop / UpdateAttachedScripts / ReloadScript。
実装は後続コミットで追加。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: ScriptEngine: Attach / Detach / ReloadScript の実装

**Files:**
- Modify: `src/scripting/ScriptEngine.cpp`

- [ ] **Step 1: Scene の GetRegistry 経由で entt::registry を取れることを確認**

`src/scripting/ScriptEngine.cpp` の既存 include を見る。`#include "scene/Scene.h"` があり、`m_scene->GetRegistry()` で取れる（既存 `autoCollider` ラムダ内で使用済み）。

- [ ] **Step 2: Attach / Detach / ReloadScript を実装**

`src/scripting/ScriptEngine.cpp` の末尾、`void ScriptEngine::Shutdown()` の直前（452 行目付近）に追加:

```cpp
void ScriptEngine::AttachScriptToEntity(entt::entity e, const std::string& scriptPath)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e)) return;

    LuaScript* existing = reg.try_get<LuaScript>(e);
    if (existing)
    {
        existing->scriptPath = scriptPath;
        existing->enabled    = true;
        existing->env.reset();
        existing->self.reset();
        existing->started    = false;
        existing->loadError  = false;
    }
    else
    {
        LuaScript ls;
        ls.scriptPath = scriptPath;
        reg.emplace<LuaScript>(e, std::move(ls));
    }
    Logger::Info("LuaScript attached: entity={} path={}",
                 static_cast<u32>(e), scriptPath);
}

void ScriptEngine::DetachScriptFromEntity(entt::entity e)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return;
    reg.remove<LuaScript>(e);
    Logger::Info("LuaScript detached: entity={}", static_cast<u32>(e));
}

void ScriptEngine::ReloadScript(entt::entity e)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return;
    auto& ls = reg.get<LuaScript>(e);
    ls.env.reset();
    ls.self.reset();
    ls.started   = false;
    ls.loadError = false;
    Logger::Info("LuaScript reload queued: entity={}", static_cast<u32>(e));
    // 実際の再構築は UpdateAttachedScripts のループで行う
}
```

- [ ] **Step 3: `LuaScript` の include を確認/追加**

`src/scripting/ScriptEngine.cpp` の先頭には既に `#include "ecs/Components.h"` があるので `LuaScript` は見える。変更不要。

- [ ] **Step 4: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Scripting 2>&1 | tail -20`

Expected: エラーなし（OnPlayStart/Stop/UpdateAttachedScripts はまだ未実装なのでリンクエラーが残る。Task 6 で解消）。

- [ ] **Step 5: コミット**

```bash
git add src/scripting/ScriptEngine.cpp
git commit -m "$(cat <<'EOF'
feat: ScriptEngine の Attach/Detach/Reload 実装

エンティティに LuaScript コンポーネントを付与・削除・再初期化する
純粋なステート操作。env/self の破棄フラグ（shared_ptr reset + 
started=false）だけを扱い、実際の再構築は UpdateAttachedScripts
のループに任せる。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: ScriptEngine: OnPlayStart / OnPlayStop / UpdateAttachedScripts の実装

**Files:**
- Modify: `src/scripting/ScriptEngine.cpp`

- [ ] **Step 1: 内部ヘルパーを追加（env 構築ロジック）**

`src/scripting/ScriptEngine.cpp` の匿名 namespace があればそこ、無ければ `namespace dx12e` の開始直後に追加（34 行目付近、`ScriptEngine::ScriptEngine()` の前）:

```cpp
namespace {

// LuaScript コンポーネントに env / self を構築し、OnStart を呼ぶ。
// 失敗時は loadError を true に、Logger::Error を出す。
// 戻り値: 成功 true
bool InitializeLuaScriptInstance(sol::state& lua,
                                  entt::registry& reg,
                                  entt::entity e,
                                  LuaScript& ls,
                                  const std::string& assetsDir,
                                  std::string& lastError)
{
    namespace fs = std::filesystem;
    fs::path abs = fs::path(assetsDir) / ls.scriptPath;

    auto env = std::make_shared<sol::environment>(lua, sol::create, lua.globals());

    // self テーブルを作る
    auto self = std::make_shared<sol::table>(lua.create_table());
    (*self)["entity"]  = e;
    const auto* tag = reg.try_get<NameTag>(e);
    (*self)["name"]   = tag ? tag->name : std::string{};
    auto* tf = reg.try_get<Transform>(e);
    if (tf) (*self)["transform"] = tf;
    (*self)["enabled"] = ls.enabled;

    (*env)["self"] = *self;

    auto result = lua.safe_script_file(
        abs.string(), *env, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        lastError = err.what();
        Logger::Error("Lua load error (entity={} path={}): {}",
                      static_cast<u32>(e), ls.scriptPath, lastError);
        ls.loadError = true;
        return false;
    }

    ls.env       = env;
    ls.self      = self;
    ls.loadError = false;

    // OnStart(self) を呼ぶ
    sol::protected_function fn = (*env)["OnStart"];
    if (fn.valid())
    {
        auto r = fn(*self);
        if (!r.valid())
        {
            sol::error err = r;
            lastError = err.what();
            Logger::Error("Lua OnStart error (entity={}): {}",
                          static_cast<u32>(e), lastError);
            ls.loadError = true;
            return false;
        }
    }
    ls.started = true;
    return true;
}

} // namespace
```

- [ ] **Step 2: OnPlayStart / OnPlayStop / UpdateAttachedScripts を実装**

`src/scripting/ScriptEngine.cpp` の Task 5 で追加したメソッドの直後に追加:

```cpp
void ScriptEngine::OnPlayStart()
{
    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        ls.loadError = false;
        if (ls.scriptPath.empty()) continue;
        InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError);
    }
    Logger::Info("ScriptEngine: OnPlayStart done");
}

void ScriptEngine::OnPlayStop()
{
    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        // loadError は残して Inspector に見せる
    }
    Logger::Info("ScriptEngine: OnPlayStop done");
}

void ScriptEngine::UpdateAttachedScripts(f32 dt)
{
    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        if (!reg.valid(e)) continue;
        if (!ls.enabled) continue;
        if (ls.loadError) continue;
        if (ls.scriptPath.empty()) continue;

        // env 未構築（Play 中に Attach された or Reload された） → 初期化
        if (!ls.env || !ls.started)
        {
            if (!InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError))
                continue;
        }

        auto* env = static_cast<sol::environment*>(ls.env.get());
        auto* self = static_cast<sol::table*>(ls.self.get());
        if (!env || !self) continue;

        // self.transform のポインタを最新化（コンポーネントが再配置される場合に備える）
        if (auto* tf = reg.try_get<Transform>(e))
            (*self)["transform"] = tf;
        (*self)["enabled"] = ls.enabled;

        sol::protected_function fn = (*env)["OnUpdate"];
        if (!fn.valid()) continue;
        auto result = fn(*self, dt);
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Lua OnUpdate error (entity={}): {}",
                          static_cast<u32>(e), m_lastError);
            ls.loadError = true;
        }
    }
}
```

- [ ] **Step 3: 全体ビルドしてリンクが通ることを確認**

Run: `cmake --build build/debug --config Debug --target DX12Engine 2>&1 | tail -30`

Expected: リンクまで成功し `DX12Engine.exe` が生成される。

- [ ] **Step 4: コミット**

```bash
git add src/scripting/ScriptEngine.cpp
git commit -m "$(cat <<'EOF'
feat: ScriptEngine: OnPlayStart/OnPlayStop/UpdateAttachedScripts 実装

各 LuaScript コンポーネントに sol::environment を作り、lua.globals() を
基底に継承。self テーブルに entity/name/transform/enabled をセットし
OnStart(self) / OnUpdate(self, dt) を呼ぶ。env/self は shared_ptr<void> で
保持し、ScriptEngine.cpp 内で sol 型に static_cast して扱う。
Play 中 Attach されたエンティティも次フレームで自動初期化。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: AssetBrowserPanel で `.lua` の D&D ソースを発行

**Files:**
- Modify: `src/editor/panels/AssetBrowserPanel.cpp`

- [ ] **Step 1: 既存の D&D ソース箇所を確認**

現状 463 行目付近に Model/Texture 用のソースがある:

```cpp
if (!entry.isDirectory && (entry.type == AssetType::Model || entry.type == AssetType::Texture))
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        std::string pathStr = entry.path.string();
        ImGui::SetDragDropPayload("DND_MODEL", pathStr.c_str(), pathStr.size() + 1);
        ImGui::Text("%s", entry.name.c_str());
        ImGui::EndDragDropSource();
    }
}
```

`AssetType::Script` 用のブロックを直後に追加する（既存コードは変更しない）。

- [ ] **Step 2: .lua のドラッグソースを追加**

上記ブロックの直後、同じファイル内の同じインデントレベルに追加:

```cpp
else if (!entry.isDirectory && entry.type == AssetType::Script)
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

注意: 既存のブロックが `if (...)` で終わっているなら `else if` でつなぐ。独立した `if` として書くなら条件を重複させないため、既存ブロックの条件部を `if (!entry.isDirectory && (entry.type == AssetType::Model || entry.type == AssetType::Texture || entry.type == AssetType::Script))` と 1つにまとめて、内部で payload ID を分岐する方が素直。以下、推奨の一括書き換え:

```cpp
if (!entry.isDirectory &&
    (entry.type == AssetType::Model || entry.type == AssetType::Texture || entry.type == AssetType::Script))
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        std::string pathStr = entry.path.string();
        const char* payloadId = (entry.type == AssetType::Script) ? "DND_SCRIPT" : "DND_MODEL";
        ImGui::SetDragDropPayload(payloadId, pathStr.c_str(), pathStr.size() + 1);
        ImGui::Text("%s", entry.name.c_str());
        ImGui::EndDragDropSource();
    }
}
```

- [ ] **Step 3: AssetType::Script に `.lua` がマップされていることを確認**

Run: `grep -n "\.lua" src/editor/panels/AssetBrowserPanel.cpp`

Expected: 拡張子判定で `.lua` → `AssetType::Script` にしている行が出る。無ければ同 .cpp 内の拡張子判定ロジックを見つけて `.lua` を `AssetType::Script` に入れる（既存コードで対応済みかは要確認）。

対応していない場合、同ファイルの拡張子判定関数（おそらく `DetectAssetType` や `if (ext == ".fbx")` のような箇所）に以下を追加:

```cpp
if (ext == ".lua") return AssetType::Script;
```

- [ ] **Step 4: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Editor 2>&1 | tail -20`

Expected: エラーなし。

- [ ] **Step 5: コミット**

```bash
git add src/editor/panels/AssetBrowserPanel.cpp
git commit -m "$(cat <<'EOF'
feat: AssetBrowser で .lua の D&D ソース発行 (DND_SCRIPT)

AssetType::Script のエントリから ImGui::SetDragDropPayload を
DND_SCRIPT ID で発行。Model/Texture の DND_MODEL と区別する。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: HierarchyPanel のエンティティ行に `DND_SCRIPT` 受け皿を追加

**Files:**
- Modify: `src/editor/panels/HierarchyPanel.cpp`

- [ ] **Step 1: 既存の D&D ターゲット（HIERARCHY_ENTITY）の隣に追加**

`src/editor/panels/HierarchyPanel.cpp:94-114` の `BeginDragDropTarget` ブロック内、既存の `AcceptDragDropPayload("HIERARCHY_ENTITY")` を処理した直後、`ImGui::EndDragDropTarget();` の前に追加:

```cpp
if (const ImGuiPayload* scriptPayload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
{
    const char* pathCStr = static_cast<const char*>(scriptPayload->Data);
    std::string absPath(pathCStr);

    // assets 相対パスに変換
    namespace fs = std::filesystem;
    auto abs = fs::path(absPath).lexically_normal().string();
    auto base = fs::path(m_assetsDir).lexically_normal().string();
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

    ctx.pendingScriptAttachments.push_back({e, rel});
}
```

- [ ] **Step 2: `m_assetsDir` が HierarchyPanel から参照できることを確認**

Run: `grep -n "m_assetsDir\|assetsDir" src/editor/panels/HierarchyPanel.h src/editor/panels/HierarchyPanel.cpp`

Expected: 存在するなら OK。無ければ `HierarchyPanel` のコンストラクタか `Render` 引数に `std::string assetsDir` を追加する必要あり。

**もし `assetsDir` が未伝播の場合の追加実装:**

`src/editor/panels/HierarchyPanel.h` に追加:

```cpp
public:
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; }

private:
    std::string m_assetsDir;
```

そして `src/editor/EditorLayer.cpp` の HierarchyPanel 生成箇所で `SetAssetsDir(ASSETS_DIR)` を呼ぶ。同ファイル内で `DrawEntityNode` に `ctx` を渡しているので `m_assetsDir` は `this` から直接参照可能。

- [ ] **Step 3: 必要な include を追加**

`src/editor/panels/HierarchyPanel.cpp` の先頭 include に以下を追加（既に入ってなければ）:

```cpp
#include <filesystem>
#include <algorithm>
```

- [ ] **Step 4: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Editor 2>&1 | tail -20`

Expected: エラーなし。

- [ ] **Step 5: コミット**

```bash
git add src/editor/panels/HierarchyPanel.cpp src/editor/panels/HierarchyPanel.h src/editor/EditorLayer.cpp
git commit -m "$(cat <<'EOF'
feat: Hierarchy の各エンティティ行に DND_SCRIPT 受け皿を追加

.lua をエンティティ上にドロップすると assets 相対パスに変換して
EditorContext::pendingScriptAttachments に積む。
実際の Attach は Application::Update 冒頭で遅延処理する。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: InspectorPanel に Lua Script セクション + パネル全体の `DND_SCRIPT` 受け皿

**Files:**
- Modify: `src/editor/panels/InspectorPanel.cpp`

- [ ] **Step 1: include を追加**

`src/editor/panels/InspectorPanel.cpp` の先頭に追加（既になければ）:

```cpp
#include "scripting/ScriptEngine.h"
#include <filesystem>
#include <algorithm>
```

- [ ] **Step 2: Lua Script セクションの描画関数を追加**

`src/editor/panels/InspectorPanel.cpp` 内、他の `DrawXxx(reg, e)` ヘルパーがある箇所に追加。無ければ `InspectorPanel::Render` の先頭直前に匿名ヘルパーとして:

```cpp
static void DrawLuaScriptSection(entt::registry& reg,
                                  entt::entity e,
                                  ScriptEngine* scriptEngine,
                                  const std::string& assetsDir)
{
    if (!reg.all_of<LuaScript>(e)) return;
    auto& ls = reg.get<LuaScript>(e);

    bool open = ImGui::CollapsingHeader("Lua Script",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowItemOverlap);

    // ヘッダ右に X ボタン
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    if (ImGui::SmallButton("X##LuaScriptDetach"))
    {
        if (scriptEngine) scriptEngine->DetachScriptFromEntity(e);
        return;
    }

    if (!open) return;

    ImGui::InputText("Script", ls.scriptPath.data(), ls.scriptPath.size(),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::Checkbox("Enabled", &ls.enabled);

    if (ImGui::Button("Reload"))
    {
        if (scriptEngine) scriptEngine->ReloadScript(e);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in Editor"))
    {
        namespace fs = std::filesystem;
        fs::path abs = fs::path(assetsDir) / ls.scriptPath;
        ShellExecuteA(nullptr, "open", abs.string().c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (ls.loadError)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "Load error (see log)");
    }
}
```

注意: `ShellExecuteA` は `<shellapi.h>` が必要。Windows.h 経由で取れてなければ追加:

```cpp
#include <Windows.h>
#include <shellapi.h>
```

- [ ] **Step 3: Render 内から DrawLuaScriptSection を呼ぶ**

`src/editor/panels/InspectorPanel.cpp` の `Render` 関数のシグネチャを確認。現状:

```cpp
void InspectorPanel::Render(entt::registry& reg,
                             EditorContext& ctx,
                             Camera* camera,
                             ...)
```

`ScriptEngine*` と `assetsDir` を渡す引数が無ければ追加する（EditorLayer 側の呼び出しも合わせて更新）。

`Render` 内の MeshRenderer / Physics セクションの描画後、シーン全体セクションの前に追加:

```cpp
DrawLuaScriptSection(reg, e, m_scriptEngine, m_assetsDir);
```

（`m_scriptEngine` / `m_assetsDir` は `InspectorPanel` のメンバとして持たせる）

- [ ] **Step 4: InspectorPanel クラスに scriptEngine / assetsDir を保持させる**

`src/editor/panels/InspectorPanel.h` に追加:

```cpp
public:
    void SetScriptEngine(ScriptEngine* e) { m_scriptEngine = e; }
    void SetAssetsDir(const std::string& d) { m_assetsDir = d; }

private:
    ScriptEngine* m_scriptEngine = nullptr;
    std::string   m_assetsDir;
```

前方宣言 `class ScriptEngine;` を追加。

- [ ] **Step 5: EditorLayer で InspectorPanel に scriptEngine / assetsDir をセット**

`src/editor/EditorLayer.cpp` の InspectorPanel 初期化箇所で:

```cpp
m_inspectorPanel.SetScriptEngine(scriptEngine);
m_inspectorPanel.SetAssetsDir(assetsDir);
```

`EditorLayer::Render` の呼び出しで既に scriptEngine を受け取っているので、このタイミングで set すればよい。

- [ ] **Step 6: Inspector パネル全体を DND_SCRIPT 受け皿にする**

`src/editor/panels/InspectorPanel.cpp` の `Render` 関数の末尾、`ImGui::End();` の直前に追加:

```cpp
// Inspector 全体を DND_SCRIPT ドロップターゲットに
if (ctx.HasSelection() && ImGui::BeginDragDropTarget())
{
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
    {
        const char* pathCStr = static_cast<const char*>(payload->Data);
        std::string absPath(pathCStr);

        namespace fs = std::filesystem;
        auto abs = fs::path(absPath).lexically_normal().string();
        auto base = fs::path(m_assetsDir).lexically_normal().string();
        std::replace(abs.begin(), abs.end(), '\\', '/');
        std::replace(base.begin(), base.end(), '\\', '/');
        std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

        for (auto ent : ctx.selectedEntities)
            ctx.pendingScriptAttachments.push_back({ent, rel});
    }
    ImGui::EndDragDropTarget();
}
```

- [ ] **Step 7: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Editor 2>&1 | tail -20`

Expected: エラーなし。

- [ ] **Step 8: コミット**

```bash
git add src/editor/panels/InspectorPanel.cpp src/editor/panels/InspectorPanel.h src/editor/EditorLayer.cpp
git commit -m "$(cat <<'EOF'
feat: Inspector に Lua Script セクション + DND_SCRIPT 受け皿

LuaScript コンポーネント所有時に折りたたみセクションを表示:
- Script パス表示 / Enabled トグル / Reload / Open in Editor
- ヘッダ右の X で Detach
- loadError 時は赤字で警告
Inspector パネル全体も DND_SCRIPT を受けて選択エンティティ全員に
ペンディングアタッチを積む。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: UndoSystem に AttachScript / DetachScript コマンドを追加

**Files:**
- Modify: `src/editor/UndoSystem.h`

- [ ] **Step 1: AttachScriptCommand を追加**

`src/editor/UndoSystem.h` の `SpawnEntityCommand` クラスの直後（143 行目付近）に追加:

```cpp
// ── LuaScript Attach コマンド ──
class AttachScriptCommand : public IUndoCommand
{
public:
    AttachScriptCommand(entt::registry* reg, entt::entity entity,
                        bool hadBefore, std::string oldPath, bool oldEnabled,
                        std::string newPath)
        : m_reg(reg), m_entity(entity),
          m_hadBefore(hadBefore), m_oldPath(std::move(oldPath)),
          m_oldEnabled(oldEnabled), m_newPath(std::move(newPath)) {}

    void Undo() override
    {
        if (!m_reg->valid(m_entity)) return;
        if (m_hadBefore)
        {
            LuaScript ls;
            ls.scriptPath = m_oldPath;
            ls.enabled    = m_oldEnabled;
            m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
        }
        else
        {
            if (m_reg->all_of<LuaScript>(m_entity))
                m_reg->remove<LuaScript>(m_entity);
        }
    }

    void Redo() override
    {
        if (!m_reg->valid(m_entity)) return;
        LuaScript ls;
        ls.scriptPath = m_newPath;
        ls.enabled    = true;
        m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
    }

    const char* GetName() const override { return "AttachScript"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    bool            m_hadBefore;
    std::string     m_oldPath;
    bool            m_oldEnabled;
    std::string     m_newPath;
};

// ── LuaScript Detach コマンド ──
class DetachScriptCommand : public IUndoCommand
{
public:
    DetachScriptCommand(entt::registry* reg, entt::entity entity,
                        std::string oldPath, bool oldEnabled)
        : m_reg(reg), m_entity(entity),
          m_oldPath(std::move(oldPath)), m_oldEnabled(oldEnabled) {}

    void Undo() override
    {
        if (!m_reg->valid(m_entity)) return;
        LuaScript ls;
        ls.scriptPath = m_oldPath;
        ls.enabled    = m_oldEnabled;
        m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
    }

    void Redo() override
    {
        if (!m_reg->valid(m_entity)) return;
        if (m_reg->all_of<LuaScript>(m_entity))
            m_reg->remove<LuaScript>(m_entity);
    }

    const char* GetName() const override { return "DetachScript"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    std::string     m_oldPath;
    bool            m_oldEnabled;
};
```

- [ ] **Step 2: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Editor 2>&1 | tail -20`

Expected: エラーなし（`LuaScript` は `ecs/Components.h` 経由で既に見えている）。

- [ ] **Step 3: コミット**

```bash
git add src/editor/UndoSystem.h
git commit -m "$(cat <<'EOF'
feat: UndoSystem に AttachScript/DetachScript コマンド追加

LuaScript コンポーネントの付与・差し替え・削除を Undo/Redo 可能に。
Attach は「元がコンポーネント無しだった」「元のパス/enabled」を保持
して Undo で復元できるようにする。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Application.cpp: Play 遷移 / 毎フレーム更新 / pendingScriptAttachments 処理

**Files:**
- Modify: `src/core/Application.cpp`

- [ ] **Step 1: include を追加（必要なら）**

`src/core/Application.cpp` の先頭に既に `#include "scripting/ScriptEngine.h"` がある前提。無ければ追加。

- [ ] **Step 2: Play 開始時に OnPlayStart を呼ぶ**

`src/core/Application.cpp` の既存 `m_scriptEngine->CallOnStart();` が呼ばれている 2 箇所（991 行目、1098 行目）の**直前**に挿入:

```cpp
m_scriptEngine->OnPlayStart();
```

両方の CallOnStart に対して同じ変更を行う。

- [ ] **Step 3: Play 中毎フレーム UpdateAttachedScripts を呼ぶ**

`src/core/Application.cpp` の `m_scriptEngine->CallOnUpdate(dt);` 行（958 行目付近）の直後に挿入:

```cpp
m_scriptEngine->UpdateAttachedScripts(dt);
```

- [ ] **Step 4: Play → Editor 復帰時に OnPlayStop を呼ぶ**

`src/core/Application.cpp` 内の `m_engineMode = EngineMode::Editor;` の**直前**（Play → Editor 遷移箇所、606 行目付近と 703 行目付近と 1306 行目付近の 3 ヶ所）に挿入:

```cpp
if (m_engineMode == EngineMode::Playing)
    m_scriptEngine->OnPlayStop();
```

**実装メモ:** 3 ヶ所すべて同じパターンで OK。既に `m_engineMode` が Playing だったかどうかチェックしてから OnPlayStop を呼ぶ（Editor→Editor 無駄呼び回避）。

- [ ] **Step 5: pendingScriptAttachments の遅延処理を追加**

`src/core/Application.cpp` の `pendingSpawns` を処理しているブロック（1441 行目付近）の**直後**、他の pending 処理の流れに合わせて追加:

```cpp
// スクリプトアタッチ遅延処理
if (!m_editorCtx->pendingScriptAttachments.empty())
{
    auto attachments = std::move(m_editorCtx->pendingScriptAttachments);
    m_editorCtx->pendingScriptAttachments.clear();

    auto& reg = m_scene->GetRegistry();
    for (const auto& req : attachments)
    {
        if (!reg.valid(req.entity)) continue;

        // Undo 用に現状を保存
        bool        hadBefore  = reg.all_of<LuaScript>(req.entity);
        std::string oldPath;
        bool        oldEnabled = true;
        if (hadBefore)
        {
            const auto& cur = reg.get<LuaScript>(req.entity);
            oldPath    = cur.scriptPath;
            oldEnabled = cur.enabled;
        }

        m_scriptEngine->AttachScriptToEntity(req.entity, req.scriptPath);

        m_editorCtx->undoSystem.PushCommand(
            std::make_unique<AttachScriptCommand>(
                &reg, req.entity,
                hadBefore, oldPath, oldEnabled,
                req.scriptPath));
    }
}
```

必要な include: `#include "editor/UndoSystem.h"`（既に入ってる前提、なければ追加）。

- [ ] **Step 6: ビルド確認**

Run: `cmake --build build/debug --config Debug --target DX12Engine 2>&1 | tail -30`

Expected: エラーなし、`DX12Engine.exe` リンク成功。

- [ ] **Step 7: コミット**

```bash
git add src/core/Application.cpp
git commit -m "$(cat <<'EOF'
feat: Application に Lua アタッチのライフサイクルを配線

- Play 開始時に ScriptEngine::OnPlayStart() を呼んで全 LuaScript
  の env/self を構築し OnStart(self) を発火
- Play 中毎フレーム UpdateAttachedScripts(dt) で OnUpdate(self, dt)
  を回す (既存のグローバル CallOnUpdate は互換維持で併走)
- Play→Editor 遷移時に OnPlayStop() で env を破棄
- Application::Update 冒頭で pendingScriptAttachments を消化し
  AttachScriptCommand を Undo スタックに積む

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: エディタスナップショットに LuaScript を含める

**Files:**
- Modify: `src/core/Application.h`
- Modify: `src/core/Application.cpp`

- [ ] **Step 1: EntitySnapshot に LuaScript 情報を追加**

`src/core/Application.h` の `EntitySnapshot` 構造体内（151 行目付近、`modelPath` / `editorSpawned` の直前）に追加:

```cpp
// Lua スクリプトアタッチ
std::string luaScriptPath;
bool        luaEnabled = true;
bool        hasLuaScript = false;
```

- [ ] **Step 2: スナップショット採取時に LuaScript をコピー**

`src/core/Application.cpp` のスナップショット作成ループ（1081 行目付近 `m_editorSnapshots[name.name] = snap;` の前）に追加:

```cpp
if (reg.all_of<LuaScript>(entity))
{
    const auto& ls = reg.get<LuaScript>(entity);
    snap.hasLuaScript  = true;
    snap.luaScriptPath = ls.scriptPath;
    snap.luaEnabled    = ls.enabled;
}
```

- [ ] **Step 3: スナップショット復元時に LuaScript を復元**

`src/core/Application.cpp` のスナップショット復元ブロック（1111 行目付近 Transform 復元の後）に追加:

```cpp
if (snap.hasLuaScript && !reg.all_of<LuaScript>(entity))
{
    LuaScript ls;
    ls.scriptPath = snap.luaScriptPath;
    ls.enabled    = snap.luaEnabled;
    reg.emplace<LuaScript>(entity, std::move(ls));
}
```

- [ ] **Step 4: ビルド確認**

Run: `cmake --build build/debug --config Debug --target DX12Engine 2>&1 | tail -30`

Expected: エラーなし。

- [ ] **Step 5: コミット**

```bash
git add src/core/Application.h src/core/Application.cpp
git commit -m "$(cat <<'EOF'
feat: エディタスナップショットに LuaScript を含める

Play→Editor 復帰時にアタッチが消えないよう、EntitySnapshot に
luaScriptPath / luaEnabled / hasLuaScript を追加。採取時に
LuaScript コンポーネントをコピーし、復元時に再付与する。

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: 手動動作確認

**Files:** なし（実機検証のみ）

- [ ] **Step 1: テスト用 .lua を用意**

`assets/scripts/test_attach.lua` を新規作成:

```lua
-- test_attach.lua
function OnStart(self)
  log("[test_attach] OnStart called for " .. self.name)
  self.t = 0.0
end

function OnUpdate(self, dt)
  self.t = self.t + dt
  self.transform.position.y = math.sin(self.t * 2.0) * 0.5 + 1.0
end
```

- [ ] **Step 2: エディタ起動**

Run: `build/debug/Debug/DX12Engine.exe`（Working Dir: `C:/Users/ryuto/Documents/github/dx12`）

Expected: エディタウィンドウが開く、以前のクラッシュは起きない。

- [ ] **Step 3: AssetBrowser から Hierarchy 上のエンティティへ D&D**

操作:
1. AssetBrowser で `scripts/test_attach.lua` を見つける
2. Hierarchy の Box エンティティ等へドラッグ
3. Inspector で「Lua Script」セクションが表示され、Script: `scripts/test_attach.lua` / Enabled: ✓ / Load error 無し

Expected: Inspector に表示される、ログに `LuaScript attached: entity=… path=scripts/test_attach.lua` が出る。

- [ ] **Step 4: Play モードで OnStart / OnUpdate 動作確認**

操作:
1. ツールバーで Play を押す
2. ログに `ScriptEngine: OnPlayStart done` と `[Lua] [test_attach] OnStart called for Box` が出る
3. Box が Y 軸方向に上下動する

Expected: 正しく上下動する。ログ出力あり。

- [ ] **Step 5: Play → Stop で env が破棄され、エディタ位置に戻る**

操作:
1. Stop を押す
2. Box の位置が Play 開始前に戻る
3. Inspector で Lua Script セクションは残っている

Expected: スナップショット復元が効いている。

- [ ] **Step 6: シーン保存 → 再起動 → ロードで復元**

操作:
1. Ctrl+S でシーン保存
2. 該当 `.scene` ファイルを開いて `luaScript: { scriptPath: "scripts/test_attach.lua", enabled: true }` が書かれている
3. エディタ再起動
4. 同じシーンを開く → Inspector で Lua Script セクションが残っている

Expected: シーンファイルに保存され、再ロードで復元される。

- [ ] **Step 7: Inspector UI: Reload / Open in Editor / X ボタン確認**

操作（Play 中）:
1. test_attach.lua を外部エディタで書き換え（例: `* 2.0` を `* 5.0` にして振幅増加）
2. Inspector の Reload 押下 → ログに `LuaScript reload queued` → 次フレームから新しい動きになる
3. Open in Editor 押下 → OS の既定アプリで .lua が開く
4. Inspector の Lua Script ヘッダ右の X 押下 → コンポーネントが消える

Expected: いずれも動作。

- [ ] **Step 8: Inspector の Undo 確認**

操作:
1. Box にアタッチ
2. Ctrl+Z → アタッチが取り消される
3. Ctrl+Y → 再度アタッチされる

Expected: Undo/Redo が通る。

- [ ] **Step 9: エラースクリプトの表示確認**

テスト用エラーファイル `assets/scripts/broken.lua`:

```lua
-- わざと構文エラー
function OnStart(self
  -- 閉じカッコ忘れ
```

操作:
1. broken.lua を Box にアタッチ
2. Play 押下
3. Inspector に赤字で `Load error (see log)` が出る、ログに詳細エラー
4. 他のエンティティのスクリプトは動き続ける

Expected: エラーエンティティのみスキップ、他は正常動作。

- [ ] **Step 10: 動作確認ログ残し**

Discord 進捗報告 or テスト済み所見を `docs/superpowers/plans/2026-04-15-lua-script-attach.md` の末尾に追記してコミット（任意）。

---

## Self-Review（プラン作者向け、実行前の最終チェック）

- [x] **Spec coverage:**
  - LuaScript コンポーネント構造 → Task 1
  - sol::environment per component → Task 6
  - OnPlayStart / OnPlayStop / UpdateAttachedScripts → Task 6 + Task 11
  - D&D DND_SCRIPT + Hierarchy + Inspector → Task 7, 8, 9
  - Inspector UI (Browse / Enabled / Reload / Open in Editor / X / エラー表示) → Task 9
  - シーン保存 → Task 3
  - Undo/Redo → Task 10 + Task 11
  - スナップショット復帰 → Task 12
  - self テーブル API (entity/name/transform/enabled) → Task 6
  - 既存グローバル API 互換 → Task 11 (CallOnUpdate は残す)

- [x] **Placeholder scan:** "TBD" / "TODO" / "実装しろ" 類は無し。すべてのコードブロックに具体実装あり。

- [x] **Type consistency:** `LuaScript` フィールド名 (`scriptPath` / `enabled` / `env` / `self` / `started` / `loadError`) は全タスク一貫。`PendingScriptAttach` / `pendingScriptAttachments` / `DND_SCRIPT` 命名一貫。`AttachScriptCommand` のコンストラクタ引数順 (hadBefore, oldPath, oldEnabled, newPath) は Task 10 / Task 11 で一致。
