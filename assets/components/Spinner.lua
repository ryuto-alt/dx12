-- Spinner.lua — エンティティを回し続ける「再利用コンポーネント」のサンプル。
--
-- properties で宣言した値は:
--   1) エディタの Inspector に型に応じた編集欄が自動で出る
--   2) エンティティ個別の値としてシーン / プレハブに保存される
--   3) Play 時に self.<name> としてこのスクリプトへ注入される
-- これで「巨大な 1 個のコントローラ」ではなく「パラメータ付きの部品」を
-- 複数のエンティティに貼って数値だけ調整する Unity ライクな作り方ができる。
--
-- 使い方: AssetBrowser からこの .lua をエンティティ（Hierarchy 行 or Inspector）へドロップ。
--         Inspector の「Lua Script > プロパティ」で speed / axis などを調整。Play で回る。

properties = {
  { name = "speed",       type = "float", default = 90.0, min = -720, max = 720, label = "回転速度(度/秒)" },
  { name = "axis",        type = "vec3",  default = {0, 1, 0},                    label = "回転軸" },
  { name = "enabledSpin", type = "bool",  default = true,                         label = "回転ON" },
}

function OnUpdate(self, dt)
  if not self.enabledSpin then return end
  local t = self.transform
  if not t then return end
  local r = t.rotation
  -- 軸の各成分に比例して各軸を回す（Euler degrees）。Vec3 を丸ごと代入して確実に書き戻す。
  t.rotation = Vec3.new(
    r.x + self.speed * self.axis.x * dt,
    r.y + self.speed * self.axis.y * dt,
    r.z + self.speed * self.axis.z * dt)
end
