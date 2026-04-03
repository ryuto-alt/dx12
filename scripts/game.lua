-- game.lua - メインゲームスクリプト
-- エンジンとゲームロジックの分離: このファイルを差し替えれば別のゲームになる
--
-- Editorモード: エンジン側C++カメラ（WASD+マウス）
-- Playモード:   このスクリプトのOnUpdateが毎フレーム呼ばれる

local humanModel    = ASSETS .. "models/human/walk.gltf"
local strutModel    = ASSETS .. "models/fbxmodel/Strut Walking.fbx"
local climbingModel = ASSETS .. "models/fbxmodel/Climbing To Top.fbx"

-- Kenney Animated Animals
local kenneyDir     = ASSETS .. "models/kennenyfbxmodel/"
local foxModel      = kenneyDir .. "animal-fox.fbx"
local penguinModel  = kenneyDir .. "animal-penguin.fbx"
local catModel      = kenneyDir .. "animal-cat.fbx"
local bunnyModel    = kenneyDir .. "animal-bunny.fbx"
local monkeyModel   = kenneyDir .. "animal-monkey.fbx"
local lionModel     = kenneyDir .. "animal-lion.fbx"
local groundModel   = ASSETS .. "models/fbxmodel/ground.fbx"

-- 既に存在するEntityはspawnしない（エディタで変更した位置を保持するため）
local function ensureEntity(name, spawnFn)
    local e = scene:findEntity(name)
    if e:isValid() then return e end
    return spawnFn()
end

function OnStart()
    log("=== Game Starting ===")

    -- グリッド床（見た目用）
    ensureEntity("grid_floor", function()
        return scene:spawnPlane("grid_floor", Vec3.new(0, -1, 0), 50.0, true)
    end)

    -- 地面モデル（物理の地面も兼ねる）
    local ground = ensureEntity("ground", function()
        return scene:spawn("ground", groundModel,
            Vec3.new(0, -1, 0), Vec3.new(-90, 0, 0), Vec3.new(5, 5, 5))
    end)
    scene:setUVScale(ground, 30, 30)  -- Blender のタイリングに合わせる
    physics:autoCollider(ground)
    physics:addRigidBody(ground, MOTION_STATIC, 0)

    -- ===== キャラクターモデル配置 =====
    -- autoCollider でメッシュAABBから自動コライダー生成

    -- Strut Walking（Mixamo FBX: cm単位→0.01スケール）
    local strut = ensureEntity("strut_walker", function()
        return scene:spawn("strut_walker", strutModel,
            Vec3.new(-3, -1, 0), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(strut)
    physics:addRigidBody(strut, MOTION_DYNAMIC, 60.0)

    -- Human（glTF）
    local human = ensureEntity("human1", function()
        return scene:spawn("human1", humanModel,
            Vec3.new(0, -1, 0), Vec3.new(90, 0, 0), Vec3.new(0.02, 0.02, 0.02))
    end)
    physics:autoCollider(human)
    physics:addRigidBody(human, MOTION_DYNAMIC, 60.0)

    -- Climbing To Top
    local climbing = ensureEntity("climbing", function()
        return scene:spawn("climbing", climbingModel,
            Vec3.new(3, -1, 0), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(climbing)
    physics:addRigidBody(climbing, MOTION_DYNAMIC, 60.0)

    -- ===== 動物モデル（ローポリ）=====

    local fox = ensureEntity("fox", function()
        return scene:spawn("fox", foxModel,
            Vec3.new(-6, 0, 4), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(fox)
    physics:addRigidBody(fox, MOTION_DYNAMIC, 5.0)

    local penguin = ensureEntity("penguin", function()
        return scene:spawn("penguin", penguinModel,
            Vec3.new(-3, 0, 4), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(penguin)
    physics:addRigidBody(penguin, MOTION_DYNAMIC, 3.0)

    local cat = ensureEntity("cat", function()
        return scene:spawn("cat", catModel,
            Vec3.new(0, 0, 4), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(cat)
    physics:addRigidBody(cat, MOTION_DYNAMIC, 4.0)

    local bunny = ensureEntity("bunny", function()
        return scene:spawn("bunny", bunnyModel,
            Vec3.new(3, 0, 4), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(bunny)
    physics:addRigidBody(bunny, MOTION_DYNAMIC, 2.0)

    local lion = ensureEntity("lion", function()
        return scene:spawn("lion", lionModel,
            Vec3.new(6, 0, 4), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    end)
    physics:autoCollider(lion)
    physics:addRigidBody(lion, MOTION_DYNAMIC, 30.0)

    log("Entities spawned: " .. scene:getEntityCount())
    log("F1: shoot impulse at model | F2: raycast down")
end

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

    -- F1: 選択中のモデルにインパルスを与える（吹っ飛ばす）
    if input:isKeyPressed(KEY_F1) then
        local fwd = getCameraForward()
        -- 全Dynamic物体に前方向のインパルス
        log("Impulse applied forward!")
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
