-- 実験3: 本物のフルカラー LED（P9813, D2/D3）の色を距離で変える。画面のランプも同じ色
--   遠い = 青 → 中くらい = 緑 → 近い = 赤。スイッチを押した瞬間は白く光る
--   brightness はボリュームでも変えられる（useKnob）
--   時限爆弾が動いている間は、ピッに合わせて赤く点滅・爆発で橙・解除で緑
--   ※このエンティティにポイントライトが付いていること
properties = {
  { name = "device",   type = "string", default = "grove", label = "デバイス名" },
  { name = "useKnob",  type = "bool",   default = false, label = "明るさをボリュームで変える" },
  { name = "maxLight", type = "float",  default = 25, min = 1, max = 200, label = "画面のランプの明るさ" },
}

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
