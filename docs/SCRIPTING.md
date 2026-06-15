# ゲームスクリプト API ガイド（Lua）

このエンジンのゲームロジックは Lua で書く。エンティティに `.lua` をアタッチすると、
`OnStart(self)` が1回、`OnUpdate(self, dt)` が毎フレーム呼ばれる。

このドキュメントは **人間（プランナー）の入門書** であり、同時に
**Claude Code がスクリプトを生成するときの参照**でもある。まずは「高レベルAPI」だけ
覚えれば、たいていのゲームは書ける。低レベルAPIは細かい制御が要るときだけ使う。

---

## いちばん短い例（レベル1相当）

```lua
local done = false

function OnStart(self)
    player = actor("Player", { speed = 9, solid = { "Wall1", "Wall2" } })
    goal   = actor("Goal")
end

function OnUpdate(self, dt)
    if done then return end
    player:moveTopDown(dt)                                   -- WASD移動＋壁の当たり判定
    cameraFollow(player, { height = 13, back = 8, pitch = 55 })  -- 見下ろし追従
    if player:reached(goal, 1.4) then
        done = true; goToScene("scenes/clear.json", 0.7)    -- ゴールでクリアへ
    end
end
```

ポイント:
- `player` や `goal` は `OnStart` で `local` を付けずに代入する → `OnUpdate` でも使える。
- 1回だけ走らせたい処理（シーン遷移など）は `done` のようなフラグで囲う。

---

## 高レベル API（まずこれだけでOK）

### actor — 名前付きエンティティを操作する
シーン内のエンティティを「名前」で掴むラッパー。

```lua
player = actor("Player", {
    speed = 9,                  -- moveTopDown の移動速度（既定 5）
    solid = { "Wall1", "Wall2" },-- 当たり判定する相手の名前（1個なら "Wall1" でも可）
    half  = 0.5,                -- 自分の当たり判定半径（XZ・既定 0.5）
})
```

| メソッド | 説明 |
|---|---|
| `a:moveTopDown(dt)` | WASD で XZ 平面を移動。`solid` の相手に当たると軸ごと止まる（壁沿いにスライド）。矢印キーにするなら `a:moveTopDown(dt, "Arrows")` |
| `a:pos()` | 現在位置 `Vec3` を返す（`.x` `.y` `.z`） |
| `a:setPos(x, y, z)` | 位置を直接セット |
| `a:reached(other, radius)` | 別の actor に近づいたか（XZ距離 < radius）。`radius` 既定 1.0 |
| `a:valid()` | エンティティが存在するか |

### cameraFollow — 見下ろしカメラ追従
`CameraComponent` を持つエンティティ（既定名 `GameCamera`）を、対象の上から追わせる。

```lua
cameraFollow(player, { height = 13, back = 8, pitch = 55 })
-- name=追従させるカメラ名(既定 "GameCamera"), height=高さ, back=後ろ距離, pitch=見下ろし角
```

### 入力
```lua
if keyDown("W")     then ... end   -- 押している間ずっと true
if keyPressed("ESC")then ... end   -- 押した瞬間だけ true
```
使えるキー名: `W A S D E Q  UP DOWN LEFT RIGHT  SPACE SHIFT TAB ENTER ESC`

### シーン遷移
```lua
goToScene("scenes/clear.json", 0.7)  -- フェード付きで指定シーンへ（秒数省略可）
win()                                -- sceneflow の「次のシーン」へ
```

### その他
```lua
log("message")                       -- ログ出力（デバッグ用）
ui:text(x, y, "HUD text", size, r, g, b, a)   -- 画面に文字（再生中のみ）
```

---

## 低レベル API（細かい制御が要るとき）

高レベル API は内部でこれらを使っている。直接使ってもよい。

```lua
local e = scene:findEntity("Player")     -- 名前でエンティティ取得
if e and e:isValid() then
    e.transform.position = Vec3.new(0, 0.5, 0)  -- 位置（.rotation .scale も同様）
end

input:isKeyDown(KEY_W)                    -- KEY_W / KEY_ESCAPE ... 定数
input:isKeyPressed(KEY_SPACE)

local hit = physics:raycast(origin, dir, maxDist)  -- RaycastHit{hit,distance,point,normal}
physics:applyImpulse(e, Vec3.new(0, 5, 0))

fadeToScene("scenes/title.json", 0.5)    -- 低レベルの遷移
loadScene("scenes/level1.json")          -- 即ロード（フェード無し）
nextScene()                              -- sceneflow の次へ
```

---

## 当たり判定について

- 高レベルの `actor(..., { solid = {...} })` + `moveTopDown` は、相手を **箱(AABB, XZ平面)** とみなした
  軽量な当たり判定。`box` プリミティブは「半径 0.5 × scale」で扱う。トップダウン移動向け。
- 物理的な剛体衝突（落下・反発・スタック）が要るなら、エディタで `RigidBody` と各コライダーを付けて
  物理エンジン（Jolt）に任せる。スクリプト不要。
- 「侵入した瞬間に発火」するトリガーや衝突イベント（onCollision/onTrigger）は今後の追加予定。

---

## 命名のおすすめ（プランナー / AI 共通）

- プレイヤー = `Player`、ゴール = `Goal`、見下ろしカメラ = `GameCamera`、壁 = `Wall1`,`Wall2`,…
- この命名にしておくと、高レベル API の既定値とそのまま噛み合う。
