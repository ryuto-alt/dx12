-- カートレース（scenes/kart.json）。つまみ = ハンドル、アクセルは自動、スイッチ = アイテム
--   サーボ = スピードメーター、LED = 信号・アイテム・ダッシュ、スピーカー = エンジン音と効果音
--   CPU 3 台と 3 周。アイテムボックスでキノコ（ダッシュ）を取る。R で GroveLab に戻る
--   autoGas を OFF にすると超音波センサがアクセル（手を近づけるほど加速）
--   実機が無ければ ← → がハンドル、Space がアイテム
properties = {
  { name = "laps",       type = "int",   default = 3,    min = 1,  max = 9,   label = "周回数" },
  { name = "autoGas",    type = "bool",  default = true, label = "アクセルを自動で全開にする（OFF でセンサがアクセル）" },
  { name = "gasNearCm",  type = "float", default = 8,    min = 2,  max = 30,  label = "アクセル全開の距離 (cm)" },
  { name = "gasFarCm",   type = "float", default = 40,   min = 10, max = 100, label = "アクセル 0 の距離 (cm)" },
  { name = "steerGain",  type = "float", default = 2.2,  min = 0.5, max = 5,  label = "ハンドルの効き" },
  { name = "invertSteer", type = "bool", default = false, label = "ハンドルの左右を逆にする" },
  { name = "cpuSpeed",   type = "float", default = 1.0,  min = 0.5, max = 1.5, label = "CPU の速さ（倍）" },
  { name = "engineSound", type = "bool", default = true, label = "エンジン音を鳴らす" },
}

local VMAX, VBOOST, VGRASS = 26, 34, 11
local KARTS = { "KART_Player", "KART_Cpu1", "KART_Cpu2", "KART_Cpu3" }
local CPU_COLOR = { { 0.9, 0.15, 0.1 }, { 0.2, 0.4, 1.0 }, { 0.2, 0.85, 0.3 }, { 1.0, 0.85, 0.1 } }
local READY, COUNT, RACE, FINISH = 0, 1, 2, 3

local SND = {
  beep    = { { 440, 0.22 } },
  go      = { { 880, 0.6 } },
  roul    = { { 1500, 0.03 } },
  got     = { { 988, 0.06 }, { 0, 0.02 }, { 1319, 0.12 } },
  boost   = { { 400, 0.06 }, { 600, 0.06 }, { 800, 0.06 }, { 1000, 0.06 }, { 1200, 0.1 } },
  bump    = { { 110, 0.07 } },
  lap     = { { 784, 0.1 }, { 0, 0.03 }, { 784, 0.1 }, { 0, 0.03 }, { 1047, 0.2 } },
  final   = { { 659, 0.1 }, { 784, 0.1 }, { 988, 0.1 }, { 1319, 0.3 } },
  win     = { { 523, 0.12 }, { 659, 0.12 }, { 784, 0.12 }, { 1047, 0.25 }, { 0, 0.05 }, { 784, 0.12 }, { 1047, 0.5 } },
  lose    = { { 392, 0.2 }, { 370, 0.2 }, { 349, 0.2 }, { 330, 0.5 } },
}

-- 関数は先に名前だけ宣言（下でまとめて定義）
local readTrack, nearest, pointAt, reset, play, drawHud, tapped

