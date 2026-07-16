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
-- gamepad: Xbox コントローラー簡易API（pad 省略時は 0 = 1台目）
-- ============================================================

---`padDown` / `padPressed` / `padReleased` に渡せるボタン名
---@alias PadButtonName
---| "A"
---| "B"
---| "X"
---| "Y"
---| "LB"
---| "RB"
---| "BACK"
---| "START"
---| "LSTICK"
---| "RSTICK"
---| "DPAD_UP"
---| "DPAD_DOWN"
---| "DPAD_LEFT"
---| "DPAD_RIGHT"

---指定パッドが接続されているか
---@param pad? integer 省略時 0（1台目）
---@return boolean
function padConnected(pad) end

---ボタンが押されている間ずっと true
---@param name PadButtonName
---@param pad? integer 省略時 0（1台目）
---@return boolean
function padDown(name, pad) end

---ボタンを押した瞬間だけ true
---@param name PadButtonName
---@param pad? integer 省略時 0（1台目）
---@return boolean
function padPressed(name, pad) end

---ボタンを離した瞬間だけ true（チャージ攻撃の離し際判定などに）
---@param name PadButtonName
---@param pad? integer 省略時 0（1台目）
---@return boolean
function padReleased(name, pad) end

---スティックの傾き（デッドゾーン適用済み、各軸 -1..1）
---@param side "left"|"right"
---@param pad? integer 省略時 0（1台目）
---@return number x
---@return number y
function padStick(side, pad) end

---トリガーの押し込み量（デッドゾーン適用済み、0..1）
---@param side "left"|"right"
---@param pad? integer 省略時 0（1台目）
---@return number
function padTrigger(side, pad) end

---振動（low=低周波・強モーター、high=高周波・弱モーター、共に0..1）
---seconds を渡すとその秒数後に自動停止。省略時は padVibrate(0, 0) するまで鳴り続ける。
---@param low number
---@param high number
---@param seconds? number
---@param pad? integer 省略時 0（1台目）
function padVibrate(low, high, seconds, pad) end

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

-- ============================================================
-- uifx: ゲーム内UIの定番演出ワンライナー集
-- ============================================================

---@class Uifx
uifx = {}

---ボタンを押した感（一瞬膨らんで戻る）。クリックハンドラの先頭に1行入れるだけ。
---```lua
---events:on("startClicked", function(e) uifx.punch(e.source) end)
---```
---@param e Entity|integer 対象（ボタンイベントの e.source そのままでよい）
---@param s? number 膨らみ倍率（既定 1.15）
---@param dur? number 全体秒（既定 0.22）
function uifx.punch(e, s, dur) end

---色フラッシュ。引数なしで白く光る。1 超えの値で輝く（ダメージ赤は uifx.flash(e, 3, 0.3, 0.3)）。
---@param e Entity|integer
---@param r? number 既定 2.5
---@param g? number 既定 2.5
---@param b? number 既定 2.5
---@param dur? number 全体秒（既定 0.3）
function uifx.flash(e, r, g, b, dur) end

---振動（ダメージ/エラー通知）。duration かけて減衰する。
---@param e Entity|integer
---@param amp? number 振幅px（既定 10）
---@param dur? number 秒（既定 0.4）
function uifx.shake(e, amp, dur) end

---赤フラッシュ + 振動（被ダメの定番セット）。
---@param e Entity|integer
---@param amp? number 振動振幅px（既定 8）
function uifx.hit(e, amp) end

---ぽよんと登場（0 からバウンドで等倍へ）。
---@param e Entity|integer
---@param dur? number 秒（既定 0.5）
function uifx.bounceIn(e, dur) end

---ぺしゃんこ→開く（フリップ風。結果表示やカード公開に）。
---@param e Entity|integer
---@param dur? number 秒（既定 0.4）
function uifx.flipIn(e, dur) end

