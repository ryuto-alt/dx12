-- 実験4: ゲームから本物の LED を光らせる（ESP32 の内蔵 LED = GPIO2、青）
--   mode: 0=ボタンの光に合わせる / 1=ゆっくり呼吸 / 2=ダイヤルの値 / 3=タッチの値
--   1〜4 キーでも切り替えられる
properties = {
  { name = "device",  type = "string", default = "lab", label = "デバイス名" },
  { name = "channel", type = "string", default = "led", label = "出力チャンネル" },
  { name = "mode",    type = "int",    default = 0, min = 0, max = 3, label = "光らせ方" },
}


function OnStart(self)
  self.dev = hw.device(self.device)
  self.t = 0
  self.keyWas = {}
end

function OnUpdate(self, dt)
  for i = 1, 4 do
    local k = keyDown(tostring(i))
    if k and not self.keyWas[i] then self.mode = i - 1 end
    self.keyWas[i] = k
  end
  self.t = self.t + dt
  local v = 0
  if self.mode == 0 then v = loadNum("lab.btnGlow", 0)
  elseif self.mode == 1 then v = 0.5 - 0.5 * math.cos(self.t * 2.0)
  elseif self.mode == 2 then v = loadNum("lab.dial", 0)
  else v = loadNum("lab.touch", 0) end
  self.dev:set(self.channel, v)          -- 未接続なら何もしない（false が返るだけ）
  saveNum("lab.led", v)
  saveNum("lab.ledMode", self.mode)
end

