---@meta
-- ============================================================
--  DX12 Engine — 高レベル prelude ヘルパーの型定義
--  ◆ 正（source of truth）: src/scripting/ScriptEngine.cpp の
--      LoadPrelude() 内 kPrelude（自動ロードされる Lua コード）
--  ◆ prelude にヘルパーを追加・変更したら必ずこのファイルも更新すること
-- ============================================================

-- ============================================================
-- 入力（文字列キー名で済ます簡易版）
-- ============================================================

---`keyDown` / `keyPressed` に渡せるキー名
---@alias KeyName
---| "W"
---| "A"
---| "S"
---| "D"
---| "E"
---| "Q"
---| "UP"
---| "DOWN"
---| "LEFT"
---| "RIGHT"
---| "SPACE"
---| "SHIFT"
---| "TAB"
---| "ENTER"
---| "ESC"
---| "ESCAPE"

---キーが押されている間ずっと true
---@param name KeyName
---@return boolean
function keyDown(name) end

---キーを押した瞬間だけ true
---@param name KeyName
---@return boolean
function keyPressed(name) end

-- ============================================================
-- Actor: 名前付きエンティティの薄いラッパー（高レベル API の中心）
-- ============================================================

---`actor()` のオプション
---@class ActorOpts
---@field speed? number moveTopDown の移動速度（既定 5）
---@field solid? string|string[] 当たり判定する相手のエンティティ名（既定 なし）
---@field half? number 自分の当たり判定半径（XZ・既定 0.5）

---名前付きエンティティの薄いラッパー
---@class Actor
---@field name string 対象エンティティ名
---@field speed number 移動速度
---@field half number 当たり判定半径
---@field solids string[] 当たり判定相手
---@field x number 現在 X（moveTopDown が管理）
---@field y number 現在 Y
---@field z number 現在 Z
Actor = {}

---シーン内のエンティティを「名前」で掴む
---```lua
---player = actor("Player", { speed = 9, solid = { "Wall1", "Wall2" } })
---```
---@param name string エンティティ名
---@param opts? ActorOpts
---@return Actor
function actor(name, opts) end

---実体の Entity を返す（無効なら nil）
---@return Entity|nil
function Actor:entity() end

---エンティティが存在するか
---@return boolean
function Actor:valid() end

---現在位置（無効時は Vec3(0,0,0)）
---@return Vec3
function Actor:pos() end

---位置を直接セット
---@param x number
---@param y number
---@param z number
function Actor:setPos(x, y, z) end

---見下ろし移動（XZ 平面）。solid の箱に当たると軸ごと停止＝壁沿いスライド。
---**毎フレーム OnUpdate 内で呼ぶ**
---@param dt number 経過秒
---@param scheme? "WASD"|"Arrows" 入力（既定 "WASD"）
function Actor:moveTopDown(dt, scheme) end

---相手に届いたか（XZ 距離 < radius）
---```lua
---if player:reached(goal, 1.4) then goToScene("scenes/clear.json") end
---```
---@param other Actor
---@param radius? number 既定 1.0
---@return boolean
function Actor:reached(other, radius) end

-- ============================================================
-- カメラ追従ヘルパー（CameraComponent 持ちエンティティを動かす）
-- ============================================================

---`cameraFollow` のオプション
---@class CameraFollowOpts
---@field name? string 追従させるカメラ名（既定 "GameCamera"）
---@field height? number 高さ（既定 13）
---@field back? number 後ろ距離（既定 8）
---@field pitch? number 見下ろし角・度（既定 55）

---見下ろしカメラ追従。**毎フレーム OnUpdate 内で呼ぶ**
---```lua
---cameraFollow(player, { height = 13, back = 8, pitch = 55 })
---```
---@param target Actor|{x:number, y:number, z:number}
---@param opts? CameraFollowOpts
function cameraFollow(target, opts) end

---`cameraTPS` のオプション
---@class CameraTpsOpts
---@field name? string カメラ名（既定 "MainCamera"）
---@field dist? number 距離（既定 10）
---@field height? number 高さ（既定 6）
---@field pitch? number 見下ろし角・度（既定 26）
---@field yaw? number カメラを置きたい方位・度（プレイヤーの向きを毎フレーム渡すと背後に回り込む）
---@field follow? number yaw 追従の補間率 0..1（小さいほどゆっくり＝トレイル感。既定 1=即時）

---三人称トレイルカメラ（キーボード TPS 向け・マウス不要）。
---カメラ方位の状態は target._camYaw に保持される（target はテーブルであること）
---@param target Actor|{x:number, y:number, z:number}
---@param opts? CameraTpsOpts
function cameraTPS(target, opts) end

---`cameraLockOn` のオプション
---@class CameraLockOnOpts
---@field name? string カメラ名（既定 "MainCamera"）
---@field dist? number 距離（既定 9）
---@field height? number 高さ（既定 5）
---@field pitch? number 見下ろし角・度（既定 18）
---@field smooth? number 角度補間率 0..1（省略=即時。近接ジッタ抑制に 0.2〜0.5）
---@field maxStep? number 度/フレーム上限（既定 9）

---ロックオン三人称カメラ（ボス戦/デュエル向け）。
---カメラを「プレイヤー → ターゲット」軸に固定し背後からターゲットを捉える。
---WASD をこの戻り値 yaw 基準にすれば向きが変わっても操作が狂わない
---@param playerPos Vec3|{x:number, y:number, z:number}
---@param targetPos Vec3|{x:number, y:number, z:number}
---@param opts? CameraLockOnOpts
---@param state? table smooth 使用時に yaw 状態を持つテーブル（state._lockYaw に保持）
---@return number yaw カメラ方位（度）
function cameraLockOn(playerPos, targetPos, opts, state) end

