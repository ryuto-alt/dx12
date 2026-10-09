-- 実験2: 超音波距離センサに手を近づけると、画面の球がセンサへ寄ってくる
--   センサ = D6。実機が無ければ ↑ ↓ キーで距離が変わる
--   ほかの部品（LED・スピーカー）は loadNum("grove.near") でこの近さ（0=遠い..1=近い）を使う
properties = {
  { name = "device",  type = "string", default = "grove", label = "デバイス名" },
  { name = "nearCm",  type = "float",  default = 5,  min = 1,  max = 30,  label = "いちばん近い (cm)" },
  { name = "farCm",   type = "float",  default = 50, min = 10, max = 300, label = "いちばん遠い (cm)" },
  { name = "travel",  type = "float",  default = 3,  min = 0.5, max = 8,  label = "球が動く幅 (m)" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.cm = self.farCm
  self.homeZ = self.transform.position.z
end

function OnUpdate(self, dt)
  if self.dev.connected then
    local raw = self.dev:raw("distance")          -- 生の cm。0 は「測れなかった」なので前の値のまま
    if raw > 0 then self.cm = lerp(self.cm, raw, math.min(1, dt * 12)) end
  else
    if keyDown("UP")   then self.cm = self.cm + dt * 25 end
    if keyDown("DOWN") then self.cm = self.cm - dt * 25 end
  end
  self.cm = clamp(self.cm, 0, 400)

  local near = 1 - clamp((self.cm - self.nearCm) / (self.farCm - self.nearCm), 0, 1)
  local p = self.transform.position
  self.transform.position = Vec3.new(p.x, p.y, self.homeZ + near * self.travel)   -- 近いほどセンサ（奥 = +Z）へ
  saveNum("grove.cm", self.cm)
  saveNum("grove.near", near)
end
