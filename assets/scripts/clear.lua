-- クリア画面: RETRY でレベル1、TITLE でタイトルへ

local t = 0.0

function OnStart(self)
    log("Clear scene started")
end

function OnUpdate(self, dt)
    t = t + dt

    local trophy = scene:findEntity("Trophy")
    if trophy and trophy:isValid() then
        trophy.transform.rotation = Vec3.new(0, t * 60.0, 0)
    end

    ui:text(500, 160, "CLEAR!", 96, 1.0, 0.9, 0.3, 1.0)

    if ui:button(540, 380, 200, 56, "RETRY") then
        fadeToScene("scenes/level1.json", 0.6)
    end
    if ui:button(540, 450, 200, 56, "TITLE") then
        fadeToScene("scenes/title.json", 0.6)
    end
end
