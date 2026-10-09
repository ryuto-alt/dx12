-- 画面左上に、つながっているか・いまの値を出す（「ちゃんと届いているか」の確認用）
properties = {
  { name = "device", type = "string", default = "grove", label = "デバイス名" },
}

local function bar(x, y, label, v, r, g, b, text)
  ui:text(x, y, label, 20, 0.85, 0.88, 0.95, 1)
  ui:rect(x + 150, y + 4, 240, 16, 0.15, 0.17, 0.22, 0.9, 4)
  ui:rect(x + 150, y + 4, 240 * clamp(v, 0, 1), 16, r, g, b, 1, 4)
  ui:text(x + 400, y, text or string.format("%.2f", v), 20, 0.85, 0.88, 0.95, 1)
end

function OnStart(self)
  self.dev = hw.device(self.device)
end

function OnUpdate(self, dt)
  local d = self.dev
  ui:rect(16, 16, 540, 344, 0.05, 0.06, 0.09, 0.78, 10)
  if d.connected then
    ui:text(32, 26, "● " .. self.device .. " (Nano Every) に接続中", 24, 0.4, 1.0, 0.55, 1)
  else
    ui:text(32, 26, "○ " .. self.device .. " 未接続（キーボードで代用中）", 24, 1.0, 0.65, 0.3, 1)
  end
  local knob = loadNum("grove.knob", 0)
  local cm = loadNum("grove.cm", 0)
  local hz = loadNum("grove.tone", 0)
  bar(32, 66,  "ボリューム", knob, 0.95, 0.8, 0.3)
  bar(32, 96,  "サーボ",     knob, 0.6, 0.85, 0.5, string.format("%d 度", math.floor(knob * 180 + 0.5)))
  bar(32, 126, "距離",       loadNum("grove.near", 0), 0.4, 0.75, 1.0, string.format("%d cm", math.floor(cm + 0.5)))
  bar(32, 156, "スイッチ",   (d:down("sw") or keyDown("SPACE")) and 1 or 0, 0.9, 0.5, 0.9, d:down("sw") and "押している" or "-")
  bar(32, 186, "スピーカー", hz / 1800, 1.0, 0.55, 0.25, (hz > 0 and string.format("%d Hz", math.floor(hz)) or "消音") .. string.format("  音量 %d%%", math.floor(loadNum("grove.volume", 0.35) * 100 + 0.5)))
  local r, g, b = loadNum("grove.r", 0), loadNum("grove.g", 0), loadNum("grove.b", 0)
  ui:text(32, 216, "フルカラー LED", 20, 0.85, 0.88, 0.95, 1)
  ui:rect(182, 218, 60, 20, r, g, b, 1, 4)
  ui:text(252, 216, string.format("R%.2f G%.2f B%.2f", r, g, b), 18, 0.6, 0.65, 0.75, 1)
  ui:text(32, 250, string.format("生の値  knob=%d  distance=%d cm  sw=%d",
    math.floor(d:raw("knob")), math.floor(d:raw("distance")), math.floor(d:raw("sw"))), 18, 0.6, 0.65, 0.75, 1)
  local bomb = loadNum("grove.bomb", 0)
  local servo = "   サーボ: " .. (loadNum("grove.servoMode", 0) > 0 and "自動往復" or "ボリューム")
  if bomb == 1 then
    local left = loadNum("grove.bombLeft", 0)
    ui:text(32, 276, string.format("時限爆弾: 残り %4.1f 秒  センサに手をかざして解除", left), 18, 1.0, 0.35, 0.3, 1)
    ui:rect(584, 100, 200, 12, 0.15, 0.17, 0.22, 0.9, 3)
    ui:rect(584, 100, 200 * loadNum("grove.bombHold", 0), 12, 0.4, 1.0, 0.55, 1, 3)
    -- パネルの右に大きく残り時間
    ui:text(580, 24, string.format("%05.2f", left), 64, 1.0, 0.25 + 0.5 * loadNum("grove.bombFlash", 0), 0.2, 1)
  elseif bomb == 2 then
    ui:text(32, 276, "時限爆弾: ドカーン！（スイッチでもう一度）" .. servo, 18, 1.0, 0.6, 0.2, 1)
    ui:text(580, 24, "BOOM!!", 72, 1.0, 0.55, 0.15, 1)
  elseif bomb == 3 then
    ui:text(32, 276, "時限爆弾: 解除成功！" .. servo, 18, 0.4, 1.0, 0.55, 1)
  else
    ui:text(32, 276, "時限爆弾: 待機中（スイッチで起動）" .. servo, 18, 0.6, 0.65, 0.75, 1)
  end
  ui:text(32, 304, "代用キー: ←→=ボリューム  ↑↓=距離  Space=スイッチ  M=サーボ自動", 18, 0.5, 0.55, 0.65, 1)
  ui:text(32, 328, "Z/X=音量  T=トロンボーン  G=ドレミ  K=金庫  D=だるま  1〜9=BGM", 18, 0.5, 0.55, 0.65, 1)
end
