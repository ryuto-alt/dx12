-- カートレース「グローブ・グランプリ」（scenes/kart.json）
--   赤 = つまみの人: つまみ = ハンドル、アクセルは自動、スイッチ = アイテム
--   青・緑・黄 = スマホの人（最大 3 人）か CPU。スマホは phone\スマホでつなぐ.bat の中継で参加する
--     中継とは PC の中だけの UDP（127.0.0.1）でやり取りする: スマホの入力を :47811 で受け、レースの状況を :47812 へ送る。
--     （以前はファイルを 1 秒に 30 回書き換えていて、OneDrive の同期と Defender の検査で PC が重くなっていた）
--     net.udpOpen が無い古いエンジンでは assets/kart/phones.txt / state.txt でやり取りする
--   サーボ = 赤のスピードメーター、LED = 信号・アイテム・ダッシュ、スピーカー = エンジン音と効果音
--   3 周。アイテムボックスでキノコ（ダッシュ）。R で GroveLab に戻る。J でつまみの人が走る / 走らない
--   実機が無ければ ← → がハンドル、Space がアイテム
properties = {
  { name = "laps",       type = "int",   default = 3,    min = 1,  max = 9,   label = "周回数" },
  { name = "autoGas",    type = "bool",  default = true, label = "赤のアクセルを自動で全開にする（OFF でセンサがアクセル）" },
  { name = "gasNearCm",  type = "float", default = 8,    min = 2,  max = 30,  label = "アクセル全開の距離 (cm)" },
  { name = "gasFarCm",   type = "float", default = 40,   min = 10, max = 100, label = "アクセル 0 の距離 (cm)" },
  { name = "steerGain",  type = "float", default = 2.2,  min = 0.5, max = 5,  label = "ハンドルの効き" },
  { name = "invertSteer", type = "bool", default = false, label = "つまみのハンドルの左右を逆にする" },
  { name = "steerCenter", type = "float", default = 0.5, min = 0.1, max = 0.9, label = "まっすぐのつまみの位置 (0〜1)" },
  { name = "cpuSpeed",   type = "float", default = 1.0,  min = 0.5, max = 1.5, label = "CPU の速さ（倍）" },
  { name = "engineSound", type = "bool", default = true, label = "エンジン音を鳴らす" },
  { name = "knobPlayer", type = "bool", default = true, label = "つまみの人（赤）が参加する（スタート画面の J でも切り替え・覚えておく）" },
}

local VMAX, VBOOST, VGRASS = 26, 34, 11
local KARTS = { "KART_Player", "KART_Cpu1", "KART_Cpu2", "KART_Cpu3" }
local COLOR = { { 0.9, 0.15, 0.1 }, { 0.24, 0.43, 1.0 }, { 0.2, 0.85, 0.3 }, { 1.0, 0.82, 0.1 } }
local CNAME = { "赤", "青", "緑", "黄" }
local READY, COUNT, RACE, FINISH = 0, 1, 2, 3
local UDP_GAME, UDP_RELAY = 47811, 47812   -- ゲームが受ける口 / 中継が受ける口（どちらも 127.0.0.1）

local SND = {
  beep    = { { 440, 0.22 } },
  go      = { { 880, 0.6 } },
  roul    = { { 1500, 0.03 } },
  got     = { { 988, 0.06 }, { 0, 0.02 }, { 1319, 0.12 } },
  boost   = { { 400, 0.06 }, { 600, 0.06 }, { 800, 0.06 }, { 1000, 0.06 }, { 1200, 0.1 } },
  bump    = { { 110, 0.07 } },
  join    = { { 660, 0.06 }, { 0, 0.02 }, { 880, 0.08 } },
  lap     = { { 784, 0.1 }, { 0, 0.03 }, { 784, 0.1 }, { 0, 0.03 }, { 1047, 0.2 } },
  final   = { { 659, 0.1 }, { 784, 0.1 }, { 988, 0.1 }, { 1319, 0.3 } },
  win     = { { 523, 0.12 }, { 659, 0.12 }, { 784, 0.12 }, { 1047, 0.25 }, { 0, 0.05 }, { 784, 0.12 }, { 1047, 0.5 } },
  lose    = { { 392, 0.2 }, { 370, 0.2 }, { 349, 0.2 }, { 330, 0.5 } },
}

-- 関数は先に名前だけ宣言（下でまとめて定義）
local readTrack, nearest, pointAt, reset, play, drawHud, tapped, readPhones, readRoom, writeState, stepHuman, stepCpu, kartName, splitPlan, chase, mapView, drawSplitHud

