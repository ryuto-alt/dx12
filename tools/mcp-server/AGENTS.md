# DX12 MCP サーバ — AI エージェント運用ガイド

Claude Code と Codex の両方が読む運用ルール集。
このファイルを読んだら **「最重要ルール」から先に把握する** こと。

---

## ★ 最重要ルール: entityId は安定。名前で探し直さない。

`dx12_create_entity` / `dx12_spawn_model` などの遅延同期ツールは、
フレーム境界での処理完了後に **本物の `entityId`** を直接返す。

```
# 正しいパターン
result = dx12_create_entity(type:"box", name:"Floor")
# → {entityId: 42, name: "Floor", sceneGeneration: 7}

dx12_set_transform(entity: 42, position:[0,-1,0])   ← 返ってきた entityId をそのまま使う
```

```
# 間違いパターン(やってはいけない)
dx12_create_entity(type:"box", name:"Floor")
dx12_list_entities()            ← "Floor" を名前で探し直す ← 不要・廃止
dx12_find_entity(name:"Floor")  ← 同上 ← 廃止
```

旧仕様の `{queued:true}` はもう返ってこない。
**返ってきた `entityId` を変数に持ち、以降の操作で一貫して使い続ける。**

---

## 典型ワークフロー

### 1. コンポーネントを設定する前に describe_components で確認

```
dx12_describe_components({component: "pointLight"})
# → fields: [{name:"color",type:"vec3",...}, {name:"intensity",type:"float",...}, ...]

dx12_set_component({entity: 42, component: "pointLight", data: {color:[1,0.8,0.5], intensity:3.0, range:8.0}})
```

### 2. エンティティ生成 → 配置 → コンポーネント設定

```
# 箱を作る
r = dx12_create_entity(type:"box", name:"RedBox")
# → {entityId: 55, ...}

# 配置
dx12_set_transform(entity: 55, position:[2,0,0], scale:[1,2,1])

# 色を付ける(PBR)
dx12_set_pbr(entity: 55, metallic:0.0, roughness:0.8)

# ライトを追加
dx12_set_component(entity: 55, component:"pointLight", data:{color:[1,0,0], intensity:5.0, range:10.0})
```

### 3. モデルをスポーン

```
r = dx12_spawn_model(path:"models/enemy.glb", position:[0,0,5], name:"Enemy_01")
# → {entityId: 88, name: "Enemy_01", sceneGeneration: 12}

dx12_set_transform(entity: 88, rotation:[0,180,0])
```

### 4. Lua スクリプトを貼る

```
dx12_create_lua_component(name:"Rotate", code:[[
  function Update(dt)
    local t = entity:GetTransform()
    t.rotation.y = t.rotation.y + 30 * dt
    entity:SetTransform(t)
  end
]])
# → {path: "components/Rotate.lua"}

dx12_attach_lua_component(entity: 88, script:"components/Rotate.lua")
```

### 5. Play/Stop して確認

```
dx12_play()
# → {mode:"Playing", sceneGeneration:13}

# ゲームが動いているのを確認したら止める
dx12_stop()
# → {mode:"Editor", sceneGeneration:13}
```

### 6. 変更をスクショで確認(検証ループ)

```
dx12_focus_and_screenshot(entity: 88)   # focus_camera + screenshot を自動でやってくれる
dx12_get_log(lines:30)                  # エラー/警告を確認
```

### 7. 保存

```
dx12_save_scene()   # 現在のシーンへ上書き
```

---

## batch でまとめ作成(往復削減)

複数のエンティティや設定を一気に送る場合は `dx12_batch` を使う。

```json
{
  "ops": [
    {"method": "create_entity", "params": {"type":"box", "name":"Wall_N", "position":[0,0,-5]}},
    {"method": "create_entity", "params": {"type":"box", "name":"Wall_S", "position":[0,0,5]}},
    {"method": "create_entity", "params": {"type":"box", "name":"Wall_E", "position":[5,0,0]}},
    {"method": "create_entity", "params": {"type":"box", "name":"Wall_W", "position":[-5,0,0]}}
  ],
  "stopOnError": true
}
```