---縮んで消える（ポップアップを閉じる）。
---@param e Entity|integer
---@param dur? number 秒（既定 0.25）
function uifx.popOut(e, dur) end

---フェードイン。
---@param e Entity|integer
---@param dur? number 秒（既定 0.3）
function uifx.fadeIn(e, dur) end

---フェードアウト（クリックは残る。消し切るなら scene:hideUi）。
---@param e Entity|integer
---@param dur? number 秒（既定 0.3）
function uifx.fadeOut(e, dur) end

-- ============================================================
-- time 拡張(純Lua部分): タイマー / 共有ビデオ時計 / 個別時計
-- (time.now/dt/setScale 等の C++ コアは dx12e.lua 側)
-- ============================================================

---sec 秒後に fn を1回呼ぶ。スケール済み時間で進む(setScale(0) 中は止まる)。Play 開始でクリア。
---@param sec number
---@param fn fun()
---@return integer id time.cancel 用
function time.after(sec, fn) end

---sec 秒ごとに fn を繰り返し呼ぶ。
---@param sec number
---@param fn fun()
---@return integer id time.cancel 用
function time.every(sec, fn) end

---after/every のタイマーを解除する。
---@param id integer
function time.cancel(id) end

---@class TimeVideo
time.video = {}

---ステージ全体に1本流れる「動画時間」を開始する(決定論タイムライン用)。
---@param duration number 制限時間(秒)
---@param opts? { skipCost?: number } skip 時の残り時間消費倍率(0=コスト無し)
function time.video.start(duration, opts) end

---ビデオ時計を停止する
function time.video.stop() end

---@return boolean
function time.video.active() end

---@return number 現在のビデオ時刻(秒)
function time.video.now() end

---@return number
function time.video.duration() end

---@return number 残り時間(秒)
function time.video.remaining() end

---@return boolean 制限時間切れか
function time.video.finished() end

---対象のオフセットを直接設定する(e は self / 名前 / 数値id)
function time.video.setOffset(e, off) end

---@return number
function time.video.getOffset(e) end

---対象のローカル時間だけを ±amount 秒動かす(先送り/巻き戻し)。skipCost 分の残り時間も消費。
---@param amount number
function time.video.skip(e, amount) end

---@return number 対象のローカル時刻 = ビデオ時刻 + オフセット
function time.video.localTime(e) end

---エンティティ単位の独立クロックの現在時刻(初アクセスで 0 から開始)
---@return number
function time.localTime(e) end

---個別時計を ±amount 秒スキップする
---@param amount number
---@return number スキップ後の時刻
function time.skipEntity(e, amount) end

---個別時計の進む速さ(0=停止、負=逆再生)
---@param s number
function time.scaleEntity(e, s) end

---@return number
function time.getEntityScale(e) end

---個別時計を破棄する(次アクセスで 0 から)
function time.resetEntity(e) end

-- ============================================================
-- charge: 押しっぱなしチャージ計測(弓を引く等)
-- ============================================================

charge = {}

---@class Charge
---@field v number 現在のチャージ量
local Charge = {}

---チャージ計測を作る。OnUpdate で :update() を呼び続ける。
---```lua
---local c = charge.new("E", { max = 2.0, rate = 1.0 })
---function OnUpdate(self, dt)
---  c:update()
---  local r = c:released()
---  if r then fireArrow(r) end
---end
---```
---@param key string keyDown のキー名("E" 等)
---@param opts? { max?: number, rate?: number, realtime?: boolean } realtime=true でポーズ中も実時間で溜まる
---@return Charge
function charge.new(key, opts) end

---毎フレーム呼ぶ(押下中に溜め、離した瞬間を検出する)
function Charge:update() end

---@return boolean 押しっぱなしでチャージ中か
function Charge:charging() end

---@return number 0..1 のチャージ割合(ゲージ表示用)
function Charge:ratio() end

---離した瞬間だけチャージ量(秒)を返す。それ以外は nil。
---@return number|nil
function Charge:released() end
