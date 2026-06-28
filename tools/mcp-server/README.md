# DX12 Engine MCP server

起動中のエディタを Codex / Claude Code から叩いてゲームを作るための MCP サーバ。
エディタ(C++)が `127.0.0.1:8787` で待ち受ける TCP ブリッジに、改行区切り JSON で繋ぐ。
ゲーム(封印ランタイム)ではブリッジは起動しない＝外から触れない。

## 構成
- `engineClient.ts` … TCP フレーミング + id 相関の薄いクライアント
- `index.ts` … MCP サーバ本体(stdio)。ツール3個を公開
- `test.ts` … mock エンジンで framing/相関/エラーを検証(`node test.ts`)

## ツール(初回スライス)
| ツール | 説明 |
|---|---|
| `dx12_list_entities` | 開いてるシーンのエンティティ一覧 (id, name) |
| `dx12_create_lua_component` | `assets/components/<name>.lua` を作成。返り値 `path` を attach に使う |
| `dx12_attach_lua_component` | エンティティに Lua 部品をアタッチして即リロード |

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
