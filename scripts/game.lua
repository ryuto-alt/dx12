-- game.lua - メインゲームスクリプト
-- エンジンとゲームロジックの分離: このファイルを差し替えれば別のゲームになる
--
-- Editorモード: エンジン側C++カメラ（WASD+マウス）
-- Playモード:   このスクリプトのOnUpdateが毎フレーム呼ばれる

local humanModel = ASSETS .. "models/human/walk.gltf"
local strutModel = ASSETS .. "models/fbxmodel/Strut Walking.fbx"

function OnStart()
    log("=== Game Starting ===")

    -- グリッド床
    scene:spawnPlane("grid_floor", Vec3.new(0, -1, 0), 50.0, true)

    -- 人間モデル 3体
    scene:spawn("human1", humanModel,
        Vec3.new(0, -1, 0), Vec3.new(90, 0, 0), Vec3.new(0.02, 0.02, 0.02))
    scene:spawn("human2", humanModel,
        Vec3.new(3, -1, 0), Vec3.new(90, 0, 0), Vec3.new(0.02, 0.02, 0.02))
    scene:spawn("human3", humanModel,
        Vec3.new(-3, -1, 0), Vec3.new(90, 180, 0), Vec3.new(0.02, 0.02, 0.02))

    -- FBX アニメーションモデル（Mixamo: cm単位→0.01スケール、Z-up→回転-90）
    scene:spawn("strut_walker", strutModel,
        Vec3.new(6, -1, 0), Vec3.new(-90, 0, 0), Vec3.new(0.01, 0.01, 0.01))

    -- プリミティブ
    scene:spawnBox("box1",
        Vec3.new(9, -0.5, 0), Vec3.new(0, 0, 0), Vec3.new(1, 1, 1))
    scene:spawnSphere("sphere1", Vec3.new(-6, -0.5, 0), 0.5)

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
