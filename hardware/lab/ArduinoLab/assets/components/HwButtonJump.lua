-- 実験1: ボタンを押すと跳ねて光る
--   ESP32 の BOOT ボタン(GPIO0)。実機が無くてもスペースキー(アクション Jump)で同じ動きになる
properties = {
  { name = "device",  type = "string", default = "lab",  label = "デバイス名(hardware.json)" },
  { name = "channel", type = "string", default = "btn",  label = "入力チャンネル" },
  { name = "jump",    type = "float",  default = 4.0, min = 0.5, max = 12, label = "跳ねる速さ" },
  { name = "gravity", type = "float",  default = 12.0, min = 1, max = 40, label = "重力" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.baseY = self.transform.position.y
  self.vy = 0
  self.glow = 0
  self.count = 0
  self.wasDown = false
end

function OnUpdate(self, dt)
  -- 押した瞬間（離れている → 押されている）を自分で見る。実機のボタンでもキーボードでも同じ
  local down = self.dev:down(self.channel) or actions.down("Jump") or keyDown("SPACE")
  local hit = down and not self.wasDown
  self.wasDown = down
  local p = self.transform.position
  if hit and p.y <= self.baseY + 0.01 then
    self.vy = self.jump
    self.glow = 1
    self.count = self.count + 1
    saveNum("lab.jumps", self.count)
  end
  self.vy = self.vy - self.gravity * dt
  local y = math.max(self.baseY, p.y + self.vy * dt)
  if y == self.baseY then self.vy = 0 end
  self.transform.position = Vec3.new(p.x, y, p.z)

  -- 押している間は明るく、離すとゆっくり消える
  if down then self.glow = 1 end
  self.glow = math.max(0, self.glow - dt * 2)
  scene:setColor(scene:findEntity(self.name), 0.2 + 0.8 * self.glow, 0.25 + 0.5 * self.glow, 1.0)
  saveNum("lab.btnGlow", self.glow)
end
