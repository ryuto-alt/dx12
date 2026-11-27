-- SC-1: 風見の腕を Y 軸まわりに一定の角速度で回す（Play 中だけ。エディタの撮影では固定角のまま）
function OnUpdate(self, dt)
    self.angle = (self.angle or 35) + 40 * dt
    self.entity.transform.rotation = Vec3.new(0, self.angle, 0)
end
