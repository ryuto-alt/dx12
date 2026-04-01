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

function OnStart()
    log("=== Game Starting ===")

    -- グリッド床
    scene:spawnPlane("grid_floor", Vec3.new(0, -1, 0), 50.0, true)

    -- ===== Mixamo / Human モデル =====
    -- Strut Walking（Mixamo FBX: cm単位→0.01スケール）
    scene:spawn("strut_walker", strutModel,
        Vec3.new(-3, -1, 0), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))

    -- Human（glTF）
    scene:spawn("human1", humanModel,
        Vec3.new(0, -1, 0), Vec3.new(90, 0, 0), Vec3.new(0.02, 0.02, 0.02))

    -- Climbing To Top（Mixamo FBX: cm単位→0.01スケール）
    scene:spawn("climbing", climbingModel,
        Vec3.new(3, -1, 0), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))

    -- ===== Kenney Animated Animals =====
    scene:spawn("fox", foxModel,
        Vec3.new(-6, -1, 2), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    scene:spawn("penguin", penguinModel,
        Vec3.new(-3, -1, 2), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    scene:spawn("cat", catModel,
        Vec3.new(0, -1, 2), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    scene:spawn("bunny", bunnyModel,
        Vec3.new(3, -1, 2), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))
    scene:spawn("monkey", monkeyModel,
        Vec3.new(6, -1, 2), Vec3.new(0, 0, 0), Vec3.new(0.01, 0.01, 0.01))

    -- マウスキャプチャ開始（FPSゲームなので即座に）
    input:setMouseCapture(true)

    log("Entities spawned: " .. scene:getEntityCount())
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

    -- ゲームロジックをここに追加
    -- 例: 敵AI、スコア管理、衝突判定など
end