---------------------------------------------------------------------------
function OnStart(self)
  self.dev = hw.device("grove")
  self.keys = { R = keyDown("R"), SPACE = keyDown("SPACE"), J = keyDown("J"), C = keyDown("C") }  -- 開いたときの押しっぱなしは無視
  self.t = 0
  local base = ASSETS or ""
  if base ~= "" and base:sub(-1) ~= "/" and base:sub(-1) ~= "\\" then base = base .. "/" end
  self.base = base
  readTrack(self)
  self.ents = {}
  for i, n in ipairs(KARTS) do self.ents[i] = scene:findEntity(n) end
  self.cam = scene:findEntity("KART_Cam")
  -- 画面分割はエンジン v2.5.5 から。古いエンジンでは 1 画面のまま動く
  self.splitOk = pcall(function() return scene:getSplitScreen() end)
  -- 中継とのやり取り（エンジン v2.5.6 から UDP。開けなければファイルに戻す）
  self.udp = nil
  if net and net.udpOpen then
    local ok, sock = pcall(net.udpOpen, UDP_GAME)
    if ok and sock then self.udp = sock end
  end
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
  -- スマホへ送るコースの形（4 点に 1 つ）
  local tk = {}
  for i = 1, #self.pts, 4 do tk[#tk + 1] = string.format("[%.1f,%.1f]", self.pts[i].x, self.pts[i].z) end
  self.trkJson = "[" .. table.concat(tk, ",") .. "]"
  self.best = loadNum("kart.best", 0)
  self.cm = 999
  -- つまみの人が参加するか: スタート画面の J で切り替えた値を覚えておく（無ければインスペクタの値）
  self.knobOn = loadNum("kart.knob", self.knobPlayer and 1 or 0) == 1
  self.phones = {}
  self.seq, self.seqT = -1, 99
  self.roomT = 0
  self.stateT = 0
  self.writeT = 0
  -- Play を始めるたびに変わる目印。中継がこれの変化を見て、スマホの参加をいったんリセットする
  self.runId = string.format("%d-%d", math.random(1, 999999999), math.random(1, 999999999))   -- エンジンの Lua に os は無い
  self.center = self.steerCenter
  reset(self)
  readRoom(self)
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
  self.firstDoneT = nil
  self.k = {}
  -- スタート位置: 線の後ろに 2 列。赤は後ろの右
  local grid = { { -14, 2.6 }, { -8, -2.6 }, { -8, 2.6 }, { -14, -2.6 } }
  for i = 1, 4 do
    local g = grid[i]
    local x, z, tx, tz = pointAt(self, self.L + g[1])
    local k = { x = x + tz * g[2], z = z - tx * g[2], yaw = math.atan(tx, tz), v = 0, prog = g[1], lane = g[2],
                done = false, finishT = 0, weave = math.random() * 6, human = false,
                item = nil, itemN = 0, roulT = 0, boostT = 0, slowT = 0, bumpT = 0, lapShown = 1, lat = 0 }
    k.idx, k.s0 = nearest(self, k.x, k.z, nil)
    self.k[i] = k
  end
  self.k[1].human = self.knobOn
  for _, p in pairs(self.phones) do if p.slot > 0 then self.k[p.slot].human = true end end
  self.cpuBase = { 0, 23.0, 24.0, 24.8 }
  self.lastBeep = nil
  self.msg = nil; self.msgT = 0
  self.snd = nil
  self.camX = nil
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

function kartName(self, i)
  if i == 1 then return self.k[1].human and "つまみ" or "CPU" end
  for _, p in pairs(self.phones) do if p.slot == i then return p.name end end
  return "CPU"
end

-- 中継（phone-relay.ps1）が書くスマホの入力を読む。"#seq N" が 2 秒変わらなければ中継が止まっている
function readPhones(self, dt)
  local text = nil
  if self.udp then
    text = self.udp:recvLatest()   -- 中継が 127.0.0.1 へ送ってくる最新のまとめ（来ていないフレームは nil）
  else
    local f = io.open(self.base .. "kart/phones.txt", "r")
    if f then text = f:read("a"); f:close() end
  end
  local seq = text and tonumber(text:match("^#seq (%d+)") or "") or nil
  if seq and seq ~= self.seq then self.seq = seq; self.seqT = 0 else self.seqT = self.seqT + dt end
  self.relayOn = (self.seq or 0) > 0 and self.seqT < 2
  -- 中継が書いている途中を読むと空や途中までになる。そのフレームは読まなかったことにする
  local fresh = seq ~= nil and seq > 0
  local seen = {}
  if self.relayOn and fresh then
    for line in text:gmatch("[^\r\n]+") do
      local id, st, b, name = line:match("^(%d+) (%-?[%d%.]+) (%d+) ?(.*)$")
      if id then
        id = tonumber(id)
        seen[id] = true
        local p = self.phones[id]
        if not p then
          p = { slot = 0, lastBtn = tonumber(b) }
          self.phones[id] = p
        end
        p.steer, p.btn, p.name = clamp(tonumber(st) or 0, -1, 1), tonumber(b) or 0, (name ~= "" and name or ("P" .. id))
      end
    end
  end
  for id, p in pairs(self.phones) do
    if seen[id] then p.goneT = 0
    elseif fresh or not self.relayOn then p.goneT = (p.goneT or 0) + dt end   -- 読みかけのフレームは数えない
    if p.goneT > 1.5 then                                 -- 1.5 秒続けて見えなければ抜けた
      if p.slot > 0 then
        local K = self.k[p.slot]
        K.human = false                                   -- 抜けたカートは CPU に戻す
        K.lane = clamp(K.lat or 0, -4, 4)
      end
      self.phones[id] = nil
    end
  end
  -- 空いている色をスタート前だけ割り当てる（レース中に来た人は次のレースから）
  if self.state == READY then
    for _, p in pairs(self.phones) do
      if p.slot == 0 then
        for s = 2, 4 do
          if not self.k[s].human then p.slot = s; self.k[s].human = true; play(self, "join"); break end
        end
      end
    end
  end
end

