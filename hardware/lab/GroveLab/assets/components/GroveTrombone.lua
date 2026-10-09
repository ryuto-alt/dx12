-- 実験5: スライド・トロンボーン（超音波センサ D6 → スピーカー D4）
--   T で ON / OFF。ON の間は、センサの前に手を入れると鳴り、手を外すと止まる
--   手を遠ざけるほど低く、近づけるほど高い。その間はなめらかに滑る（トロンボーンのスライド）
--   G で「スライド」⇄「ドレミ（音階に吸い付く）」を切り替える。ボリュームのつまみでビブラートが深くなる
--   出す音は grove.tromboneHz に置くだけ。鳴らすのは GroveSpeaker（時限爆弾 > トロンボーン > BGM の順）
--   実機が無ければ ↑ ↓ で距離が変わる（手はいつも入っている扱い）
properties = {
  { name = "nearCm",   type = "float", default = 6,  min = 2,  max = 30,  label = "いちばん高い音の距離 (cm)" },
  { name = "farCm",    type = "float", default = 40, min = 10, max = 120, label = "いちばん低い音の距離 (cm)" },
  { name = "handCm",   type = "float", default = 50, min = 10, max = 150, label = "これより近いと「手がある」(cm)" },
  { name = "lowNote",  type = "int",   default = 55, min = 36, max = 84,  label = "いちばん低い音（MIDI 番号。55=ソ3）" },
  { name = "highNote", type = "int",   default = 79, min = 48, max = 96,  label = "いちばん高い音（MIDI 番号。79=ソ5）" },
  { name = "glide",    type = "float", default = 0.08, min = 0.01, max = 0.5, label = "滑らかさ（秒。大きいほどゆっくり）" },
  { name = "showUi",   type = "bool",  default = true, label = "画面に出す" },
}

local NAMES = { "ド", "ド#", "レ", "レ#", "ミ", "ファ", "ファ#", "ソ", "ソ#", "ラ", "ラ#", "シ" }
local MAJOR = { [0] = true, [2] = true, [4] = true, [5] = true, [7] = true, [9] = true, [11] = true }

local function noteName(n)
  local r = math.floor(n + 0.5)
  return NAMES[r % 12 + 1] .. math.floor(r / 12 - 1)
end

-- いちばん近いドレミ（ハ長調の音）へ
local function snapMajor(n)
  local best, bestD = n, 99
  for k = math.floor(n) - 2, math.floor(n) + 2 do
    if MAJOR[k % 12] and math.abs(k - n) < bestD then best, bestD = k, math.abs(k - n) end
  end
  return best
end

local function tapped(self, key)
  local d = keyDown(key)
  local was = self.keys[key]
  self.keys[key] = d
  return d and not was
end

function OnStart(self)
  self.dev = hw.device("grove")
  self.on = false
  self.scale = false
  self.keys = {}
  self.pitch = nil        -- いま鳴らしている高さ（MIDI 番号、小数あり）
  self.gone = 0           -- 手が外れてからの秒（ちらつき防止に少し待つ）
  self.t = 0
  saveNum("grove.trombone", 0)
  saveNum("grove.tromboneHz", 0)
end

function OnUpdate(self, dt)
  if tapped(self, "T") then
    self.on = not self.on; self.pitch = nil
    if self.on then saveNum("grove.mode", 3) end
  end
  if tapped(self, "G") then self.scale = not self.scale end
  if self.on and loadNum("grove.mode", 0) ~= 3 then self.on = false end   -- 金庫・だるまが後から ON になった
  self.t = self.t + dt

  local hz, target = 0, nil
  local cm = loadNum("grove.cm", 999)
  local hand = (not self.dev.connected) or (cm > 0 and cm <= self.handCm)
  if hand then self.gone = 0 else self.gone = self.gone + dt end

  if self.on and self.gone < 0.15 then
    -- 距離 → 高さ（遠い = 低い）
    local k = 1 - clamp((cm - self.nearCm) / (self.farCm - self.nearCm), 0, 1)
    target = lerp(self.lowNote, self.highNote, k)
    if self.scale then target = snapMajor(target) end
    if not self.pitch then self.pitch = target end
    -- 高さを指数的に追いかける（スライドの「ぬるっ」とした動き）
    self.pitch = self.pitch + (target - self.pitch) * math.min(1, dt / self.glide)
    -- ビブラート（ボリュームのつまみで深さ。最大 ±0.4 半音・毎秒 5.5 回）
    local depth = 0.4 * loadNum("grove.knob", 0)
    local p = self.pitch + depth * math.sin(self.t * 2 * math.pi * 5.5)
    hz = 440 * 2 ^ ((p - 69) / 12)
  else
    if self.gone >= 0.15 then self.pitch = nil end
  end

  saveNum("grove.trombone", self.on and 1 or 0)
  saveNum("grove.tromboneHz", hz)

  if self.on and self.showUi then
    local x, y, w = 16, 372, 540
    ui:rect(x, y, w, 96, 0.05, 0.06, 0.09, 0.78, 10)
    local mode = self.scale and "ドレミ" or "スライド"
    if hz > 0 then
      ui:text(x + 16, y + 10, string.format("トロンボーン（%s）  %s  %d Hz", mode, noteName(self.pitch), math.floor(hz + 0.5)),
        22, 1.0, 0.82, 0.35, 1)
    else
      ui:text(x + 16, y + 10, "トロンボーン（" .. mode .. "）  センサの前に手を入れると鳴る", 22, 0.75, 0.78, 0.85, 1)
    end
    -- スライドのバー: 左 = 低い（遠い）、右 = 高い（近い）。ドの位置に目盛り
    local bx, by, bw = x + 16, y + 52, w - 32
    ui:rect(bx, by, bw, 14, 0.15, 0.17, 0.22, 0.9, 4)
    for n = self.lowNote, self.highNote do
      if n % 12 == 0 then
        local fx = bx + bw * (n - self.lowNote) / (self.highNote - self.lowNote)
        ui:rect(fx - 1, by - 4, 2, 22, 0.6, 0.65, 0.75, 1, 0)
        ui:text(fx - 8, by + 18, "ド" .. math.floor(n / 12 - 1), 14, 0.6, 0.65, 0.75, 1)
      end
    end
    if self.pitch then
      local fx = bx + bw * clamp((self.pitch - self.lowNote) / (self.highNote - self.lowNote), 0, 1)
      ui:rect(fx - 6, by - 3, 12, 20, 1.0, 0.82, 0.35, hz > 0 and 1 or 0.4, 3)
    end
  end
end
