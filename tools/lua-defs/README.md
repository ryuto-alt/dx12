# Lua API 型定義（VSCode 補完用）

エンジンの Lua API を VSCode で**予測変換（IntelliSense）**できるようにする型定義スタブ。
[Lua Language Server（sumneko.lua 拡張）](https://marketplace.visualstudio.com/items?itemName=sumneko.lua) が読む。

| ファイル | 内容 | 正（source of truth） |
|---|---|---|
| `dx12e.lua` | C++ バインディング全部（scene/input/camera/audio/physics/events/ui/fx、型、KEY_* 定数、log 等） | `src/scripting/ScriptEngine.cpp` の RegisterBindings / RegisterPhysicsBindings / RegisterEventsBinding |
| `dx12e-prelude.lua` | 高レベルヘルパー（actor/cameraFollow/cameraTPS/cameraLockOn/keyDown/FX.\*/vfx/goToScene/clamp…） | 同ファイル LoadPrelude() の kPrelude |

## ゲームプロジェクトへの導入

1. このフォルダの `.lua` 2 ファイルをプロジェクトの `.dx12/lua-defs/` へコピー
2. プロジェクトルートに `.luarc.json` を作成:

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": [".dx12/lua-defs"],
  "workspace.checkThirdParty": false,
  "diagnostics.disable": ["lowercase-global"]
}
```

3. `.vscode/extensions.json` で拡張を推奨（開いたときにインストール提案が出る）:

```json
{ "recommendations": ["sumneko.lua"] }
```

これで `.lua` を開けば `scene:` `fx:burst{` `actor(` などが説明付きで補完される。

- `runtime.version` は LuaLS が 5.5 未対応バージョンでも動くよう 5.4 を指定（エンジン本体の Lua は 5.5。スクリプト API 範囲では差なし）
- `lowercase-global` を無効にしているのは、ゲームスクリプトの流儀が `player = actor(...)`（OnStart で local なし代入）のため

### self の補完を効かせる

`OnStart`/`OnUpdate` の `self` は注釈を付けると補完が効く:

```lua
---@param self ScriptSelf
function OnStart(self)
  self.transform.position = Vec3.new(0, 1, 0)  -- ← .position が補完される
end
```

## メンテナンスのルール（重要）

**Lua API を追加・変更・削除したら、このスタブも必ず同時に更新すること。**

- `ScriptEngine.cpp` のバインディング変更 → `dx12e.lua`
- `LoadPrelude()` の kPrelude 変更 → `dx12e-prelude.lua`
- あわせて `docs/SCRIPTING.md` / `docs/API_REFERENCE.md` / 使い方サイト `docs/index.html`（master）も更新

スタブがコードとズレると「存在する API に波線が出る」「無い API が補完される」で逆に害になる。
