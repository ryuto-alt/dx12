-- タイトル画面: START でゲーム開始、QUIT で終了
-- ui / loadScene / fadeToScene / quit はグローバルとして利用可能

local t = 0.0

function OnStart(self)
    log("Title scene started")
end

function OnUpdate(self, dt)
    t = t + dt

    -- タイトル中央のボックスをゆっくり回す（演出）
    local spinner = scene:findEntity("Spinner")
    if spinner and spinner:isValid() then
        spinner.transform.rotation = Vec3.new(0, t * 40.0, 0)
    end

    -- タイトル文字
    ui:text(440, 150, "MY 3D GAME", 72, 1.0, 0.95, 0.6, 1.0)
    ui:text(470, 250, "DX12 Engine Sample", 26, 0.8, 0.8, 0.85, 1.0)

    -- メニュー
    if ui:button(540, 360, 200, 56, "START") then
        fadeToScene("scenes/level1.json", 0.6)
    end
    if ui:button(540, 430, 200, 56, "QUIT") then
        quit()
    end
end
