-- 指定した軸でくるくる回すだけの部品（プロパティ付き Lua コンポーネントの最小サンプル）
properties = {
  { name = "speed", type = "float", default = 45.0, min = -360.0, max = 360.0, label = "回転速度(度/秒)" },
  { name = "axis",  type = "vec3",  default = {0.0, 1.0, 0.0}, label = "回転軸" },
}

function OnUpdate(self, dt)
  local t = self.transform
  t.rotation = Vec3.new(t.rotation.x + self.axis.x * self.speed * dt,
                        t.rotation.y + self.axis.y * self.speed * dt,
                        t.rotation.z + self.axis.z * self.speed * dt)
end