---------------------------------------------------------------------------
function OnStart(self)
  self.dev = hw.device("grove")
  self.keys = { R = keyDown("R"), SPACE = keyDown("SPACE") }   -- シーンを開いたときに押しっぱなしのキーは無視
  self.t = 0
  readTrack(self)
  self.ents = {}
  for i, n in ipairs(KARTS) do self.ents[i] = scene:findEntity(n) end
  self.cam = scene:findEntity("KART_Cam")
  self.signals = { scene:findEntity("KART_Signal1"), scene:findEntity("KART_Signal2"), scene:findEntity("KART_Signal3") }
  self.items = {}
  for row = 0, 2 do
    for c = 0, 3 do
      local e = scene:findEntity("KART_Item" .. row .. c)
      if e and e:isValid() then
        local p = e.transform.position
        self.items[#self.items + 1] = { e = e, x = p.x, z = p.z, y = p.y, off = 0 }
      end
    end
  end
  self.best = loadNum("kart.best", 0)
  self.cm = 999
  reset(self)
end

-- 2m ごとの中心線を読む
function readTrack(self)
  self.pts = {}
  self.width = 12
  local base = ASSETS or ""
  if base ~= "" and base:sub(-1) ~= "/" and base:sub(-1) ~= "\\" then base = base .. "/" end
  local f = io.open(base .. "kart/track.txt", "r")
  if f then
    for line in f:lines() do
      local w = line:match("^width%s+([%d%.]+)")
      if w then self.width = tonumber(w)
      else
        local x, z = line:match("^(%-?[%d%.]+)%s+(%-?[%d%.]+)")
        if x then self.pts[#self.pts + 1] = { x = tonumber(x), z = tonumber(z) } end
      end
    end
    f:close()
  end
  local n = #self.pts
  self.L = 0
  for i = 1, n do
    local a, b = self.pts[i], self.pts[i % n + 1]
    a.s = self.L
    local dx, dz = b.x - a.x, b.z - a.z
    a.len = math.sqrt(dx * dx + dz * dz)
    a.tx, a.tz = dx / a.len, dz / a.len           -- 進む向き
    self.L = self.L + a.len
  end
  -- 曲がり具合（前後 3 点の向きの差 / 距離）→ CPU がカーブで落とす速さ
  for i = 1, n do
    local a, b = self.pts[(i - 4) % n + 1], self.pts[(i + 2) % n + 1]
    local ang = math.abs(math.atan(a.tx * b.tz - a.tz * b.tx, a.tx * b.tx + a.tz * b.tz))
    local r = 12 / math.max(ang, 1e-3)
    self.pts[i].vmax = math.min(VMAX, math.sqrt(15 * r))
  end
  -- ミニマップの範囲
  local minx, maxx, minz, maxz = 1e9, -1e9, 1e9, -1e9
  for _, p in ipairs(self.pts) do
    minx = math.min(minx, p.x); maxx = math.max(maxx, p.x); minz = math.min(minz, p.z); maxz = math.max(maxz, p.z)
  end
  self.mapB = { minx, maxx, minz, maxz }
end

-- 中心線上の位置 s（0..L）→ x, z, 進む向き
function pointAt(self, s)
  s = s % self.L
  local n = #self.pts
  local lo, hi = 1, n
  while lo < hi do
    local mid = math.floor((lo + hi + 1) / 2)
    if self.pts[mid].s <= s then lo = mid else hi = mid - 1 end
  end
  local a = self.pts[lo]
  local u = (s - a.s)
  return a.x + a.tx * u, a.z + a.tz * u, a.tx, a.tz, a
end

-- いちばん近い中心線の点（hint の前後だけ探す。hint が無ければ全部）→ 番号, s, 横のずれ（右が +）
function nearest(self, x, z, hint)
  local n = #self.pts
  local best, bi = 1e18, 1
  local from, to = 1, n
  if hint then from, to = hint - 10, hint + 10 end
  for k = from, to do
    local i = (k - 1) % n + 1
    local p = self.pts[i]
    local dx, dz = x - p.x, z - p.z
    local u = clamp(dx * p.tx + dz * p.tz, 0, p.len)
    local ex, ez = dx - p.tx * u, dz - p.tz * u
    local d = ex * ex + ez * ez
    if d < best then best, bi = d, i end
  end
  local p = self.pts[bi]
  local dx, dz = x - p.x, z - p.z
  local u = clamp(dx * p.tx + dz * p.tz, 0, p.len)
  local lat = dx * p.tz - dz * p.tx                  -- 右向きの法線 (tz, -tx) との内積
  return bi, p.s + u, lat
end

function reset(self)
  self.state = READY
  self.stateT = 0
  self.raceT = 0
  self.k = {}
  -- スタート位置: 線の後ろに 2 列。自分は後ろの右
  local grid = { { -14, 2.6 }, { -8, -2.6 }, { -8, 2.6 }, { -14, -2.6 } }
  for i = 1, 4 do
    local g = grid[i]
    local x, z, tx, tz = pointAt(self, self.L + g[1])
    local k = { x = x + tz * g[2], z = z - tx * g[2], yaw = math.atan(tx, tz), v = 0, prog = g[1], lane = g[2],
                idx = nil, done = false, finishT = 0, weave = math.random() * 6 }
    k.idx, k.s0 = nearest(self, k.x, k.z, nil)
    self.k[i] = k
  end
  self.cpuBase = { 0, 23.0, 24.0, 24.8 }
  self.item = nil; self.itemN = 0; self.roulT = 0; self.boostT = 0
  self.rocket = false
  self.lapShown = 1
  self.msg = nil; self.msgT = 0
  self.center = 0.5
  self.snd = nil
  self.camX, self.camY, self.camZ = nil, nil, nil
end

function play(self, name)
  self.snd = SND[name]; self.sndI = 1; self.sndT = self.snd[1][2]
end

function tapped(self, key)
  local d = keyDown(key)
  local was = self.keys[key]
  self.keys[key] = d
  return d and not was
end

---------------------------------------------------------------------------
function OnUpdate(self, dt)
  dt = math.min(dt, 0.05)
  self.t = self.t + dt
  self.stateT = self.stateT + dt
  if tapped(self, "R") then
    self.dev:set("tone", 0)
    loadScene("scenes/grove.json")
    return
  end

  local dev = self.dev
  -- 入力: ハンドル（つまみ）・アクセル（距離）・アイテム（スイッチ）
  local steer, gas
  local swNow = dev:down("sw") or keyDown("SPACE")
  local press = swNow and not self.keys.SPACE
  self.keys.SPACE = swNow
  if dev.connected then
    local knob = dev:get("knob")
    self.knob = knob
    steer = (knob - self.center) * 2
    if math.abs(steer) < 0.05 then steer = 0 end
    local raw = dev:raw("distance")
    if raw > 0 then self.cm = (self.cm > 300) and raw or lerp(self.cm, raw, math.min(1, dt * 14)) end
    gas = (self.cm <= 60) and clamp((self.gasFarCm - self.cm) / (self.gasFarCm - self.gasNearCm), 0, 1) or 0
  else
    steer = (keyDown("RIGHT") and 1 or 0) - (keyDown("LEFT") and 1 or 0)
    gas = (keyDown("UP") or keyDown("W")) and 1 or 0
  end
  if self.invertSteer then steer = -steer end
  steer = clamp(steer * 1.25, -1, 1)
  if self.autoGas then gas = 1 end
  self.gas, self.steer = gas, steer

  local P = self.k[1]
  local led = { 0.05, 0.05, 0.08 }

  if self.state == READY then
    if press then
      self.state = COUNT; self.stateT = 0
      if dev.connected then self.center = self.knob or 0.5 end   -- いまの位置をまっすぐにする
    end
  elseif self.state == COUNT then
    -- 信号: 赤・赤・赤 → 緑。緑の直前（2.6〜3.0 秒）にスイッチでロケットスタート。早すぎると失敗
    local n = math.floor(self.stateT)
    if n ~= self.lastBeep and n < 3 then self.lastBeep = n; play(self, "beep") end
    if press and self.stateT >= 0.3 then
      if self.stateT >= 2.6 then self.rocket = true else self.burnt = true end
    end
    led = { 1, 0, 0 }
    if self.stateT >= 3 then
      self.state = RACE; self.stateT = 0; self.lastBeep = nil
      play(self, "go")
      self.msg = "GO!"; self.msgT = 1.2
      if self.rocket and not self.burnt then self.boostT = 1.2; P.v = 18; self.msg = "ロケットスタート！"
      elseif self.burnt then self.msg = "フライング……"; self.slowT = 1.0 end
      self.burnt = false; self.rocket = false
    end
  end

  local racing = self.state == RACE or self.state == FINISH
  if racing then
    self.raceT = self.raceT + dt
    -- 自分のカート
    if P.done then
      -- ゴール後はコースに沿って流す（ウイニングラン）: 12m 先の中心線へ向ける
      local x, z = pointAt(self, P.s0 + 12)
      local d = (math.atan(x - P.x, z - P.z) - P.yaw + math.pi) % (2 * math.pi) - math.pi
      gas, steer = 0.5, clamp(d * 3, -1, 1)
    end
    local idx, s, lat = nearest(self, P.x, P.z, P.idx)
    local off = math.abs(lat) > self.width / 2 + 0.6
    local vcap = (self.boostT > 0) and VBOOST or (off and VGRASS or VMAX)
    if (self.slowT or 0) > 0 then self.slowT = self.slowT - dt; vcap = 4 end   -- フライングは少しエンスト
    local acc = gas * (self.boostT > 0 and 30 or 14) - 0.35 * P.v - (gas < 0.05 and 4 or 0)
    if P.v > vcap then acc = math.min(acc, -(P.v - vcap) * 3) end
    P.v = clamp(P.v + acc * dt, 0, VBOOST + 2)
    local turn = steer * self.steerGain * clamp(P.v / 6, 0, 1) * (1 - 0.35 * P.v / VBOOST)
    P.yaw = P.yaw + turn * dt
    P.x = P.x + math.sin(P.yaw) * P.v * dt
    P.z = P.z + math.cos(P.yaw) * P.v * dt
    -- 遠くまで飛び出したら引き戻す
    idx, s, lat = nearest(self, P.x, P.z, idx)
    local lim = self.width / 2 + 12
    if math.abs(lat) > lim then
      local p = self.pts[idx]
      local push = (math.abs(lat) - lim) * (lat > 0 and 1 or -1)
      P.x = P.x - p.tz * push; P.z = P.z + p.tx * push
      P.v = P.v * 0.9
    end
    -- 周回: s が L から 0 へ戻ったら 1 周
    local ds = s - P.s0
    if P.s0 and ds < -self.L / 2 then P.prog = P.prog + (self.L - P.s0) + s
    elseif P.s0 and ds > self.L / 2 then P.prog = P.prog - (P.s0 + self.L - s)
    elseif P.s0 then P.prog = P.prog + ds end
    P.s0, P.idx, P.lat, P.off = s, idx, lat, off
    self.boostT = math.max(0, self.boostT - dt)

    -- CPU
    for i = 2, 4 do
      local C = self.k[i]
      local _, _, _, _, a = pointAt(self, C.prog + 14)        -- 少し先のカーブで速さを決める
      local want = math.min(self.cpuBase[i] * self.cpuSpeed, a.vmax * (self.cpuBase[i] / 24))
      local diff = P.prog - C.prog                            -- 自分が前にいるほど CPU は少し速くなる
      want = want * clamp(1 + diff * 0.003, 0.88, 1.1)
      if C.done then want = 14 end
      if self.state == COUNT or self.state == READY then want = 0 end
      C.v = C.v + clamp(want - C.v, -16 * dt, 10 * dt)
      C.prog = C.prog + C.v * dt
      local lane = C.lane * 0.6 + 1.6 * math.sin(self.t * 0.35 + C.weave)
      local x, z, tx, tz = pointAt(self, C.prog)
      C.x, C.z = x + tz * lane, z - tx * lane
      C.yaw = math.atan(tx, tz)
      -- ぶつかったら押し合う
      local dx, dz = P.x - C.x, P.z - C.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 2.1 and d > 1e-3 then
        P.x = P.x + dx / d * (2.1 - d); P.z = P.z + dz / d * (2.1 - d)
        if (self.bumpT or 0) <= 0 then P.v = P.v * 0.85; play(self, "bump"); self.bumpT = 0.4 end
      end
      if not C.done and C.prog >= self.laps * self.L then C.done = true; C.finishT = self.raceT end
    end
    self.bumpT = (self.bumpT or 0) - dt

    -- アイテムボックス
    for _, it in ipairs(self.items) do
      if it.off > 0 then it.off = it.off - dt
      else
        for i = 1, 4 do
          local K = self.k[i]
          local dx, dz = K.x - it.x, K.z - it.z
          if dx * dx + dz * dz < 4.5 then
            it.off = 3
            if i == 1 and not self.item and self.roulT <= 0 then self.roulT = 1.2 end
          end
        end
      end
    end
    if self.roulT > 0 then
      self.roulT = self.roulT - dt
      if math.floor(self.roulT * 12) ~= self.lastRoul then self.lastRoul = math.floor(self.roulT * 12); play(self, "roul") end
      if self.roulT <= 0 then
        if math.random() < 0.3 then self.item, self.itemN = "金のキノコ", 3 else self.item, self.itemN = "キノコ", 1 end
        play(self, "got")
      end
    end
    if press and self.item and not P.done then
      self.boostT = 1.6; P.v = math.min(P.v + 8, VBOOST)
      play(self, "boost")
      self.itemN = self.itemN - 1
      if self.itemN <= 0 then self.item = nil end
    end

    -- ゴールと周回の知らせ
    local lap = math.floor(math.max(P.prog, 0) / self.L) + 1
    if not P.done and lap > self.lapShown then
      self.lapShown = lap
      if lap > self.laps then
        P.done = true; P.finishT = self.raceT
        local rank = 1
        for i = 2, 4 do if self.k[i].done then rank = rank + 1 end end
        self.rank = rank
        self.state = FINISH; self.stateT = 0
        if self.best == 0 or P.finishT < self.best then self.best = P.finishT; saveNum("kart.best", self.best) end
        play(self, rank == 1 and "win" or "lose")
        self.msg = rank .. " 位でゴール！"; self.msgT = 99
      elseif lap == self.laps then
        play(self, "final"); self.msg = "ファイナルラップ！"; self.msgT = 1.6
      else
        play(self, "lap"); self.msg = "LAP " .. lap; self.msgT = 1.2
      end
    end
    if self.state == FINISH and self.stateT > 2 and press then reset(self) end
  end
  self.msgT = (self.msgT or 0) - dt

  -- 順位
  local rank = 1
  for i = 2, 4 do if self.k[i].prog > P.prog then rank = rank + 1 end end
  if self.state ~= FINISH then self.curRank = rank end

  -- カートとアイテムボックスを動かす
  for i = 1, 4 do
    local K, e = self.k[i], self.ents[i]
    if e and e:isValid() then
      e.transform.position = Vec3.new(K.x, 0, K.z)
      local roll = (i == 1) and (-self.steer * 5 * clamp(P.v / 10, 0, 1)) or 0
      e.transform.rotation = Vec3.new(0, math.deg(K.yaw), roll)
    end
  end
  for _, it in ipairs(self.items) do
    local s = it.off > 0 and 0.01 or 1.3
    it.e.transform.scale = Vec3.new(s, s, s)
    it.e.transform.position = Vec3.new(it.x, it.y + 0.25 * math.sin(self.t * 2 + it.x), it.z)
    it.e.transform.rotation = Vec3.new(25, self.t * 90, 25)
  end

  -- カメラ（後ろから追いかける。速いほど広角）
  if self.cam and self.cam:isValid() then
    local fx, fz = math.sin(P.yaw), math.cos(P.yaw)
    local tx, ty, tz = P.x - fx * 7.5, 3.4, P.z - fz * 7.5
    if not self.camX then self.camX, self.camY, self.camZ = tx, ty, tz end
    local k = math.min(1, dt * 6)
    self.camX = lerp(self.camX, tx, k); self.camY = lerp(self.camY, ty, k); self.camZ = lerp(self.camZ, tz, k)
    local lx, ly, lz = P.x + fx * 5, 1.0, P.z + fz * 5
    local dx, dy, dz = lx - self.camX, ly - self.camY, lz - self.camZ
    local h = math.sqrt(dx * dx + dz * dz)
    self.cam.transform.position = Vec3.new(self.camX, self.camY, self.camZ)
    self.cam.transform.rotation = Vec3.new(math.deg(math.atan(-dy, h)), math.deg(math.atan(dx, dz)), 0)
    self.cam:setFov(60 + P.v * 0.45 + (self.boostT > 0 and 8 or 0))
  end

  -- 信号の灯り
  for i, e in ipairs(self.signals) do
    if e and e:isValid() then
      local on = self.state == COUNT and self.stateT >= i - 1
      local green = self.state == RACE and self.stateT < 2
      if green then scene:setColor(e, 0.1, 1, 0.2)
      elseif on then scene:setColor(e, 1, 0.1, 0.05)
      else scene:setColor(e, 0.15, 0.03, 0.03) end
    end
  end

  -- LED: 信号 > ルーレット > ダッシュ > アイテムを持っている > コースアウト > 順位
  if self.state == RACE and self.stateT < 1 then led = { 0, 1, 0 }
  elseif self.roulT > 0 then
    local h = (self.t * 5) % 1
    led = { clamp(math.abs(h * 6 - 3) - 1, 0, 1), clamp(2 - math.abs(h * 6 - 2), 0, 1), clamp(2 - math.abs(h * 6 - 4), 0, 1) }
  elseif self.boostT > 0 then
    local k = (math.floor(self.t * 14) % 2 == 0) and 1 or 0.2
    led = { k, k * 0.6, 0 }
  elseif self.item then
    local k = 0.5 + 0.5 * math.sin(self.t * 5)
    led = { 1 * k, 0.45 * k, 0 }
  elseif racing and P.off then led = { 0.4, 0.25, 0.05 }
  elseif racing then
    if self.curRank == 1 then led = { 0.9, 0.7, 0.05 } else led = { 0.05, 0.1, 0.3 } end
  end
  if self.state == FINISH then
    local h = (self.t * 1.2) % 1
    led = self.rank == 1 and { clamp(math.abs(h * 6 - 3) - 1, 0, 1), clamp(2 - math.abs(h * 6 - 2), 0, 1), clamp(2 - math.abs(h * 6 - 4), 0, 1) }
                         or { 0.1, 0.1, 0.4 }
  end
  dev:set("r", led[1]); dev:set("g", led[2]); dev:set("b", led[3])

  -- サーボ = スピードメーター
  dev:set("servo", clamp(P.v / VBOOST, 0, 1))

  -- 音: 効果音が鳴っていなければエンジン音
  local hz = 0
  if self.snd then
    local sd = self.snd[self.sndI]
    hz = sd[1]
    self.sndT = self.sndT - dt
    if self.sndT <= 0 then
      self.sndI = self.sndI + 1
      if self.sndI > #self.snd then self.snd = nil else self.sndT = self.snd[self.sndI][2] end
    end
  elseif self.engineSound and (racing or self.state == COUNT) then
    hz = 80 + P.v * 13 + (self.boostT > 0 and 80 or 0) + gas * 25
    hz = hz * (1 + 0.03 * math.sin(self.t * 110))
    if P.off and racing then hz = hz * (0.85 + 0.15 * math.sin(self.t * 40)) end
  end
  dev:set("vol", loadNum("grove.volume", 0.35))
  dev:set("tone", clamp(hz, 0, 4000) / 4000)

  drawHud(self, P)
end

---------------------------------------------------------------------------
local function fmtTime(t)
  return string.format("%d:%05.2f", math.floor(t / 60), t % 60)
end

function drawHud(self, P)
  -- 周・タイム・順位
  ui:rect(16, 16, 250, 120, 0.04, 0.05, 0.08, 0.75, 10)
  local lap = clamp(math.floor(math.max(P.prog, 0) / self.L) + 1, 1, self.laps)
  ui:text(32, 24, string.format("LAP %d / %d", lap, self.laps), 26, 1, 1, 1, 1)
  ui:text(32, 60, fmtTime(self.state == FINISH and P.finishT or self.raceT), 24, 0.85, 0.9, 1, 1)
  if self.best > 0 then ui:text(32, 96, "ベスト " .. fmtTime(self.best), 16, 0.7, 0.75, 0.85, 1) end
  local rank = self.state == FINISH and self.rank or self.curRank or 4
  local rc = rank == 1 and { 1, 0.8, 0.1 } or { 1, 1, 1 }
  ui:text(280, 14, rank .. "位", 64, rc[1], rc[2], rc[3], 1)

  -- アイテム枠
  ui:rect(470, 16, 120, 100, 0.04, 0.05, 0.08, 0.75, 12)
  ui:rect(478, 24, 104, 84, 0.12, 0.13, 0.17, 1, 10)
  if self.roulT > 0 then
    local names = { "キノコ", "？？？", "金のキノコ", "★" }
    ui:text(494, 52, names[math.floor(self.t * 14) % 4 + 1], 20, 1, 1, 1, 1)
  elseif self.item then
    ui:text(490, 44, self.item, 20, 1, 0.75, 0.2, 1)
    ui:text(500, 74, "×" .. self.itemN .. "  スイッチ", 16, 0.9, 0.9, 0.9, 1)
  else
    ui:text(504, 54, "アイテム", 16, 0.45, 0.48, 0.55, 1)
  end

  -- 速さ・アクセル・ハンドル
  ui:rect(16, 440, 330, 110, 0.04, 0.05, 0.08, 0.75, 10)
  ui:text(32, 448, string.format("%3d km/h", math.floor(P.v * 3.6 + 0.5)), 30, 1, 1, 1, 1)
  ui:text(32, 490, "アクセル", 14, 0.7, 0.75, 0.85, 1)
  ui:rect(100, 492, 230, 12, 0.15, 0.17, 0.22, 1, 4)
  ui:rect(100, 492, 230 * (self.gas or 0), 12, 0.3, 0.9, 0.4, 1, 4)
  ui:text(32, 516, "ハンドル", 14, 0.7, 0.75, 0.85, 1)
  ui:rect(100, 518, 230, 12, 0.15, 0.17, 0.22, 1, 4)
  ui:rect(214, 514, 2, 20, 0.6, 0.6, 0.7, 1, 0)
  ui:rect(214 + 110 * (self.steer or 0) - 6, 515, 12, 18, 1, 0.75, 0.2, 1, 4)

  -- ミニマップ
  local mx, my, mw = 820, 360, 170
  ui:rect(mx - 10, my - 10, mw + 20, mw + 20, 0.04, 0.05, 0.08, 0.65, 10)
  local b = self.mapB
  local sc = mw / math.max(b[2] - b[1], b[4] - b[3])
  local function mp(x, z) return mx + (x - b[1]) * sc, my + mw - (z - b[3]) * sc end
  for i = 1, #self.pts, 3 do
    local px, py = mp(self.pts[i].x, self.pts[i].z)
    ui:rect(px - 1.5, py - 1.5, 3, 3, 0.7, 0.72, 0.78, 1, 1)
  end
  for i = 4, 1, -1 do
    local K = self.k[i]
    local px, py = mp(K.x, K.z)
    local c = CPU_COLOR[i]
    local s = (i == 1) and 10 or 8
    ui:rect(px - s / 2, py - s / 2, s, s, c[1], c[2], c[3], 1, s / 2)
  end

  -- 真ん中の知らせ
  if self.state == READY then
    ui:rect(250, 150, 520, 230, 0.04, 0.05, 0.08, 0.8, 14)
    ui:text(300, 166, "グローブ・グランプリ", 34, 1, 0.8, 0.2, 1)
    ui:text(290, 220, "ハンドル: つまみ（いまの位置がまっすぐ）", 18, 0.9, 0.92, 1, 1)
    ui:text(290, 248, self.autoGas and "アクセル: 自動（両手でハンドルに集中）" or "アクセル: センサに手を近づける", 18, 0.9, 0.92, 1, 1)
    ui:text(290, 276, "アイテム: スイッチ（キノコでダッシュ）", 18, 0.9, 0.92, 1, 1)
    ui:text(290, 304, "信号が緑になる直前にスイッチでロケットスタート", 16, 0.75, 0.8, 0.9, 1)
    ui:text(290, 340, "スイッチでスタート     R で GroveLab に戻る", 20, 1, 0.85, 0.35, 1)
  elseif self.state == COUNT then
    local n = 3 - math.floor(self.stateT)
    ui:text(480, 190, tostring(n), 96, 1, 0.25, 0.2, 1)
  end
  if self.msgT > 0 and self.msg then
    ui:text(380, 200, self.msg, 44, 1, 0.9, 0.3, 1)
    if self.state == FINISH then
      ui:text(380, 260, "タイム " .. fmtTime(P.finishT) .. "   スイッチでもう一回", 22, 1, 1, 1, 1)
    end
  end
end
