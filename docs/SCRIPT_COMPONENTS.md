# スクリプトコンポーネント & プレハブ

「振る舞いを再利用部品（コンポーネント）にして、エディタでもコードでも作れる」ための仕組み。
人間は Inspector でポチポチ、Claude Code は `.lua` と `.prefab`（テキスト）を書くだけ。同じファイルを両者が編集する。

---

## 1. スクリプトコンポーネント（プロパティ付き Lua）

`.lua` の先頭で `properties` を宣言すると、その値は **Inspector に自動で編集欄が出て / シーンに保存され / Play 時に `self.<name>` として注入** される。
これで「巨大な 1 個のコントローラに全部書く」のをやめて、**部品を貼って数値だけ変える** Unity ライクな作り方ができる。

### 宣言フォーマット

```lua
properties = {
  { name = "speed", type = "float", default = 90.0, min = -720, max = 720, label = "回転速度" },
  { name = "axis",  type = "vec3",  default = {0, 1, 0} },
  { name = "on",    type = "bool",  default = true },
}

function OnStart(self) end          -- Play 開始時に 1 回
function OnUpdate(self, dt)         -- 毎フレーム
  if not self.on then return end
  local t = self.transform
  t.rotation = Vec3.new(t.rotation.x, t.rotation.y + self.speed * dt, t.rotation.z)
end
```

### 型一覧

| `type`   | Inspector の編集欄 | `self.<name>` の値 |
|----------|--------------------|--------------------|
| `float`  | スライダー / ドラッグ（`min`/`max` 指定でスライダー） | number |
| `int`    | 整数スライダー / ドラッグ | number(整数) |
| `bool`   | チェックボックス | boolean |
| `string` | テキスト入力 | string |
| `vec3`   | XYZ ドラッグ | `Vec3`（`.x/.y/.z`） |
| `color`  | カラーピッカー | `Vec3`（RGB 0..1） |

- `label` は省略可（省略時は `name` をそのまま表示）。
- `default` は省略すると 0 / false / "" / (0,0,0) になる（color のみ既定は白）。
- プロパティの値はエンティティ**個別**。同じ `.lua` を複数エンティティに貼っても各自の値を持つ。
- `.lua` を編集して `properties` を足し引きしたら、Inspector の **Reload** を押すとスキーマが再読込される。

### self に最初から入っているもの

| キー | 内容 |
|------|------|
| `self.entity`    | エンティティ ID（u32） |
| `self.name`      | エンティティ名 |
| `self.transform` | Transform（`.position` `.rotation` `.scale` は `Vec3`） |
| `self.enabled`   | この LuaScript が有効か |
| `self.<prop>`    | 宣言した各プロパティの値 |

グローバル API（`scene` / `input` / `camera` / `audio` / `physics` / `fx` / `vfx` / `ui` / `KEY_*` …）は従来どおり全スクリプトから使える。

### 貼り方
- **人間**: AssetBrowser から `.lua` を Hierarchy の行か Inspector パネルへドロップ → Inspector の「Lua Script > プロパティ」で調整。
- **Claude Code / コード**: シーン JSON のエンティティに `luaScript` を書く（下記）。

```json
{
  "name": "Coin",
  "transform": { "position": [0,1,0], "rotation": [0,0,0], "scale": [1,1,1] },
  "primitive": "box",
  "luaScript": {
    "scriptPath": "components/Spinner.lua",
    "enabled": true,
    "props": [
      { "name": "speed", "type": "float", "value": 180.0 },
      { "name": "axis",  "type": "vec3",  "value": [0, 1, 0] },
      { "name": "enabledSpin", "type": "bool", "value": true }
    ]
  }
}
```

`props` の `type` は自己記述的なのでスキーマ無しで復元できる。`value` は `float/int`=数値、`bool`=真偽、`string`=文字列、`vec3/color`=`[x,y,z]`。
`props` を省略すると `.lua` の `default` が使われる。

サンプル: [`assets/components/Spinner.lua`](../assets/components/Spinner.lua)

> **フォルダ構成（重要）**: プロジェクトの中身は **`assets/` の下にまとめる**のが基本。
> `scriptPath` は `assets/` からの相対パスで解決される（`"components/Spinner.lua"` → `assets/components/Spinner.lua`）。
> 推奨レイアウト:
> ```
> MyGame/
>   assets/
>     components/*.lua   ← エンティティに貼る振る舞い（このドキュメントの主役）
>     scenes/*.json
>     prefabs/*.prefab
>     models/ textures/ audio/ …
>     game.lua           ← (任意) 起動時に1度だけ読まれるグローバルフック
>   MyGame.dx12proj
> ```
> グローバル `game.lua` は `scripts/game.lua` でも `assets/game.lua` でも読まれる（後者推奨＝全部 assets/ 配下で完結）。
> 無くても動く（その場合ログに警告が出るだけ）。**「assets = ゲームの中身全部、code も data も」** と覚えればよい。

---

## 2. プレハブ（再利用テンプレート）

エンティティ（+子）を 1 ファイルにまとめて、何度でも配置できるテンプレート。

### 作る
- **人間**: Hierarchy でエンティティを右クリック →「プレハブにする」。`assets/prefabs/<名前>.prefab` に保存される。
- **コード**: `.prefab` は JSON（シーンと同形式 + `parent` はローカル index 参照）。手で書いてもよい。

### 置く
- **人間**: AssetBrowser から `.prefab` をシーンビューへドロップ、またはダブルクリック / 右クリック「シーンに追加」。
- 子も含めてサブツリーごと生成される。配置は 1 回の Undo/Redo で戻せる。

### `.prefab` の形

```json
{
  "version": 1,
  "prefab": true,
  "entities": [
    { "name": "Turret",     "transform": { "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1] }, "primitive": "box" },
    { "name": "Barrel",     "transform": { "position": [0,1,0], "rotation": [0,0,0], "scale": [0.3,0.3,1] }, "primitive": "box", "parent": 0 }
  ]
}
```

- `entities[0]` がルート。`parent` は同じ配列内の index（ルートは親を持たない）。
- 各エンティティは通常のシーンエンティティと同じキー（`meshRenderer` / `pointLight` / `luaScript` / `boxCollider` …）を持てる。

---

## まとめ: どう作るか

| やりたいこと | 人間（エディタ） | Claude Code / コード |
|---|---|---|
| 新しい振る舞いを足す | `.lua` を書いて D&D で貼る | `assets/scripts/components/X.lua` を書く |
| 振る舞いの数値を調整 | Inspector のプロパティ欄 | シーン JSON の `luaScript.props` |
| モデル+構成を再利用 | 右クリック「プレハブにする」→ D&D | `assets/prefabs/X.prefab` を書いて配置 |

エンジン本体（C++）を触らずに、ファイルを足すだけでコンポーネントとプレハブが増やせる。