返り値の `results[i].result.entityId` にそれぞれの id が入る。

---

## MODE_CONFLICT(3): Playing 中は生成系が失敗する

Playing 中に `create_entity` / `spawn_model` / `delete_entity` / `open_scene` 等を呼ぶと
`error_code=3(MODE_CONFLICT)` が返る。

**対処**: 先に `dx12_stop()` で Editor モードに戻してから再試行する。

```
dx12_get_mode()         # 現在のモードを確認
# → {mode:"Playing"}

dx12_stop()             # 先に止める
dx12_create_entity(...)  # その後に生成
```

---

## よくある間違い

### transform は remove 不可

`dx12_remove_component(component:"transform")` は常にエラー(core 不変)。
transform の変更は `dx12_set_transform` または `dx12_set_component(component:"transform", data:{...})` を使う。

### meshRenderer は set/remove 不可

メッシュは `dx12_spawn_model` でモデルごとスポーンする。
既存の meshRenderer を差し替えたい場合は `delete_entity` → `spawn_model` の手順で。

### tags は文字列配列で渡す

jsonKey は `tags`(複数形)。`tag` は無効で `UNKNOWN_COMPONENT(6)` になる。

```
# 正しい
dx12_set_component({entity:42, component:"tags", data:["enemy","dynamic"]})

# 間違い(キー名 / オブジェクト形式)
dx12_set_component({entity:42, component:"tag",  data:["enemy"]})        # tag は無効
dx12_set_component({entity:42, component:"tags", data:{tags:["enemy"]}}) # data は配列で渡す
```

### rigidBody と characterController は排他

同一エンティティに両方はアタッチできない。どちらか一方を選ぶ。

### quaternion と rotation は同時に指定しない

`dx12_set_transform` で `rotation`(Euler度) と `quaternion` の両方を送った場合の動作は不定。
どちらか一方だけ使う。

### idempotency_key はリトライ時だけ使う

通常の生成に付ける必要はない。タイムアウト等でリトライするときに同じキーを再利用して
重複を防ぐための仕組み。

---

## シーンを開き直したら entityId を取り直す

`dx12_open_scene` / `dx12_new_scene` の後は以前の `entityId` は無効になる(`sceneGeneration` が変わる)。
古い id を使うと `NOT_FOUND(1)` が返る。各レスポンスの `sceneGeneration` を見て、変わっていたら引き直す:

```
dx12_ping()             # sceneGeneration を確認
dx12_list_entities()    # 現在のエンティティ一覧を再取得
```

※ `error_code=4 (STALE_SCENE)` は将来用に予約されているが現状は未送出。今は上記のとおり
`NOT_FOUND(1)` + `sceneGeneration` の変化で判断する。

---

## エラーコード早見表

| コード | 意味 | 典型的な対処 |
|--------|------|------------|
| 1 | NOT_FOUND | entityId / path / jsonKey を確認 |
| 2 | INVALID_PARAM | describe_components でフィールド型を確認 |
| 3 | MODE_CONFLICT | dx12_stop → 再試行 |
| 4 | STALE_SCENE | dx12_ping → dx12_list_entities で引き直し |
| 6 | UNKNOWN_COMPONENT | dx12_describe_components で jsonKey を確認 |
| 7 | INTERNAL | dx12_get_log でエンジンログを確認 |

---

## 禁止事項まとめ

- **`dx12_find_entity` で名前検索してから使う** → 廃止パターン。返ってきた entityId を使う。
- **`dx12_list_entities` で名前引きする** → 廃止パターン。create/spawn の返り値を使う。
- **Playing 中に生成系を呼ぶ** → MODE_CONFLICT。先に stop する。
- **transform / name を remove_component で消す** → core 不変。不可。
- **meshRenderer を set_component で差し替える** → 不可。delete → spawn_model で。
