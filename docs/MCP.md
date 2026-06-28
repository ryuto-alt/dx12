# MCP / AI Bridge

起動中の DX12 エディタを Claude Code / Codex から操作するための MCP(Model Context Protocol)連携ガイド。
AI がシーンを読み・エンティティを足し・コンポーネントを設定し・Lua を貼り・Play/Stop まで回せる。

- エディタ(C++)が `127.0.0.1:8787` で TCP ブリッジを待ち受ける(改行区切り JSON、単一クライアント)。
- MCP サーバ(`tools/mcp-server/`)は純 Node(v24+)製で C++ 非依存。Windows / macOS / Linux で動く。
- ゲーム(封印ランタイム)ではブリッジは起動しない=外から触れない。

---

## 1. セットアップ

```bash
cd tools/mcp-server
# Windows:
./install.ps1
# Linux / macOS:
./install.sh
```

スクリプトは Node v24+ を確認し、`npm install` と自己テスト(`npm test`、エンジン不要)を実行したあと、
絶対パス解決済みの登録コマンドを表示する。手動でやる場合:

```bash
cd tools/mcp-server
npm install
node test.ts        # 自己テスト(framing/相関/エラー)
```

Node v24+ が `.ts` を直接実行する(型ストリップ)ので `tsc` ビルドは不要。

---

## 2. 接続

`<REPO>` は clone した絶対パスに置換する(Windows でもパスは `/` 区切りで可。Node が解釈する)。

### Claude Code(CLI)
```bash
claude mcp add dx12-engine -- node <REPO>/tools/mcp-server/index.ts
```

### Claude Code(`.mcp.json`)
`tools/mcp-server/.mcp.json.example` をコピーして `.mcp.json` を作り、`<REPO>` を置換する。
`.mcp.json` は API トークンが入りうるため `.gitignore` 済み=各自で生成する。

```json
{
  "mcpServers": {
    "dx12-engine": {
      "command": "node",
      "args": ["<REPO>/tools/mcp-server/index.ts"],
      "env": { "DX12_MCP_HOST": "127.0.0.1", "DX12_MCP_PORT": "8787" }
    }
  }
}
```

### Codex(`~/.codex/config.toml`)
```toml
[mcp_servers.dx12-engine]
command = "node"
args = ["<REPO>/tools/mcp-server/index.ts"]
```

### 環境変数
| 変数 | 既定 | 用途 |
|---|---|---|
| `DX12_MCP_HOST` | `127.0.0.1` | エディタのホスト。別マシンの Windows エディタを叩くならその IP |
| `DX12_MCP_PORT` | `8787` | ブリッジのポート。変える場合はエディタ側 `Application.cpp` の `Start()` と合わせる |

---

## 3. 使い方(基本フロー)

1. エディタ(`DX12Engine.exe`)を起動してシーンを開く(ブリッジが 8787 で待受)。
2. AI から `dx12_list_entities` で対象 id を確認。
3. `dx12_create_entity` / `dx12_spawn_model` で物を足す(遅延処理。`name` を付けて後追い)。
4. `dx12_set_transform` / `dx12_set_component` で配置・部品を設定。
5. `dx12_create_lua_component` → `dx12_attach_lua_component` でロジックを貼る。
6. `dx12_play` で実行、`dx12_stop` で編集に戻す。`dx12_save_scene` で保存。

> 遅延処理: 生成・モデルロード・モード遷移は GPU/cmdList を伴うためフレーム境界で実行される。
> `dx12_create_entity` / `dx12_spawn_model` は id を即返さない(`queued`)ので、`name` で `dx12_list_entities` から引く。

---

## 4. ツール一覧(全20)

MCP ツール名は `dx12_` 接頭辞付き。応答は `result`(下表)か、失敗時は `エラー: <message>`。

### シーン / エンティティ読み取り
| ツール | params | 返り値 |
|---|---|---|
| `dx12_list_entities` | `{}` | `[{ id, name }]` |
| `dx12_get_entity` | `{ entity:int }` | 全コンポーネントと値の JSON |
| `dx12_list_scenes` | `{}` | `[{ path, name }]`(`assets/scenes/` 配下の `.json`) |
| `dx12_list_assets` | `{ type?: "model"\|"texture"\|"script"\|"audio"\|"scene"\|"prefab" }` | `[{ path, type, name }]`(省略で全種別) |
| `dx12_get_mode` | `{}` | `{ mode: "Editor"\|"Playing" }` |
| `dx12_get_log` | `{ lines?:int=50 }` | `[ "ログ行", ... ]`(末尾 N 行。ファイル無しは空配列) |

### エンティティ生成 / 削除
| ツール | params | 返り値 |
|---|---|---|
| `dx12_create_entity` | `{ type:"box"\|"sphere"\|"plane"\|"empty", name?, position?:[x,y,z] }` | `{ queued:true }`(遅延) |
| `dx12_spawn_model` | `{ path:string(assets相対 .gltf/.glb/.fbx/.obj), position?:[x,y,z], name? }` | `{ queued:true, name }`(遅延) |
| `dx12_delete_entity` | `{ entity:int }` | `ok`(子ごと、Undo可。遅延) |
| `dx12_rename_entity` | `{ entity:int, name:string }` | `{ name }`(重複時は連番付与) |