-- 参加用の部屋番号・URL・QR（中継が書く）
function readRoom(self)
  local f = io.open(self.base .. "kart/room.txt", "r")
  if not f then self.room = nil; return end
  local text = f:read("a"); f:close()
  local lines = {}
  for line in text:gmatch("[^\r\n]+") do lines[#lines + 1] = line end
  if #lines < 2 then return end
  self.room = { code = lines[1], url = lines[2], qr = {} }
  for i = 3, #lines do if lines[i]:match("^[01]+$") then self.room.qr[#self.room.qr + 1] = lines[i] end end
end

local function jsonStr(s)
  return '"' .. tostring(s):gsub('[%c"\\]', function(c) return c == '"' and '\\"' or (c == "\\" and "\\\\" or " ") end) .. '"'
end

-- スマホへ送るレースの状況（中継が 0.1 秒ごとに読んで送る）
function writeState(self)
  local ps = {}
  for id, p in pairs(self.phones) do
    local K = p.slot > 0 and self.k[p.slot] or nil
    ps[#ps + 1] = string.format('{"id":%d,"slot":%d,"name":%s,"rank":%d,"lap":%d,"item":%s,"n":%d,"roul":%s,"kmh":%d,"done":%s}',
      id, p.slot, jsonStr(p.name), K and (K.rank or 4) or 0, K and clamp(math.floor(math.max(K.prog, 0) / self.L) + 1, 1, self.laps + 1) or 0,
      (K and K.item) and jsonStr(K.item) or '""', K and K.itemN or 0, (K and K.roulT > 0) and "true" or "false",
      K and math.floor(K.v * 3.6 + 0.5) or 0, (K and K.done) and "true" or "false")
  end
  local ks = {}
  for i = 1, 4 do ks[i] = string.format("[%.1f,%.1f,%d]", self.k[i].x, self.k[i].z, i) end
  local s = string.format('{"t":"state","run":"%s","st":%d,"laps":%d,"p":[%s],"k":[%s],"trk":%s}\n',
    self.runId or "", self.state, self.laps, table.concat(ps, ","), table.concat(ks, ","), self.trkJson)
  if self.udp then self.udp:send(UDP_RELAY, s); return end
  local f = io.open(self.base .. "kart/state.txt", "w")
  if f then f:write(s); f:close() end
end

-- 人が運転するカートを 1 フレーム進める（main = 赤。音は赤のぶんだけ鳴らす）
function stepHuman(self, i, steer, gas, press, dt)
  local K = self.k[i]
  local main = i == 1
  if K.done then
    -- ゴール後はコースに沿って流す（ウイニングラン）: 12m 先の中心線へ向ける
    local x, z = pointAt(self, K.s0 + 12)
    local d = (math.atan(x - K.x, z - K.z) - K.yaw + math.pi) % (2 * math.pi) - math.pi
    gas, steer = 0.5, clamp(d * 3, -1, 1)
  end
  local idx, s, lat = nearest(self, K.x, K.z, K.idx)
  local off = math.abs(lat) > self.width / 2 + 0.6
  local vcap = (K.boostT > 0) and VBOOST or (off and VGRASS or VMAX)
  if K.slowT > 0 then K.slowT = K.slowT - dt; vcap = 4 end    -- フライングは少しエンスト
  local acc = gas * (K.boostT > 0 and 30 or 14) - 0.35 * K.v - (gas < 0.05 and 4 or 0)
  if K.v > vcap then acc = math.min(acc, -(K.v - vcap) * 3) end
  K.v = clamp(K.v + acc * dt, 0, VBOOST + 2)
  local turn = steer * self.steerGain * clamp(K.v / 6, 0, 1) * (1 - 0.35 * K.v / VBOOST)
  K.yaw = K.yaw + turn * dt
  K.x = K.x + math.sin(K.yaw) * K.v * dt
  K.z = K.z + math.cos(K.yaw) * K.v * dt
  K.steer = steer
  -- 遠くまで飛び出したら引き戻す
  idx, s, lat = nearest(self, K.x, K.z, idx)
  -- 道の端から 3.5m に見えない壁（観客席・タイヤの壁・木は 4m より外にあるので、突き抜けて中に入らない）
  local lim = self.width / 2 + 3.5
  if math.abs(lat) > lim then
    local p = self.pts[idx]
    local push = (math.abs(lat) - lim) * (lat > 0 and 1 or -1)
    K.x = K.x - p.tz * push; K.z = K.z + p.tx * push
    K.v = K.v * 0.9
    if main and (K.bumpT or 0) <= 0 and not self.snd then play(self, "bump") end
    K.bumpT = 0.3
  end
  if K.bumpT then K.bumpT = K.bumpT - dt end
  -- 周回: s が L から 0 へ戻ったら 1 周
  local ds = s - K.s0
  if ds < -self.L / 2 then K.prog = K.prog + (self.L - K.s0) + s
  elseif ds > self.L / 2 then K.prog = K.prog - (K.s0 + self.L - s)
  else K.prog = K.prog + ds end
  K.s0, K.idx, K.lat, K.off = s, idx, lat, off
  K.boostT = math.max(0, K.boostT - dt)

  -- アイテム: ルーレット → キノコ / 金のキノコ。ボタンでダッシュ
  if K.roulT > 0 then
    K.roulT = K.roulT - dt
    if main and math.floor(K.roulT * 12) ~= self.lastRoul then self.lastRoul = math.floor(K.roulT * 12); play(self, "roul") end
    if K.roulT <= 0 then
      if math.random() < 0.3 then K.item, K.itemN = "金のキノコ", 3 else K.item, K.itemN = "キノコ", 1 end
      if main then play(self, "got") end
    end
  end
  if press and K.item and not K.done then
    K.boostT = 1.6; K.v = math.min(K.v + 8, VBOOST)
    if main then play(self, "boost") end
    K.itemN = K.itemN - 1
    if K.itemN <= 0 then K.item = nil end
  end

  -- 周回とゴール
  local lap = math.floor(math.max(K.prog, 0) / self.L) + 1
  if not K.done and lap > K.lapShown then
    K.lapShown = lap
    if lap > self.laps then
      K.done = true; K.finishT = self.raceT
      self.firstDoneT = self.firstDoneT or self.raceT
      if main and (self.best == 0 or K.finishT < self.best) then self.best = K.finishT; saveNum("kart.best", self.best) end
      if main then
        self.msg = "ゴール！"; self.msgT = 2
        play(self, (K.rank or 4) == 1 and "win" or "lose")
      end
      K.msg, K.msgT = string.format("ゴール！ %d位", K.rank or 4), 3
    elseif lap == self.laps then
      if main then play(self, "final"); self.msg = "ファイナルラップ！"; self.msgT = 1.6 end
      K.msg, K.msgT = "ファイナルラップ！", 1.6
    else
      if main then play(self, "lap"); self.msg = "LAP " .. lap; self.msgT = 1.2 end
      K.msg, K.msgT = "LAP " .. lap, 1.2
    end
  end
end

-- 画面分割の割り当て: 人が 2 人以上いてスタートしたら、人ごとに区画を持つ（3 人なら 4 つ目はコース全体）
function splitPlan(self)
  if not self.splitOk or self.state == READY then return nil end
  local hs = {}
  for i = 1, 4 do if self.k[i].human then hs[#hs + 1] = i end end
  if #hs < 2 then return nil end
  if #hs == 3 then hs[4] = "map" end
  return hs
end

-- カートの後ろから追うカメラ（カートごとになめらかに追う）。
--   上下 2 分割の横長の区画でも、横の見え方が 16:9 の 1 画面と同じになるように縦の画角を決める
function chase(K, dt, aspect)
  local fx, fz = math.sin(K.yaw), math.cos(K.yaw)
  local wide = math.max(0, aspect - 1.8)   -- 横長の区画（上下 2 分割）は少し引いて高くする
  local back, up = 7.5 + wide * 2.2, 3.4 + wide * 0.9
  local tx, ty, tz = K.x - fx * back, up, K.z - fz * back
  if not K.cx then K.cx, K.cy, K.cz = tx, ty, tz end
  local k = math.min(1, dt * 6)
  K.cx = lerp(K.cx, tx, k); K.cy = lerp(K.cy, ty, k); K.cz = lerp(K.cz, tz, k)
  local hTan = math.tan(math.rad(30)) * 16 / 9 * (1 + K.v * 0.0075)
  local fov = clamp(math.deg(2 * math.atan(hTan / aspect)), 20, 75)
  return K.cx, K.cy, K.cz, K.x + fx * 5, 1.0, K.z + fz * 5, fov
end

-- コース全体を上から見るカメラ（3 人のときの 4 つ目の区画）
function mapView(self, aspect)
  local b = self.mapB
  local cx, cz = (b[1] + b[2]) / 2, (b[3] + b[4]) / 2
  local ext = math.max((b[2] - b[1]) / aspect, b[4] - b[3]) * 0.5 + 8
  local h = ext / math.tan(math.rad(25))
  return cx, h, cz - h * 0.2, cx, 0, cz, 50
end

-- CPU（中心線に沿って走る。カーブでは落とし、人より遅れると少し速くなる）
function stepCpu(self, i, lead, dt)
  local C = self.k[i]
  local _, _, _, _, a = pointAt(self, C.prog + 14)
  local want = math.min(self.cpuBase[i] * self.cpuSpeed, a.vmax * (self.cpuBase[i] / 24))
  want = want * clamp(1 + (lead - C.prog) * 0.003, 0.88, 1.1)
  if C.done then want = 14 end
  if self.state ~= RACE and self.state ~= FINISH then want = 0 end
  C.v = C.v + clamp(want - C.v, -16 * dt, 10 * dt)
  C.prog = C.prog + C.v * dt
  local lane = C.lane * 0.6 + 1.6 * math.sin(self.t * 0.35 + C.weave)
  local x, z, tx, tz = pointAt(self, C.prog)
  C.x, C.z = x + tz * lane, z - tx * lane
  C.yaw = math.atan(tx, tz)
  C.s0 = C.prog % self.L
  C.idx = nil
  C.lat = lane
  if not C.done and C.prog >= self.laps * self.L then C.done = true; C.finishT = self.raceT end
end

---------------------------------------------------------------------------
function OnUpdate(self, dt)
  dt = math.min(dt, 0.05)
  self.t = self.t + dt
  self.stateT = self.stateT + dt
  if tapped(self, "R") then
    self.dev:set("tone", 0)
    if self.splitOk then scene:setSplitScreen(0) end
    if self.udp then self.udp:close(); self.udp = nil end
    loadScene("scenes/grove.json")
    return
  end
  readPhones(self, dt)
  self.roomT = self.roomT + dt
  if self.roomT > 2 then self.roomT = 0; readRoom(self) end

  local dev = self.dev
  -- 赤（つまみ）の入力
  local steer, gas
  local swNow = dev:down("sw") or keyDown("SPACE")
  local press1 = swNow and not self.keys.SPACE
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

  -- 全員のボタン（赤 = スイッチ、スマホ = 押した回数が増えたら）
  local press = { press1 and self.k[1].human, false, false, false }
  local phoneSteer = {}
  for _, p in pairs(self.phones) do
    local pr = p.btn ~= p.lastBtn
    p.lastBtn = p.btn
    if p.slot > 0 then press[p.slot] = pr; phoneSteer[p.slot] = p.steer end
  end
  local anyPress = press[1] or press[2] or press[3] or press[4]

  if self.state == READY then
    if tapped(self, "J") then
      self.knobOn = not self.knobOn; self.k[1].human = self.knobOn
      saveNum("kart.knob", self.knobOn and 1 or 0)
      play(self, self.knobOn and "join" or "bump")
    end
    if tapped(self, "C") and dev.connected then self.center = self.knob or self.steerCenter; play(self, "got") end
    if anyPress then self.state = COUNT; self.stateT = 0 end
  elseif self.state == COUNT then
    -- 信号: 赤・赤・赤 → 緑。緑の直前（2.6〜3.0 秒）にボタンでロケットスタート。早すぎるとフライング
    local n = math.floor(self.stateT)
    if n ~= self.lastBeep and n < 3 then self.lastBeep = n; play(self, "beep") end
    for i = 1, 4 do
      if press[i] and self.stateT >= 0.3 then
        if self.stateT >= 2.6 then self.k[i].rocket = true else self.k[i].burnt = true end
      end
    end
    if self.stateT >= 3 then
      self.state = RACE; self.stateT = 0; self.lastBeep = nil
      play(self, "go")
      self.msg = "GO!"; self.msgT = 1.2
      for i = 1, 4 do
        local K = self.k[i]
        if K.human and K.rocket and not K.burnt then
          K.boostT = 1.2; K.v = 18; K.msg, K.msgT = "ロケットスタート！", 1.2
          if i == 1 then self.msg = "ロケットスタート！" end
        elseif K.human and K.burnt then
          K.slowT = 1.0; K.msg, K.msgT = "フライング……", 1.2
          if i == 1 then self.msg = "フライング……" end
        end
        K.rocket, K.burnt = false, false
      end
    end
  end

  local racing = self.state == RACE or self.state == FINISH
  if racing then
    self.raceT = self.raceT + dt
    local lead = -1e9
    for i = 1, 4 do if self.k[i].human then lead = math.max(lead, self.k[i].prog) end end
    if lead < -1e8 then lead = self.k[2].prog end
    for i = 1, 4 do
      local K = self.k[i]
      if K.human then
        if i == 1 then stepHuman(self, 1, steer, gas, press[1], dt)
        else stepHuman(self, i, phoneSteer[i] or 0, 1, press[i], dt) end
      else
        stepCpu(self, i, lead, dt)
      end
    end
    -- ぶつかったら押し合う（人のカートだけ押される）
    for a = 1, 4 do
      for b = a + 1, 4 do
        local A, B = self.k[a], self.k[b]
        if A.human or B.human then
          local dx, dz = A.x - B.x, A.z - B.z
          local d = math.sqrt(dx * dx + dz * dz)
          if d < 2.1 and d > 1e-3 then
            local push = (2.1 - d) / ((A.human and B.human) and 2 or 1)
            if A.human then A.x = A.x + dx / d * push; A.z = A.z + dz / d * push end
            if B.human then B.x = B.x - dx / d * push; B.z = B.z - dz / d * push end
            for _, K in ipairs({ A, B }) do
              if K.human and K.bumpT <= 0 then K.v = K.v * 0.85; K.bumpT = 0.4; if K == self.k[1] then play(self, "bump") end end
            end
          end
        end
      end
    end
    for i = 1, 4 do self.k[i].bumpT = self.k[i].bumpT - dt end

    -- アイテムボックス
    for _, it in ipairs(self.items) do
      if it.off > 0 then it.off = it.off - dt
      else
        for i = 1, 4 do
          local K = self.k[i]
          local dx, dz = K.x - it.x, K.z - it.z
          if dx * dx + dz * dz < 4.5 then
            it.off = 3
            if K.human and not K.item and K.roulT <= 0 then K.roulT = 1.2 end
          end
        end
      end
    end

    -- 順位（ゴールした順 → 進んだ距離）
    local order = { 1, 2, 3, 4 }
    table.sort(order, function(a, b)
      local A, B = self.k[a], self.k[b]
      if A.done and B.done then return A.finishT < B.finishT end
      if A.done ~= B.done then return A.done end
      return A.prog > B.prog
    end)
    for r, i in ipairs(order) do self.k[i].rank = r end
    self.order = order

    -- 終わり: 人が全員ゴール、または最初のゴールから 25 秒
    if self.state == RACE then
      local allDone, anyHuman = true, false
      for i = 1, 4 do if self.k[i].human then anyHuman = true; if not self.k[i].done then allDone = false end end end
      if (anyHuman and allDone) or (self.firstDoneT and self.raceT - self.firstDoneT > 25) then
        self.state = FINISH; self.stateT = 0
        self.msg = "レース終了"; self.msgT = 99
      end
    elseif self.stateT > 3 and anyPress then
      reset(self)
    end
  end
  self.msgT = (self.msgT or 0) - dt
  for i = 1, 4 do local K = self.k[i]; if K.msgT then K.msgT = K.msgT - dt end end

  -- カートとアイテムボックスを動かす
  for i = 1, 4 do
    local K, e = self.k[i], self.ents[i]
    if e and e:isValid() then
      e.transform.position = Vec3.new(K.x, 0, K.z)
      local roll = K.human and (-(K.steer or 0) * 5 * clamp(K.v / 10, 0, 1)) or 0
      e.transform.rotation = Vec3.new(0, math.deg(K.yaw), roll)
    end
  end
  for _, it in ipairs(self.items) do
    local sc = it.off > 0 and 0.01 or 1.3
    it.e.transform.scale = Vec3.new(sc, sc, sc)
    it.e.transform.position = Vec3.new(it.x, it.y + 0.25 * math.sin(self.t * 2 + it.x), it.z)
    it.e.transform.rotation = Vec3.new(25, self.t * 90, 25)
  end

  -- カメラ: 人が 2 人以上でレースが始まったら画面分割（人ごとに後ろから追う）。
  --   それ以外は、人が 1 人ならその後ろ、2 人以上（スタート前）なら全員が入るように引く
  self.plan = splitPlan(self)
  if self.splitOk then scene:setSplitScreen(self.plan and #self.plan or 0) end
  if self.plan and self.cam and self.cam:isValid() then
    for a, who in ipairs(self.plan) do
      local _, _, w, h = scene:getSplitRect(a)
      local aspect = (h and h > 0) and w / h or 16 / 9
      local px, py, pz, lx, ly, lz, fov
      if who == "map" then px, py, pz, lx, ly, lz, fov = mapView(self, aspect)
      else px, py, pz, lx, ly, lz, fov = chase(self.k[who], dt, aspect) end
      if a == 1 then
        local dx, dy, dz = lx - px, ly - py, lz - pz
        self.cam.transform.position = Vec3.new(px, py, pz)
        self.cam.transform.rotation = Vec3.new(math.deg(math.atan(-dy, math.sqrt(dx * dx + dz * dz))), math.deg(math.atan(dx, dz)), 0)
        self.cam:setFov(fov)
      else
        scene:setSplitView(a, px, py, pz, lx, ly, lz, fov)
      end
    end
    self.camX = nil   -- 分割をやめたら 1 画面のカメラはその場から追い直す
  elseif self.cam and self.cam:isValid() then
    local hs = {}
    for i = 1, 4 do if self.k[i].human then hs[#hs + 1] = self.k[i] end end
    if #hs == 0 then hs = { self.k[1] } end
    local cx, cz, fx, fz, vmax = 0, 0, 0, 0, 0
    for _, K in ipairs(hs) do
      cx = cx + K.x / #hs; cz = cz + K.z / #hs
      fx = fx + math.sin(K.yaw); fz = fz + math.cos(K.yaw)
      vmax = math.max(vmax, K.v)
    end
    local fl = math.sqrt(fx * fx + fz * fz)
    if fl < 0.3 then fx, fz = math.sin(hs[1].yaw), math.cos(hs[1].yaw) else fx, fz = fx / fl, fz / fl end
    local spread = 0
    for _, K in ipairs(hs) do spread = math.max(spread, math.sqrt((K.x - cx) ^ 2 + (K.z - cz) ^ 2)) end
    spread = math.min(spread, 60)
    local back, up = 7.5 + spread * 0.9, 3.4 + spread * 0.55
    local tx, ty, tz = cx - fx * back, up, cz - fz * back
    if not self.camX then self.camX, self.camY, self.camZ = tx, ty, tz end
    local k = math.min(1, dt * (#hs > 1 and 3 or 6))
    self.camX = lerp(self.camX, tx, k); self.camY = lerp(self.camY, ty, k); self.camZ = lerp(self.camZ, tz, k)
    local lx, ly, lz = cx + fx * 5, 1.0, cz + fz * 5
    local dx, dy, dz = lx - self.camX, ly - self.camY, lz - self.camZ
    local h = math.sqrt(dx * dx + dz * dz)
    self.cam.transform.position = Vec3.new(self.camX, self.camY, self.camZ)
    self.cam.transform.rotation = Vec3.new(math.deg(math.atan(-dy, h)), math.deg(math.atan(dx, dz)), 0)
    self.cam:setFov(60 + vmax * 0.45 + (#hs > 1 and 6 or 0))
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

  -- LED（赤の人のぶん）: 信号 > ルーレット > ダッシュ > アイテム > コースアウト > 順位
  local P = self.k[1]
  local led = { 0.05, 0.05, 0.08 }
  if self.state == COUNT then led = { 1, 0, 0 }
  elseif self.state == RACE and self.stateT < 1 then led = { 0, 1, 0 }
  elseif P.roulT > 0 then
    local h = (self.t * 5) % 1
    led = { clamp(math.abs(h * 6 - 3) - 1, 0, 1), clamp(2 - math.abs(h * 6 - 2), 0, 1), clamp(2 - math.abs(h * 6 - 4), 0, 1) }
  elseif P.boostT > 0 then
    local kk = (math.floor(self.t * 14) % 2 == 0) and 1 or 0.2
    led = { kk, kk * 0.6, 0 }
  elseif P.item then
    local kk = 0.5 + 0.5 * math.sin(self.t * 5)
    led = { kk, 0.45 * kk, 0 }
  elseif racing and P.off then led = { 0.4, 0.25, 0.05 }
  elseif racing then led = (P.rank == 1) and { 0.9, 0.7, 0.05 } or { 0.05, 0.1, 0.3 } end
  dev:set("r", led[1]); dev:set("g", led[2]); dev:set("b", led[3])
  dev:set("servo", clamp(P.v / VBOOST, 0, 1))      -- サーボ = 赤のスピードメーター

  -- 音: 効果音が鳴っていなければ赤のエンジン音
  local hz = 0
  if self.snd then
    local sd = self.snd[self.sndI]
    hz = sd[1]
    self.sndT = self.sndT - dt
    if self.sndT <= 0 then
      self.sndI = self.sndI + 1
      if self.sndI > #self.snd then self.snd = nil else self.sndT = self.snd[self.sndI][2] end
    end
  elseif self.engineSound and P.human and (racing or self.state == COUNT) then
    hz = 80 + P.v * 13 + (P.boostT > 0 and 80 or 0) + gas * 25
    hz = hz * (1 + 0.03 * math.sin(self.t * 110))
    if P.off and racing then hz = hz * (0.85 + 0.15 * math.sin(self.t * 40)) end
  end
  dev:set("vol", loadNum("grove.volume", 0.35))
  dev:set("tone", clamp(hz, 0, 4000) / 4000)

  self.writeT = self.writeT + dt
  if self.writeT >= 0.1 and self.relayOn then self.writeT = 0; writeState(self) end

  drawHud(self)
end

---------------------------------------------------------------------------
local function fmtTime(t)
  return string.format("%d:%05.2f", math.floor(t / 60), t % 60)
end

-- コースの点とカートの点を (mx,my) から mw 四方に描く
local function drawMinimap(self, mx, my, mw, alpha)
  ui:rect(mx - 10, my - 10, mw + 20, mw + 20, 0.04, 0.05, 0.08, alpha, 10)
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
    local c = COLOR[i]
    local s = K.human and 11 or 7
    ui:rect(px - s / 2, py - s / 2, s, s, c[1], c[2], c[3], 1, s / 2)
  end
end

local function lapText(self, K)
  if K.done then return "ゴール" end
  return string.format("LAP %d/%d", clamp(math.floor(math.max(K.prog, 0) / self.L) + 1, 1, self.laps), self.laps)
end

-- 順位表（x,y から。sc は文字の倍率）
local function drawStandings(self, x, y, sc)
  local rowH = 26 * sc
  ui:rect(x, y, 300 * sc, 40 * sc + 4 * rowH, 0.04, 0.05, 0.08, 0.75, 10)
  ui:text(x + 14 * sc, y + 6 * sc, fmtTime(self.raceT), 22 * sc, 0.9, 0.93, 1, 1)
  if self.best > 0 then ui:text(x + 134 * sc, y + 10 * sc, "ベスト " .. fmtTime(self.best), 14 * sc, 0.65, 0.7, 0.8, 1) end
  for r, i in ipairs(self.order or { 1, 2, 3, 4 }) do
    local K = self.k[i]
    local ry = y + 36 * sc + (r - 1) * rowH
    local c = COLOR[i]
    ui:rect(x + 14 * sc, ry + 4 * sc, 14 * sc, 14 * sc, c[1], c[2], c[3], 1, 7 * sc)
    local bright = K.human and 1 or 0.6
    ui:text(x + 36 * sc, ry, string.format("%d位 %s %s", r, CNAME[i], kartName(self, i)), 18 * sc, bright, bright, bright, 1)
    ui:text(x + 216 * sc, ry + 2 * sc, lapText(self, K), 15 * sc, 0.7 * bright, 0.75 * bright, 0.85 * bright, 1)
  end
end

-- 結果の表（W,H の真ん中）
local function drawResults(self, W, H)
  local px, py = W / 2 - 210, H / 2 - 120
  ui:rect(px, py, 420, 60 + 4 * 32, 0.04, 0.05, 0.08, 0.88, 14)
  ui:text(px + 30, py + 12, "結果", 28, 1, 0.85, 0.3, 1)
  for r, i in ipairs(self.order or { 1, 2, 3, 4 }) do
    local K = self.k[i]
    local c = COLOR[i]
    ui:rect(px + 30, py + 60 + (r - 1) * 32, 16, 16, c[1], c[2], c[3], 1, 8)
    ui:text(px + 56, py + 56 + (r - 1) * 32, string.format("%d位  %s %s  %s", r, CNAME[i], kartName(self, i),
      K.done and fmtTime(K.finishT) or "--"), 20, 1, 1, 1, 1)
  end
  if self.stateT > 3 then ui:text(px + 30, py + 190, "だれかのボタンでもう一回", 16, 1, 0.85, 0.35, 1) end
end

-- 画面分割のときの HUD: 区画ごとに、その人の順位・周回・速さ・アイテム・お知らせ
function drawSplitHud(self, W, H)
  for a, who in ipairs(self.plan) do
    local x, y, w, h = scene:getSplitRect(a)
    local sc = clamp(h / 360, 0.6, 1.3)
    if who == "map" then
      ui:text(x + 14, y + 10, "コース全体", 18 * sc, 0.85, 0.88, 1, 1)
      drawStandings(self, x + 12, y + 40 * sc, sc * 0.85)
    else
      local K = self.k[who]
      local c = COLOR[who]
      -- 左上: 色・順位・名前・周回（空の明るさに負けないよう暗い下地を敷く）
      ui:rect(x + 8, y + 8, 250 * sc, 72 * sc, 0.04, 0.05, 0.08, 0.7, 10)
      ui:rect(x + 12, y + 12, 8 * sc, 64 * sc, c[1], c[2], c[3], 1, 3)
      ui:text(x + 28 * sc, y + 8, string.format("%d位", K.rank or who), 44 * sc, 1, 1, 1, 1)
      ui:text(x + 126 * sc, y + 14, CNAME[who] .. " " .. kartName(self, who), 18 * sc, c[1] * 0.5 + 0.5, c[2] * 0.5 + 0.5, c[3] * 0.5 + 0.5, 1)
      ui:text(x + 126 * sc, y + 14 + 24 * sc, lapText(self, K), 16 * sc, 0.8, 0.84, 0.95, 1)
      -- 左下: 速さ
      ui:rect(x + 8, y + h - 50 * sc, 150 * sc, 42 * sc, 0.04, 0.05, 0.08, 0.6, 8)
      ui:text(x + 16, y + h - 44 * sc, string.format("%3d km/h", math.floor(K.v * 3.6 + 0.5)), 28 * sc, 1, 1, 1, 1)
      -- 右上: アイテム
      local bw, bh = 104 * sc, 70 * sc
      local bx, by = x + w - bw - 14, y + 12
      ui:rect(bx, by, bw, bh, 0.04, 0.05, 0.08, 0.7, 10)
      if K.roulT > 0 then
        local names = { "キノコ", "？？？", "金のキノコ", "★" }
        ui:text(bx + 10 * sc, by + 22 * sc, names[math.floor(self.t * 14) % 4 + 1], 18 * sc, 1, 1, 1, 1)
      elseif K.item then
        ui:text(bx + 8 * sc, by + 12 * sc, K.item, 18 * sc, 1, 0.75, 0.2, 1)
        ui:text(bx + 8 * sc, by + 40 * sc, "×" .. K.itemN, 16 * sc, 0.9, 0.9, 0.9, 1)
      else
        ui:text(bx + 14 * sc, by + 24 * sc, "アイテム", 15 * sc, 0.45, 0.48, 0.55, 1)
      end
      -- 真ん中: その人へのお知らせ（周回・ゴール・ロケット）
      if K.msg and (K.msgT or 0) > 0 then
        ui:text(x + w * 0.5 - 90 * sc, y + h * 0.3, K.msg, 34 * sc, 1, 0.9, 0.3, 1)
      end
      -- 赤（つまみ）の区画にはハンドルのバー
      if who == 1 then
        local hx, hy = x + w / 2 - 75 * sc, y + h - 30 * sc
        ui:rect(hx, hy, 150 * sc, 10 * sc, 0.15, 0.17, 0.22, 0.9, 4)
        ui:rect(hx + 75 * sc + 70 * sc * (self.steer or 0) - 5, hy - 3 * sc, 10, 16 * sc, 1, 0.75, 0.2, 1, 4)
      end
    end
  end
  -- 真ん中: 小さなコース図（3 人のときは 4 つ目の区画がコース全体なので出さない）
  if self.plan[4] ~= "map" then
    local mw = math.floor(math.min(W, H) * 0.18)
    if #self.plan == 2 then   -- 左右 2 分割: 境目の下のほう（カートは各区画の真ん中に映る）
      drawMinimap(self, W / 2 - mw / 2, H - mw - 30, mw, 0.55)
    else
      drawMinimap(self, W / 2 - mw / 2, H / 2 - mw / 2, mw, 0.55)
    end
  end
  -- 全員へのお知らせ: カウントダウン・GO・結果
  if self.state == COUNT then
    ui:text(W / 2 - 28, H / 2 - 70, tostring(3 - math.floor(self.stateT)), 96, 1, 0.25, 0.2, 1)
  elseif self.state == RACE and self.stateT < 1.2 then
    ui:text(W / 2 - 50, H / 2 - 40, "GO!", 64, 0.3, 1, 0.4, 1)
  end
  if self.state == FINISH then drawResults(self, W, H) end
end

function drawHud(self)
  local W, H = 1024, 576
  if self.splitOk then W, H = scene:getViewSize() end
  if self.plan then drawSplitHud(self, W, H); return end
  -- 1 画面: 真ん中の飾り（スタート前・カウント・お知らせ）は 1024x576 を画面の真ん中に置いた座標
  local ox, oy = math.max(0, (W - 1024) / 2), math.max(0, (H - 576) / 2)
  local P = self.k[1]
  drawStandings(self, 16, 16, 1)

  -- 赤のアイテム枠と速さ
  if P.human then
    local ix = W / 2 - 42
    ui:rect(ix, 16, 120, 100, 0.04, 0.05, 0.08, 0.75, 12)
    ui:rect(ix + 8, 24, 104, 84, 0.12, 0.13, 0.17, 1, 10)
    if P.roulT > 0 then
      local names = { "キノコ", "？？？", "金のキノコ", "★" }
      ui:text(ix + 24, 52, names[math.floor(self.t * 14) % 4 + 1], 20, 1, 1, 1, 1)
    elseif P.item then
      ui:text(ix + 20, 44, P.item, 20, 1, 0.75, 0.2, 1)
      ui:text(ix + 30, 74, "×" .. P.itemN .. "  スイッチ", 16, 0.9, 0.9, 0.9, 1)
    else
      ui:text(ix + 34, 54, "アイテム", 16, 0.45, 0.48, 0.55, 1)
    end
    local sy = H - 136
    ui:rect(16, sy, 330, 110, 0.04, 0.05, 0.08, 0.75, 10)
    ui:text(32, sy + 8, string.format("赤 %3d km/h", math.floor(P.v * 3.6 + 0.5)), 30, 1, 1, 1, 1)
    ui:text(32, sy + 50, "アクセル", 14, 0.7, 0.75, 0.85, 1)
    ui:rect(100, sy + 52, 230, 12, 0.15, 0.17, 0.22, 1, 4)
    ui:rect(100, sy + 52, 230 * (self.gas or 0), 12, 0.3, 0.9, 0.4, 1, 4)
    ui:text(32, sy + 76, "ハンドル", 14, 0.7, 0.75, 0.85, 1)
    ui:rect(100, sy + 78, 230, 12, 0.15, 0.17, 0.22, 1, 4)
    ui:rect(214, sy + 74, 2, 20, 0.6, 0.6, 0.7, 1, 0)
    ui:rect(214 + 110 * (self.steer or 0) - 6, sy + 75, 12, 18, 1, 0.75, 0.2, 1, 4)
  end

  -- ミニマップ（右下）
  drawMinimap(self, W - 194, H - 206, 170, 0.65)

  -- スタート前: 遊び方と、スマホで参加する QR
  if self.state == READY then
    ui:rect(ox + 150, oy + 176, 720, 290, 0.04, 0.05, 0.08, 0.85, 14)
    ui:text(ox + 176, oy + 190, "グローブ・グランプリ", 32, 1, 0.8, 0.2, 1)
    ui:text(ox + 176, oy + 240, "赤: つまみ = ハンドル、スイッチ = アイテム", 17, 0.9, 0.92, 1, 1)
    ui:text(ox + 176, oy + 266, "スマホ: 傾けてハンドル、ボタン = アイテム", 17, 0.9, 0.92, 1, 1)
    ui:text(ox + 176, oy + 292, "アクセルは自動。2 人以上なら画面分割", 15, 0.75, 0.8, 0.9, 1)
    local y = oy + 326
    for i = 1, 4 do
      local K = self.k[i]
      local c = COLOR[i]
      ui:rect(ox + 176, y + 4, 14, 14, c[1], c[2], c[3], 1, 7)
      local label = K.human and kartName(self, i) or "CPU"
      if i == 1 then label = self.knobOn and "つまみ（J で外す）" or "CPU  ― つまみは参加しない（J で参加）" end
      ui:text(ox + 198, y, CNAME[i] .. "  " .. label, 17, K.human and 1 or 0.55, K.human and 1 or 0.55, K.human and 1 or 0.6, 1)
      y = y + 24
    end
    ui:text(ox + 176, oy + 428, "だれかのボタンでスタート   J=つまみの人を入れる/外す  C=まっすぐ合わせ  R=戻る", 14, 1, 0.85, 0.35, 1)
    -- QR
    local qx, qy = ox + 640, oy + 196
    if self.room and #self.room.qr > 0 then
      local n = #self.room.qr
      local m = math.floor(170 / (n + 4))
      ui:rect(qx, qy, (n + 4) * m, (n + 4) * m, 1, 1, 1, 1, 6)
      for r, row in ipairs(self.room.qr) do
        local x0 = nil
        for c = 1, #row + 1 do
          local on = row:sub(c, c) == "1"
          if on and not x0 then x0 = c end
          if not on and x0 then
            ui:rect(qx + (x0 + 1) * m, qy + (r + 1) * m, (c - x0) * m, m, 0, 0, 0, 1, 0)
            x0 = nil
          end
        end
      end
      local qs = (n + 4) * m
      ui:text(qx, qy + qs + 6, "スマホで参加  部屋 " .. self.room.code, 18, 1, 1, 1, 1)
      ui:text(qx, qy + qs + 30, self.relayOn and "中継: つながっています" or "中継: 止まっています", 13,
        self.relayOn and 0.5 or 1, self.relayOn and 1 or 0.6, self.relayOn and 0.6 or 0.4, 1)
    else
      ui:text(qx, qy + 40, "スマホで参加するには", 16, 0.8, 0.82, 0.9, 1)
      ui:text(qx, qy + 64, "GroveLab\\phone の", 16, 0.8, 0.82, 0.9, 1)
      ui:text(qx, qy + 88, "「スマホでつなぐ」を起動", 16, 0.8, 0.82, 0.9, 1)
    end
  elseif self.state == COUNT then
    ui:text(ox + 480, oy + 190, tostring(3 - math.floor(self.stateT)), 96, 1, 0.25, 0.2, 1)
  end
  if self.state == FINISH then
    drawResults(self, W, H)
  elseif self.msgT > 0 and self.msg then
    ui:text(ox + 380, oy + 200, self.msg, 44, 1, 0.9, 0.3, 1)
  end
end
