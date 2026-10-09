-- 画面左上に、つながっているか・いまの値を出す（実験中の「ちゃんと届いているか」の確認用）
properties = {
  { name = "device", type = "string", default = "lab", label = "デバイス名" },
}

local MODES = { [0] = "ボタン", [1] = "呼吸", [2] = "ダイヤル", [3] = "タッチ" }

local function bar(x, y, label, v, r, g, b)
  ui:text(x, y, label, 20, 0.85, 0.88, 0.95, 1)
  ui:rect(x + 150, y + 4, 240, 16, 0.15, 0.17, 0.22, 0.9, 4)
  ui:rect(x + 150, y + 4, 240 * clamp(v, 0, 1), 16, r, g, b, 1, 4)
  ui:text(x + 400, y, string.format("%.2f", v), 20, 0.85, 0.88, 0.95, 1)
end

function OnStart(self)
  self.dev = hw.device(self.device)
end

function OnUpdate(self, dt)
  local d = self.dev
  ui:rect(16, 16, 500, 270, 0.05, 0.06, 0.09, 0.78, 10)
  if d.connected then
    ui:text(32, 26, "● " .. self.device .. " に接続中", 24, 0.4, 1.0, 0.55, 1)
  else
    ui:text(32, 26, "○ " .. self.device .. " 未接続（キーボードで代用中）", 24, 1.0, 0.65, 0.3, 1)
  end
  bar(32, 66,  "ボタン",  d:raw("btn") ~= 0 and 1 or loadNum("lab.btnGlow", 0), 0.35, 0.6, 1.0)
  bar(32, 96,  "ダイヤル", loadNum("lab.dial", 0), 0.95, 0.8, 0.3)
  bar(32, 126, "タッチ",  loadNum("lab.touch", 0), 1.0, 0.55, 0.25)
  bar(32, 156, "LED 出力", loadNum("lab.led", 0), 0.4, 0.75, 1.0)
  ui:text(32, 190, string.format("生の値  pot=%d  touch=%d   ジャンプ %d 回",
    math.floor(d:raw("pot")), math.floor(d:raw("touch")), math.floor(loadNum("lab.jumps", 0))), 18, 0.6, 0.65, 0.75, 1)
  ui:text(32, 218, "LED の光らせ方: " .. (MODES[math.floor(loadNum("lab.ledMode", 0))] or "?") .. "（1〜4 キーで切替）", 18, 0.6, 0.65, 0.75, 1)
  ui:text(32, 244, "代用キー: Space=ボタン  ←→=ダイヤル  T=タッチ", 18, 0.5, 0.55, 0.65, 1)
end
