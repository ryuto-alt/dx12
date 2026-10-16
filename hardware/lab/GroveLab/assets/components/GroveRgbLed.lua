-- 実験3: 本物のフルカラー LED（P9813, D2/D3）の色を距離で変える。画面のランプも同じ色
--   遠い = 青 → 中くらい = 緑 → 近い = 赤。スイッチを押した瞬間は白く光る
--   brightness はボリュームでも変えられる（useKnob）
--   時限爆弾が動いている間は、ピッに合わせて赤く点滅・爆発で橙・解除で緑
--   BGM（GroveJukebox）が流れている間は、音の頭ごとに音の高さの色へ切り替えてパッと光る
--   ※このエンティティにポイントライトが付いていること
properties = {
  { name = "device",     type = "string", default = "grove", label = "デバイス名" },
  { name = "useKnob",    type = "bool",   default = false, label = "明るさをボリュームで変える" },
  { name = "maxLight",   type = "float",  default = 25, min = 1, max = 200, label = "画面のランプの明るさ" },
  { name = "musicLight", type = "bool",   default = true, label = "BGM の音に合わせて光る" },
}

-- 色相 0..1 → 鮮やかな RGB
local function hueRgb(h)
  h = (h % 1) * 6
  local i = math.floor(h)
  local f = h - i
  if i == 0 then return 1, f, 0
  elseif i == 1 then return 1 - f, 1, 0
  elseif i == 2 then return 0, 1, f
  elseif i == 3 then return 0, 1 - f, 1
  elseif i == 4 then return f, 0, 1
  end
  return 1, 0, 1 - f
end

local function hue(near)
  -- 0(遠い)=青 → 0.5=緑 → 1(近い)=赤
  if near < 0.5 then
    local t = near / 0.5
    return 0, t, 1 - t
  end
  local t = (near - 0.5) / 0.5
  return t, 1 - t, 0
end

function OnStart(self)
  self.dev = hw.device(self.device)
  self.e = scene:findEntity(self.name)
  self.light = self.e:isValid() and (self.e:light() or self.e:addLight("point")) or nil
  self.flash = 0
  self.swWas = false
  self.onsetSeen = loadNum("grove.bgmOnset", 0)
  self.mR, self.mG, self.mB = 0, 0, 0   -- いまの音の色
  self.since = 1                        -- 音の頭からの秒数
  self.musicNote = -1
end

function OnUpdate(self, dt)
  local sw = self.dev:down("sw") or keyDown("SPACE")
  if sw and not self.swWas then self.flash = 1 end
  self.swWas = sw
  self.flash = math.max(0, self.flash - dt * 3)

  local r, g, b = hue(loadNum("grove.near", 0))
  local bright = self.useKnob and loadNum("grove.knob", 1) or 1
  r = lerp(r, 1, self.flash) * bright
  g = lerp(g, 1, self.flash) * bright
  b = lerp(b, 1, self.flash) * bright

  -- BGM: 音の頭で色を決め直す（ド=赤 → ミ=黄緑 → ソ=水色 → シ=紫。半音ごとに色相 30 度）
  local music = self.musicLight and loadNum("grove.bgmOn", 0) == 1
  local onset = loadNum("grove.bgmOnset", 0)
  self.musicNote = loadNum("grove.bgmNote", -1)
  if onset ~= self.onsetSeen and self.musicNote >= 0 then
    self.onsetSeen = onset
    self.mR, self.mG, self.mB = hueRgb((self.musicNote % 12) / 12)
    self.since = 0
  else
    self.since = self.since + dt
  end

  -- 時限爆弾（GroveSpeaker）が動いている間は爆弾の色を優先する
  local bomb = loadNum("grove.bomb", 0)
  local bf = loadNum("grove.bombFlash", 0)
  if bomb == 1 then          -- 作動中: ピッに合わせて赤く点滅
    r, g, b = 0.08 + 0.92 * bf, 0, 0
  elseif bomb == 2 then      -- 爆発: 白〜橙がちらつく
    local k = bf * (0.5 + 0.5 * math.random())
    r, g, b = k, k * 0.55, k * 0.15
  elseif bomb == 3 then      -- 解除: 緑
    r, g, b = 0, 1, 0.2
  elseif loadNum("grove.safe", 0) > 0 then   -- 金庫: 正解への近さ（GroveSafe が決める）
    r, g, b = loadNum("grove.safeR", 0), loadNum("grove.safeG", 0), loadNum("grove.safeB", 0)
  elseif loadNum("grove.daruma", 0) > 0 then -- だるま: 歌の間は緑、振り向いたら赤
    r, g, b = loadNum("grove.darumaR", 0), loadNum("grove.darumaG", 0), loadNum("grove.darumaB", 0)
  elseif music then
    -- 頭の 0.05 秒は白っぽく全開 → 0.15 秒まで全開 → 伸ばしている間は 45% → 休みは 8%。
    -- なめらかに減らすと毎フレーム値が変わってシリアルが埋まるので、4 段で切り替える
    local w, k = 0, 0.08
    if self.musicNote >= 0 then
      if self.since < 0.05 then w, k = 0.35, 1
      elseif self.since < 0.15 then k = 1
      else k = 0.45 end
    end
    r, g, b = lerp(self.mR, 1, w) * k, lerp(self.mG, 1, w) * k, lerp(self.mB, 1, w) * k
  end

  self.dev:set("r", r)
  self.dev:set("g", g)
  self.dev:set("b", b)
  scene:setColor(self.e, 0.15 + 0.85 * r, 0.15 + 0.85 * g, 0.15 + 0.85 * b)
  if self.light then
    self.light:setColor(math.max(r, 0.05), math.max(g, 0.05), math.max(b, 0.05))
    self.light.intensity = self.maxLight * math.max(r, g, b)
  end
  saveNum("grove.r", r); saveNum("grove.g", g); saveNum("grove.b", b)
end
