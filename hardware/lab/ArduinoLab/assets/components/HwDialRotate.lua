-- 実験2: ダイヤル(可変抵抗)を回すと物が回る
--   可変抵抗の中央の足を GPIO34、両端を 3V3 と GND へ。実機が無ければ ← → キーで回る
properties = {
  { name = "device",  type = "string", default = "lab", label = "デバイス名" },
  { name = "channel", type = "string", default = "pot", label = "入力チャンネル" },
  { name = "angle",   type = "float",  default = 270, min = 30, max = 720, label = "ダイヤル全体で回る角度" },
  { name = "follow",  type = "float",  default = 10, min = 1, max = 30, label = "追従の速さ" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.v = 0.5
  self.shown = 0.5
end

function OnUpdate(self, dt)
  if self.dev.connected then
    self.v = self.dev:get(self.channel)            -- 0..1（hardware.json で校正・平滑済み）
  else
    if keyDown("LEFT")  then self.v = self.v - dt * 0.5 end
    if keyDown("RIGHT") then self.v = self.v + dt * 0.5 end
    self.v = clamp(self.v, 0, 1)
  end
  self.shown = lerp(self.shown, self.v, math.min(1, dt * self.follow))
  local r = self.transform.rotation
  self.transform.rotation = Vec3.new(r.x, (self.shown - 0.5) * self.angle, r.z)
  saveNum("lab.dial", self.v)
end
