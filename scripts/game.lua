-- game.lua - メインゲームスクリプト
-- エンジンとゲームロジックの分離: このファイルを差し替えれば別のゲームになる
--
-- Editorモード: エンジン側C++カメラ（WASD+マウス）
-- Playモード:   このスクリプトのOnUpdateが毎フレーム呼ばれる

local humanModel    = ASSETS .. "models/human/walk.gltf"
local strutModel    = ASSETS .. "models/fbxmodel/Strut Walking.fbx"
local climbingModel = ASSETS .. "models/fbxmodel/Climbing To Top.fbx"

-- Kenney Animated Animals（アニメーション付きローポリ動物）
local kenneyDir     = ASSETS .. "models/kennenyfbxmodel/"
local foxModel      = kenneyDir .. "animal-fox.fbx"
local penguinModel  = kenneyDir .. "animal-penguin.fbx"
local catModel      = kenneyDir .. "animal-cat.fbx"
local bunnyModel    = kenneyDir .. "animal-bunny.fbx"
local monkeyModel   = kenneyDir .. "animal-monkey.fbx"

-- 既に存在するEntityはspawnしない（エディタで変更した位置を保持するため）
local function ensureEntity(name, spawnFn)
    local e = scene:findEntity(name)
    if e:isValid() then return e end
    return spawnFn()
end

function OnStart()
    log("=== Physics Test Starting ===")

    -- グリッド床（見た目用）
    ensureEntity("grid_floor", function()
        return scene:spawnPlane("grid_floor", Vec3.new(0, -1, 0), 50.0, true)
    end)

    -- ===== 物理テスト =====

    -- 1) 地面: 薄い Static ボックス（見えないけど当たる）
    local ground = ensureEntity("ground", function()
        return scene:spawnBox("ground", Vec3.new(0, -1.5, 0),
            Vec3.new(0, 0, 0), Vec3.new(50, 1, 50))
    end)
    physics:addBoxCollider(ground, 25, 0.5, 25)
    physics:addRigidBody(ground, MOTION_STATIC, 0)

    -- 2) 落下するボックス（高さ5から落ちる → 地面で止まれば成功）
    local box1 = ensureEntity("falling_box_1", function()
        return scene:spawnBox("falling_box_1", Vec3.new(0, 5, 0),
            Vec3.new(0, 0, 0), Vec3.new(1, 1, 1))
    end)
    physics:addBoxCollider(box1, 0.5, 0.5, 0.5)
    physics:addRigidBody(box1, MOTION_DYNAMIC, 1.0)

    -- 3) もう1個、少しずらして落とす（箱同士の衝突テスト）
    local box2 = ensureEntity("falling_box_2", function()
        return scene:spawnBox("falling_box_2", Vec3.new(0.3, 8, 0.1),
            Vec3.new(0, 0, 0), Vec3.new(1, 1, 1))
    end)
    physics:addBoxCollider(box2, 0.5, 0.5, 0.5)
    physics:addRigidBody(box2, MOTION_DYNAMIC, 1.0)

    -- 4) 球体の落下テスト
    local sphere = ensureEntity("falling_sphere", function()
        return scene:spawnSphere("falling_sphere", Vec3.new(3, 6, 0), 0.5)
    end)
    physics:addSphereCollider(sphere, 0.5)
    physics:addRigidBody(sphere, MOTION_DYNAMIC, 1.0)

    -- 5) 既存モデルも配置（見た目の参考用）
    ensureEntity("human1", function()
        return scene:spawn("human1", humanModel,
            Vec3.new(-3, -1, 0), Vec3.new(90, 0, 0), Vec3.new(0.02, 0.02, 0.02))
    end)

    log("Physics test entities spawned: " .. scene:getEntityCount())
    log("Press F1 to shoot a box, F2 to raycast down from camera")
end

local shootCooldown = 0

-- カメラの前方ベクトルをYaw/Pitchから計算
local function getCameraForward()
    local yaw   = math.rad(camera:getYaw())
    local pitch = math.rad(camera:getPitch())
    local x = math.cos(pitch) * math.sin(yaw)
    local y = math.sin(pitch)
    local z = math.cos(pitch) * math.cos(yaw)
    return Vec3.new(x, y, z)
end

function OnUpdate(dt)
    -- TABでマウスキャプチャ解除/復帰
    if input:isKeyPressed(KEY_TAB) then
        input:setMouseCapture(not input:isMouseCaptured())
    end

    -- FPSカメラ操作
    if input:isMouseCaptured() then
        local sens = camera:getMouseSensitivity()
        camera:rotate(
            input:getMouseDeltaX() * sens,
            -input:getMouseDeltaY() * sens
        )

        local speed = camera:getMoveSpeed() * dt
        if input:isAsyncKeyDown(KEY_W) then camera:moveForward(speed) end
        if input:isAsyncKeyDown(KEY_S) then camera:moveForward(-speed) end
        if input:isAsyncKeyDown(KEY_D) then camera:moveRight(speed) end
        if input:isAsyncKeyDown(KEY_A) then camera:moveRight(-speed) end
        if input:isAsyncKeyDown(KEY_SPACE) then camera:moveUp(speed) end
        if input:isAsyncKeyDown(KEY_SHIFT) then camera:moveUp(-speed) end
    end

    -- ゲームロジック

    -- F1: カメラの前に箱を射出（インパルスで飛ばす）
    shootCooldown = shootCooldown - dt
    if input:isKeyPressed(KEY_F1) and shootCooldown <= 0 then
        shootCooldown = 0.3  -- 連射制限
        local pos = camera:getPosition()
        local box = scene:spawnBox("shot_box", pos,
            Vec3.new(0, 0, 0), Vec3.new(0.4, 0.4, 0.4))
        physics:addBoxCollider(box, 0.2, 0.2, 0.2)
        physics:addRigidBody(box, MOTION_DYNAMIC, 0.5)

        -- カメラの向いてる方向に飛ばす
        local fwd = getCameraForward()
        physics:applyImpulse(box, Vec3.new(fwd.x * 10, fwd.y * 10, fwd.z * 10))
        log("Shot a box!")
    end

    -- F2: カメラから下にレイキャスト
    if input:isKeyPressed(KEY_F2) then
        local pos = camera:getPosition()
        local hit = physics:raycast(pos, Vec3.new(0, -1, 0), 100)
        if hit.hit then
            log("Raycast hit! distance=" .. string.format("%.2f", hit.distance))
        else
            log("Raycast missed")
        end
    end
end