-- ============================================================
-- シーン遷移の別名 / 汎用ユーティリティ
-- ============================================================

---フェード付きでシーン遷移（fadeToScene の分かりやすい別名）
---@param path string assets/ からの相対パス（例: "scenes/clear.json"）
---@param dur? number フェード秒（既定 0.6）
function goToScene(path, dur) end

---SceneFlow の「次のシーン」へ（クリア演出向けの別名）
---@param dur? number （現状未使用）
function win(dur) end

---値を [lo, hi] に収める
---@param v number
---@param lo number
---@param hi number
---@return number
function clamp(v, lo, hi) end

---線形補間
---@param a number
---@param b number
---@param t number 0..1
---@return number
function lerp(a, b, t) end

---角度の最短差（度）。-180..180 を返す。カメラ/向きの補間に
---@param from number
---@param to number
---@return number
function angleDelta(from, to) end

-- ============================================================
-- FX: ド派手パーティクルプリセット（fx:burst / fx:ring を包む）
-- 色は 0..1、intensity>1 で HDR 白熱 → ブルームで光る
-- ============================================================

---@class FXPresets
FX = {}

---爆発（撃破など）: 白熱フラッシュ + 火球 + 火花 + 衝撃波リング + 煙
---@param x number
---@param y number
---@param z number
---@param scale? number 規模（既定 1.0）
---@param r? number 炎の色 R（既定 1.0）
---@param g? number 炎の色 G（既定 0.45）
---@param b? number 炎の色 B（既定 0.12）
function FX.explosion(x, y, z, scale, r, g, b) end

---衝撃波リング（ノヴァ等）: 拡大リング + 放射状の光筋
---@param x number
---@param y number
---@param z number
---@param count? integer 光筋の数（既定 24）
---@param speed? number 拡がる速さ（既定 16）
---@param r? number 既定 0.6
---@param g? number 既定 1.0
---@param b? number 既定 1.0
function FX.shockwave(x, y, z, count, speed, r, g, b) end

---着弾火花（小さく速い・光の筋）
---@param x number
---@param y number
---@param z number
---@param count? integer 既定 6
---@param r? number 既定 0.6
---@param g? number 既定 0.95
---@param b? number 既定 1.0
function FX.spark(x, y, z, count, r, g, b) end

---立ち上る軌跡/オーラ点（**1粒ずつ毎フレーム呼ぶ用**）
---@param x number
---@param y number
---@param z number
---@param r? number 既定 1
---@param g? number 既定 0.9
---@param b? number 既定 0.4
function FX.trail(x, y, z, r, g, b) end

---レベルアップ超新星: 白熱フラッシュ + 金リング + 火球 + 金火花 + 画面パルス
---@param x number
---@param y number
---@param z number
---@param scale? number 規模（既定 1.0）
function FX.supernova(x, y, z, scale) end

---ヒット時の画面パルス（クロマ + 放射ブラー）
---@param amount? number 強さ（既定 0.5）
function FX.hit(amount) end

---連続ビーム（レーザー/エネルギー線）。**毎フレーム呼ぶ用**（点線にならない一本線）
---@param x0 number
---@param y0 number
---@param z0 number
---@param x1 number
---@param y1 number
---@param z1 number
---@param r? number 既定 0.4
---@param g? number 既定 0.9
---@param b? number 既定 1.0
---@param width? number 太さ（既定 0.4）
---@param kind? "energy"|"electric"|"lightning"|"fire" ビーム種別（既定 "energy"）
---@param intensity? number HDR 強度（既定 7）
function FX.beam(x0, y0, z0, x1, y1, z1, r, g, b, width, kind, intensity) end

---稲妻ビーム（始点→終点をギザギザの放電で繋ぐ）
---@param x0 number
---@param y0 number
---@param z0 number
---@param x1 number
---@param y1 number
---@param z1 number
---@param r? number 既定 0.6
---@param g? number 既定 0.8
---@param b? number 既定 1.0
---@param width? number 太さ（既定 0.6）
function FX.lightning(x0, y0, z0, x1, y1, z1, r, g, b, width) end

---動的火柱（噴き上がり→うねり→崩れをシェーダがアニメ。火の粉/閃光/地面リング/煙つき）
---@param x number
---@param y number
---@param z number
---@param height? number 高さ（既定 6.0）
---@param radius? number 半径（既定 1.2）
---@param r? number 既定 1.0
---@param g? number 既定 0.5
---@param b? number 既定 0.15
function FX.pillar(x, y, z, height, radius, r, g, b) end

-- ============================================================
-- vfx: 統一 VFX 窓口（コード自前 と Effekseer を両立）
-- ============================================================

---@class Vfx
vfx = {}

---名前で VFX を再生する統一窓口。
---Effekseer 実体（.efkefc）が登録されていればそちら、無ければコードプリセット。
---既定プリセット: "explosion" / "supernova" / "spark" / "hit"
---@param name string
---@param x number
---@param y number
---@param z number
---@param scale? number 規模（既定 1.0）
function vfx.play(name, x, y, z, scale) end

---コードプリセットを登録（vfx.play で名前から引けるように）
---@param name string
---@param fn fun(x: number, y: number, z: number, scale: number)
function vfx.register(name, fn) end
