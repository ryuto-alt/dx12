# 作業ログ / 引き継ぎメモ

複数マシン（自宅 / 学校）で続きをやるための、直近の実装内容と再開手順のメモ。
詳細な設計は各 `docs/*.md`、MCP は [docs/MCP.md](MCP.md)、ビルドは [README.md](../README.md) を見る。

---

## 別マシンで再開するとき（学校 PC など）

```bash
git clone https://github.com/ryuto-alt/dx12.git
cd dx12
git checkout feat/engine-mcp        # 現在の作業ブランチ
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

---

## 次にやる候補 / TODO

- FPS/TPS/empty の既存アイコンは据え置き。全部の作り直し要望が来たら `tools/gen_icons.ps1` で対応。
- `WM_SYSKEYDOWN`/`WM_SYSKEYUP`（Alt 系）は未配線。Alt 入力を拾いたくなったら `Window.cpp` に追加。
