-- 実験4: 時限爆弾（マイクロスイッチ A6 / スピーカー D4 / 超音波センサ D6）
--   スイッチを押すと起動。「ピッ…ピッ…」の間隔がだんだん速くなり、最後は「ピーーー」→ ドカーン
--   超音波センサに手をかざし続けると解除できる（解除すると「ピロリン」）
--   実機が無ければ Space がスイッチ、↑ で手を近づける代わり
--   音量は Play 中に 9（小さく）/ 0（大きく）、または インスペクタの「音量」で変える
--   画面のスピーカーの箱は鳴っている間ふるえる。状態は grove.bomb に置く（LED と HUD が読む）
properties = {
  { name = "device",   type = "string", default = "grove", label = "デバイス名" },
  { name = "seconds",  type = "float",  default = 15,   min = 3,   max = 120,  label = "爆発までの秒数" },
  { name = "volume",   type = "float",  default = 0.35, min = 0,   max = 1,    label = "音量 (0〜1)" },
  { name = "tickHz",   type = "float",  default = 2000, min = 200, max = 4000, label = "ピッの音 (Hz)" },
  { name = "defuseNear", type = "float", default = 0.85, min = 0.3, max = 1, label = "解除に要る近さ (0〜1)" },
  { name = "defuseHold", type = "float", default = 1.2,  min = 0.2, max = 5, label = "かざし続ける秒数" },
}

-- 状態: 0=待機 1=作動中 2=爆発 3=解除
local IDLE, ARMED, BOOM, DEFUSED = 0, 1, 2, 3
local BOOM_SEC = 1.6
local TONE_MAX = 4000                           -- スケッチの addOutput("tone", UL_INT, 0, 4000) と合わせる
local JINGLE = { 1047, 1319, 1568, 2093 }     -- ド ミ ソ ド（解除の「ピロリン」）

function OnStart(self)
  self.dev = hw.device(self.device)
  self.state = IDLE
  self.timer = 0        -- 作動中: 残り秒 / 爆発・解除: 経過秒
  self.nextTick = 0     -- 次のピッまでの秒
  self.beep = 0         -- いま鳴っているピッの残り秒
  self.hold = 0         -- 手をかざしている秒
  self.swWas = false
  local s0 = self.transform.scale
  self.baseScale = Vec3.new(s0.x, s0.y, s0.z)  -- 値で持つ（参照だと毎フレーム掛け算が積もる）
  self.t = 0
  -- Play をやり直しても 9 / 0 で変えた音量を覚えておく。インスペクタの「音量」を変えたらそちらを使う
  self.vol = loadNum("grove.volume", self.volume)
  if loadNum("grove.volumeProp", -1) ~= self.volume then self.vol = self.volume end
  saveNum("grove.volumeProp", self.volume)
  self.volKeys = {}
end

-- 押した瞬間だけ true（keyPressed は仮想入力で来ないので自前で見る）
local function tapped(self, key)
  local d = keyDown(key)
  local was = self.volKeys[key]
  self.volKeys[key] = d
  return d and not was
end

local function arm(self)
  self.state = ARMED
  self.timer = self.seconds
  self.nextTick = 0.35                       -- 起動の「ピピッ」のあとから刻み始める
  self.beep = 0
  self.hold = 0
  self.armBeep = 0.25
end

-- 残りが減るほど間隔が詰まる（1 秒 → 0.07 秒）
local function interval(self)
  local f = clamp(self.timer / self.seconds, 0, 1)
  return 0.07 + 0.93 * f ^ 1.6
end

function OnUpdate(self, dt)
  if tapped(self, "9") then self.vol = math.max(0, self.vol - 0.05) end
  if tapped(self, "0") then self.vol = math.min(1, self.vol + 0.05) end
  self.vol = math.floor(self.vol * 20 + 0.5) / 20
  self.dev:set("vol", self.vol)                -- 0〜1 → ボードの 0〜100
  saveNum("grove.volume", self.vol)

  local sw = self.dev:down("sw") or keyDown("SPACE")
  local pressed = sw and not self.swWas
  self.swWas = sw

  local hz, flash = 0, 0
  if self.state == IDLE then
    if pressed then arm(self) end

  elseif self.state == ARMED then
    self.timer = self.timer - dt

    -- 手をかざし続けると解除
    if loadNum("grove.near", 0) >= self.defuseNear then
      self.hold = self.hold + dt
    else
      self.hold = math.max(0, self.hold - dt * 2)
    end

    if self.hold >= self.defuseHold then
      self.state, self.timer = DEFUSED, 0
    elseif self.timer <= 0 then
      self.state, self.timer = BOOM, 0
    elseif self.armBeep > 0 then
      -- 起動の「ピピッ」
      self.armBeep = self.armBeep - dt
      if self.armBeep > 0.15 or self.armBeep < 0.08 then hz = 2400 end
      if self.armBeep < 0 then hz = 0 end
      flash = hz > 0 and 1 or 0
    elseif self.timer < 0.7 then
      -- 最後は鳴りっぱなしで音が上がっていく「ピーーー」
      hz = lerp(self.tickHz * 1.2, self.tickHz * 1.8, 1 - self.timer / 0.7)
      flash = 1
    else
      self.nextTick = self.nextTick - dt
      if self.nextTick <= 0 then
        self.beep = math.min(0.06, interval(self) * 0.5)
        self.nextTick = self.nextTick + interval(self)
      end
      if self.beep > 0 then
        self.beep = self.beep - dt
        -- 残り 5 秒からは音を少し高くして焦らせる
        hz = self.tickHz * (self.timer < 5 and 1.2 or 1)
        flash = 1
      end
    end

  elseif self.state == BOOM then
    -- ドカーン: ガタガタした低い音が沈んでいく
    self.timer = self.timer + dt
    local k = self.timer / BOOM_SEC
    if k < 1 then
      local top = lerp(900, 60, k ^ 0.5)
      hz = math.random(40, math.max(41, math.floor(top)))
      flash = 1 - k
    elseif pressed then
      arm(self)
    else
      self.state = IDLE
    end

  elseif self.state == DEFUSED then
    -- 解除: ピロリン
    self.timer = self.timer + dt
    local i = math.floor(self.timer / 0.09) + 1
    if i <= #JINGLE then
      hz = JINGLE[i]
    elseif self.timer > 2.5 or pressed then
      self.state = IDLE
      if pressed then arm(self) end
    end
  end

  -- set は 0〜1 の割合で受け取り、ボードが名乗った範囲（tone は 0〜4000 Hz）へ引き伸ばす。0 で消音
  self.dev:set("tone", clamp(hz, 0, TONE_MAX) / TONE_MAX)

  -- 鳴っている間は箱をふるわせる。爆発は大きく揺らす
  self.t = self.t + dt
  local s = 1
  if self.state == BOOM and hz > 0 then
    s = 1 + 0.25 * (math.random() - 0.5)
  elseif hz > 0 then
    s = 1 + 0.06 * math.sin(self.t * math.min(hz, 600) * 0.2)
  end
  local b = self.baseScale
  self.transform.scale = Vec3.new(b.x * s, b.y * s, b.z * s)

  saveNum("grove.tone", hz)
  saveNum("grove.bomb", self.state)
  saveNum("grove.bombLeft", self.state == ARMED and math.max(0, self.timer) or 0)
  saveNum("grove.bombHold", clamp(self.hold / self.defuseHold, 0, 1))
  saveNum("grove.bombFlash", flash)
end
