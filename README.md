# DX12 Engine

自作の DirectX 12 ゲームエンジン + エディタ（`DX12Engine.exe` がエディタ兼ランタイム）。
ECS(entt) / 物理(Jolt) / Lua スクリプト / glTF・FBX モデル / シーンの暗号化パック配布に対応。

目玉のひとつが **MCP / AI Bridge** — 起動中のエディタを Claude Code / Codex から
操作してゲームを組み立てられる（エンティティ生成・コンポーネント設定・Lua アタッチ・
シーン保存・Play/Stop まで、全20ツール）。

---

## ビルド（Windows 専用）

VS2026 の vcvars を読んでから CMake(Ninja) で増分ビルドする。

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build "build\release"
```

成果物 = `build\release\DX12Engine.exe`。

> 初回はパック暗号鍵 `src/core/generated/AssetKey.h` を `tools/gen_asset_key.ps1` で生成する
> （gitignore 対象。editor/runtime が同一ツリーなら鍵が一致する）。

### ゲームの配布
エディタでプロジェクトを開く → Build、または CLI:
```bat
DX12Engine.exe --build "<projectDir>"
```
`<projectDir>/build/game/` に `Game.exe` + DLL + 暗号化 `game.pak` が出る（平文アセットは出さない）。

---

## MCP / AI Bridge（AI でゲームを作る）

エディタが `127.0.0.1:8787` で待ち受ける TCP ブリッジに、Node 製 MCP サーバ
（`tools/mcp-server/`、C++ 非依存・Win/macOS/Linux）が繋ぐ。

```bash
cd tools/mcp-server
./install.ps1        # Windows（Linux/macOS は ./install.sh）
```
スクリプトが `npm install` → 自己テスト → 登録コマンドを表示する。あとはエディタを起動して
シーンを開き、AI から `dx12_list_entities` などを呼ぶだけ。エディタの「ウィンドウ →
MCP / AI Bridge」パネルで接続状態とコマンド履歴を確認できる。

**完全な手順・全20ツールのリファレンス・セキュリティモデルは [docs/MCP.md](docs/MCP.md)。**

ゲーム（封印ランタイム）ではブリッジは起動しない＝外から触れない。

---

## リポジトリ構成

| パス | 中身 |
|---|---|
| `src/` | エンジン本体（core / renderer / ecs / physics / scripting / editor …） |
| `tools/mcp-server/` | MCP サーバ（Node v24+、`index.ts`）と接続スクリプト |
| `docs/` | 設計ドキュメント（MCP 連携・アセット保護など） |
| `tests/` | 単体テスト |

---

## ライセンス
[MIT](LICENSE)。
