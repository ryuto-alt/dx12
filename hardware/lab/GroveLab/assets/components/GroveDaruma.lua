-- 実験7: だるまさんがころんだ（サーボ A2 = 鬼の首 / 超音波センサ D6 / スピーカー D4 / LED / スイッチ A6）
--   D でだるまモードの ON / OFF。スイッチで始める
--   鬼（サーボ）が向こうを向いて「だ・る・ま・さ・ん・が・こ・ろ・ん・だ」と歌う間に、センサへ手を近づける
--   「だ！」で鬼が振り向いたら手を止める。距離が動く・手が見えなくなると見つかって負け
--   winCm まで近づいて、鬼が向こうを向いている間にスイッチを押せばタッチで勝ち
--   出す音は grove.darumaHz、サーボは grove.darumaServo、LED は grove.darumaR/G/B に置く
--   実機が無ければ ↑ ↓ が距離、Space がスイッチ
properties = {
  { name = "startCm",  type = "float", default = 35,  min = 15, max = 100, label = "スタートの距離 (cm)" },
  { name = "winCm",    type = "float", default = 6,   min = 2,  max = 20,  label = "タッチできる距離 (cm)" },
  { name = "handCm",   type = "float", default = 60,  min = 20, max = 150, label = "これより遠いと「手が見えない」(cm)" },
  { name = "moveCm",   type = "float", default = 3,   min = 0.5, max = 10, label = "動いたとみなす距離 (cm)" },
  { name = "awayServo", type = "float", default = 0.05, min = 0, max = 1,  label = "向こうを向いたサーボ (0〜1)" },
  { name = "lookServo", type = "float", default = 0.95, min = 0, max = 1,  label = "振り向いたサーボ (0〜1)" },
}

local MODE_ID = 2   -- grove.mode: 1=金庫 2=だるま 3=トロンボーン（後から ON にしたほうが勝つ）

-- 歌: { 文字, Hz }。最後の「だ」で振り向く
local CHANT = {
  { "だ", 392 }, { "る", 392 }, { "ま", 440 }, { "さ", 392 }, { "ん", 330 }, { "が", 330 },
  { "こ", 392 }, { "ろ", 440 }, { "ん", 392 }, { "だ", 523 },
}

local SND = {
  start  = { { 523, 0.08 }, { 0, 0.03 }, { 659, 0.08 }, { 0, 0.03 }, { 784, 0.15 } },
  turn   = { { 196, 0.06 } },                                                          -- 振り向く「ザッ」
  caught = { { 180, 0.25 }, { 0, 0.05 }, { 150, 0.25 }, { 0, 0.05 }, { 120, 0.5 } },  -- ブーー
  win    = { { 523, 0.1 }, { 659, 0.1 }, { 784, 0.1 }, { 1047, 0.2 }, { 0, 0.05 },
             { 988, 0.1 }, { 1047, 0.35 } },
}

local IDLE, CHANTING, LOOKING, CAUGHT, WON = 0, 1, 2, 3, 4

local draw

local function play(self, name)
  self.snd = SND[name]; self.sndI = 1; self.sndT = self.snd[1][2]
end

local function tapped(self, key)
  local d = keyDown(key)
  local was = self.keys[key]
  self.keys[key] = d
  return d and not was
end