### Transform / コンポーネント / 階層
| ツール | params | 返り値 |
|---|---|---|
| `dx12_set_transform` | `{ entity:int, position?, rotation?(Euler度), scale? }` | `ok`(指定分のみ更新) |
| `dx12_set_component` | `{ entity:int, component:string, data:object }` | `ok`(無ければ追加・有れば上書き) |
| `dx12_remove_component` | `{ entity:int, component:string }` | `ok`(core 部品は不可) |
| `dx12_set_parent` | `{ entity:int(child), parent?:int }` | `ok`(parent 省略で親解除。サイクルは拒否) |

### Lua スクリプト
| ツール | params | 返り値 |
|---|---|---|
| `dx12_create_lua_component` | `{ name:string, code:string }` | `{ path }`(構文検証付。`assets/components/<name>.lua`) |
| `dx12_attach_lua_component` | `{ entity:int, script:string(assets相対) }` | `ok`(実行は Play 時) |

### シーン I/O / 実行制御
| ツール | params | 返り値 |
|---|---|---|
| `dx12_save_scene` | `{ path?:string(assets相対 例 "scenes/title.json") }` | `{ path }`(省略時は現在のシーンへ上書き) |
| `dx12_open_scene` | `{ path:string(assets相対) }` | `{ queued:true }`(遅延ロード) |
| `dx12_play` | `{}` | `{ mode:"Playing" }` or `{ queued:true }` |
| `dx12_stop` | `{}` | `{ mode:"Editor" }` or `{ queued:true }` |

### `component` で使える jsonKey(set/remove 共通)
`pointLight` / `directionalLight` / `spotLight` / `camera` / `rigidBody` /
`boxCollider` / `sphereCollider` / `capsuleCollider` / `characterController` /
`tag` / `dataComponent` / `sprite2D`

- `transform` は `dx12_set_component` で設定可(`position` / `rotation` / `scale`)。ただし削除は不可。
- `data` の形は `dx12_get_entity` が返す jsonKey と同じ(例 `component:"pointLight", data:{color,intensity,range}`)。
- `meshRenderer` は所有メッシュとの整合が要るため set/remove 非対応。`name` / `transform` は core 不変で remove 不可。

---

## 5. MCP / AI Bridge パネル(エディタ内モニタ)

エディタの `MCP / AI Bridge` ウィンドウ(ツールメニューから開閉)で接続状態を監視できる。送信はしない読み取り専用モニタ。

- **ヘッダ**: 待受 `127.0.0.1:<Port>` と接続インジケータ(接続中=緑●、未接続=灰○)。ゲームモードでは「無効」表示。
- **使い方**: 接続中の MCP サーバ名 / 公開ツール数(新規13+既存7=20)を表示。
- **直近コマンド履歴**: 受けた `method` と成否(✓ / ✗)を新しい順に最大64件。AI が何を叩いたか追える。

---

## 6. セキュリティモデル

ブリッジは **エディタ専用**(ゲーム=封印ランタイムでは起動しない)。

- 受けるのは **`127.0.0.1` のみ**。外部ホストからは到達不可。
- 最初の1行が JSON オブジェクト(`{`)で始まらない接続は即切断 → ブラウザの HTTP/WebSocket ドライブバイ(localhost CSRF)を遮断。
- パス系ツール(`attach_lua_component` / `spawn_model` / `save_scene` / `open_scene`)は **assets 相対のみ**。
  絶対パス・`..`・`\`・`:` を拒否し、assets 外のファイルに触らせない。
- `create_lua_component` の Lua は書き込み前に構文チェック(コンパイルのみ・非実行)。

### 残存リスク
ポートは無認証。同一マシンの別ローカルプロセス(=既にユーザ権限を持つ)は接続でき、貼った Lua は `io` 有効の state で走る。
**エディタ=信頼された開発機**という前提で許容。アップグレード経路: ポートのトークン認証、MCP 実行用に `io`/`os` を外した別 sol::state。

---

## 7. トラブルシュート

| 症状 | 対処 |
|---|---|
| `エディタに繋がらへん (127.0.0.1:8787)` | エディタが起動してるか・シーンを開いてるか確認。ゲームモードではブリッジは起動しない。 |
| `engine timeout` | 重い遅延処理待ち、またはエディタがフレームを回してない(別モーダル等)。エディタを前面にしてリトライ。 |
| ポート競合 | クライアント側 `DX12_MCP_PORT` とエディタ側 `Application.cpp` の `Start()` を同じ値に。 |
| `node が見つからへん` / `.ts` が実行できない | Node **v24+** を入れる(型ストリップに必要)。`node --version` で確認。 |
| ツールが AI 側に出ない | `claude mcp add` 済みか、`.mcp.json` の `args` パスが正しいか確認。MCP サーバ登録後はクライアント再起動。 |
| `queued:true` のまま id が分からない | 遅延処理。少し待って `dx12_list_entities` を `name` で引く。 |
| 別マシンから繋ぎたい | `DX12_MCP_HOST` をエディタ機の IP に。エディタは `127.0.0.1` 待受なので、リモート時は SSH ポートフォワード等で localhost に橋渡しする。 |

---

関連: `tools/mcp-server/README.md`(サーバ構成)、`docs/SCRIPTING.md` / `docs/SCRIPT_COMPONENTS.md`(Lua)。
