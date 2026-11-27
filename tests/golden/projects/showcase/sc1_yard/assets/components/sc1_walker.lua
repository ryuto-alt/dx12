-- SC-1: 人形が焚き火のまわり（半径 4m）を Walk のループで歩く（Play 中だけ）
function OnStart(self)
    local e = scene:findEntity(self.name)
    e:playAnimByName('Walk', 0.0)
    e:setLooping(true)
    self.t = 0
end
function OnUpdate(self, dt)
    self.t = self.t + dt * 0.35
    local x, z = 4.0 * math.cos(self.t), 4.0 * math.sin(self.t)
    self.transform.position = Vec3.new(x, 0, z)
    self.transform.rotation = Vec3.new(0, -math.deg(self.t), 0)
end
