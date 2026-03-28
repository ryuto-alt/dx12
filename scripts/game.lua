-- game.lua - メインゲームスクリプト
-- エンジンとゲームロジックの分離: このファイルを差し替えれば別のゲームになる
-- カメラ操作(WASD+マウス)はエンジン側で処理される

local humanModel = ASSETS .. "models/human/walk.gltf"

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

    -- プリミティブ
    scene:spawnBox("box1",
        Vec3.new(6, -0.5, 0), Vec3.new(0, 0, 0), Vec3.new(1, 1, 1))
    scene:spawnSphere("sphere1", Vec3.new(-6, -0.5, 0), 0.5)

    log("Entities spawned: " .. scene:getEntityCount())
end

function OnUpdate(dt)
    -- ゲームロジックをここに書く
    -- 例: Entityの移動、敵AI、スコア管理など
end
