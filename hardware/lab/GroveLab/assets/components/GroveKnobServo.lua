-- 実験1: ボリュームを回すと、画面の針と本物のサーボ (SG90) が同じ角度に動く
--   ボリューム = A0、サーボ = A2。実機が無ければ ← → キーで回る
--   mode: 0=ボリュームに合わせる / 1=自動で往復（ワイパー）
properties = {
  { name = "device", type = "string", default = "grove", label = "デバイス名(hardware.json)" },
  { name = "mode",   type = "int",    default = 0, min = 0, max = 1, label = "動かし方" },
  { name = "follow", type = "float",  default = 10, min = 1, max = 30, label = "針の追従の速さ" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.v = 0.5
  self.shown = 0.5
  self.t = 0
  self.mWas = false
end

function OnUpdate(self, dt)
  -- M キーで動かし方を切り替え
  local m = keyDown("M")
  if m and not self.mWas then self.mode = 1 - self.mode end
  self.mWas = m

  if self.mode == 1 then
    self.t = self.t + dt
    self.v = 0.5 - 0.5 * math.cos(self.t * 1.5)
  elseif self.dev.connected then
    self.v = self.dev:get("knob")                  -- 0..1（hardware.json で平滑済み）
  else
    if keyDown("LEFT")  then self.v = self.v - dt * 0.6 end
    if keyDown("RIGHT") then self.v = self.v + dt * 0.6 end
    self.v = clamp(self.v, 0, 1)
  end

  self.dev:set("servo", self.v)                    -- 本物のサーボ（0..1 = 0..180 度）
  self.shown = lerp(self.shown, self.v, math.min(1, dt * self.follow))
  local r = self.transform.rotation
  self.transform.rotation = Vec3.new(r.x, r.y, 90 - self.shown * 180)   -- 画面の針（左 0 度 → 右 180 度）
  saveNum("grove.knob", self.v)
  saveNum("grove.servoMode", self.mode)
end
