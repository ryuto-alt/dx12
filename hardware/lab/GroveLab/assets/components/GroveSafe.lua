-- 実験6: 金庫破り（ボリューム A0 = ダイヤル / スピーカー D4 / フルカラー LED / サーボ A2 = 錠 / スイッチ A6 = ハンドル）
--   K で金庫モードの ON / OFF。ON にするたびに番号が 3 つ決まる（画面には出ない）
--   ダイヤルを回すと「カチ、カチ」。正解に近づくと音が低くなり、LED が赤 → 黄 → 緑に変わる
--   正解の番号でぴたりと止めて少し待つと「カチャン」。3 つそろったらスイッチ（ハンドル）を引く
--   開くと本物のサーボが回って錠が外れ、ファンファーレ。そろう前に引くと「ガチャガチャ」（開かない）
--   出す音は grove.safeHz、サーボは grove.safeServo、LED は grove.safeR/G/B に置く（鳴らす・動かすのは各部品）
--   実機が無ければ ← → がダイヤル、Space がハンドル
properties = {
  { name = "count",   type = "int",   default = 3,   min = 1, max = 5,   label = "番号の数" },
  { name = "hold",    type = "float", default = 0.8, min = 0.2, max = 3, label = "正解で止めておく秒数" },
  { name = "nearDial", type = "int",  default = 6,   min = 1, max = 20,  label = "「近い」とみなす目盛りの数" },
  { name = "lockedServo", type = "float", default = 0,   min = 0, max = 1, label = "錠がかかっているときのサーボ (0〜1)" },
  { name = "openServo",   type = "float", default = 1,   min = 0, max = 1, label = "開いたときのサーボ (0〜1)" },
}

local DIAL = 100   -- 目盛りは 0〜99
local draw

-- 効果音: { {Hz, 秒}, ... }。0 Hz は無音
local SND = {
  click   = { { 2600, 0.012 } },
  near    = { { 1100, 0.018 } },
  gate    = { { 300, 0.05 }, { 0, 0.01 }, { 220, 0.05 } },                -- 正解の目盛り「ゴトッ」
  confirm = { { 660, 0.06 }, { 0, 0.03 }, { 990, 0.1 } },                 -- 1 つそろった「カチャン」
  ready   = { { 784, 0.08 }, { 0, 0.02 }, { 988, 0.08 }, { 0, 0.02 }, { 1175, 0.14 } },
  locked  = { { 150, 0.05 }, { 0, 0.03 }, { 130, 0.05 }, { 0, 0.03 }, { 150, 0.05 }, { 0, 0.03 }, { 120, 0.1 } },
  fanfare = { { 523, 0.12 }, { 0, 0.02 }, { 659, 0.12 }, { 0, 0.02 }, { 784, 0.12 }, { 0, 0.02 },
              { 1047, 0.3 }, { 0, 0.05 }, { 784, 0.1 }, { 1047, 0.5 } },
  enter   = { { 440, 0.05 }, { 0, 0.02 }, { 330, 0.08 } },
}

local function play(self, name)
  self.snd = SND[name]
  self.sndI = 1
  self.sndT = self.snd[1][2]
end

local function circDist(a, b)
  local d = math.abs(a - b) % DIAL
  return math.min(d, DIAL - d)
end

local function newCombo(self)
  -- 乱数を経過時間ぶん進めて、毎回ちがう番号にする（os が無いので時刻で種をまけない）
  for _ = 1, math.floor((self.t or 0) * 997) % 97 do math.random() end
  self.combo = {}
  for i = 1, self.count do
    local n
    repeat
      n = math.random(0, DIAL - 1)
      local ok = circDist(n, self.dial) >= 15                      -- いまの位置のすぐ近くは避ける
      for j = 1, i - 1 do if circDist(n, self.combo[j]) < 12 then ok = false end end
    until ok
    self.combo[i] = n
  end
  self.found = 0
  self.onT = 0
  self.open = false
  self.openT = 0
  self.flash = 0
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
  self.keys = {}
  self.dial = 0
  self.snd = nil
  self.swWas = false
  self.servo = self.lockedServo
  self.t = 0
  newCombo(self)
  saveNum("grove.safe", 0)
