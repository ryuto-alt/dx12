-- 実験4: マイクロスイッチ（A6）とスピーカー（D4）
--   スイッチを押すと「ピッ」と鳴り、テルミンの ON / OFF が切り替わる
--   テルミン ON の間は、超音波センサに手を近づけるほど高い音になる
--   実機が無ければ Space がスイッチ。スピーカーの箱は音が鳴っている間ふるえる
properties = {
  { name = "device", type = "string", default = "grove", label = "デバイス名" },
  { name = "lowHz",  type = "float",  default = 220,  min = 31,  max = 2000, label = "遠いときの音 (Hz)" },
  { name = "highHz", type = "float",  default = 1320, min = 100, max = 4000, label = "近いときの音 (Hz)" },
  { name = "beepHz", type = "float",  default = 1760, min = 100, max = 4000, label = "ピッの音 (Hz)" },
}

function OnStart(self)
  self.dev = hw.device(self.device)
  self.theremin = false
  self.beep = 0
  self.swWas = false
  local s0 = self.transform.scale
  self.baseScale = Vec3.new(s0.x, s0.y, s0.z)       -- 値で持つ（参照だと毎フレーム掛け算が積もる）
  self.t = 0
end

function OnUpdate(self, dt)
  local sw = self.dev:down("sw") or keyDown("SPACE")
  if sw and not self.swWas then
    self.theremin = not self.theremin
    self.beep = 0.08                              -- ピッ（0.08 秒）
  end
  self.swWas = sw

  local hz = 0
  if self.beep > 0 then
    self.beep = self.beep - dt
    hz = self.beepHz
  elseif self.theremin then
    hz = lerp(self.lowHz, self.highHz, loadNum("grove.near", 0))
  end
  self.dev:set("tone", math.floor(hz + 0.5))      -- 0 で消音

  -- 鳴っている間は箱をふるわせる
  self.t = self.t + dt
  local s = 1
  if hz > 0 then s = 1 + 0.06 * math.sin(self.t * math.min(hz, 600) * 0.2) end
  local b = self.baseScale
  self.transform.scale = Vec3.new(b.x * s, b.y * s, b.z * s)
  saveNum("grove.tone", hz)
  saveNum("grove.theremin", self.theremin and 1 or 0)
end
