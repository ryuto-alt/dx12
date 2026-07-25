-- ライティング演出デモ（フリッカ + フェード + 時間帯変化）。
-- ライト付きエンティティ（PointLight / SpotLight）に貼る。シーンに DirectionalLight が
-- 1 つあると時間帯変化も見える。Play 中に Space で雷が落ちる。
properties = {
  { name = "style",      type = "string", default = "candle", label = "明滅スタイル(candle/fluorescent/broken/pulse/storm/strobe)" },
  { name = "flickerHz",  type = "float",  default = 10.0, min = 1,  max = 30,  label = "明滅の速さ(Hz)" },
  { name = "intensity",  type = "float",  default = 6.0,  min = 0,  max = 30,  label = "基準の明るさ" },
  { name = "startHour",  type = "float",  default = 5.0,  min = 0,  max = 24,  label = "開始時刻" },
  { name = "endHour",    type = "float",  default = 20.0, min = 0,  max = 24,  label = "終了時刻" },
  { name = "daySeconds", type = "float",  default = 12.0, min = 1,  max = 120, label = "1日の長さ(秒)" },
}

-- startHour ⇄ endHour を延々と往復させる（onComplete で自分を呼び直すだけ）
local function cycle(self)
  self._toEnd = not self._toEnd
  Lighting.tweenTimeOfDay(self._toEnd and self.endHour or self.startHour, self.daySeconds,
                          { onComplete = function() cycle(self) end })
end

function OnStart(self)
  -- ① フリッカ: 自分のライトを lightstyle 文字列で明滅させる
  local lamp = findLight(self)
  if lamp then
    lamp.intensity = self.intensity
    Flicker(lamp, self.style, self.flickerHz)
  else
    logWarn(self.name .. ": ライトが無いので明滅はスキップ（entity:addLight(\"point\") で付けられる）")
  end

  -- ② フェード: 真っ暗から明けて始める
  Lighting.fadeFromBlack(1.2)

  -- ③ 時間帯変化: 太陽の向き/色/強度/環境光を時刻で駆動して往復させる
  Lighting.setTimeOfDay(self.startHour)
  cycle(self)
end

function OnUpdate(self)
  if keyPressed("SPACE") then Lighting.lightningFlash({ power = 8 }) end
end