end

function OnUpdate(self, dt)
  self.t = self.t + dt
  if tapped(self, "K") then
    self.on = not self.on
    if self.on then newCombo(self); play(self, "enter"); saveNum("grove.mode", 1) end
  end
  if self.on and loadNum("grove.mode", 0) ~= 1 then self.on = false end   -- ほかのモードが後から ON になった

  local sw = self.dev:down("sw") or keyDown("SPACE")
  local pull = sw and not self.swWas
  self.swWas = sw

  local hz = 0
  if self.on then
    -- ダイヤル: つまみ 0〜1 → 0〜99。境目でちらつかないよう 0.6 目盛りの遊びを持たせる
    local raw = loadNum("grove.knob", 0) * (DIAL - 1)
    if math.abs(raw - self.dial) >= 0.6 then
      local nd = math.floor(raw + 0.5)
      if nd ~= self.dial and not self.open then
        self.dial = nd
        local target = self.combo[self.found + 1]
        if target then
          local d = circDist(nd, target)
          if d == 0 then play(self, "gate")
          elseif d <= self.nearDial then play(self, "near")
          else play(self, "click") end
        else
          play(self, "click")
        end
        self.onT = 0
      end
    end

    -- 正解で止めておくと 1 つそろう
    local target = self.combo[self.found + 1]
    if target and not self.open then
      if self.dial == target then
        self.onT = self.onT + dt
        if self.onT >= self.hold then
          self.found = self.found + 1
          self.onT = 0
          play(self, self.found == self.count and "ready" or "confirm")
        end
      end
    end

    -- ハンドル
    if pull and not self.open then
      if self.found >= self.count then
        self.open = true
        self.openT = 0
        play(self, "fanfare")
      else
        play(self, "locked")
        self.flash = 0.6
      end
    end
    if self.open then self.openT = self.openT + dt end
    self.flash = math.max(0, self.flash - dt)

    -- サーボ: 開いたらゆっくり回して錠を外す
    local want = self.open and self.openServo or self.lockedServo
    self.servo = self.servo + (want - self.servo) * math.min(1, dt * 3)

    -- LED: 近さで赤 → 黄 → 緑。そろったら緑、開いたら緑と白の点滅、引いて開かなければ赤の点滅
    local r, g, b = 0.6, 0, 0
    local tgt = self.combo[self.found + 1]
    if self.open then
      local k = (math.floor(self.openT * 6) % 2 == 0) and 1 or 0.3
      r, g, b = k * 0.5, k, k * 0.5
    elseif self.flash > 0 then
      local k = (math.floor(self.flash * 12) % 2 == 0) and 1 or 0
      r, g, b = k, 0, 0
    elseif not tgt then
      r, g, b = 0, 1, 0.2
    else
      local d = circDist(self.dial, tgt)
      local k = 1 - clamp(d / 25, 0, 1)                      -- 1 = ぴったり
      if k < 0.5 then r, g, b = 0.6, k * 1.2, 0 else r, g, b = 0.6 * (1 - (k - 0.5) * 2), 0.6 + 0.4 * k, 0 end
      if d == 0 then r, g, b = 0, 1, 0 end
    end
    saveNum("grove.safeR", r); saveNum("grove.safeG", g); saveNum("grove.safeB", b)
  end

  -- 効果音を順に鳴らす
  if self.snd then
    local s = self.snd[self.sndI]
    hz = s[1]
    self.sndT = self.sndT - dt
    if self.sndT <= 0 then
      self.sndI = self.sndI + 1
      if self.sndI > #self.snd then self.snd = nil
      else self.sndT = self.snd[self.sndI][2] end
    end
  end

  saveNum("grove.safe", self.on and 1 or 0)
  saveNum("grove.safeHz", hz)
  saveNum("grove.safeServo", self.servo)

  if self.on then draw(self) end
end

