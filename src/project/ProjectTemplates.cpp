// 新規プロジェクトテンプレートの実体データ。
//
// 各テンプレートは「タイトル → ゲーム → クリア」の 3 シーン + sceneflow + プロパティ付き
// Lua コンポーネント一式を持つ、そのまま遊べるミニゲームとして構成する（empty のみ最小構成）。
// エンジンの現行機能（CharacterController 物理 / Trigger / ParticleEmitter / retained UI /
// UIAnimator / EventBus / ポストプロセス / SSAO）を使った作例を兼ねる。
//
// 検証: DX12Engine.exe --new-project <dir> --template <id> で生成し、
//       DX12Engine.exe --validate <scene.json> で参照整合を確認できる。

#include "project/ProjectTemplates.h"

namespace dx12e::templates
{
namespace
{

// ============================================================================
// 共通 Lua コンポーネント（複数テンプレートで共用）
// ============================================================================

// タイトル画面の進行。BtnStart(ev_start) → ゲームシーンへ。
constexpr const char* kTitleControllerLua = R"LUA(-- タイトル画面の進行: スタートボタン(ev_start) → ゲームシーンへ
properties = {
  { name = "gameScene", type = "string", default = "scenes/main.json", label = "開始シーン" },
  { name = "fade",      type = "float",  default = 0.5, label = "フェード秒" },
}

function OnStart(self)
  input:setMouseCapture(false)
  local target, fadeSec = self.gameScene, self.fade
  events:on("ev_start", function() goToScene(target, fadeSec) end)
  -- 既定フォーカスを当てておくと矢印/D-pad + Enter/A だけでも開始できる
  local btn = scene:findEntity("BtnStart")
  if btn and btn:isValid() then setUiFocus(btn) end
end
)LUA";

// クリア画面の進行。もういちど(ev_retry) / タイトルへ(ev_title)。
constexpr const char* kClearControllerLua = R"LUA(-- クリア画面の進行: もういちど(ev_retry) / タイトルへ(ev_title)
properties = {
  { name = "gameScene",  type = "string", default = "scenes/main.json",  label = "ゲームシーン" },
  { name = "titleScene", type = "string", default = "scenes/title.json", label = "タイトルシーン" },
}

function OnStart(self)
  input:setMouseCapture(false)
  local g, t = self.gameScene, self.titleScene
  events:on("ev_retry", function() goToScene(g, 0.5) end)
  events:on("ev_title", function() goToScene(t, 0.6) end)
  local btn = scene:findEntity("BtnRetry")
  if btn and btn:isValid() then setUiFocus(btn) end
end
)LUA";

// タイトル画面用: 注視点のまわりをゆっくり周回するカメラ。
constexpr const char* kOrbitCameraLua = R"LUA(-- 注視点のまわりをゆっくり周回するカメラ（タイトル画面の背景用）
properties = {
  { name = "center",    type = "vec3",  default = {0.0, 1.0, 0.0}, label = "注視点" },
  { name = "radius",    type = "float", default = 13.0, min = 2.0, max = 60.0, label = "半径" },
  { name = "height",    type = "float", default = 4.5,  label = "高さ" },
  { name = "degPerSec", type = "float", default = 4.0,  label = "周回速度(度/秒)" },
}

function OnUpdate(self, dt)
  local a = math.rad(time.now() * self.degPerSec)
  local t = self.transform
  t.position = Vec3.new(self.center.x + math.sin(a) * self.radius,
                        self.center.y + self.height,
                        self.center.z - math.cos(a) * self.radius)
  local pitch = math.deg(math.atan(self.height, self.radius))
  t.rotation = Vec3.new(pitch, math.deg(a), 0)
end
)LUA";

// sceneflow（3 シーン構成の全テンプレート共通）
constexpr const char* kSceneFlowJson = R"JSON({
  "start": "scenes/title.json",
  "flow": {
    "scenes/title.json": { "next": "scenes/main.json",  "onFail": "" },
    "scenes/main.json":  { "next": "scenes/clear.json", "onFail": "scenes/main.json" },
    "scenes/clear.json": { "next": "scenes/title.json", "onFail": "" }
  }
})JSON";

