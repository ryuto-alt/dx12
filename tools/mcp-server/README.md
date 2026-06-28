# DX12 Engine MCP server

起動中のエディタを Codex / Claude Code から叩いてゲームを作るための MCP サーバ。
エディタ(C++)が `127.0.0.1:8787` で待ち受ける TCP ブリッジに、改行区切り JSON で繋ぐ。
ゲーム(封印ランタイム)ではブリッジは起動しない＝外から触れない。

接続手順・全ツール詳細・トラブルシュートは **[../../docs/MCP.md](../../docs/MCP.md)**。
セットアップは `install.ps1`(Windows)/ `install.sh`(Linux・macOS)で自動化できる(`npm install` + 自己テスト + 登録コマンド表示)。クローン用の設定テンプレは `.mcp.json.example`。

## 構成
- `engineClient.ts` … TCP フレーミング + id 相関の薄いクライアント
- `index.ts` … MCP サーバ本体(stdio)。ツール20個を公開
- `test.ts` … mock エンジンで framing/相関/エラーを検証(`node test.ts`)

## ツール
### エンティティ基本
| ツール | 説明 |
|---|---|
| `dx12_list_entities` | 開いてるシーンのエンティティ一覧 (id, name) |
| `dx12_get_entity` | エンティティの全コンポーネントと値を JSON で読む |
| `dx12_create_entity` | エンティティ生成 (box/sphere/plane/empty, name, position)。遅延処理 |
| `dx12_delete_entity` | エンティティ削除 (子ごと, Undo可)。遅延処理 |
| `dx12_set_transform` | Transform 設定 (位置/回転/スケール、指定分のみ) |
| `dx12_set_parent` | 親を設定/解除 (サイクルは拒否) |
| `dx12_rename_entity` | 名前変更 (重複は連番付与、確定 name を返す) |

### コンポーネント
| ツール | 説明 |
|---|---|
| `dx12_set_component` | コンポーネントを設定(上書き)。jsonKey + data (get_entity と同形)。meshRenderer 非対応 |
| `dx12_remove_component` | コンポーネント除去。transform/name は除去不可 |
| `dx12_create_lua_component` | `assets/components/<name>.lua` を作成 (構文検証付き) |
| `dx12_attach_lua_component` | エンティティに Lua 部品をアタッチ (実行は Play 時) |

### シーン / アセット
| ツール | 説明 |
|---|---|
| `dx12_save_scene` | シーン保存 (assets 相対 path、省略で上書き) |
| `dx12_open_scene` | シーンを開く (assets 相対 path)。遅延処理 |
| `dx12_list_scenes` | `assets/scenes` 配下の .json 一覧 (path, name) |
| `dx12_list_assets` | assets 配下のアセット一覧 (type フィルタ可: model/texture/script/audio/scene/prefab) |
| `dx12_spawn_model` | モデル (.gltf/.glb/.fbx/.obj) を生成。GPU ロードのため遅延処理 |

### 再生制御 / 状態
| ツール | 説明 |
|---|---|
| `dx12_play` | Editor -> Playing。遅延処理 |
| `dx12_stop` | Playing -> Editor (スナップショット復元)。遅延処理 |
| `dx12_get_mode` | 現在のモード (Editor / Playing) |
| `dx12_get_log` | `dx12_engine.log` の末尾 N 行 (既定 50) |

生成/削除/モデル生成/シーン読込/再生切替はメッシュ構築や GPU、重い遷移を伴うためフレーム境界で遅延処理。`create_entity`/`spawn_model` は id を即返さないので、`name` を付けて `dx12_list_entities`/`dx12_get_entity` で引く。

## セットアップ
```bash
cd tools/mcp-server
npm install
node test.ts        # 自己テスト(エンジン不要)
```
Node v24+ が `.ts` を直接実行する(型ストリップ)ので tsc ビルドは不要。

## 接続
### Claude Code
```bash
claude mcp add dx12-engine -- node C:\Users\ryuto\Documents\dx12\tools\mcp-server\index.ts
```
または `.mcp.json`:
```json
{
  "mcpServers": {
    "dx12-engine": {
      "command": "node",
      "args": ["C:\\Users\\ryuto\\Documents\\dx12\\tools\\mcp-server\\index.ts"]
    }
  }
}
```

### Codex (`~/.codex/config.toml`)
```toml
[mcp_servers.dx12-engine]
command = "node"
args = ["C:\\Users\\ryuto\\Documents\\dx12\\tools\\mcp-server\\index.ts"]
```

## 使い方
1. エディタ(`DX12Engine.exe`)を起動してシーンを開く(ブリッジが 8787 で待ち受け)
2. AI から `dx12_list_entities` → 対象 id を確認
3. `dx12_create_lua_component` で `.lua` を作る → 返った `path`
4. `dx12_attach_lua_component` で id に貼る → Play で動く

ポート変更は env `DX12_MCP_PORT`(クライアント側)と `Application.cpp` の `Start(8787)` を合わせる。

## セキュリティ(設計判断・残存リスク)
ブリッジは**エディタ専用**(`!m_isGameMode`)。ゲーム(封印ランタイム)では起動せず、外から触れない。

- 受けるのは `127.0.0.1` のみ。外部ホストからは到達不可。
- 最初の1行が JSON オブジェクト(`{`)で始まらない接続は即切断 → ブラウザの HTTP/WebSocket ドライブバイ(localhost CSRF)を遮断。
- `attach` の `script` は assets 相対のみ(絶対パス/`..`/`\`/`:` を拒否)＝ assets 外の任意ファイルを Lua 実行させない。
- `create` の Lua は書き込み前に構文チェック(コンパイルのみ・非実行)。

**残存リスク**: ポートは無認証。同一マシンの**別ローカルプロセス**(=既にユーザ権限を持つ)は接続でき、貼った Lua は `io` ライブラリ有効の state で走る(任意ファイル read/write、PUC-Lua なら `io.popen` でシェル)。エディタ=信頼された開発機という前提で許容。**アップグレード経路**: ポートにトークン認証(エンジンが生成しユーザのみ読めるファイルに置く→クライアントが添付)、MCP 実行用に `io`/`os` を外した別 sol::state。
