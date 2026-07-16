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

### ゲームパッド（Xbox コントローラー）
```lua
if padDown("A")     then jump() end        -- 押している間ずっと true
if padPressed("RB") then dash() end        -- 押した瞬間だけ true
if padReleased("A") then release() end     -- 離した瞬間だけ true（チャージ攻撃向け）

local lx, ly = padStick("left")            -- 左スティック(-1..1)。移動入力に使う
local rt = padTrigger("right")             -- 右トリガー(0..1)。アクセル/ADS等

padVibrate(0.6, 0.3, 0.2)                  -- 低周波0.6・高周波0.3の振動を0.2秒だけ
```
使えるボタン名: `A B X Y  LB RB  BACK START  LSTICK RSTICK  DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT`
2台目以降は各関数の最後に `pad` 引数（0始まり）を渡す: `padDown("A", 1)`。`padConnected(pad?)` で接続確認。
低レベルAPI（`input:isPadButtonDown` 等、`PAD_*` 定数）は下記§と API_REFERENCE.md の Gamepad セクション参照。

### シーン遷移
```lua
goToScene("scenes/clear.json", 0.7)  -- フェード付きで指定シーンへ（秒数省略可）
win()                                -- sceneflow の「次のシーン」へ
```

### 時間（time）
```lua
time.now()                           -- Play開始からの経過秒
time.setScale(0)                     -- ポーズ（0.5=スローモ, 2=早送り, 1=通常）
time.after(2.0, function() ... end)  -- 2秒後に1回実行
local id = time.every(1.0, spawn)    -- 1秒ごとに繰り返し
time.cancel(id)                      -- 解除
```
`setScale` は `OnUpdate` の dt に掛かるので、既存の移動コードはそのままスローモ/ポーズに追従する。
ポーズ中も動かしたい処理（メニュー等）は `time.realDt()` / `time.realtime()` を使う。

```lua
-- 共有ビデオ時計: ステージに1本流れる"動画時間"。ギミックは t の純関数で動きを書く
time.video.start(10, { skipCost = 1.0 })   -- 10秒。skip すると残り時間も減る
local t = time.video.localTime(self)        -- 動画時間 + 自分のオフセット
time.video.skip("Wall1", 1.0)               -- Wall1 だけ1秒先送り(-1.0 で巻き戻し)
time.video.remaining()                      -- 残り時間(HUD / time.video.finished() でゲームオーバー)

-- 個別時計: オブジェクト単位で進む/止まる/スキップ
time.localTime(self)                        -- 自分の経過秒
time.scaleEntity("Enemy1", 0)               -- Enemy1 だけ停止(負で逆再生)

-- チャージ計測(弓を引く等): c:update() を毎フレーム、離した瞬間 c:released() が量を返す
local c = charge.new("E", { max = 2.0 })
```
詳細は API_REFERENCE.md の time セクションを参照。

### その他
```lua
log("hp:", hp)                       -- ログ出力（任意個・任意型を tostring 連結、[Lua] 接頭辞）
logWarn("弾切れ")                     -- 警告ログ（黄色）
logError("想定外:", state)            -- エラーログ（赤）
print("同上")                         -- log と同じ経路に出る（素の print はどこにも出ない）
ui:text(x, y, "HUD text", size, r, g, b, a)   -- 画面に文字（再生中のみ）
```
`log`/`logWarn`/`logError`/`print` の出力はエディタの **コンソールパネル**（アセットブラウザの隣タブ）にリアルタイムで出る。
重大度フィルタ（情報/警告/エラー、既定は警告+エラーのみ表示）・テキスト検索・同一メッセージの折りたたみ・行クリックで詳細ペインがある。
下部の入力欄からは Lua を1行その場で実行できる（`scene`/`fx`/`camera` などそのまま使える簡易コンソール）。
入力中は**予測変換**が出る: `time.` や `scene:fi` まで打つと候補がポップアップし、Tab で確定・↑↓ で選択・クリックで挿入できる（候補は実際の Lua 環境から動的に列挙されるので自作のグローバルも出る）。

### ゲーム内UI（コンポーネント方式）
`ui:text/button/image/rect` は**簡易/デバッグ用**の即時 API。タイトルメニューや HUD 一式など恒常的な画面は、
Hierarchy の「作成」→「UI（ゲーム内UI）」で `UICanvas`/`UIRect`/`UIImage`/`UIText`/`UIButton` コンポーネントの
ツリーをエディタで組んで作る（Unity uGUI 相当の retained-mode UI）。スクリプトからは表示中の値だけを書き換える:

