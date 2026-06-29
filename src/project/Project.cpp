#include "project/Project.h"
#include "core/Logger.h"

#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace dx12e
{

bool Project::Save(const ProjectInfo& info, const std::string& path)
{
    nlohmann::json j;
    j["name"]             = info.name;
    j["version"]          = info.engineVersion;
    j["defaultScene"]     = info.defaultScene;
    j["lastOpenedScene"]  = info.lastOpenedScene;
    j["assetsDir"]        = "assets";
    j["scriptsDir"]       = "scripts";

    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        Logger::Error("Failed to save project: {}", path);
        return false;
    }
    ofs << j.dump(2);
    Logger::Info("Project saved: {}", path);
    return true;
}

bool Project::Load(const std::string& path, ProjectInfo& outInfo)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        Logger::Error("Failed to load project: {}", path);
        return false;
    }

    nlohmann::json j;
    ifs >> j;

    namespace fs = std::filesystem;
    fs::path projDir = fs::path(path).parent_path();

    outInfo.name             = j.value("name", j.value("title", "Untitled"));
    outInfo.engineVersion    = j.value("version", "0.1.0");
    // game.json は "startScene"、プロジェクトファイルは "defaultScene" を使う（両対応）
    outInfo.defaultScene     = j.value("startScene", j.value("defaultScene", "scenes/default.json"));
    outInfo.lastOpenedScene  = j.value("lastOpenedScene", "");
    outInfo.rootDir          = projDir.string();
    outInfo.assetsDir     = (projDir / j.value("assetsDir", "assets")).string() + "/";
    outInfo.scriptsDir    = (projDir / j.value("scriptsDir", "scripts")).string() + "/";

    Logger::Info("Project loaded: {} ({})", outInfo.name, path);
    return true;
}