function draw(self)
  local cx, cy, R = 800, 300, 150
  -- 金庫の扉
  local open = self.open and clamp(self.openT / 0.6, 0, 1) or 0
  ui:rect(cx - R - 40, cy - R - 60, 2 * R + 80, 2 * R + 130, 0.16, 0.17, 0.2, 0.95, 14)
  ui:rect(cx - R - 28, cy - R - 48, 2 * R + 56, 2 * R + 106, 0.24, 0.25, 0.29, 1, 10)
  if open > 0 then
    -- 開いた中身
    ui:rect(cx - R - 28, cy - R - 48, (2 * R + 56) * open, 2 * R + 106, 0.05, 0.05, 0.06, 1, 10)
    -- 金の延べ棒（下から 3 段、きらっと光る）
    local shine = 0.85 + 0.15 * math.sin(self.openT * 6)
    for row = 0, 2 do
      local n = 4 - row
      for i = 0, n - 1 do
        local bx = cx - n * 34 + i * 68 + 4
        local by = cy + R - 10 - row * 34
        ui:rect(bx, by, 60, 28, 0.95 * shine, 0.72 * shine, 0.18, open, 4)
        ui:rect(bx + 6, by + 4, 48, 6, 1.0, 0.92, 0.55, open * 0.8, 3)
      end
    end
    ui:text(cx - 96, cy - 30, "金庫が開いた！", 32, 1.0, 0.85, 0.3, open)
    ui:text(cx - 120, cy + 20, "K で閉じる（新しい番号になる）", 18, 0.8, 0.82, 0.9, open)
    return
  end
  -- ダイヤル（角丸を半径にした四角 = 円）
  ui:rect(cx - R, cy - R, 2 * R, 2 * R, 0.1, 0.1, 0.12, 1, R)
  ui:rect(cx - R + 10, cy - R + 10, 2 * R - 20, 2 * R - 20, 0.33, 0.34, 0.38, 1, R - 10)
  -- 目盛り: いまの番号が真上に来るように回す
  for n = 0, DIAL - 1, 2 do
    local a = (n - self.dial) / DIAL * 2 * math.pi
    local big = n % 10 == 0
    local rr = R - (big and 22 or 18)
    local x, y = cx + rr * math.sin(a), cy - rr * math.cos(a)
    local s = big and 5 or 3
    ui:rect(x - s / 2, y - s / 2, s, s, 0.9, 0.9, 0.92, 1, s / 2)
    if big then
      local tr = R - 46
      ui:text(cx + tr * math.sin(a) - 10, cy - tr * math.cos(a) - 10, string.format("%d", n), 18, 0.95, 0.95, 0.97, 1)
    end
  end
  ui:text(cx - 8, cy - R - 34, "▼", 22, 1.0, 0.3, 0.25, 1)
  ui:rect(cx - 46, cy - 30, 92, 60, 0.08, 0.08, 0.1, 1, 30)
  ui:text(cx - 22, cy - 20, string.format("%02d", self.dial), 34, 1.0, 1.0, 1.0, 1)
  -- 見つけた番号
  for i = 1, self.count do
    local x = cx - (self.count * 70) / 2 + (i - 1) * 70 + 4
    local done = i <= self.found
    ui:rect(x, cy + R + 14, 62, 36, done and 0.15 or 0.1, done and 0.45 or 0.1, done and 0.2 or 0.12, 1, 6)
    ui:text(x + 14, cy + R + 18, done and string.format("%02d", self.combo[i]) or "??", 24,
      done and 0.6 or 0.5, done and 1.0 or 0.5, done and 0.6 or 0.55, 1)
  end
  local msg
  if self.found >= self.count then msg = "そろった！ スイッチ（ハンドル）を引け"
  elseif self.flash > 0 then msg = "ガチャガチャ……開かない"
  else msg = string.format("%d つ目の番号を探せ（音と LED がヒント）", self.found + 1) end
  ui:text(cx - R - 20, cy - R - 58 + 2, "金庫  K=やめる", 16, 0.7, 0.72, 0.8, 1)
  ui:text(cx - R - 20, cy + R + 58, msg, 18, 1.0, 0.85, 0.35, 1)
end
