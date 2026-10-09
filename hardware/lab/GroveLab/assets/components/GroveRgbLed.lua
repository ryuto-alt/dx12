-- 実験3: 本物のフルカラー LED（P9813, D2/D3）の色を距離で変える。画面のランプも同じ色
--   遠い = 青 → 中くらい = 緑 → 近い = 赤。スイッチを押した瞬間は白く光る
--   brightness はボリュームでも変えられる（useKnob）
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
