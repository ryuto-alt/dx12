-- 実験3: 導線に触ると明かりがつく（ESP32 のタッチセンサー。部品はジャンパー線 1 本だけ）
--   GPIO4 にジャンパー線を挿し、反対側の金属部分を指で触る。実機が無ければ T キー
--   ※このエンティティにポイントライトが付いていること
properties = {
  { name = "device",  type = "string", default = "lab",   label = "デバイス名" },
  { name = "channel", type = "string", default = "touch", label = "入力チャンネル" },
  { name = "maxIntensity", type = "float", default = 30, min = 1, max = 200, label = "最大の明るさ" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.e = scene:findEntity(self.name)
  self.light = self.e:isValid() and (self.e:light() or self.e:addLight("point")) or nil
  self.v = 0
end

function OnUpdate(self, dt)
  local target = 0
  if self.dev.connected then target = self.dev:get(self.channel) end   -- 触る=1 に近づく（invert 済み）
  if keyDown("T") then target = 1 end
  self.v = lerp(self.v, target, math.min(1, dt * 12))
  if self.light then self.light.intensity = self.v * self.maxIntensity end
  scene:setColor(self.e, 1.0, 0.6 + 0.4 * self.v, 0.2 + 0.8 * self.v)
  saveNum("lab.touch", self.v)
end