// グローバル game.lua（3 シーン構成の全テンプレート共通）
constexpr const char* kGameLua = R"LUA(-- グローバルフック: Play 中に毎フレーム呼ばれる。
-- ゲームの中身は assets/components/*.lua（エンティティに貼る部品）にある。
-- シーンの流れは assets/sceneflow.json（title → main → clear）。
function OnUpdate(dt)
end
)LUA";

// ============================================================================
// empty テンプレート
// ============================================================================

constexpr const char* kSpinnerLua = R"LUA(-- 指定した軸でくるくる回すだけの部品（プロパティ付き Lua コンポーネントの最小サンプル）
properties = {
  { name = "speed", type = "float", default = 45.0, min = -360.0, max = 360.0, label = "回転速度(度/秒)" },
  { name = "axis",  type = "vec3",  default = {0.0, 1.0, 0.0}, label = "回転軸" },
}

function OnUpdate(self, dt)
  local t = self.transform
  t.rotation = Vec3.new(t.rotation.x + self.axis.x * self.speed * dt,
                        t.rotation.y + self.axis.y * self.speed * dt,
                        t.rotation.z + self.axis.z * self.speed * dt)
end
)LUA";

constexpr const char* kEmptyMainScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.35], "color": [1.0, 0.97, 0.9], "intensity": 1.1, "ambient": 0.4 },
      "transform": { "position": [0.0, 12.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "MainCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 1000.0, "isActive": true },
      "transform": { "position": [0.0, 8.0, -12.0], "rotation": [28.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Grid", "gridPlane": { "size": 50.0 },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Cube", "primitive": "box", "color": [0.6, 0.65, 0.75],
      "luaScript": { "scriptPath": "components/Spinner.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } }
  ],
  "postProcess": { "enabled": true, "fxaaOn": true },
  "ssao": { "enabled": true },
  "shadows": true
})JSON";

constexpr const char* kEmptyGameLua = R"LUA(-- グローバルフック: Play 中に毎フレーム呼ばれる。
-- エンティティ単位の振る舞いは assets/components/*.lua に部品として書き、
-- エディタで D&D するかシーン JSON の luaScript で貼るのがおすすめ（例: Spinner.lua）。
-- 高レベル API の一覧は docs/SCRIPTING.md。
function OnUpdate(dt)
  -- 例: ui:text(24, 24, "Hello, DX12 Engine!", 24, 1, 1, 1, 1)
end
)LUA";

// ============================================================================
// fps テンプレート — 「STEEL RANGE」物理ベースの射撃レンジ
// ============================================================================

constexpr const char* kFpsControllerLua = R"LUA(-- 一人称プレイヤー操作（CharacterController 物理前提）
-- 操作: WASD/左スティック=移動  マウス/右スティック=視点  SPACE/A=ジャンプ
--       SHIFT/L3=ダッシュ  左クリック/RT=射撃  ESC=マウス解放
properties = {
  { name = "cam",          type = "entity", label = "一人称カメラ" },
  { name = "speed",        type = "float",  default = 6.5,  min = 1.0,  max = 20.0, label = "移動速度(m/s)" },
  { name = "sprintMul",    type = "float",  default = 1.6,  min = 1.0,  max = 3.0,  label = "ダッシュ倍率" },
  { name = "sens",         type = "float",  default = 0.12, min = 0.02, max = 0.5,  label = "マウス感度" },
  { name = "eyeHeight",    type = "float",  default = 0.6,  label = "目線オフセット" },
  { name = "fireInterval", type = "float",  default = 0.14, label = "連射間隔(秒)" },
  { name = "range",        type = "float",  default = 80.0, label = "射程(m)" },
}

local yaw, pitch = 0, 0
local cool = 0

local function shoot(self, eye)
  local yr, pr = math.rad(yaw), math.rad(pitch)
  local dx = math.sin(yr) * math.cos(pr)
  local dy = math.sin(pr)
  local dz = math.cos(yr) * math.cos(pr)
  -- 自分のカプセルに当たらないよう少し前から飛ばす
  local origin = Vec3.new(eye.x + dx * 0.7, eye.y + dy * 0.7, eye.z + dz * 0.7)
  padVibrate(0.35, 0.15, 0.08)
  local hit = physics:raycast(origin, Vec3.new(dx, dy, dz), self.range)
  if not hit.hit then return end
  FX.spark(hit.point.x, hit.point.y, hit.point.z, 10, 1.0, 0.8, 0.35)
  for _, e in ipairs(physics:overlapSphere(hit.point, 0.35, 8)) do
    if e and e:isValid() then
      local n = e.name or ""
      if n:find("Target", 1, true) == 1 then
        events:emit("targetHit", { name = n })
      elseif e:hasComponent("RigidBody") then
        -- 物理オブジェクト（木箱など）は撃った方向へ弾き飛ばす
        physics:applyImpulse(e, Vec3.new(dx * 5, dy * 5 + 1.5, dz * 5))
      end
    end
  end
end

function OnStart(self)
  input:setMouseCapture(true)
  yaw, pitch, cool = 0, 0, 0
  local me = scene:findEntity(self.name)
  if me and me:isValid() then yaw = me.transform.rotation.y end
end

function OnUpdate(self, dt)
  local me = scene:findEntity(self.name)
  if not (me and me:isValid()) then return end
  local cam = self.cam
  if not (cam and cam:isValid()) then return end

  if keyPressed("ESC") then input:setMouseCapture(not input:isMouseCaptured()) end

  -- 視点（マウス + 右スティック）
  if input:isMouseCaptured() then
    yaw   = yaw   + input:getMouseDeltaX() * self.sens
    pitch = pitch - input:getMouseDeltaY() * self.sens
  end
  local rsx, rsy = padStick("right")
  yaw   = yaw   + rsx * 170 * dt
  pitch = pitch + rsy * 120 * dt
  if pitch >  85 then pitch =  85 end
  if pitch < -85 then pitch = -85 end

  -- 移動（yaw 基準の目標速度を物理へ渡す。衝突とスロープは物理側が解決）
  local yr = math.rad(yaw)
  local fx, fz = math.sin(yr), math.cos(yr)
  local sx, sz = math.cos(yr), -math.sin(yr)
  local ix, iz = 0, 0
  if keyDown("W") then ix = ix + fx; iz = iz + fz end
  if keyDown("S") then ix = ix - fx; iz = iz - fz end
  if keyDown("D") then ix = ix + sx; iz = iz + sz end
  if keyDown("A") then ix = ix - sx; iz = iz - sz end
  local lsx, lsy = padStick("left")
  ix = ix + fx * lsy + sx * lsx
  iz = iz + fz * lsy + sz * lsx
  local len = math.sqrt(ix * ix + iz * iz)
  if len > 1 then ix, iz = ix / len, iz / len end
  local spd = self.speed
  if keyDown("SHIFT") or padDown("LSTICK") then spd = spd * self.sprintMul end
  physics:move(me, ix * spd, iz * spd)
  if keyPressed("SPACE") or padPressed("A") then physics:jump(me) end

  -- カメラを目線へ
  local p = me.transform.position
  cam.transform.position = Vec3.new(p.x, p.y + self.eyeHeight, p.z)
  cam.transform.rotation = Vec3.new(-pitch, yaw, 0)

  -- 射撃
  cool = cool - dt
  local firing = (input:isMouseCaptured() and input:isAsyncKeyDown(1)) or padTrigger("right") > 0.5
  if firing and cool <= 0 then
    cool = self.fireInterval
    shoot(self, cam.transform.position)
  end
end
)LUA";

constexpr const char* kFpsTargetLua = R"LUA(-- 撃つと壊れる的。ふわふわ浮遊 + ゆっくり回転し、被弾で爆散 → targetDown を発火する
properties = {
  { name = "points",    type = "int",   default = 100,  label = "得点" },
  { name = "bobAmount", type = "float", default = 0.12, label = "浮遊の振れ幅" },
  { name = "spinSpeed", type = "float", default = 40.0, label = "回転速度(度/秒)" },
}

function OnStart(self)
  self._baseY = self.transform.position.y
  self._phase = (self.entity % 7) * 0.9
  local name, pts = self.name, self.points
  events:on("targetHit", function(d)
    if d.name ~= name then return end
    local e = scene:findEntity(name)
    if not (e and e:isValid()) then return end
    local p = e.transform.position
    FX.explosion(p.x, p.y, p.z, 0.7, 1.0, 0.45, 0.25)
    events:emit("targetDown", { points = pts })
    scene:remove(e)
  end)
end

function OnUpdate(self, dt)
  local t = self.transform
  t.position = Vec3.new(t.position.x,
                        self._baseY + math.sin(time.now() * 2.0 + self._phase) * self.bobAmount,
                        t.position.z)
  t.rotation = Vec3.new(0, t.rotation.y + self.spinSpeed * dt, 0)
end
)LUA";

constexpr const char* kFpsGameManagerLua = R"LUA(-- 射撃レンジの進行管理: スコア/残り的の HUD 更新、全的破壊でクリア → 次のシーンへ
properties = {
  { name = "totalTargets", type = "int",   default = 5,   label = "的の総数" },
  { name = "clearDelay",   type = "float", default = 2.2, label = "クリア後の待ち(秒)" },
}

local function refreshHud(self)
  local s = scene:findEntity("HudScore")
  if s and s:isValid() then scene:setUiText(s, string.format("SCORE %04d", self._score)) end
  local t = scene:findEntity("HudTargets")
  if t and t:isValid() then
    scene:setUiText(t, string.format("TARGETS %d/%d", self.totalTargets - self._left, self.totalTargets))
  end
end

function OnStart(self)
  self._score, self._left = 0, self.totalTargets
  local mgr = self
  refreshHud(mgr)

  events:on("targetDown", function(d)
    mgr._score = mgr._score + (d.points or 0)
    mgr._left  = mgr._left - 1
    refreshHud(mgr)
    local s = scene:findEntity("HudScore")
    if s and s:isValid() then uifx.punch(s) end
    if mgr._left > 0 then return end
    -- 全的クリア: パネルを出して sceneflow の次（クリア画面）へ
    FX.hit(0.5)
    local cs = scene:findEntity("ClearScore")
    if cs and cs:isValid() then scene:setUiText(cs, string.format("SCORE %04d", mgr._score)) end
    local panel = scene:findEntity("ClearPanel")
    if panel and panel:isValid() then scene:showUi(panel) end
    time.after(mgr.clearDelay, function() win() end)
  end)

  -- 操作ヒントはしばらくしたら消す
  time.after(6.0, function()
    local hint = scene:findEntity("HudHint")
    if hint and hint:isValid() then scene:tweenUi(hint, { alpha = 0, duration = 0.8 }) end
  end)
end
)LUA";

constexpr const char* kFpsMainScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.45, -1.0, -0.3], "color": [1.0, 0.96, 0.88], "intensity": 1.15, "ambient": 0.38 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "MainCamera",
      "camera": { "fovDegrees": 74.0, "nearClip": 0.05, "farClip": 500.0, "isActive": true },
      "transform": { "position": [0.0, 1.8, -12.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Player",
      "characterController": { "radius": 0.4, "halfHeight": 0.55, "jumpSpeed": 7.5, "stepHeight": 0.4 },
      "luaScript": { "scriptPath": "components/FpsController.lua", "enabled": true,
                     "props": [ { "name": "cam", "type": "entity", "value": "MainCamera" } ] },
      "transform": { "position": [0.0, 1.2, -12.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Floor", "primitive": "plane", "color": [0.16, 0.18, 0.23],
      "rigidBody": { "motionType": 0 },
      "boxCollider": { "halfExtents": [25.0, 0.5, 25.0], "offset": [0.0, -0.5, 0.0] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Wall_N", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [0.0, 1.8, 19.0], "rotation": [0.0, 0.0, 0.0], "scale": [40.0, 3.6, 1.6] } },
    { "name": "Wall_S", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [0.0, 1.8, -19.0], "rotation": [0.0, 0.0, 0.0], "scale": [40.0, 3.6, 1.6] } },
    { "name": "Wall_E", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [19.0, 1.8, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.6, 3.6, 40.0] } },
    { "name": "Wall_W", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-19.0, 1.8, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.6, 3.6, 40.0] } },
    { "name": "Pillar_1", "primitive": "box", "color": [0.5, 0.3, 0.26],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-7.0, 1.5, 4.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Flame_1",
      "particleEmitter": { "kind": 1, "blend": 0, "rate": 34, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.28, "speed": 1.8, "speedVar": 0.4, "size": 0.32, "sizeEnd": 0.0,
        "life": 0.6, "lifeVar": 0.3, "color": [1.0, 0.62, 0.2], "colorEnd": [1.0, 0.12, 0.04],
        "intensity": 4.0, "gravity": 0.6, "drag": 1.0, "turbStrength": 0.6, "flicker": 0.5,
        "light": true, "lightRange": 6.0 },
      "transform": { "position": [-7.0, 3.3, 4.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Pillar_2", "primitive": "box", "color": [0.5, 0.3, 0.26],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [8.0, 1.5, 7.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Flame_2",
      "particleEmitter": { "kind": 1, "blend": 0, "rate": 34, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.28, "speed": 1.8, "speedVar": 0.4, "size": 0.32, "sizeEnd": 0.0,
        "life": 0.6, "lifeVar": 0.3, "color": [1.0, 0.62, 0.2], "colorEnd": [1.0, 0.12, 0.04],
        "intensity": 4.0, "gravity": 0.6, "drag": 1.0, "turbStrength": 0.6, "flicker": 0.5,
        "light": true, "lightRange": 6.0 },
      "transform": { "position": [8.0, 3.3, 7.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Target_1", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 1.6, 14.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Target_2", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true, "props": [] },
      "transform": { "position": [-8.0, 2.2, 12.0], "rotation": [0.0, 25.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Target_3", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true, "props": [] },
      "transform": { "position": [8.0, 1.4, 13.0], "rotation": [0.0, -20.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Target_4", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true,
                     "props": [ { "name": "points", "type": "int", "value": 200 } ] },
      "transform": { "position": [-4.0, 3.2, 17.0], "rotation": [0.0, 10.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Target_5", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true,
                     "props": [ { "name": "points", "type": "int", "value": 200 } ] },
      "transform": { "position": [5.0, 2.8, 17.0], "rotation": [0.0, -10.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Crate_1", "primitive": "box", "color": [0.72, 0.58, 0.34],
      "rigidBody": { "motionType": 2, "mass": 2.0, "friction": 0.6, "restitution": 0.25 },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [3.0, 0.6, 0.0], "rotation": [0.0, 15.0, 0.0], "scale": [1.2, 1.2, 1.2] } },
    { "name": "Crate_2", "primitive": "box", "color": [0.72, 0.58, 0.34],
      "rigidBody": { "motionType": 2, "mass": 2.0, "friction": 0.6, "restitution": 0.25 },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [3.5, 1.8, 0.2], "rotation": [0.0, -10.0, 0.0], "scale": [1.2, 1.2, 1.2] } },
    { "name": "Crate_3", "primitive": "box", "color": [0.72, 0.58, 0.34],
      "rigidBody": { "motionType": 2, "mass": 2.0, "friction": 0.6, "restitution": 0.25 },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-3.0, 0.6, 2.0], "rotation": [0.0, 40.0, 0.0], "scale": [1.2, 1.2, 1.2] } },
    { "name": "GameManager",
      "luaScript": { "scriptPath": "components/GameManager.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "CrossDot", "parent": 21,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-3.0, -3.0], "offsetMax": [3.0, 3.0], "order": 10 },
      "uiImage": { "texturePath": "", "color": [1.0, 1.0, 1.0, 0.9], "shape": 1, "raycastBlock": false } },
    { "name": "CrossRing", "parent": 21,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-15.0, -15.0], "offsetMax": [15.0, 15.0], "order": 10 },
      "uiImage": { "texturePath": "", "color": [1.0, 1.0, 1.0, 0.45], "shape": 2, "ringThickness": 2.0, "raycastBlock": false } },
    { "name": "HudScore", "parent": 21,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 0.0], "pivot": [0.0, 0.0],
                  "offsetMin": [40.0, 30.0], "offsetMax": [460.0, 84.0], "order": 5 },
      "uiText": { "text": "SCORE 0000", "fontSize": 40, "color": [0.9, 0.97, 1.0, 0.95],
                  "alignH": 0, "alignV": 1, "letterSpacing": 2.0,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 2.0] } },
    { "name": "HudTargets", "parent": 21,
      "uiRect": { "anchorMin": [1.0, 0.0], "anchorMax": [1.0, 0.0], "pivot": [1.0, 0.0],
                  "offsetMin": [-460.0, 30.0], "offsetMax": [-40.0, 84.0], "order": 5 },
      "uiText": { "text": "TARGETS 0/5", "fontSize": 40, "color": [0.9, 0.97, 1.0, 0.95],
                  "alignH": 2, "alignV": 1, "letterSpacing": 2.0,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 2.0] } },
    { "name": "HudHint", "parent": 21,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 1.0],
                  "offsetMin": [-640.0, -92.0], "offsetMax": [640.0, -40.0], "order": 5 },
      "uiText": { "text": "WASD 移動 / マウス 視点 / 左クリック 射撃 / SPACE ジャンプ — 赤い的を全部撃て！",
                  "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.75], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 2.0] } },
    { "name": "ClearPanel", "parent": 21,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-340.0, -140.0], "offsetMax": [340.0, 140.0], "order": 20, "visible": false },
      "uiImage": { "texturePath": "", "color": [0.06, 0.08, 0.12, 0.94], "cornerRadius": 18.0,
                   "outlineWidth": 2.0, "outlineColor": [0.4, 0.8, 0.95, 0.5],
                   "shadowColor": [0.0, 0.0, 0.0, 0.5], "shadowOffset": [0.0, 8.0], "shadowSoftness": 16.0 },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.4, "showEasing": 4 } },
    { "name": "ClearTitle", "parent": 27,
      "uiRect": { "anchorMin": [0.0, 0.08], "anchorMax": [1.0, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "RANGE CLEAR!", "fontSize": 64, "color": [1.0, 0.92, 0.55, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 4.0,
                  "gradientDir": 2, "gradientColor2": [0.9, 0.55, 0.15, 1.0],
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 3.0] } },
    { "name": "ClearScore", "parent": 27,
      "uiRect": { "anchorMin": [0.0, 0.5], "anchorMax": [1.0, 0.74], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "SCORE 0000", "fontSize": 36, "color": [0.9, 0.97, 1.0, 0.95],
                  "alignH": 1, "alignV": 1, "letterSpacing": 3.0 } },
    { "name": "ClearSub", "parent": 27,
      "uiRect": { "anchorMin": [0.0, 0.74], "anchorMax": [1.0, 0.95], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "まもなくリザルトへ…", "fontSize": 22, "color": [1.0, 1.0, 1.0, 0.6],
                  "alignH": 1, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.45, "bloomThreshold": 0.75,
    "vignetteOn": true, "vignette": 0.3,
    "saturationOn": true, "saturation": 1.08,
    "contrastOn": true, "contrast": 1.04,
    "fxaaOn": true },
  "ssao": { "enabled": true },
  "shadows": true
})JSON";

constexpr const char* kFpsTitleScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.3], "color": [0.75, 0.85, 1.0], "intensity": 0.7, "ambient": 0.3 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "TitleCamera",
      "camera": { "fovDegrees": 58.0, "nearClip": 0.1, "farClip": 500.0, "isActive": true },
      "luaScript": { "scriptPath": "components/OrbitCamera.lua", "enabled": true,
                     "props": [ { "name": "center", "type": "vec3", "value": [0.0, 1.5, 6.0] } ] },
      "transform": { "position": [0.0, 5.0, -8.0], "rotation": [15.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Floor", "primitive": "plane", "color": [0.12, 0.14, 0.18],
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Pillar_L", "primitive": "box", "color": [0.5, 0.3, 0.26],
      "transform": { "position": [-5.0, 1.5, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Flame_L",
      "particleEmitter": { "kind": 1, "blend": 0, "rate": 30, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.28, "speed": 1.8, "speedVar": 0.4, "size": 0.3, "sizeEnd": 0.0,
        "life": 0.6, "lifeVar": 0.3, "color": [1.0, 0.62, 0.2], "colorEnd": [1.0, 0.12, 0.04],
        "intensity": 4.0, "gravity": 0.6, "turbStrength": 0.6, "flicker": 0.5, "light": true, "lightRange": 6.0 },
      "transform": { "position": [-5.0, 3.3, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Pillar_R", "primitive": "box", "color": [0.5, 0.3, 0.26],
      "transform": { "position": [5.0, 1.5, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Flame_R",
      "particleEmitter": { "kind": 1, "blend": 0, "rate": 30, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.28, "speed": 1.8, "speedVar": 0.4, "size": 0.3, "sizeEnd": 0.0,
        "life": 0.6, "lifeVar": 0.3, "color": [1.0, 0.62, 0.2], "colorEnd": [1.0, 0.12, 0.04],
        "intensity": 4.0, "gravity": 0.6, "turbStrength": 0.6, "flicker": 0.5, "light": true, "lightRange": 6.0 },
      "transform": { "position": [5.0, 3.3, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "DecoTarget_1", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 2.0, 10.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "DecoTarget_2", "primitive": "box", "color": [0.95, 0.28, 0.2],
      "luaScript": { "scriptPath": "components/Target.lua", "enabled": true, "props": [] },
      "transform": { "position": [-8.0, 2.6, 9.0], "rotation": [0.0, 30.0, 0.0], "scale": [1.1, 1.1, 0.25] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/TitleController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.04, 0.05, 0.08, 0.86], "raycastBlock": false,
                   "gradientDir": 2, "gradientColor2": [0.02, 0.03, 0.05, 1.0] },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.4 } },
    { "name": "AccentBand", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [96.0, 70.0], "offsetMax": [104.0, -70.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [0.35, 0.75, 0.95, 0.5], "raycastBlock": false },
      "uiAnimator": { "showAnim": 5, "showDuration": 0.5, "showDelay": 0.1, "showEasing": 7, "slideOffset": 200.0 } },
    { "name": "Title", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.32], "anchorMax": [0.0, 0.32], "pivot": [0.0, 0.5],
                  "offsetMin": [140.0, -85.0], "offsetMax": [1100.0, 85.0], "order": 2 },
      "uiText": { "text": "STEEL RANGE", "fontSize": 110, "color": [0.92, 0.97, 1.0, 1.0],
                  "alignH": 0, "alignV": 1, "letterSpacing": 12.0,
                  "gradientDir": 2, "gradientColor2": [0.35, 0.72, 0.95, 1.0],
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [3.0, 4.0] },
      "uiAnimator": { "showAnim": 3, "showDuration": 0.45, "showDelay": 0.15, "showEasing": 7, "slideOffset": 120.0 } },
    { "name": "Subtitle", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.45], "anchorMax": [0.0, 0.45], "pivot": [0.0, 0.5],
                  "offsetMin": [144.0, -22.0], "offsetMax": [1000.0, 22.0], "order": 2 },
      "uiText": { "text": "DX12 ENGINE  —  FPS TEMPLATE", "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.55],
                  "alignH": 0, "alignV": 1, "letterSpacing": 6.0 },
      "uiAnimator": { "showAnim": 3, "showDuration": 0.45, "showDelay": 0.28, "showEasing": 7, "slideOffset": 100.0 } },
    { "name": "BtnStart", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.62], "anchorMax": [0.0, 0.62], "pivot": [0.0, 0.5],
                  "offsetMin": [140.0, -34.0], "offsetMax": [480.0, 34.0], "order": 3 },
      "uiImage": { "texturePath": "", "color": [0.08, 0.12, 0.17, 0.92], "raycastBlock": true,
                   "outlineWidth": 1.5, "outlineColor": [0.45, 0.8, 0.95, 0.45],
                   "outlineStyle": 2, "outlineDash": 16.0,
                   "shadowColor": [0.0, 0.0, 0.0, 0.4], "shadowOffset": [0.0, 4.0], "shadowSoftness": 8.0 },
      "uiButton": { "onClickEvent": "ev_start", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.6, 1.6, 1.6, 1.0], "pressedColor": [0.7, 0.7, 0.7, 1.0] },
      "uiAnimator": { "showAnim": 3, "showDuration": 0.45, "showDelay": 0.42, "showEasing": 7,
                      "slideOffset": 100.0, "hoverScale": 1.04, "pressScale": 0.96 } },
    { "name": "BtnStartLabel", "parent": 15,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "MISSION START", "fontSize": 30, "color": [0.92, 0.98, 1.0, 0.95],
                  "alignH": 1, "alignV": 1, "letterSpacing": 4.0 } },
    { "name": "HintText", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 1.0], "anchorMax": [0.0, 1.0], "pivot": [0.0, 1.0],
                  "offsetMin": [140.0, -110.0], "offsetMax": [1100.0, -60.0], "order": 2 },
      "uiText": { "text": "WASD 移動 / マウス 視点 / 左クリック 射撃", "fontSize": 24,
                  "color": [1.0, 1.0, 1.0, 0.55], "alignH": 0, "alignV": 1 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.5, "showDelay": 0.7 } },
    { "name": "Version", "parent": 10,
      "uiRect": { "anchorMin": [1.0, 1.0], "anchorMax": [1.0, 1.0], "pivot": [1.0, 1.0],
                  "offsetMin": [-300.0, -70.0], "offsetMax": [-40.0, -30.0], "order": 2 },
      "uiText": { "text": "prototype v0.1", "fontSize": 20, "color": [1.0, 1.0, 1.0, 0.35],
                  "alignH": 2, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.7,
    "vignetteOn": true, "vignette": 0.35,
    "fxaaOn": true },
  "ssao": { "enabled": true },
  "shadows": true
})JSON";

constexpr const char* kFpsClearScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.3], "color": [0.75, 0.85, 1.0], "intensity": 0.6, "ambient": 0.35 },
      "transform": { "position": [0.0, 10.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Camera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 200.0, "isActive": true },
      "transform": { "position": [0.0, 2.0, -8.0], "rotation": [8.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_1",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 10, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.5, "speed": 4.0, "speedVar": 0.5, "size": 0.2, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [1.0, 0.85, 0.3], "colorEnd": [1.0, 0.5, 0.1],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [-4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_2",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 10, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.5, "speed": 4.0, "speedVar": 0.5, "size": 0.2, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [0.4, 0.8, 1.0], "colorEnd": [0.2, 0.4, 1.0],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/ClearController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 5,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.05, 0.07, 0.1, 0.88], "raycastBlock": false,
                   "gradientDir": 4, "gradientColor2": [0.02, 0.03, 0.05, 1.0] } },
    { "name": "Panel", "parent": 5,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-390.0, -200.0], "offsetMax": [390.0, 200.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [0.06, 0.08, 0.12, 0.95], "cornerRadius": 20.0,
                   "outlineWidth": 2.0, "outlineColor": [0.4, 0.8, 0.95, 0.45],
                   "outlineStyle": 2, "outlineDash": 22.0,
                   "shadowColor": [0.0, 0.0, 0.0, 0.5], "shadowOffset": [0.0, 10.0], "shadowSoftness": 20.0 },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.5, "showEasing": 5 } },
    { "name": "ClearTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.08], "anchorMax": [1.0, 0.42], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "MISSION COMPLETE", "fontSize": 62, "color": [1.0, 0.92, 0.55, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 4.0,
                  "gradientDir": 2, "gradientColor2": [0.9, 0.55, 0.15, 1.0],
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 3.0] },
      "uiAnimator": { "showAnim": 5, "showDuration": 0.45, "showDelay": 0.2, "showEasing": 7 } },
    { "name": "ClearSub", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.42], "anchorMax": [1.0, 0.6], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "射撃レンジ制覇！おつかれさま", "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.7],
                  "alignH": 1, "alignV": 1 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.4, "showDelay": 0.4 } },
    { "name": "BtnRetry", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [-330.0, -32.0], "offsetMax": [-30.0, 32.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [0.1, 0.16, 0.22, 0.95], "cornerRadius": 10.0, "raycastBlock": true,
                   "outlineWidth": 1.5, "outlineColor": [0.45, 0.8, 0.95, 0.4] },
      "uiButton": { "onClickEvent": "ev_retry", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.6, 1.6, 1.6, 1.0], "pressedColor": [0.7, 0.7, 0.7, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.55, "showEasing": 7,
                      "hoverScale": 1.05, "pressScale": 0.95 } },
    { "name": "BtnRetryLabel", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "もういちど", "fontSize": 28, "color": [0.92, 0.98, 1.0, 0.95], "alignH": 1, "alignV": 1 } },
    { "name": "BtnTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [30.0, -32.0], "offsetMax": [330.0, 32.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [0.1, 0.16, 0.22, 0.95], "cornerRadius": 10.0, "raycastBlock": true,
                   "outlineWidth": 1.5, "outlineColor": [0.45, 0.8, 0.95, 0.4] },
      "uiButton": { "onClickEvent": "ev_title", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.6, 1.6, 1.6, 1.0], "pressedColor": [0.7, 0.7, 0.7, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.65, "showEasing": 7,
                      "hoverScale": 1.05, "pressScale": 0.95 } },
    { "name": "BtnTitleLabel", "parent": 12,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "タイトルへ", "fontSize": 28, "color": [0.92, 0.98, 1.0, 0.95], "alignH": 1, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.55, "bloomThreshold": 0.65,
    "vignetteOn": true, "vignette": 0.38,
    "fxaaOn": true },
  "shadows": false
})JSON";

// ============================================================================
// tps テンプレート — 「COIN RUSH」三人称コイン集めアクション
// ============================================================================

constexpr const char* kTpsControllerLua = R"LUA(-- 三人称プレイヤー操作（CharacterController 物理前提・追従カメラつき）
-- 操作: WASD/左スティック=移動（カメラ基準）  マウス/右スティック=カメラ周回
--       SPACE/A=ジャンプ  ESC=マウス解放
properties = {
  { name = "cam",        type = "entity", label = "追従カメラ" },
  { name = "speed",      type = "float",  default = 6.0,  min = 1.0,  max = 20.0, label = "移動速度(m/s)" },
  { name = "sens",       type = "float",  default = 0.15, min = 0.02, max = 0.5,  label = "マウス感度" },
  { name = "dist",       type = "float",  default = 7.5,  min = 2.0,  max = 20.0, label = "カメラ距離" },
  { name = "lookHeight", type = "float",  default = 1.2,  label = "注視点の高さ" },
  { name = "turnSpeed",  type = "float",  default = 720.0, label = "旋回速度(度/秒)" },
}

local yaw, pitch = 0, 24

function OnStart(self)
  input:setMouseCapture(true)
  yaw, pitch = 0, 24
end

function OnUpdate(self, dt)
  local me = scene:findEntity(self.name)
  if not (me and me:isValid()) then return end
  local cam = self.cam
  if not (cam and cam:isValid()) then return end

  if keyPressed("ESC") then input:setMouseCapture(not input:isMouseCaptured()) end

  -- カメラ周回（マウス + 右スティック）
  if input:isMouseCaptured() then
    yaw   = yaw   + input:getMouseDeltaX() * self.sens
    pitch = pitch + input:getMouseDeltaY() * self.sens
  end
  local rsx, rsy = padStick("right")
  yaw   = yaw   + rsx * 170 * dt
  pitch = pitch - rsy * 100 * dt
  if pitch <  8 then pitch =  8 end
  if pitch > 70 then pitch = 70 end

  -- 移動（カメラ基準の目標速度を物理へ）
  local yr = math.rad(yaw)
  local fx, fz = math.sin(yr), math.cos(yr)
  local sx, sz = math.cos(yr), -math.sin(yr)
  local ix, iz = 0, 0
  if keyDown("W") then ix = ix + fx; iz = iz + fz end
  if keyDown("S") then ix = ix - fx; iz = iz - fz end
  if keyDown("D") then ix = ix + sx; iz = iz + sz end
  if keyDown("A") then ix = ix - sx; iz = iz - sz end
  local lsx, lsy = padStick("left")
  ix = ix + fx * lsy + sx * lsx
  iz = iz + fz * lsy + sz * lsx
  local len = math.sqrt(ix * ix + iz * iz)
  if len > 1 then ix, iz = ix / len, iz / len end
  physics:move(me, ix * self.speed, iz * self.speed)
  if keyPressed("SPACE") or padPressed("A") then physics:jump(me) end

  -- 進行方向へなめらかに向く
  if len > 0.1 then
    local target = math.deg(math.atan(ix, iz))
    local cur = me.transform.rotation.y
    local diff = (target - cur + 180) % 360 - 180
    local step = self.turnSpeed * dt
    if diff >  step then diff =  step end
    if diff < -step then diff = -step end
    me.transform.rotation = Vec3.new(0, cur + diff, 0)
  end

  -- カメラをキャラの背後へ（ピッチに応じて高さも変える）
  local p  = me.transform.position
  local pr = math.rad(pitch)
  local horiz = self.dist * math.cos(pr)
  cam.transform.position = Vec3.new(p.x - fx * horiz,
                                    p.y + self.dist * math.sin(pr) + self.lookHeight * 0.5,
                                    p.z - fz * horiz)
  cam.transform.rotation = Vec3.new(pitch, yaw, 0)
end
)LUA";

constexpr const char* kTpsCollectibleLua = R"LUA(-- コイン: くるくる回って近づくと取れる（coinGet を発火して消える）
properties = {
  { name = "value",     type = "int",   default = 1,    label = "枚数" },
  { name = "radius",    type = "float", default = 1.4,  label = "取得半径" },
  { name = "spinSpeed", type = "float", default = 140.0, label = "回転速度(度/秒)" },
  { name = "bobAmount", type = "float", default = 0.18, label = "浮遊の振れ幅" },
}

function OnStart(self)
  self._baseY = self.transform.position.y
  self._phase = (self.entity % 9) * 0.7
end

function OnUpdate(self, dt)
  local t = self.transform
  t.rotation = Vec3.new(0, t.rotation.y + self.spinSpeed * dt, 0)
  t.position = Vec3.new(t.position.x,
                        self._baseY + math.sin(time.now() * 2.2 + self._phase) * self.bobAmount,
                        t.position.z)

  local player = scene:findEntity("Player")
  if not (player and player:isValid()) then return end
  local p, q = t.position, player.transform.position
  local dx, dy, dz = p.x - q.x, p.y - q.y, p.z - q.z
  if dx * dx + dy * dy + dz * dz < self.radius * self.radius then
    fx:burst{ x = p.x, y = p.y, z = p.z, count = 18, kind = "star", speed = 3.5,
              size = 0.28, sizeEnd = 0.0, life = 0.5, r = 1.0, g = 0.85, b = 0.25,
              intensity = 4.0, gravity = -2.0 }
    events:emit("coinGet", { value = self.value })
    local me = scene:findEntity(self.name)
    if me and me:isValid() then scene:remove(me) end
  end
end
)LUA";

constexpr const char* kTpsGameManagerLua = R"LUA(-- コイン集めの進行管理: HUD 更新、全部集めてゴールパッドでクリア → 次のシーンへ
properties = {
  { name = "totalCoins", type = "int",   default = 8,   label = "コインの総数" },
  { name = "clearDelay", type = "float", default = 1.8, label = "クリア後の待ち(秒)" },
}

local function refresh(self)
  local c = scene:findEntity("HudCoins")
  if c and c:isValid() then scene:setUiText(c, string.format("%d / %d", self._got, self.totalCoins)) end
end

function OnStart(self)
  self._got, self._cleared = 0, false
  local mgr = self
  refresh(mgr)

  events:on("coinGet", function(d)
    mgr._got = mgr._got + (d.value or 1)
    refresh(mgr)
    local c = scene:findEntity("HudCoins")
    if c and c:isValid() then uifx.punch(c) end
    if mgr._got >= mgr.totalCoins then
      local hint = scene:findEntity("HudHint")
      if hint and hint:isValid() then
        scene:setUiText(hint, "コインコンプリート！ 光るゴールパッドへ！")
        scene:tweenUi(hint, { alpha = 1, duration = 0.3 })
        uifx.punch(hint)
      end
    end
  end)

  -- ゴールパッドの Trigger（EmitEvent アクション）から飛んでくる
  events:on("reachedGoal", function()
    if mgr._cleared then return end
    if mgr._got >= mgr.totalCoins then
      mgr._cleared = true
      local g = scene:findEntity("GoalPad")
      if g and g:isValid() then
        local p = g.transform.position
        FX.supernova(p.x, p.y + 1.0, p.z, 1.0)
      end
      local panel = scene:findEntity("ClearPanel")
      if panel and panel:isValid() then scene:showUi(panel) end
      time.after(mgr.clearDelay, function() win() end)
    else
      local hint = scene:findEntity("HudHint")
      if hint and hint:isValid() then
        scene:setUiText(hint, "コインがあと " .. (mgr.totalCoins - mgr._got) .. " まい！")
        scene:tweenUi(hint, { alpha = 1, duration = 0.2 })
        uifx.hit(hint)
      end
    end
  end)

  time.after(6.0, function()
    if mgr._got > 0 then return end
    local hint = scene:findEntity("HudHint")
    if hint and hint:isValid() then scene:tweenUi(hint, { alpha = 0.4, duration = 0.8 }) end
  end)
end
)LUA";

constexpr const char* kTpsMainScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.35, -1.0, -0.4], "color": [1.0, 0.95, 0.82], "intensity": 1.2, "ambient": 0.42 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "MainCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 500.0, "isActive": true },
      "transform": { "position": [0.0, 6.0, -16.0], "rotation": [24.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Player", "primitive": "box", "color": [0.3, 0.85, 0.5],
      "characterController": { "radius": 0.4, "halfHeight": 0.55, "jumpSpeed": 8.0, "stepHeight": 0.45 },
      "luaScript": { "scriptPath": "components/TpsController.lua", "enabled": true,
                     "props": [ { "name": "cam", "type": "entity", "value": "MainCamera" } ] },
      "transform": { "position": [0.0, 1.2, -10.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.8, 1.6, 0.8] } },
    { "name": "Floor", "primitive": "plane", "color": [0.2, 0.3, 0.18],
      "rigidBody": { "motionType": 0 },
      "boxCollider": { "halfExtents": [25.0, 0.5, 25.0], "offset": [0.0, -0.5, 0.0] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Wall_N", "primitive": "box", "color": [0.32, 0.38, 0.3],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [0.0, 1.2, 19.0], "rotation": [0.0, 0.0, 0.0], "scale": [40.0, 2.4, 1.6] } },
    { "name": "Wall_S", "primitive": "box", "color": [0.32, 0.38, 0.3],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [0.0, 1.2, -19.0], "rotation": [0.0, 0.0, 0.0], "scale": [40.0, 2.4, 1.6] } },
    { "name": "Wall_E", "primitive": "box", "color": [0.32, 0.38, 0.3],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [19.0, 1.2, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.6, 2.4, 40.0] } },
    { "name": "Wall_W", "primitive": "box", "color": [0.32, 0.38, 0.3],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-19.0, 1.2, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.6, 2.4, 40.0] } },
    { "name": "Step_1", "primitive": "box", "color": [0.52, 0.44, 0.62],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-8.0, 0.5, 4.0], "rotation": [0.0, 0.0, 0.0], "scale": [3.0, 1.0, 3.0] } },
    { "name": "Step_2", "primitive": "box", "color": [0.52, 0.44, 0.62],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-8.0, 1.5, 7.0], "rotation": [0.0, 0.0, 0.0], "scale": [3.0, 1.0, 3.0] } },
    { "name": "Step_3", "primitive": "box", "color": [0.52, 0.44, 0.62],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [-8.0, 2.5, 10.0], "rotation": [0.0, 0.0, 0.0], "scale": [3.0, 1.0, 3.0] } },
    { "name": "Block_1", "primitive": "box", "color": [0.42, 0.5, 0.62],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [6.0, 0.75, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.4, 1.5, 2.4] } },
    { "name": "Block_2", "primitive": "box", "color": [0.42, 0.5, 0.62],
      "rigidBody": { "motionType": 0 }, "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [10.0, 1.4, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.2, 2.8, 2.2] } },
    { "name": "Crate_1", "primitive": "box", "color": [0.72, 0.58, 0.34],
      "rigidBody": { "motionType": 2, "mass": 2.0, "friction": 0.6, "restitution": 0.25 },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [2.0, 0.6, -4.0], "rotation": [0.0, 20.0, 0.0], "scale": [1.2, 1.2, 1.2] } },
    { "name": "Crate_2", "primitive": "box", "color": [0.72, 0.58, 0.34],
      "rigidBody": { "motionType": 2, "mass": 2.0, "friction": 0.6, "restitution": 0.25 },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "transform": { "position": [3.4, 0.6, -3.0], "rotation": [0.0, -15.0, 0.0], "scale": [1.2, 1.2, 1.2] } },
    { "name": "Coin_1", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 1.0, -2.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_2", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [6.0, 2.2, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_3", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [10.0, 3.5, 6.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_4", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [-8.0, 3.7, 10.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_5", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [-4.0, 1.0, 8.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_6", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [4.0, 1.0, 12.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_7", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [-12.0, 1.0, -6.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "Coin_8", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true, "props": [] },
      "transform": { "position": [12.0, 1.0, -8.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.55, 0.55, 0.55] } },
    { "name": "GoalPad", "primitive": "box", "color": [0.2, 0.8, 0.9],
      "transform": { "position": [0.0, 0.1, 15.0], "rotation": [0.0, 0.0, 0.0], "scale": [3.0, 0.2, 3.0] } },
    { "name": "GoalZone",
      "trigger": { "shape": 0, "halfExtents": [1.6, 1.5, 1.6], "offset": [0.0, 0.0, 0.0],
                   "filter": "Player", "once": false,
                   "actions": [
                     { "when": 0, "type": 10, "str": "reachedGoal", "num": 0.0 },
                     { "when": 0, "type": 4,  "target": "GoalBurst" } ] },
      "transform": { "position": [0.0, 1.2, 15.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "GoalBurst",
      "particleEmitter": { "kind": 4, "blend": 0, "rate": 140, "playOnStart": false, "looping": false, "duration": 0.8,
        "dir": [0.0, 1.0, 0.0], "spread": 0.6, "speed": 4.5, "speedVar": 0.5, "size": 0.3, "sizeEnd": 0.0,
        "life": 0.8, "lifeVar": 0.3, "color": [0.5, 0.95, 1.0], "colorEnd": [0.2, 0.4, 1.0],
        "intensity": 5.0, "gravity": -1.5 },
      "transform": { "position": [0.0, 1.0, 15.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "GoalAura",
      "particleEmitter": { "kind": 4, "blend": 0, "rate": 9, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.25, "speed": 1.2, "speedVar": 0.4, "size": 0.22, "sizeEnd": 0.0,
        "life": 1.2, "lifeVar": 0.3, "color": [0.4, 0.9, 1.0], "colorEnd": [0.15, 0.35, 0.9],
        "intensity": 3.0, "gravity": -0.4 },
      "transform": { "position": [0.0, 0.4, 15.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bush_1", "primitive": "box", "color": [0.25, 0.5, 0.28],
      "transform": { "position": [-13.0, 0.6, 12.0], "rotation": [0.0, 20.0, 0.0], "scale": [1.8, 1.2, 1.8] } },
    { "name": "Bush_2", "primitive": "box", "color": [0.25, 0.5, 0.28],
      "transform": { "position": [13.0, 0.6, 10.0], "rotation": [0.0, -30.0, 0.0], "scale": [1.8, 1.2, 1.8] } },
    { "name": "GameManager",
      "luaScript": { "scriptPath": "components/CoinGameManager.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "CoinIcon", "parent": 30,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 0.0], "pivot": [0.0, 0.0],
                  "offsetMin": [40.0, 34.0], "offsetMax": [78.0, 72.0], "order": 5 },
      "uiImage": { "texturePath": "", "color": [1.0, 0.84, 0.2, 1.0], "shape": 1, "raycastBlock": false,
                   "outlineWidth": 3.0, "outlineColor": [0.8, 0.55, 0.1, 1.0] },
      "uiAnimator": { "showAnim": 0, "loopAnim": 2, "loopSpeed": 1.2, "loopAmount": 0.06 } },
    { "name": "HudCoins", "parent": 30,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 0.0], "pivot": [0.0, 0.0],
                  "offsetMin": [92.0, 30.0], "offsetMax": [340.0, 78.0], "order": 5 },
      "uiText": { "text": "0 / 8", "fontSize": 38, "color": [1.0, 0.97, 0.85, 1.0],
                  "alignH": 0, "alignV": 1, "letterSpacing": 2.0,
                  "shadowColor": [0.0, 0.0, 0.0, 0.55], "shadowOffset": [2.0, 2.0] } },
    { "name": "HudHint", "parent": 30,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 1.0],
                  "offsetMin": [-640.0, -92.0], "offsetMax": [640.0, -40.0], "order": 5 },
      "uiText": { "text": "WASD 移動 / マウス カメラ / SPACE ジャンプ — コインを全部あつめてゴールへ！",
                  "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.78], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 2.0] } },
    { "name": "ClearPanel", "parent": 30,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-340.0, -130.0], "offsetMax": [340.0, 130.0], "order": 20, "visible": false },
      "uiImage": { "texturePath": "", "color": [0.1, 0.08, 0.04, 0.94], "cornerRadius": 22.0,
                   "outlineWidth": 2.0, "outlineColor": [1.0, 0.84, 0.3, 0.55],
                   "shadowColor": [0.0, 0.0, 0.0, 0.5], "shadowOffset": [0.0, 8.0], "shadowSoftness": 16.0 },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.45, "showEasing": 5 } },
    { "name": "ClearTitle", "parent": 34,
      "uiRect": { "anchorMin": [0.0, 0.1], "anchorMax": [1.0, 0.6], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "COURSE CLEAR!", "fontSize": 60, "color": [1.0, 0.92, 0.5, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 4.0,
                  "gradientDir": 2, "gradientColor2": [0.95, 0.6, 0.15, 1.0],
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 3.0] } },
    { "name": "ClearSub", "parent": 34,
      "uiRect": { "anchorMin": [0.0, 0.6], "anchorMax": [1.0, 0.92], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "まもなくリザルトへ…", "fontSize": 22, "color": [1.0, 1.0, 1.0, 0.6],
                  "alignH": 1, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.7,
    "vignetteOn": true, "vignette": 0.26,
    "saturationOn": true, "saturation": 1.15,
    "warmthOn": true, "warmth": 0.06,
    "fxaaOn": true },
  "ssao": { "enabled": true },
  "shadows": true
})JSON";

constexpr const char* kTpsTitleScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.35, -1.0, -0.4], "color": [1.0, 0.93, 0.78], "intensity": 1.1, "ambient": 0.45 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "TitleCamera",
      "camera": { "fovDegrees": 55.0, "nearClip": 0.1, "farClip": 500.0, "isActive": true },
      "luaScript": { "scriptPath": "components/OrbitCamera.lua", "enabled": true,
                     "props": [ { "name": "center", "type": "vec3", "value": [0.0, 1.0, 0.0] },
                                { "name": "radius", "type": "float", "value": 11.0 },
                                { "name": "height", "type": "float", "value": 4.0 },
                                { "name": "degPerSec", "type": "float", "value": 5.0 } ] },
      "transform": { "position": [0.0, 4.0, -11.0], "rotation": [18.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Floor", "primitive": "plane", "color": [0.2, 0.3, 0.18],
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "DecoCoin_1", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true,
                     "props": [ { "name": "radius", "type": "float", "value": 0.0 } ] },
      "transform": { "position": [-3.0, 1.6, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.9, 0.9, 0.9] } },
    { "name": "DecoCoin_2", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Collectible.lua", "enabled": true,
                     "props": [ { "name": "radius", "type": "float", "value": 0.0 } ] },
      "transform": { "position": [3.2, 1.2, 1.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.7, 0.7, 0.7] } },
    { "name": "DecoAura",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 7, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.7, "speed": 1.4, "speedVar": 0.5, "size": 0.2, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [1.0, 0.85, 0.3], "colorEnd": [1.0, 0.5, 0.1],
        "intensity": 3.5, "gravity": -0.6 },
      "transform": { "position": [0.0, 0.6, 1.5], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/TitleController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.1, 0.09, 0.05, 0.72], "raycastBlock": false,
                   "gradientDir": 2, "gradientColor2": [0.03, 0.04, 0.03, 1.0] },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.4 } },
    { "name": "Title", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.32], "anchorMax": [0.5, 0.32], "pivot": [0.5, 0.5],
                  "offsetMin": [-700.0, -100.0], "offsetMax": [700.0, 100.0], "order": 2 },
      "uiText": { "text": "COIN RUSH", "fontSize": 128, "color": [1.0, 0.9, 0.45, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 8.0,
                  "gradientDir": 2, "gradientColor2": [0.9, 0.55, 0.12, 1.0],
                  "shadowColor": [0.25, 0.12, 0.0, 0.7], "shadowOffset": [0.0, 6.0] },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.55, "showDelay": 0.1, "showEasing": 5,
                      "loopAnim": 1, "loopSpeed": 0.5, "loopAmount": 6.0 } },
    { "name": "Subtitle", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.47], "anchorMax": [0.5, 0.47], "pivot": [0.5, 0.5],
                  "offsetMin": [-500.0, -22.0], "offsetMax": [500.0, 22.0], "order": 2 },
      "uiText": { "text": "DX12 ENGINE  —  3D ACTION TEMPLATE", "fontSize": 24, "color": [1.0, 1.0, 1.0, 0.55],
                  "alignH": 1, "alignV": 1, "letterSpacing": 5.0 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.5, "showDelay": 0.35 } },
    { "name": "BtnStart", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.64], "anchorMax": [0.5, 0.64], "pivot": [0.5, 0.5],
                  "offsetMin": [-170.0, -36.0], "offsetMax": [170.0, 36.0], "order": 3 },
      "uiImage": { "texturePath": "", "color": [0.85, 0.55, 0.12, 0.96], "cornerRadius": 36.0, "raycastBlock": true,
                   "gradientDir": 2, "gradientColor2": [0.65, 0.35, 0.05, 1.0],
                   "shadowColor": [0.0, 0.0, 0.0, 0.4], "shadowOffset": [0.0, 5.0], "shadowSoftness": 10.0 },
      "uiButton": { "onClickEvent": "ev_start", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.35, 1.35, 1.35, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 8, "showDuration": 0.6, "showDelay": 0.5, "showEasing": 5,
                      "hoverScale": 1.07, "pressScale": 0.94 } },
    { "name": "BtnStartLabel", "parent": 11,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "スタート！", "fontSize": 32, "color": [1.0, 0.98, 0.9, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 2.0,
                  "shadowColor": [0.3, 0.15, 0.0, 0.5], "shadowOffset": [0.0, 2.0] } },
    { "name": "HintText", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 1.0],
                  "offsetMin": [-600.0, -100.0], "offsetMax": [600.0, -52.0], "order": 2 },
      "uiText": { "text": "コインを全部あつめて ゴールをめざせ！", "fontSize": 24,
                  "color": [1.0, 1.0, 1.0, 0.6], "alignH": 1, "alignV": 1 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.5, "showDelay": 0.7 } },
    { "name": "Version", "parent": 7,
      "uiRect": { "anchorMin": [1.0, 1.0], "anchorMax": [1.0, 1.0], "pivot": [1.0, 1.0],
                  "offsetMin": [-300.0, -64.0], "offsetMax": [-40.0, -28.0], "order": 2 },
      "uiText": { "text": "prototype v0.1", "fontSize": 20, "color": [1.0, 1.0, 1.0, 0.35],
                  "alignH": 2, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.65,
    "vignetteOn": true, "vignette": 0.3,
    "saturationOn": true, "saturation": 1.12,
    "warmthOn": true, "warmth": 0.08,
    "fxaaOn": true },
  "ssao": { "enabled": true },
  "shadows": true
})JSON";

constexpr const char* kTpsClearScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.35, -1.0, -0.4], "color": [1.0, 0.93, 0.78], "intensity": 0.9, "ambient": 0.4 },
      "transform": { "position": [0.0, 10.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Camera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 200.0, "isActive": true },
      "transform": { "position": [0.0, 2.0, -8.0], "rotation": [8.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_1",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 12, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.55, "speed": 4.5, "speedVar": 0.5, "size": 0.22, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [1.0, 0.85, 0.3], "colorEnd": [1.0, 0.5, 0.1],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [-4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_2",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 12, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.55, "speed": 4.5, "speedVar": 0.5, "size": 0.22, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [0.5, 1.0, 0.6], "colorEnd": [0.15, 0.7, 0.4],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/ClearController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 5,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.1, 0.08, 0.04, 0.88], "raycastBlock": false,
                   "gradientDir": 4, "gradientColor2": [0.03, 0.03, 0.02, 1.0] } },
    { "name": "Panel", "parent": 5,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-390.0, -200.0], "offsetMax": [390.0, 200.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [0.12, 0.09, 0.04, 0.96], "cornerRadius": 26.0,
                   "outlineWidth": 2.0, "outlineColor": [1.0, 0.84, 0.3, 0.5],
                   "shadowColor": [0.0, 0.0, 0.0, 0.5], "shadowOffset": [0.0, 10.0], "shadowSoftness": 20.0 },
      "uiAnimator": { "showAnim": 8, "showDuration": 0.55, "showEasing": 5 } },
    { "name": "ClearTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.08], "anchorMax": [1.0, 0.42], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "ALL CLEAR!", "fontSize": 68, "color": [1.0, 0.92, 0.5, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 5.0,
                  "gradientDir": 2, "gradientColor2": [0.95, 0.6, 0.15, 1.0],
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 3.0] },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.5, "showDelay": 0.25, "showEasing": 5 } },
    { "name": "ClearSub", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.42], "anchorMax": [1.0, 0.6], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "コインを全部あつめた！おつかれさま", "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.7],
                  "alignH": 1, "alignV": 1 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.4, "showDelay": 0.45 } },
    { "name": "BtnRetry", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [-330.0, -34.0], "offsetMax": [-30.0, 34.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [0.85, 0.55, 0.12, 0.95], "cornerRadius": 34.0, "raycastBlock": true,
                   "gradientDir": 2, "gradientColor2": [0.65, 0.35, 0.05, 1.0] },
      "uiButton": { "onClickEvent": "ev_retry", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.35, 1.35, 1.35, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.6, "showEasing": 7,
                      "hoverScale": 1.06, "pressScale": 0.94 } },
    { "name": "BtnRetryLabel", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "もういちど", "fontSize": 28, "color": [1.0, 0.98, 0.9, 1.0], "alignH": 1, "alignV": 1 } },
    { "name": "BtnTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [30.0, -34.0], "offsetMax": [330.0, 34.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [0.25, 0.2, 0.1, 0.95], "cornerRadius": 34.0, "raycastBlock": true,
                   "outlineWidth": 1.5, "outlineColor": [1.0, 0.84, 0.3, 0.4] },
      "uiButton": { "onClickEvent": "ev_title", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.4, 1.4, 1.4, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.7, "showEasing": 7,
                      "hoverScale": 1.06, "pressScale": 0.94 } },
    { "name": "BtnTitleLabel", "parent": 12,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "タイトルへ", "fontSize": 28, "color": [1.0, 0.98, 0.9, 0.95], "alignH": 1, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.55, "bloomThreshold": 0.65,
    "vignetteOn": true, "vignette": 0.34,
    "warmthOn": true, "warmth": 0.08,
    "fxaaOn": true },
  "shadows": false
})JSON";

// ============================================================================
// 2d テンプレート — 「SKY HOPPER」横スクロールプラットフォーマー
// ============================================================================

constexpr const char* kPlatformerControllerLua = R"LUA(-- 横スクロール2D プレイヤー操作（手書き AABB 物理・コヨーテタイム/先行入力つき）
-- 操作: A/D or ←/→ =移動  SPACE/A =ジャンプ
-- 足場は Ground_* / Platform_* / MovingPlat_* の名前で自動収集する
properties = {
  { name = "speed",     type = "float", default = 7.0,  label = "移動速度" },
  { name = "jumpSpeed", type = "float", default = 10.5, label = "ジャンプ初速" },
  { name = "gravity",   type = "float", default = 26.0, label = "重力" },
  { name = "halfW",     type = "float", default = 0.42, label = "当たり半幅" },
  { name = "halfH",     type = "float", default = 0.8,  label = "当たり半高" },
  { name = "respawn",   type = "vec3",  default = {-14.0, 1.5, 0.0}, label = "復活地点" },
  { name = "deadY",     type = "float", default = -7.0, label = "落下死ライン" },
}

local function collectSolids()
  local list = {}
  for _, pre in ipairs({ "Ground_", "Platform_", "MovingPlat_" }) do
    for i = 1, 32 do
      local e = scene:findEntity(pre .. i)
      if e and e:isValid() then list[#list + 1] = pre .. i end
    end
  end
  return list
end

local function aabb(e)
  local t = e.transform
  return t.position.x, t.position.y, t.scale.x * 0.5, t.scale.y * 0.5
end

function OnStart(self)
  self._vy, self._grounded = 0, false
  self._coyote, self._buffer = 0, 0
  self._solids = collectSolids()
  self._standOn, self._standX, self._standY = nil, 0, 0
  self._done = false
end

function OnUpdate(self, dt)
  local me = scene:findEntity(self.name)
  if not (me and me:isValid()) then return end
  local t = me.transform
  local x, y = t.position.x, t.position.y

  -- 動く床に乗っていたら床の移動ぶんだけ運ばれる
  if self._standOn then
    local pf = scene:findEntity(self._standOn)
    if pf and pf:isValid() then
      x = x + (pf.transform.position.x - self._standX)
      y = y + (pf.transform.position.y - self._standY)
    end
    self._standOn = nil
  end

  -- 入力
  local dir = 0
  if keyDown("D") or keyDown("RIGHT") then dir = dir + 1 end
  if keyDown("A") or keyDown("LEFT")  then dir = dir - 1 end
  local lsx = padStick("left")
  if math.abs(lsx) > 0.25 then dir = (lsx > 0) and 1 or -1 end

  -- ジャンプ（コヨーテタイム + 先行入力）
  self._coyote = self._grounded and 0.1 or (self._coyote - dt)
  if keyPressed("SPACE") or padPressed("A") then self._buffer = 0.12
  else self._buffer = self._buffer - dt end
  if self._buffer > 0 and self._coyote > 0 then
    self._vy = self.jumpSpeed
    self._grounded, self._coyote, self._buffer = false, 0, 0
    fx:burst{ x = x, y = y - self.halfH, z = 0, count = 6, kind = "smoke", blend = 1,
              size = 0.22, sizeEnd = 0.5, speed = 1.2, life = 0.35,
              r = 0.8, g = 0.8, b = 0.85, intensity = 1.0 }
  end

  -- X 移動 + 壁判定
  x = x + dir * self.speed * dt
  for _, n in ipairs(self._solids) do
    local s = scene:findEntity(n)
    if s and s:isValid() then
      local sx, sy, hw, hh = aabb(s)
      if math.abs(x - sx) < self.halfW + hw and math.abs(y - sy) < self.halfH + hh - 0.05 then
        if x > sx then x = sx + hw + self.halfW else x = sx - hw - self.halfW end
      end
    end
  end

  -- Y 移動 + 接地/天井判定
  self._vy = self._vy - self.gravity * dt
  local prevY = y
  y = y + self._vy * dt
  self._grounded = false
  for _, n in ipairs(self._solids) do
    local s = scene:findEntity(n)
    if s and s:isValid() then
      local sx, sy, hw, hh = aabb(s)
      if math.abs(x - sx) < self.halfW + hw and math.abs(y - sy) < self.halfH + hh then
        if self._vy <= 0 and prevY - self.halfH >= sy + hh - 0.15 then
          y = sy + hh + self.halfH
          self._vy, self._grounded = 0, true
          self._standOn, self._standX, self._standY = n, sx, sy
        elseif self._vy > 0 and prevY + self.halfH <= sy - hh + 0.15 then
          y = sy - hh - self.halfH
          self._vy = 0
        end
      end
    end
  end

  t.position = Vec3.new(x, y, 0)

  -- トゲ / 落下 → 復活
  local dead = y < self.deadY
  if not dead then
    for i = 1, 16 do
      local s = scene:findEntity("Spike_" .. i)
      if s and s:isValid() then
        local sx, sy, hw, hh = aabb(s)
        if math.abs(x - sx) < self.halfW + hw - 0.1 and math.abs(y - sy) < self.halfH + hh - 0.1 then
          dead = true
          break
        end
      end
    end
  end
  if dead then
    FX.explosion(x, y, 0, 0.5, 1.0, 0.35, 0.25)
    FX.hit(0.4)
    t.position = Vec3.new(self.respawn.x, self.respawn.y, 0)
    self._vy = 0
    events:emit("playerDied", {})
    return
  end

  -- ゴール（旗に触れたらクリア）
  if not self._done then
    local g = scene:findEntity("GoalFlag")
    if g and g:isValid() then
      local gx, gy, hw, hh = aabb(g)
      if math.abs(x - gx) < self.halfW + hw + 0.3 and math.abs(y - gy) < self.halfH + hh + 1.0 then
        self._done = true
        events:emit("reachedGoal", {})
      end
    end
  end
end
)LUA";

constexpr const char* kCoin2DLua = R"LUA(-- 2D コイン: ぴこぴこ回転して見え、プレイヤーが近づくと取れる
properties = {
  { name = "value",  type = "int",   default = 1,   label = "枚数" },
  { name = "radius", type = "float", default = 1.0, label = "取得半径" },
}

function OnStart(self)
  self._baseY = self.transform.position.y
  self._phase = (self.entity % 9) * 0.8
  self._baseSX = self.transform.scale.x
end

function OnUpdate(self, dt)
  local t  = self.transform
  local tm = time.now() + self._phase
  t.position = Vec3.new(t.position.x, self._baseY + math.sin(tm * 2.4) * 0.12, 0)
  -- 横スケールの振動で「コインが回っている」ように見せる
  t.scale = Vec3.new(self._baseSX * (0.25 + 0.75 * math.abs(math.cos(tm * 2.0))), t.scale.y, t.scale.z)

  local player = scene:findEntity("Player")
  if not (player and player:isValid()) then return end
  local p, q = t.position, player.transform.position
  local dx, dy = p.x - q.x, p.y - q.y
  if dx * dx + dy * dy < self.radius * self.radius then
    fx:burst{ x = p.x, y = p.y, z = 0, count = 14, kind = "star", speed = 3.0,
              size = 0.24, sizeEnd = 0.0, life = 0.45, r = 1.0, g = 0.85, b = 0.25,
              intensity = 4.0, gravity = -2.0 }
    events:emit("coinGet", { value = self.value })
    local me = scene:findEntity(self.name)
    if me and me:isValid() then scene:remove(me) end
  end
end
)LUA";

constexpr const char* kMovingPlatformLua = R"LUA(-- 往復する足場（cos カーブでなめらかに往復。プレイヤーは乗ると運ばれる）
properties = {
  { name = "travel", type = "vec3",  default = {0.0, 2.5, 0.0}, label = "移動量" },
  { name = "period", type = "float", default = 3.0, min = 0.2, max = 30.0, label = "往復周期(秒)" },
}

function OnStart(self)
  local p = self.transform.position
  self._bx, self._by, self._bz = p.x, p.y, p.z
end

function OnUpdate(self, dt)
  local k = 0.5 - 0.5 * math.cos(time.now() * 2.0 * math.pi / math.max(self.period, 0.1))
  self.transform.position = Vec3.new(self._bx + self.travel.x * k,
                                     self._by + self.travel.y * k,
                                     self._bz + self.travel.z * k)
end
)LUA";

constexpr const char* kCameraFollow2DLua = R"LUA(-- 2D 横スクロールのカメラ追従（X をなめらかに追い、範囲でクランプ）
properties = {
  { name = "target", type = "entity", label = "追従対象" },
  { name = "smooth", type = "float",  default = 6.0,  label = "追従の速さ" },
  { name = "minX",   type = "float",  default = -5.0, label = "左端" },
  { name = "maxX",   type = "float",  default = 12.0, label = "右端" },
  { name = "baseY",  type = "float",  default = 3.2,  label = "基準の高さ" },
}

function OnUpdate(self, dt)
  if not (self.target and self.target:isValid()) then return end
  local t = self.transform
  local p = self.target.transform.position
  local goalX = p.x
  if goalX < self.minX then goalX = self.minX end
  if goalX > self.maxX then goalX = self.maxX end
  local goalY = self.baseY + (p.y - 1.0) * 0.25
  local k = 1.0 - math.exp(-self.smooth * dt)
  t.position = Vec3.new(t.position.x + (goalX - t.position.x) * k,
                        t.position.y + (goalY - t.position.y) * k,
                        t.position.z)
end
)LUA";

constexpr const char* kGameManager2DLua = R"LUA(-- 2D コースの進行管理: コイン HUD・ゴールでクリア演出 → 次のシーンへ
properties = {
  { name = "totalCoins", type = "int",   default = 6,   label = "コインの総数" },
  { name = "clearDelay", type = "float", default = 1.8, label = "クリア後の待ち(秒)" },
}

local function refresh(self)
  local c = scene:findEntity("HudCoins")
  if c and c:isValid() then scene:setUiText(c, string.format("%d / %d", self._got, self.totalCoins)) end
end

function OnStart(self)
  self._got = 0
  local mgr = self
  refresh(mgr)

  events:on("coinGet", function(d)
    mgr._got = mgr._got + (d.value or 1)
    refresh(mgr)
    local c = scene:findEntity("HudCoins")
    if c and c:isValid() then uifx.punch(c) end
  end)

  events:on("reachedGoal", function()
    local g = scene:findEntity("GoalFlag")
    if g and g:isValid() then
      local p = g.transform.position
      FX.supernova(p.x, p.y + 1.0, p.z, 0.9)
    end
    local panel = scene:findEntity("ClearPanel")
    if panel and panel:isValid() then scene:showUi(panel) end
    time.after(mgr.clearDelay, function() win() end)
  end)

  time.after(6.0, function()
    local hint = scene:findEntity("HudHint")
    if hint and hint:isValid() then scene:tweenUi(hint, { alpha = 0, duration = 0.8 }) end
  end)
end
)LUA";

constexpr const char* k2dMainScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.3, -0.8, -0.5], "color": [1.0, 0.97, 0.9], "intensity": 1.15, "ambient": 0.55 },
      "transform": { "position": [0.0, 12.0, -6.0], "rotation": [45.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "GameCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 1000.0, "isActive": true, "projection": 1, "orthoSize": 6.5 },
      "luaScript": { "scriptPath": "components/CameraFollow2D.lua", "enabled": true,
                     "props": [ { "name": "target", "type": "entity", "value": "Player" } ] },
      "transform": { "position": [-5.0, 3.2, -12.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Player", "primitive": "box", "color": [0.3, 0.85, 0.5],
      "luaScript": { "scriptPath": "components/PlatformerController.lua", "enabled": true, "props": [] },
      "transform": { "position": [-14.0, 1.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.84, 1.6, 0.8] } },
    { "name": "Ground_1", "primitive": "box", "color": [0.24, 0.3, 0.42],
      "transform": { "position": [-11.0, -0.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [10.0, 1.0, 2.0] } },
    { "name": "Ground_2", "primitive": "box", "color": [0.24, 0.3, 0.42],
      "transform": { "position": [-1.0, -0.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [6.0, 1.0, 2.0] } },
    { "name": "Ground_3", "primitive": "box", "color": [0.24, 0.3, 0.42],
      "transform": { "position": [9.0, -0.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [8.0, 1.0, 2.0] } },
    { "name": "Ground_4", "primitive": "box", "color": [0.24, 0.3, 0.42],
      "transform": { "position": [17.5, -0.5, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [7.0, 1.0, 2.0] } },
    { "name": "Platform_1", "primitive": "box", "color": [0.52, 0.44, 0.68],
      "transform": { "position": [-5.0, 1.4, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.6, 0.5, 1.6] } },
    { "name": "Platform_2", "primitive": "box", "color": [0.52, 0.44, 0.68],
      "transform": { "position": [3.5, 1.9, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.4, 0.5, 1.6] } },
    { "name": "Platform_3", "primitive": "box", "color": [0.52, 0.44, 0.68],
      "transform": { "position": [12.0, 2.6, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.4, 0.5, 1.6] } },
    { "name": "MovingPlat_1", "primitive": "box", "color": [0.7, 0.55, 0.3],
      "luaScript": { "scriptPath": "components/MovingPlatform.lua", "enabled": true,
                     "props": [ { "name": "travel", "type": "vec3", "value": [0.0, 2.8, 0.0] },
                                { "name": "period", "type": "float", "value": 3.2 } ] },
      "transform": { "position": [13.5, 0.6, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [2.2, 0.5, 1.6] } },
    { "name": "Spike_1", "primitive": "box", "color": [0.9, 0.25, 0.25],
      "transform": { "position": [0.0, 0.25, 0.0], "rotation": [0.0, 0.0, 45.0], "scale": [0.7, 0.7, 0.7] } },
    { "name": "Spike_2", "primitive": "box", "color": [0.9, 0.25, 0.25],
      "transform": { "position": [10.0, 0.25, 0.0], "rotation": [0.0, 0.0, 45.0], "scale": [0.7, 0.7, 0.7] } },
    { "name": "Coin_1", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [-5.0, 2.6, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "Coin_2", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [-2.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "Coin_3", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [3.5, 3.1, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "Coin_4", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [7.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "Coin_5", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [12.0, 3.8, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "Coin_6", "primitive": "sphere", "color": [1.0, 0.84, 0.2],
      "luaScript": { "scriptPath": "components/Coin2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [13.5, 4.6, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.6, 0.6, 0.6] } },
    { "name": "GoalFlag", "primitive": "box", "color": [0.85, 0.88, 0.92],
      "transform": { "position": [19.0, 1.6, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.18, 3.2, 0.18] } },
    { "name": "GoalPennant", "primitive": "box", "color": [0.3, 0.85, 0.5],
      "transform": { "position": [19.55, 2.75, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.9, 0.55, 0.12] } },
    { "name": "GoalSparkle",
      "particleEmitter": { "kind": 4, "blend": 0, "rate": 8, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.4, "speed": 1.2, "speedVar": 0.4, "size": 0.2, "sizeEnd": 0.0,
        "life": 1.0, "lifeVar": 0.3, "color": [0.5, 1.0, 0.7], "colorEnd": [0.2, 0.6, 1.0],
        "intensity": 3.0, "gravity": -0.5 },
      "transform": { "position": [19.0, 3.3, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "BgHill_1", "primitive": "box", "color": [0.16, 0.22, 0.34],
      "transform": { "position": [-6.0, 1.2, 7.0], "rotation": [0.0, 0.0, 0.0], "scale": [14.0, 6.0, 1.0] } },
    { "name": "BgHill_2", "primitive": "box", "color": [0.12, 0.17, 0.28],
      "transform": { "position": [8.0, 1.6, 9.5], "rotation": [0.0, 0.0, 0.0], "scale": [18.0, 8.0, 1.0] } },
    { "name": "BgHill_3", "primitive": "box", "color": [0.16, 0.22, 0.34],
      "transform": { "position": [20.0, 1.0, 7.0], "rotation": [0.0, 0.0, 0.0], "scale": [12.0, 5.0, 1.0] } },
    { "name": "GameManager",
      "luaScript": { "scriptPath": "components/GameManager2D.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "CoinIcon", "parent": 26,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 0.0], "pivot": [0.0, 0.0],
                  "offsetMin": [40.0, 34.0], "offsetMax": [78.0, 72.0], "order": 5 },
      "uiImage": { "texturePath": "", "color": [1.0, 0.84, 0.2, 1.0], "shape": 1, "raycastBlock": false,
                   "outlineWidth": 3.0, "outlineColor": [0.8, 0.55, 0.1, 1.0] },
      "uiAnimator": { "showAnim": 0, "loopAnim": 2, "loopSpeed": 1.2, "loopAmount": 0.06 } },
    { "name": "HudCoins", "parent": 26,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [0.0, 0.0], "pivot": [0.0, 0.0],
                  "offsetMin": [92.0, 30.0], "offsetMax": [340.0, 78.0], "order": 5 },
      "uiText": { "text": "0 / 6", "fontSize": 38, "color": [1.0, 0.97, 0.85, 1.0],
                  "alignH": 0, "alignV": 1, "letterSpacing": 2.0,
                  "shadowColor": [0.0, 0.0, 0.0, 0.55], "shadowOffset": [2.0, 2.0] } },
    { "name": "HudHint", "parent": 26,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 1.0],
                  "offsetMin": [-640.0, -92.0], "offsetMax": [640.0, -40.0], "order": 5 },
      "uiText": { "text": "A/D or ←→ 移動 / SPACE ジャンプ — トゲに気をつけて 旗までいこう！",
                  "fontSize": 26, "color": [1.0, 1.0, 1.0, 0.78], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 2.0] } },
    { "name": "ClearPanel", "parent": 26,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-340.0, -130.0], "offsetMax": [340.0, 130.0], "order": 20, "visible": false },
      "uiImage": { "texturePath": "", "color": [0.07, 0.1, 0.16, 0.94], "cornerRadius": 22.0,
                   "outlineWidth": 2.0, "outlineColor": [0.4, 0.9, 0.6, 0.55],
                   "shadowColor": [0.0, 0.0, 0.0, 0.5], "shadowOffset": [0.0, 8.0], "shadowSoftness": 16.0 },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.45, "showEasing": 5 } },
    { "name": "ClearTitle", "parent": 30,
      "uiRect": { "anchorMin": [0.0, 0.1], "anchorMax": [1.0, 0.6], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "COURSE CLEAR!", "fontSize": 58, "color": [1.0, 1.0, 1.0, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 3.0, "charAnim": 3, "charAnimSpeed": 1.6,
                  "shadowColor": [0.0, 0.0, 0.0, 0.6], "shadowOffset": [2.0, 3.0] } },
    { "name": "ClearSub", "parent": 30,
      "uiRect": { "anchorMin": [0.0, 0.6], "anchorMax": [1.0, 0.92], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "まもなくリザルトへ…", "fontSize": 22, "color": [1.0, 1.0, 1.0, 0.6],
                  "alignH": 1, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.7,
    "saturationOn": true, "saturation": 1.18,
    "vignetteOn": true, "vignette": 0.2,
    "fxaaOn": true },
  "ssao": { "enabled": false },
  "shadows": true
})JSON";

constexpr const char* k2dTitleScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.3, -0.8, -0.5], "color": [1.0, 0.97, 0.9], "intensity": 1.1, "ambient": 0.6 },
      "transform": { "position": [0.0, 12.0, -6.0], "rotation": [45.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Camera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 200.0, "isActive": true },
      "transform": { "position": [0.0, 2.0, -10.0], "rotation": [5.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/TitleController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 3,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.36, 0.62, 0.94, 1.0], "raycastBlock": false,
                   "gradientDir": 2, "gradientColor2": [0.78, 0.9, 1.0, 1.0] } },
    { "name": "SunDeco", "parent": 3,
      "uiRect": { "anchorMin": [0.82, 0.16], "anchorMax": [0.82, 0.16], "pivot": [0.5, 0.5],
                  "offsetMin": [-90.0, -90.0], "offsetMax": [90.0, 90.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [1.0, 0.95, 0.6, 0.9], "shape": 1, "raycastBlock": false },
      "uiAnimator": { "showAnim": 0, "loopAnim": 2, "loopSpeed": 0.5, "loopAmount": 0.05 } },
    { "name": "Cloud_1", "parent": 3,
      "uiRect": { "anchorMin": [0.18, 0.2], "anchorMax": [0.18, 0.2], "pivot": [0.5, 0.5],
                  "offsetMin": [-130.0, -40.0], "offsetMax": [130.0, 40.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [1.0, 1.0, 1.0, 0.85], "shape": 1, "raycastBlock": false },
      "uiAnimator": { "showAnim": 0, "loopAnim": 1, "loopSpeed": 0.35, "loopAmount": 10.0 } },
    { "name": "Cloud_2", "parent": 3,
      "uiRect": { "anchorMin": [0.65, 0.34], "anchorMax": [0.65, 0.34], "pivot": [0.5, 0.5],
                  "offsetMin": [-100.0, -30.0], "offsetMax": [100.0, 30.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [1.0, 1.0, 1.0, 0.7], "shape": 1, "raycastBlock": false },
      "uiAnimator": { "showAnim": 0, "loopAnim": 1, "loopSpeed": 0.28, "loopAmount": 14.0 } },
    { "name": "Hill", "parent": 3,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [-1300.0, -180.0], "offsetMax": [1300.0, 500.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [0.35, 0.72, 0.42, 1.0], "shape": 1, "raycastBlock": false,
                   "gradientDir": 2, "gradientColor2": [0.22, 0.55, 0.3, 1.0] } },
    { "name": "Title", "parent": 3,
      "uiRect": { "anchorMin": [0.5, 0.3], "anchorMax": [0.5, 0.3], "pivot": [0.5, 0.5],
                  "offsetMin": [-700.0, -100.0], "offsetMax": [700.0, 100.0], "order": 3 },
      "uiText": { "text": "SKY HOPPER", "fontSize": 130, "color": [1.0, 1.0, 1.0, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 6.0,
                  "charAnim": 1, "charAnimAmount": 8.0, "charAnimSpeed": 1.4,
                  "outlineWidth": 6.0, "outlineColor": [0.2, 0.42, 0.75, 1.0],
                  "shadowColor": [0.1, 0.25, 0.5, 0.5], "shadowOffset": [0.0, 6.0] },
      "uiAnimator": { "showAnim": 8, "showDuration": 0.7, "showDelay": 0.1, "showEasing": 5 } },
    { "name": "Subtitle", "parent": 3,
      "uiRect": { "anchorMin": [0.5, 0.45], "anchorMax": [0.5, 0.45], "pivot": [0.5, 0.5],
                  "offsetMin": [-500.0, -22.0], "offsetMax": [500.0, 22.0], "order": 3 },
      "uiText": { "text": "DX12 ENGINE  —  2D ACTION TEMPLATE", "fontSize": 24, "color": [1.0, 1.0, 1.0, 0.85],
                  "alignH": 1, "alignV": 1, "letterSpacing": 5.0,
                  "shadowColor": [0.15, 0.3, 0.55, 0.5], "shadowOffset": [0.0, 2.0] },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.5, "showDelay": 0.4 } },
    { "name": "BtnStart", "parent": 3,
      "uiRect": { "anchorMin": [0.5, 0.66], "anchorMax": [0.5, 0.66], "pivot": [0.5, 0.5],
                  "offsetMin": [-160.0, -38.0], "offsetMax": [160.0, 38.0], "order": 4 },
      "uiImage": { "texturePath": "", "color": [1.0, 0.62, 0.2, 1.0], "cornerRadius": 38.0, "raycastBlock": true,
                   "gradientDir": 2, "gradientColor2": [0.9, 0.4, 0.1, 1.0],
                   "outlineWidth": 4.0, "outlineColor": [1.0, 1.0, 1.0, 0.9],
                   "shadowColor": [0.2, 0.3, 0.5, 0.4], "shadowOffset": [0.0, 6.0], "shadowSoftness": 10.0 },
      "uiButton": { "onClickEvent": "ev_start", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.3, 1.3, 1.3, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 8, "showDuration": 0.6, "showDelay": 0.55, "showEasing": 5,
                      "hoverScale": 1.08, "pressScale": 0.93,
                      "loopAnim": 2, "loopSpeed": 0.8, "loopAmount": 0.03 } },
    { "name": "BtnStartLabel", "parent": 11,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "はじめる", "fontSize": 34, "color": [1.0, 1.0, 1.0, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 4.0,
                  "shadowColor": [0.4, 0.2, 0.0, 0.5], "shadowOffset": [0.0, 2.0] } },
    { "name": "HintText", "parent": 3,
      "uiRect": { "anchorMin": [0.5, 1.0], "anchorMax": [0.5, 1.0], "pivot": [0.5, 1.0],
                  "offsetMin": [-600.0, -100.0], "offsetMax": [600.0, -52.0], "order": 3 },
      "uiText": { "text": "A/D 移動 / SPACE ジャンプ", "fontSize": 24,
                  "color": [1.0, 1.0, 1.0, 0.8], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.15, 0.3, 0.55, 0.4], "shadowOffset": [0.0, 2.0] },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.5, "showDelay": 0.75 } },
    { "name": "Version", "parent": 3,
      "uiRect": { "anchorMin": [1.0, 1.0], "anchorMax": [1.0, 1.0], "pivot": [1.0, 1.0],
                  "offsetMin": [-300.0, -64.0], "offsetMax": [-40.0, -28.0], "order": 3 },
      "uiText": { "text": "prototype v0.1", "fontSize": 20, "color": [1.0, 1.0, 1.0, 0.5],
                  "alignH": 2, "alignV": 1 } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.4, "bloomThreshold": 0.8,
    "saturationOn": true, "saturation": 1.15,
    "fxaaOn": true },
  "shadows": false
})JSON";

constexpr const char* k2dClearScene = R"JSON({
  "entities": [
    { "name": "Sun",
      "directionalLight": { "direction": [-0.3, -0.8, -0.5], "color": [1.0, 0.97, 0.9], "intensity": 1.0, "ambient": 0.55 },
      "transform": { "position": [0.0, 10.0, -6.0], "rotation": [45.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Camera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 200.0, "isActive": true },
      "transform": { "position": [0.0, 2.0, -8.0], "rotation": [8.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_1",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 14, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.6, "speed": 4.5, "speedVar": 0.5, "size": 0.22, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [1.0, 0.85, 0.3], "colorEnd": [0.4, 0.9, 1.0],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [-4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Confetti_2",
      "particleEmitter": { "kind": 7, "blend": 0, "rate": 14, "playOnStart": true, "looping": true,
        "dir": [0.0, 1.0, 0.0], "spread": 0.6, "speed": 4.5, "speedVar": 0.5, "size": 0.22, "sizeEnd": 0.0,
        "life": 1.4, "lifeVar": 0.3, "color": [0.5, 1.0, 0.6], "colorEnd": [1.0, 0.5, 0.8],
        "intensity": 4.0, "gravity": -2.0, "drag": 1.2 },
      "transform": { "position": [4.0, 0.0, 2.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Controller",
      "luaScript": { "scriptPath": "components/ClearController.lua", "enabled": true, "props": [] },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "UICanvas",
      "uiCanvas": { "refWidth": 1920.0, "refHeight": 1080.0, "scaleMode": 0, "sortOrder": 0, "visible": true },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Bg", "parent": 5,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 0 },
      "uiImage": { "texturePath": "", "color": [0.3, 0.55, 0.9, 0.92], "raycastBlock": false,
                   "gradientDir": 2, "gradientColor2": [0.12, 0.25, 0.5, 1.0] } },
    { "name": "Panel", "parent": 5,
      "uiRect": { "anchorMin": [0.5, 0.5], "anchorMax": [0.5, 0.5], "pivot": [0.5, 0.5],
                  "offsetMin": [-390.0, -200.0], "offsetMax": [390.0, 200.0], "order": 1 },
      "uiImage": { "texturePath": "", "color": [1.0, 1.0, 1.0, 0.96], "cornerRadius": 30.0,
                   "outlineWidth": 6.0, "outlineColor": [1.0, 0.62, 0.2, 0.9],
                   "shadowColor": [0.1, 0.2, 0.4, 0.5], "shadowOffset": [0.0, 10.0], "shadowSoftness": 20.0 },
      "uiAnimator": { "showAnim": 8, "showDuration": 0.6, "showEasing": 5 } },
    { "name": "ClearTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.08], "anchorMax": [1.0, 0.42], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "COURSE CLEAR!", "fontSize": 64, "color": [1.0, 1.0, 1.0, 1.0],
                  "alignH": 1, "alignV": 1, "letterSpacing": 3.0,
                  "charAnim": 3, "charAnimSpeed": 1.6,
                  "outlineWidth": 4.0, "outlineColor": [0.2, 0.42, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 2, "showDuration": 0.5, "showDelay": 0.25, "showEasing": 5 } },
    { "name": "ClearSub", "parent": 7,
      "uiRect": { "anchorMin": [0.0, 0.42], "anchorMax": [1.0, 0.6], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "ゴールまでたどりついた！おつかれさま", "fontSize": 26, "color": [0.25, 0.35, 0.5, 1.0],
                  "alignH": 1, "alignV": 1 },
      "uiAnimator": { "showAnim": 1, "showDuration": 0.4, "showDelay": 0.45 } },
    { "name": "BtnRetry", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [-330.0, -34.0], "offsetMax": [-30.0, 34.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [1.0, 0.62, 0.2, 1.0], "cornerRadius": 34.0, "raycastBlock": true,
                   "gradientDir": 2, "gradientColor2": [0.9, 0.4, 0.1, 1.0],
                   "outlineWidth": 3.0, "outlineColor": [1.0, 1.0, 1.0, 0.9] },
      "uiButton": { "onClickEvent": "ev_retry", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.3, 1.3, 1.3, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.6, "showEasing": 7,
                      "hoverScale": 1.07, "pressScale": 0.93 } },
    { "name": "BtnRetryLabel", "parent": 10,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "もういちど", "fontSize": 28, "color": [1.0, 1.0, 1.0, 1.0], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.4, 0.2, 0.0, 0.5], "shadowOffset": [0.0, 2.0] } },
    { "name": "BtnTitle", "parent": 7,
      "uiRect": { "anchorMin": [0.5, 0.76], "anchorMax": [0.5, 0.76], "pivot": [0.5, 0.5],
                  "offsetMin": [30.0, -34.0], "offsetMax": [330.0, 34.0], "order": 2 },
      "uiImage": { "texturePath": "", "color": [0.4, 0.6, 0.9, 1.0], "cornerRadius": 34.0, "raycastBlock": true,
                   "gradientDir": 2, "gradientColor2": [0.25, 0.42, 0.75, 1.0],
                   "outlineWidth": 3.0, "outlineColor": [1.0, 1.0, 1.0, 0.9] },
      "uiButton": { "onClickEvent": "ev_title", "normalColor": [1.0, 1.0, 1.0, 1.0],
                    "hoverColor": [1.3, 1.3, 1.3, 1.0], "pressedColor": [0.75, 0.75, 0.75, 1.0] },
      "uiAnimator": { "showAnim": 6, "showDuration": 0.4, "showDelay": 0.7, "showEasing": 7,
                      "hoverScale": 1.07, "pressScale": 0.93 } },
    { "name": "BtnTitleLabel", "parent": 12,
      "uiRect": { "anchorMin": [0.0, 0.0], "anchorMax": [1.0, 1.0], "pivot": [0.5, 0.5],
                  "offsetMin": [0.0, 0.0], "offsetMax": [0.0, 0.0], "order": 1 },
      "uiText": { "text": "タイトルへ", "fontSize": 28, "color": [1.0, 1.0, 1.0, 1.0], "alignH": 1, "alignV": 1,
                  "shadowColor": [0.1, 0.2, 0.4, 0.5], "shadowOffset": [0.0, 2.0] } }
  ],
  "postProcess": { "enabled": true,
    "bloomOn": true, "bloom": 0.45, "bloomThreshold": 0.75,
    "saturationOn": true, "saturation": 1.12,
    "fxaaOn": true },
  "shadows": false
})JSON";

} // namespace

const std::vector<TemplateFile>& GetFiles(const std::string& templateId)
{
    static const std::vector<TemplateFile> kEmptyFiles = {
        { "assets/scenes/main.json",                kEmptyMainScene },
        { "assets/components/Spinner.lua",          kSpinnerLua },
        { "scripts/game.lua",                       kEmptyGameLua },
    };

    static const std::vector<TemplateFile> kFpsFiles = {
        { "assets/scenes/title.json",               kFpsTitleScene },
        { "assets/scenes/main.json",                kFpsMainScene },
        { "assets/scenes/clear.json",               kFpsClearScene },
        { "assets/sceneflow.json",                  kSceneFlowJson },
        { "assets/components/FpsController.lua",    kFpsControllerLua },
        { "assets/components/Target.lua",           kFpsTargetLua },
        { "assets/components/GameManager.lua",      kFpsGameManagerLua },
        { "assets/components/TitleController.lua",  kTitleControllerLua },
        { "assets/components/ClearController.lua",  kClearControllerLua },
        { "assets/components/OrbitCamera.lua",      kOrbitCameraLua },
        { "scripts/game.lua",                       kGameLua },
    };

    static const std::vector<TemplateFile> kTpsFiles = {
        { "assets/scenes/title.json",               kTpsTitleScene },
        { "assets/scenes/main.json",                kTpsMainScene },
        { "assets/scenes/clear.json",               kTpsClearScene },
        { "assets/sceneflow.json",                  kSceneFlowJson },
        { "assets/components/TpsController.lua",    kTpsControllerLua },
        { "assets/components/Collectible.lua",      kTpsCollectibleLua },
        { "assets/components/CoinGameManager.lua",  kTpsGameManagerLua },
        { "assets/components/TitleController.lua",  kTitleControllerLua },
        { "assets/components/ClearController.lua",  kClearControllerLua },
        { "assets/components/OrbitCamera.lua",      kOrbitCameraLua },
        { "scripts/game.lua",                       kGameLua },
    };

    static const std::vector<TemplateFile> k2dFiles = {
        { "assets/scenes/title.json",                   k2dTitleScene },
        { "assets/scenes/main.json",                    k2dMainScene },
        { "assets/scenes/clear.json",                   k2dClearScene },
        { "assets/sceneflow.json",                      kSceneFlowJson },
        { "assets/components/PlatformerController.lua", kPlatformerControllerLua },
        { "assets/components/Coin2D.lua",               kCoin2DLua },
        { "assets/components/MovingPlatform.lua",       kMovingPlatformLua },
        { "assets/components/CameraFollow2D.lua",       kCameraFollow2DLua },
        { "assets/components/GameManager2D.lua",        kGameManager2DLua },
        { "assets/components/TitleController.lua",      kTitleControllerLua },
        { "assets/components/ClearController.lua",      kClearControllerLua },
        { "scripts/game.lua",                           kGameLua },
    };

    if (templateId == "fps") return kFpsFiles;
    if (templateId == "tps") return kTpsFiles;
    if (templateId == "2d")  return k2dFiles;
    return kEmptyFiles;
}

} // namespace dx12e::templates