```lua
scene:setUiText(scoreLabel, "SCORE: " .. tostring(score))   -- テキスト書き換え
scene:setUiColor(hpBarFill, 1, hpRatio, 0, 1)                -- 色（UIImage優先/無ければUIText）
scene:setUiFill(hpBarFill, hp / maxHp)                       -- ゲージの残量（UIImage.fillAmount 0..1）
scene:setUiVisible(pausePanel, isPaused)                     -- 表示/非表示
scene:setUiTexture(weaponIcon, "textures/ui/icon_sword.png") -- 画像差し替え
scene:setUiRotation(titleLogo, -8)                           -- 回転（度・時計回り。子孫ごと回る見た目の変換）
local deg = scene:getUiRotation(titleLogo)                   -- 現在の回転角を読む

-- スライダー/トグル（値変更は onChangeEvent で受ける。e.value に実値/1・0 が入る）
events:on("volumeChanged", function(e) audio:setMasterVolume(e.value) end)
events:on("muteToggled",   function(e) audio:setMasterVolume(e.value == 1 and 0 or 1) end)
local v  = scene:getUiSlider(volSlider)   -- 現在値を読む
scene:setUiSlider(volSlider, 0.8)         -- スクリプトから設定（イベントは発火しない）
local on = scene:getUiToggle(muteToggle)
scene:setUiToggle(muteToggle, true)
```

ゲームパッド/キーボードの UI 操作は**設定不要で常時有効**: 矢印/D-pad/左スティックでフォーカス移動、
Enter/Space/A で決定（フォーカスリング表示）。フォーカス中のスライダーは左右で値が変わる。

見た目の装飾は Inspector で設定する: `UIImage` はグラデーション・枠線・ドロップシャドウ、`UIText` は縁取り・影・カスタムフォント
（assets 相対の .ttf/.otf）、`UIRect` は `rotation`/`skewX`（度）による傾きに対応している。

**動的 UI（アニメーション/イージング）**: `UIAnimator` コンポーネント（Inspector の「✚ コンポーネント追加 > UI Animator」）で
出現アニメ（フェード/ポップ/スライド/スピン）・ボタンのホバー/押下スケール・ループ（浮遊/パルス/点滅/スピン/スウィング）をノーコード設定できる。
スクリプトからはトゥイーンで自由に動かせる:

```lua
scene:tweenUi(menu,  { dx = 200, duration = 0.4, easing = "bounce" })  -- 右へ200pxバウンス移動
scene:tweenUi(popup, { scale = 1.2, alpha = 0, duration = 0.25 })      -- 拡大しながらフェードアウト
scene:tweenUi(icon,  { rotate = 360, duration = 0.5 })                 -- 1回転（度・絶対目標値）
scene:showUi(winPanel)   -- UIAnimator の出現アニメを再生して表示
scene:hideUi(pauseMenu)  -- 出現アニメの逆再生で消す（子孫ごと。戻すのは showUi）
```
`easing` は `"linear" / "in" / "out" / "inOut" / "back"（勢い） / "bounce" / "elastic"`。
`dx/dy` はレイアウト（UIRect offset）を実際に動かすので終了位置でクリックも効く。`scale/alpha` は見た目だけ（子孫にまとめて掛かる）。
`rotate`（度・絶対目標値）も見た目だけで、`setUiRotation` の値に加算合成される。

ボタンのクリックは `events:on` で受ける（`UIButton.onClickEvent` に設定した名前で発火。`data.source` にボタンのエンティティID）:
```lua
function OnStart(self)
    events:on("start_clicked", function(data) goToScene("scenes/game.json") end)
end
```
詳細は API_REFERENCE.md の「ゲーム内UI（コンポーネント方式）」セクション、使い方サイト(`docs/index.html`)を参照。

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

input:isPadButtonDown(0, PAD_A)           -- pad=0(1台目), PAD_A / PAD_RB ... 定数
input:getPadLeftStickX(0)                 -- スティックXY・トリガー・振動も input: 経由

local hit = physics:raycast(origin, dir, maxDist)  -- RaycastHit{hit,distance,point,normal}
physics:applyImpulse(e, Vec3.new(0, 5, 0))

scene:setSpriteAlpha(e, 0.4)             -- Sprite2D の不透明度(0..1)。半透明演出(毎フレーム可)
scene:setSpriteEffect(e, 0.5)            -- カスタムスプライトシェーダーへの汎用値(毎フレーム可)
scene:setSpriteParams(e, 1, 0.5, 0, 0)   -- カスタムスプライトシェーダーへの汎用float4(毎フレーム可)
scene:setMeshEffect(e, 0.5)              -- カスタムメッシュシェーダーへの汎用値(毎フレーム可)
scene:setMeshParams(e, 1, 0.5, 0, 0)     -- カスタムメッシュシェーダーへの汎用float4(毎フレーム可)

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
