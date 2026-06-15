-- レベル1: WASD でプレイヤー(箱)を動かし、ゴール(球)に到達でクリア。
-- 見下ろしカメラがプレイヤーを追従する。ESC でタイトルへ。

local px, pz = 0.0, 0.0
local speed = 9.0
local cleared = false

function OnStart(self)
    px, pz = 0.0, 0.0
    cleared = false
    log("Level1 started")
end

function OnUpdate(self, dt)
    if cleared then return end

    -- 入力で XZ 移動
    local mv = speed * dt
    if input:isKeyDown(KEY_W) then pz = pz + mv end
    if input:isKeyDown(KEY_S) then pz = pz - mv end
    if input:isKeyDown(KEY_A) then px = px - mv end
    if input:isKeyDown(KEY_D) then px = px + mv end

    -- プレイヤー反映
    local player = scene:findEntity("Player")
    if player and player:isValid() then
        player.transform.position = Vec3.new(px, 0.5, pz)
    end

    -- 見下ろしカメラ追従（CameraComponent の Transform を更新 → 自動でビューに反映）
    local cam = scene:findEntity("GameCamera")
    if cam and cam:isValid() then
        cam.transform.position = Vec3.new(px, 13.0, pz - 8.0)
        cam.transform.rotation = Vec3.new(55, 0, 0)
    end

    -- HUD
    ui:text(24, 24, "WASD: move    reach the golden sphere!", 26, 1, 1, 1, 1)
    ui:text(24, 56, "ESC: back to title", 20, 0.8, 0.8, 0.85, 1)

    -- クリア判定（ゴールとの XZ 距離）
    local goal = scene:findEntity("Goal")
    if goal and goal:isValid() then
        local gp = goal.transform.position
        local dx = px - gp.x
        local dz = pz - gp.z
        if (dx * dx + dz * dz) < 2.0 then
            cleared = true
            fadeToScene("scenes/clear.json", 0.7)
        end
    end

    if input:isKeyPressed(KEY_ESCAPE) then
        fadeToScene("scenes/title.json", 0.5)
    end
end