// ── テンプレート別の開始シーン(JSON) ──────────────────────────────
// プリミティブ(box/plane)・ライト・カメラだけで構成。GPU 不要なのでワーカー
// スレッドからテキストとして書き出せる。SceneSerializer の読込形式に一致させる。
static const char* TemplateSceneJson(const std::string& tmpl)
{
    if (tmpl == "fps")
    {
        return R"JSON({
  "entities": [
    { "name": "MainCamera",
      "camera": { "fovDegrees": 72.0, "nearClip": 0.05, "farClip": 1000.0, "isActive": true },
      "transform": { "position": [0.0, 1.7, -14.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.35], "color": [1.0, 0.97, 0.9], "intensity": 1.1, "ambient": 0.4 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Floor", "primitive": "plane", "color": [0.13, 0.15, 0.2],
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Wall_N", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "transform": { "position": [0.0, 1.6, 19.0], "rotation": [0,0,0], "scale": [40.0, 3.2, 2.0] } },
    { "name": "Wall_S", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "transform": { "position": [0.0, 1.6, -19.0], "rotation": [0,0,0], "scale": [40.0, 3.2, 2.0] } },
    { "name": "Wall_E", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "transform": { "position": [19.0, 1.6, 0.0], "rotation": [0,0,0], "scale": [2.0, 3.2, 40.0] } },
    { "name": "Wall_W", "primitive": "box", "color": [0.3, 0.34, 0.42],
      "transform": { "position": [-19.0, 1.6, 0.0], "rotation": [0,0,0], "scale": [2.0, 3.2, 40.0] } },
    { "name": "Pillar_1", "primitive": "box", "color": [0.55, 0.3, 0.25],
      "transform": { "position": [-7.0, 1.5, 6.0], "rotation": [0,0,0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Pillar_2", "primitive": "box", "color": [0.55, 0.3, 0.25],
      "transform": { "position": [8.0, 1.5, 9.0], "rotation": [0,0,0], "scale": [2.0, 3.0, 2.0] } },
    { "name": "Target_1", "primitive": "box", "color": [0.95, 0.25, 0.2],
      "transform": { "position": [0.0, 1.0, 12.0], "rotation": [0,0,0], "scale": [1.0, 2.0, 1.0] } },
    { "name": "Crate_1", "primitive": "box", "color": [0.7, 0.6, 0.3],
      "transform": { "position": [4.0, 0.6, 3.0], "rotation": [0,20,0], "scale": [1.2, 1.2, 1.2] } }
  ],
  "postProcess": {
    "enabled": true,
    "bloomOn": true, "bloom": 0.45, "bloomThreshold": 0.7,
    "vignetteOn": true, "vignette": 0.3,
    "saturationOn": true, "saturation": 1.12
  }
})JSON";
    }
    if (tmpl == "tps")
    {
        return R"JSON({
  "entities": [
    { "name": "MainCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 1000.0, "isActive": true },
      "transform": { "position": [0.0, 5.0, -8.0], "rotation": [22.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.35], "color": [1.0, 0.97, 0.9], "intensity": 1.15, "ambient": 0.42 },
      "transform": { "position": [0.0, 14.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Floor", "primitive": "plane", "color": [0.12, 0.16, 0.14],
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Player", "primitive": "box", "color": [0.3, 0.85, 0.5],
      "transform": { "position": [0.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [0.8, 2.0, 0.8] } },
    { "name": "Wall_N", "primitive": "box", "color": [0.28, 0.4, 0.34],
      "transform": { "position": [0.0, 1.6, 19.0], "rotation": [0,0,0], "scale": [40.0, 3.2, 2.0] } },
    { "name": "Wall_S", "primitive": "box", "color": [0.28, 0.4, 0.34],
      "transform": { "position": [0.0, 1.6, -19.0], "rotation": [0,0,0], "scale": [40.0, 3.2, 2.0] } },
    { "name": "Wall_E", "primitive": "box", "color": [0.28, 0.4, 0.34],
      "transform": { "position": [19.0, 1.6, 0.0], "rotation": [0,0,0], "scale": [2.0, 3.2, 40.0] } },
    { "name": "Wall_W", "primitive": "box", "color": [0.28, 0.4, 0.34],
      "transform": { "position": [-19.0, 1.6, 0.0], "rotation": [0,0,0], "scale": [2.0, 3.2, 40.0] } },
    { "name": "Block_1", "primitive": "box", "color": [0.6, 0.45, 0.7],
      "transform": { "position": [-6.0, 1.0, 7.0], "rotation": [0,0,0], "scale": [2.0, 2.0, 2.0] } },
    { "name": "Block_2", "primitive": "box", "color": [0.6, 0.45, 0.7],
      "transform": { "position": [7.0, 0.8, 10.0], "rotation": [0,30,0], "scale": [1.6, 1.6, 1.6] } },
    { "name": "Coin_1", "primitive": "sphere", "color": [1.0, 0.85, 0.2],
      "transform": { "position": [0.0, 1.0, 11.0], "rotation": [0,0,0], "scale": [0.6, 0.6, 0.6] } }
  ],
  "postProcess": {
    "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.65,
    "vignetteOn": true, "vignette": 0.32,
    "saturationOn": true, "saturation": 1.15,
    "contrastOn": true, "contrast": 1.05
  }
})JSON";
    }
    if (tmpl == "2d")
    {
        // 横スクロール 2D（正射投影 projection=1）。カメラは -Z から +Z を素直に見る
        // ので回転ゼロ。アクションは XY 平面（X=横, Y=縦）、Z は 0 固定で進める。
        return R"JSON({
  "entities": [
    { "name": "MainCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 1000.0, "isActive": true, "projection": 1, "orthoSize": 6.0 },
      "transform": { "position": [0.0, 3.0, -12.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Sun",
      "directionalLight": { "direction": [-0.3, -0.7, -0.6], "color": [1.0, 0.97, 0.9], "intensity": 1.15, "ambient": 0.55 },
      "transform": { "position": [0.0, 12.0, -6.0], "rotation": [45.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Ground", "primitive": "box", "color": [0.18, 0.22, 0.3],
      "transform": { "position": [0.0, -0.5, 0.0], "rotation": [0,0,0], "scale": [40.0, 1.0, 1.0] } },
    { "name": "Player", "primitive": "box", "color": [0.3, 0.85, 0.5],
      "transform": { "position": [0.0, 1.0, 0.0], "rotation": [0,0,0], "scale": [0.9, 1.6, 0.9] } },
    { "name": "Platform_1", "primitive": "box", "color": [0.55, 0.45, 0.7],
      "transform": { "position": [-5.0, 2.0, 0.0], "rotation": [0,0,0], "scale": [3.0, 0.5, 1.0] } },
    { "name": "Platform_2", "primitive": "box", "color": [0.55, 0.45, 0.7],
      "transform": { "position": [4.5, 3.2, 0.0], "rotation": [0,0,0], "scale": [3.0, 0.5, 1.0] } },
    { "name": "Coin_1", "primitive": "sphere", "color": [1.0, 0.85, 0.2],
      "transform": { "position": [-5.0, 3.0, 0.0], "rotation": [0,0,0], "scale": [0.5, 0.5, 0.5] } },
    { "name": "Coin_2", "primitive": "sphere", "color": [1.0, 0.85, 0.2],
      "transform": { "position": [4.5, 4.2, 0.0], "rotation": [0,0,0], "scale": [0.5, 0.5, 0.5] } }
  ],
  "postProcess": {
    "enabled": true,
    "bloomOn": true, "bloom": 0.5, "bloomThreshold": 0.65,
    "saturationOn": true, "saturation": 1.18
  }
})JSON";
    }
    // empty（最小構成: グリッド + 床 + 立方体 + 光源 + カメラ）
    return R"JSON({
  "entities": [
    { "name": "MainCamera",
      "camera": { "fovDegrees": 60.0, "nearClip": 0.1, "farClip": 1000.0, "isActive": true },
      "transform": { "position": [0.0, 8.0, -12.0], "rotation": [28.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Sun",
      "directionalLight": { "direction": [-0.4, -1.0, -0.35], "color": [1.0, 0.97, 0.9], "intensity": 1.1, "ambient": 0.4 },
      "transform": { "position": [0.0, 12.0, 0.0], "rotation": [55.0, -30.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Grid", "gridPlane": { "size": 50.0 },
      "transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0] } },
    { "name": "Cube", "primitive": "box", "color": [0.6, 0.65, 0.75],
      "transform": { "position": [0.0, 0.5, 0.0], "rotation": [0,0,0], "scale": [1.0, 1.0, 1.0] } }
  ]
})JSON";
}

// ── テンプレート別の game.lua ──────────────────────────────────────
// グローバル OnUpdate(dt) は Play 中に毎フレーム呼ばれる（OnStart は呼ばれない仕様
// なので「started」フラグで遅延初期化する）。FPS/TPS のカメラ制御はこの中で行う。
static const char* TemplateGameLua(const std::string& tmpl)
{
    if (tmpl == "fps")
    {
        return R"LUA(-- FPS テンプレート: 一人称マウスルック + WASD 移動
-- 操作: WASD=移動 / マウス=視点 / ESC=マウス解放(エディタに戻る時に押す)
local SPEED = 8.0     -- 移動速度(m/s)
local SENS  = 0.12    -- マウス感度(度/ピクセル)
local EYE   = 1.7     -- 目線の高さ
local W, H  = 1280, 720

local yaw, pitch = 0, 0
local started = false

function OnUpdate(dt)
  local cam = scene:findEntity("MainCamera")
  if not (cam and cam:isValid()) then return end

  if not started then
    started = true
    local r = cam.transform.rotation
    yaw, pitch = r.y, -r.x
    input:setMouseCapture(true)
  end

  if keyPressed("ESC") then
    input:setMouseCapture(not input:isMouseCaptured())
  end

  -- マウスルック(キャプチャ中のみ)
  if input:isMouseCaptured() then
    yaw   = yaw   + input:getMouseDeltaX() * SENS
    pitch = pitch - input:getMouseDeltaY() * SENS
    if pitch >  89 then pitch =  89 end
    if pitch < -89 then pitch = -89 end
  end

  -- yaw 基準の前方/右ベクトル(yaw=0 で +Z を向く)
  local ry = math.rad(yaw)
  local fx, fz = math.sin(ry), math.cos(ry)
  local rx, rz = math.cos(ry), -math.sin(ry)
  local mv = SPEED * dt

  local p = cam.transform.position
  local x, z = p.x, p.z
  if keyDown("W") then x = x + fx*mv; z = z + fz*mv end
  if keyDown("S") then x = x - fx*mv; z = z - fz*mv end
  if keyDown("D") then x = x + rx*mv; z = z + rz*mv end
  if keyDown("A") then x = x - rx*mv; z = z - rz*mv end

  cam.transform.position = Vec3.new(x, EYE, z)
  cam.transform.rotation = Vec3.new(-pitch, yaw, 0)

  -- クロスヘア + 操作ヒント
  ui:text(W/2 - 8, H/2 - 16, "+", 32, 1, 1, 1, 0.9)
  ui:text(24, 24, "WASD: move   Mouse: look   ESC: release mouse", 20, 0.85, 0.9, 1.0, 1.0)
end
)LUA";
    }
    if (tmpl == "tps")
    {
        return R"LUA(-- TPS テンプレート: 三人称追従カメラ + キャラ移動
-- 操作: WASD=移動(カメラ基準) / マウス=カメラ周回 / ESC=マウス解放
local SPEED  = 7.0    -- 移動速度
local SENS   = 0.15   -- マウス感度
local DIST   = 8.0    -- カメラ距離
local HEIGHT = 1.2    -- 注視点の高さオフセット

local yaw, pitch = 0, 20
local started = false

function OnUpdate(dt)
  local player = scene:findEntity("Player")
  local cam    = scene:findEntity("MainCamera")
  if not (player and player:isValid() and cam and cam:isValid()) then return end

  if not started then started = true; input:setMouseCapture(true) end
  if keyPressed("ESC") then input:setMouseCapture(not input:isMouseCaptured()) end

  -- カメラ周回(キャプチャ中のみ)
  if input:isMouseCaptured() then
    yaw   = yaw   + input:getMouseDeltaX() * SENS
    pitch = pitch + input:getMouseDeltaY() * SENS
    if pitch <  5 then pitch =  5 end
    if pitch > 70 then pitch = 70 end
  end

  local ry = math.rad(yaw)
  local fx, fz = math.sin(ry), math.cos(ry)
  local rx, rz = math.cos(ry), -math.sin(ry)

  -- 入力方向(カメラ基準)
  local dx, dz = 0, 0
  if keyDown("W") then dx = dx + fx; dz = dz + fz end
  if keyDown("S") then dx = dx - fx; dz = dz - fz end
  if keyDown("D") then dx = dx + rx; dz = dz + rz end
  if keyDown("A") then dx = dx - rx; dz = dz - rz end

  local p = player.transform.position
  local x, y, z = p.x, p.y, p.z
  if dx ~= 0 or dz ~= 0 then
    local len = math.sqrt(dx*dx + dz*dz)
    dx, dz = dx/len, dz/len
    x = x + dx * SPEED * dt
    z = z + dz * SPEED * dt
    -- 進行方向へ向く
    player.transform.rotation = Vec3.new(0, math.deg(math.atan(dx, dz)), 0)
  end
  player.transform.position = Vec3.new(x, y, z)

  -- カメラをキャラの背後へ
  local rp = math.rad(pitch)
  local horiz = DIST * math.cos(rp)
  cam.transform.position = Vec3.new(x - fx*horiz, y + DIST*math.sin(rp) + HEIGHT, z - fz*horiz)
  cam.transform.rotation = Vec3.new(pitch, yaw, 0)

  ui:text(24, 24, "WASD: move   Mouse: orbit   ESC: release mouse", 20, 0.85, 0.9, 1.0, 1.0)
end
)LUA";
    }
    if (tmpl == "2d")
    {
        return R"LUA(-- 2D テンプレート: 横スクロール プラットフォーマー
-- 操作: A/D or ←/→=移動 / Space=ジャンプ。アクションは XY 平面、Z は固定。
local SPEED   = 7.0     -- 横移動速度(m/s)
local JUMP    = 9.0     -- ジャンプ初速
local GRAVITY = 22.0    -- 重力加速度
local GROUND  = 1.0     -- 接地時のプレイヤー中心Y

local vy = 0
local onGround = true

function OnUpdate(dt)
  local player = scene:findEntity("Player")
  if not (player and player:isValid()) then return end

  local p = player.transform.position
  local x, y = p.x, p.y

  -- 横移動(X のみ)
  local dir = 0
  if keyDown("D") or keyDown("RIGHT") then dir = dir + 1 end
  if keyDown("A") or keyDown("LEFT")  then dir = dir - 1 end
  x = x + dir * SPEED * dt

  -- ジャンプ + 重力(Y)
  if onGround and keyPressed("SPACE") then vy = JUMP; onGround = false end
  vy = vy - GRAVITY * dt
  y = y + vy * dt
  if y <= GROUND then y = GROUND; vy = 0; onGround = true end

  player.transform.position = Vec3.new(x, y, 0)

  ui:text(24, 24, "A/D or Arrows: move   Space: jump", 20, 0.85, 0.9, 1.0, 1.0)
end
)LUA";
    }
    // empty
    return R"LUA(-- 空テンプレート
-- グローバル OnUpdate(dt) は Play 中に毎フレーム呼ばれます。
-- ここにゲームロジックを書いてください。高レベルAPIの一覧は docs/SCRIPTING.md。
function OnUpdate(dt)
  -- 例: ui:text(24, 24, "Hello, DX12 Engine!", 24, 1, 1, 1, 1)
end
)LUA";
}

void Project::CreateDefaultStructure(const ProjectInfo& info)
{
    namespace fs = std::filesystem;

    fs::create_directories(info.assetsDir + "scenes");
    fs::create_directories(info.assetsDir + "models");
    fs::create_directories(info.assetsDir + "textures");
    fs::create_directories(info.assetsDir + "audio/bgm");
    fs::create_directories(info.assetsDir + "audio/sfx");
    fs::create_directories(info.scriptsDir);

    const std::string tmpl = info.templateId.empty() ? "empty" : info.templateId;

    // テンプレート別の game.lua
    std::string luaPath = info.scriptsDir + "game.lua";
    {
        std::ofstream ofs(luaPath);
        ofs << "-- " << info.name << "  (template: " << tmpl << ")\n\n";
        ofs << TemplateGameLua(tmpl);
    }

    // テンプレート別の開始シーン
    std::string sceneRel = info.defaultScene.empty() ? "scenes/main.json" : info.defaultScene;
    std::string scenePath = info.assetsDir + sceneRel;
    fs::create_directories(fs::path(scenePath).parent_path());
    {
        std::ofstream ofs(scenePath);
        ofs << TemplateSceneJson(tmpl);
    }

    Logger::Info("Created project structure ({} template): {}", tmpl, info.rootDir);
}

} // namespace dx12e