-- 1 回ぶんの歌を決める（速さと「ため」が毎回ちがう）
local function newChant(self)
  self.state = CHANTING
  self.syl = 0
  self.sylT = 0
  local base = 0.1 + math.random() * 0.22                 -- 1 文字の長さ（秒）
  self.durs = {}
  for i = 1, #CHANT do
    local d = base
    if i == 7 and math.random() < 0.4 then d = d + 0.3 + math.random() * 0.7 end   -- 「が」のあとに「ため」
    if i >= 7 and math.random() < 0.3 then d = d * 0.5 end                          -- 「ころんだ」を急に速く
    self.durs[i] = d
  end
  self.durs[#CHANT] = math.max(self.durs[#CHANT], 0.15)
end

local function caught(self, why)
  self.state = CAUGHT
  self.why = why
  self.stateT = 0
  play(self, "caught")
end

function OnStart(self)
  self.dev = hw.device("grove")
  self.on = false
  self.keys = {}
  self.state = IDLE
  self.stateT = 0
  self.servo = self.awayServo
  self.swWas = false
  self.snd = nil
  self.best = loadNum("grove.darumaBest", 0)
  self.t = 0
  saveNum("grove.daruma", 0)
end

function OnUpdate(self, dt)
  self.t = self.t + dt
  if tapped(self, "D") then
    self.on = not self.on
    self.state = IDLE
    if self.on then saveNum("grove.mode", MODE_ID) end
  end
  if self.on and loadNum("grove.mode", 0) ~= MODE_ID then self.on = false end   -- ほかのモードが後から ON になった

  local sw = self.dev:down("sw") or keyDown("SPACE")
  local press = sw and not self.swWas
  self.swWas = sw

  local cm = loadNum("grove.cm", 999)
  local hand = cm <= self.handCm          -- GroveDistance は測れなかった回（0）を前の値で埋めている
  self.stateT = self.stateT + dt
  self.tooNear = math.max(0, (self.tooNear or 0) - dt)
  local servoWant = self.awayServo
  local r, g, b = 0, 0, 0

  if self.on then
    for _ = 1, math.floor(self.t * 1000) % 7 do math.random() end   -- 乱数を時刻ぶん進める

    if self.state == IDLE then
      r, g, b = 0.2, 0.2, 0.6
      if press and cm < self.startCm - 5 then
        self.tooNear = 1.5                                  -- スタート位置より近いと始めない
        self.snd = { { 200, 0.15 } }; self.sndI = 1; self.sndT = 0.15
      elseif press then
        newChant(self)
        self.runT = 0
        play(self, "start")
        self.startDelay = 0.6
      end

    elseif self.state == CHANTING then
      r, g, b = 0, 0.8, 0.1
      self.runT = self.runT + dt
      if self.startDelay > 0 then
        self.startDelay = self.startDelay - dt
      else
        self.sylT = self.sylT - dt
        if self.sylT <= 0 then
          self.syl = self.syl + 1
          if self.syl > #CHANT then
            -- 振り向く
            self.state = LOOKING
            self.stateT = 0
            self.lookFor = 1.2 + math.random() * 1.3
            self.base = nil
            self.moveT = 0
            play(self, "turn")
          else
            self.sylT = self.durs[self.syl]
            self.snd = { { CHANT[self.syl][2], self.durs[self.syl] * 0.8 }, { 0, 0.01 } }
            self.sndI = 1; self.sndT = self.snd[1][2]
          end
        end
      end
      -- タッチ
      if press and self.state == CHANTING then
        if cm <= self.winCm then
          self.state = WON
          self.stateT = 0
          self.time = self.runT
          if self.best == 0 or self.time < self.best then self.best = self.time; saveNum("grove.darumaBest", self.best) end
          play(self, "win")
        end
      end

    elseif self.state == LOOKING then
      servoWant = self.lookServo
      r, g, b = 1, 0, 0
      self.runT = self.runT + dt
      -- 振り向いて 0.3 秒は反応の猶予。そのあとの位置を基準にする
      if self.stateT >= 0.3 then
        if not self.base then self.base = cm end
        if not hand then
          caught(self, "手が見えない！")
        elseif math.abs(cm - self.base) > self.moveCm then
          self.moveT = self.moveT + dt
          if self.moveT > 0.12 then caught(self, string.format("動いた！（%.0f cm → %.0f cm）", self.base, cm)) end
        else
          self.moveT = 0
        end
      end
      if press and self.state == LOOKING then caught(self, "見てる間にタッチしようとした！") end
      if self.state == LOOKING and self.stateT >= self.lookFor then newChant(self) end

    elseif self.state == CAUGHT then
      -- 首を横に振る「ちがうちがう」
      servoWant = self.lookServo + 0.15 * math.sin(self.stateT * 18) * math.max(0, 1 - self.stateT / 1.2)
      local k = (math.floor(self.stateT * 8) % 2 == 0) and 1 or 0
      r, g, b = k, 0, 0
      if self.stateT > 1.5 and press then self.state = IDLE end

    elseif self.state == WON then
      -- 鬼がゆっくり倒れる
      servoWant = self.awayServo + (self.lookServo - self.awayServo) * math.max(0, 1 - self.stateT / 1.5)
      local h = (self.stateT * 1.5) % 1
      r = math.abs(h * 6 - 3) - 1; g = 2 - math.abs(h * 6 - 2); b = 2 - math.abs(h * 6 - 4)
      r, g, b = clamp(r, 0, 1), clamp(g, 0, 1), clamp(b, 0, 1)
      if self.stateT > 1.5 and press then self.state = IDLE end
    end
  end

  -- サーボは振り向くときは一気に、それ以外はなめらかに
  local fast = (self.state == LOOKING or self.state == CAUGHT)
  self.servo = self.servo + (servoWant - self.servo) * math.min(1, dt * (fast and 30 or 4))

  -- 効果音
  local hz = 0
  if self.snd then
    local s = self.snd[self.sndI]
    hz = s[1]
    self.sndT = self.sndT - dt
    if self.sndT <= 0 then
      self.sndI = self.sndI + 1
      if self.sndI > #self.snd then self.snd = nil else self.sndT = self.snd[self.sndI][2] end
    end
  end

  saveNum("grove.daruma", self.on and 1 or 0)
  saveNum("grove.darumaHz", self.on and hz or 0)
  saveNum("grove.darumaServo", self.servo)
  saveNum("grove.darumaR", r); saveNum("grove.darumaG", g); saveNum("grove.darumaB", b)

  if self.on then draw(self, cm, hand) end
end

function draw(self, cm, hand)
  local x, y, w, h = 590, 110, 420, 420
  ui:rect(x, y, w, h, 0.05, 0.06, 0.09, 0.85, 12)
  ui:text(x + 16, y + 10, "だるまさんがころんだ  D=やめる", 16, 0.7, 0.72, 0.8, 1)

  -- 鬼の顔（振り向いているときは目、向こうを向いているときは後頭部）
  local cx, cy = x + w / 2, y + 120
  local looking = self.state == LOOKING or self.state == CAUGHT
  ui:rect(cx - 60, cy - 60, 120, 120, 0.85, 0.2, 0.15, 1, 60)
  if looking then
    ui:rect(cx - 34, cy - 18, 22, 22, 1, 1, 1, 1, 11)
    ui:rect(cx + 12, cy - 18, 22, 22, 1, 1, 1, 1, 11)
    ui:rect(cx - 27, cy - 11, 9, 9, 0, 0, 0, 1, 4)
    ui:rect(cx + 19, cy - 11, 9, 9, 0, 0, 0, 1, 4)
    ui:rect(cx - 20, cy + 22, 40, 8, 0.3, 0, 0, 1, 4)
  else
    ui:rect(cx - 60, cy - 60, 120, 50, 0.15, 0.1, 0.1, 1, 30)    -- 髪
  end

  -- 歌の文字
  local line = ""
  if self.state == CHANTING then
    for i = 1, math.min(self.syl, #CHANT) do line = line .. CHANT[i][1] end
  end
  if self.state == LOOKING then
    ui:text(x + 110, y + 196, "ころんだ！", 34, 1, 0.3, 0.25, 1)
  elseif self.state == CAUGHT then
    ui:text(x + 100, y + 196, "見つかった！", 34, 1, 0.3, 0.25, 1)
    ui:text(x + 20, y + 240, self.why or "", 18, 1, 0.75, 0.6, 1)
  elseif self.state == WON then
    ui:text(x + 90, y + 196, "タッチ！ 勝ち！", 34, 1, 0.85, 0.3, 1)
    ui:text(x + 20, y + 240, string.format("タイム %.1f 秒   いちばん速い記録 %.1f 秒", self.time, self.best), 18, 1, 0.9, 0.6, 1)
  elseif self.state == CHANTING then
    ui:text(x + 30, y + 196, line, 34, 0.5, 1, 0.6, 1)
  else
    local warn = (self.tooNear or 0) > 0
    ui:text(x + 30, y + 196, "手を " .. math.floor(self.startCm) .. "cm より遠くに置いて" .. (warn and "（近すぎ！）" or ""), 20,
      warn and 1 or 0.85, warn and 0.5 or 0.88, warn and 0.4 or 0.95, 1)
    ui:text(x + 30, y + 226, "スイッチでスタート", 20, 0.85, 0.88, 0.95, 1)
  end

  -- 鬼までの道のり: 左 = スタート、右 = 鬼
  local bx, by, bw = x + 24, y + 300, w - 48
  ui:rect(bx, by, bw, 16, 0.15, 0.17, 0.22, 0.9, 6)
  ui:text(bx, by + 22, "スタート", 14, 0.6, 0.65, 0.75, 1)
  ui:text(bx + bw - 28, by + 22, "鬼", 14, 1, 0.5, 0.4, 1)
  local winX = bx + bw * 0.92
  ui:rect(winX, by - 4, 2, 24, 1, 0.5, 0.4, 1, 0)
  if hand then
    local k = clamp((self.startCm - cm) / (self.startCm - self.winCm), 0, 1)
    ui:rect(bx + bw * 0.92 * k - 8, by - 4, 16, 24, 0.4, 0.8, 1, 1, 6)
    ui:text(bx, by + 44, string.format("手まで %.0f cm", cm), 16, 0.75, 0.8, 0.9, 1)
  else
    ui:text(bx, by + 44, "手が見えない（センサの前に手を入れる）", 16, 1, 0.65, 0.3, 1)
  end
  if self.state == LOOKING and self.base then
    ui:text(bx + 200, by + 44, string.format("止まれ！ %.0f ± %.0f cm", self.base, self.moveCm), 16, 1, 0.4, 0.3, 1)
  end
  if self.best > 0 and self.state ~= WON then
    ui:text(bx, by + 70, string.format("いちばん速い記録 %.1f 秒", self.best), 16, 0.7, 0.72, 0.8, 1)
  end
end
