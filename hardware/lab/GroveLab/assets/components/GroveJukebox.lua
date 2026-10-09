-- BGM ジュークボックス: assets/bgm/1.mid 〜 9.mid を GROVE スピーカーで鳴らす
--   1〜9      その番号の曲に切り替える（同じ番号ならはじめから）
--   Shift+1〜9 予約（キュー）。いまの曲が終わったら順に流す。予約が無ければ同じ曲をくり返す
--   0         止める（予約も消す）
--   N         次の曲へ（予約があればそれ、無ければ次の番号）
--   スピーカーは 1 音しか出せないので、和音はいちばん高い音（ふつうはメロディ）を鳴らす。
--   arpeggio を ON にすると、鳴っている音を高速で切り替えて和音っぽく鳴らす（ファミコン風）
--   曲を変えたいときは assets/bgm/<番号>.mid を好きな MIDI ファイルに置き換える。
--   曲名や鳴らすトラックは assets/bgm/bgm.txt で指定できる（書き方はそのファイルの先頭）
--   出す音の高さは grove.bgmHz に置くだけ。実際に鳴らすのは GroveSpeaker（時限爆弾が優先）
properties = {
  { name = "speed",    type = "float", default = 1,    min = 0.25, max = 3,    label = "速さ（倍）" },
  { name = "transpose", type = "int",  default = 0,    min = -3,   max = 3,    label = "オクターブ上げ下げ" },
  { name = "lowestHz", type = "float", default = 120,  min = 31,   max = 1000, label = "これより低い音はオクターブ上げる (Hz)" },
  { name = "arpeggio", type = "bool",  default = false, label = "和音をアルペジオで鳴らす" },
  { name = "arpMs",    type = "float", default = 40,   min = 15,   max = 200,  label = "アルペジオの速さ (ms)" },
  { name = "autoPlay", type = "int",   default = 0,    min = 0,    max = 9,    label = "Play したら流す曲（0=流さない）" },
  { name = "showList", type = "bool",  default = true, label = "曲の一覧を画面に出す" },
}

---------------------------------------------------------------------------
-- MIDI（Standard MIDI File, format 0 / 1）を読む
---------------------------------------------------------------------------
local function readAll(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local s = f:read("a")
  f:close()
  return s
end

local function u16(s, i) local a, b = s:byte(i, i + 1); return a * 256 + b end
local function u32(s, i) local a, b, c, d = s:byte(i, i + 3); return ((a * 256 + b) * 256 + c) * 256 + d end

-- 可変長の数値 → 値, 次の位置
local function varlen(s, i)
  local v = 0
  repeat
    local b = s:byte(i)
    if not b then return v, i end
    i = i + 1
    v = v * 128 + (b % 128)
  until b < 128
  return v, i
end

-- UTF-8 として正しいか（Shift-JIS の曲名を画面に出して文字化けさせない）
local function validUtf8(s)
  local i, n = 1, #s
  while i <= n do
    local c = s:byte(i)
    local len = c < 0x80 and 1 or (c >= 0xC2 and c < 0xE0) and 2 or (c >= 0xE0 and c < 0xF0) and 3
             or (c >= 0xF0 and c < 0xF5) and 4 or 0
    if len == 0 or i + len - 1 > n then return false end
    for k = i + 1, i + len - 1 do
      local d = s:byte(k)
      if d < 0x80 or d > 0xBF then return false end
    end
    i = i + len
  end
  return true
end

-- opts.track: 鳴らすトラック。トラック名か 1 から数えた番号を「,」で並べると優先順になる
--   （例 "MIKU,1" = 歌が鳴っている所は歌、歌が 0.3 秒以上休む所はトラック 1）。
--   "all" は全トラックからいちばん高い音。nil なら平均の音がいちばん高いトラックだけ（ふつうはメロディ）
-- 戻り値: { title, length(秒), start(秒), segs = { {t0, t1, note(-1=休み), attack, chord} ... } } / nil, エラー文
local function loadMidi(path, opts)
  opts = opts or {}
  local s = readAll(path)
  if not s then return nil, "ファイルがない" end
  if s:sub(1, 4) ~= "MThd" then return nil, "MIDI ファイルではない" end
  local hlen = u32(s, 5)
  local ntrk = u16(s, 11)
  local div = u16(s, 13)
  if div >= 0x8000 then return nil, "SMPTE 時間の MIDI には未対応" end
  local pos = 9 + hlen

  local tempos = { { tick = 0, us = 500000 } }   -- 指定が無ければ 120bpm
  local tracks = {}                                -- { trk, name, notes = { {s, e, n} }, mean }
  local title = nil
  local trackList = {}                             -- ログ用 "番号:名前(音の数)"

  for trk = 1, ntrk do
    if s:sub(pos, pos + 3) ~= "MTrk" then break end
    local tname = nil
    local tnotes = {}
    local tlen = u32(s, pos + 4)
    local i, stop = pos + 8, pos + 8 + tlen
    local tick, status = 0, 0
    local open = {}                                -- [ch*128+note] = { 開始tick の列 }
    while i < stop do
      local dt; dt, i = varlen(s, i)
      tick = tick + dt
      local b = s:byte(i)
      if b >= 0x80 then status = b; i = i + 1 end  -- 0x80 未満は直前のステータスの続き（ランニングステータス）
      if status == 0xFF then
        local typ = s:byte(i); i = i + 1
        local len; len, i = varlen(s, i)
        if typ == 0x51 and len == 3 then
          local a, b2, c = s:byte(i, i + 2)
          tempos[#tempos + 1] = { tick = tick, us = (a * 256 + b2) * 256 + c }
        elseif typ == 0x03 and not tname and len > 0 then
          tname = s:sub(i, i + len - 1)
          if not title then title = tname end
        elseif typ == 0x2F then
          i = i + len
          break
        end
        i = i + len
        status = 0
      elseif status == 0xF0 or status == 0xF7 then
        local len; len, i = varlen(s, i)
        i = i + len
        status = 0
      else
        local kind = status - status % 16
        local ch = status % 16
        if kind == 0xC0 or kind == 0xD0 then
          i = i + 1
        else
          local d1, d2 = s:byte(i, i + 1)
          i = i + 2
          local key = ch * 128 + d1
          if kind == 0x90 and d2 > 0 then
            open[key] = open[key] or {}
            table.insert(open[key], tick)
          elseif kind == 0x80 or kind == 0x90 then
            local q = open[key]
            if q and #q > 0 then
              local st = table.remove(q, 1)
              if ch ~= 9 then tnotes[#tnotes + 1] = { s = st, e = tick, n = d1 } end   -- ch10 はドラム
            end
          end
        end
      end
    end
    for key, q in pairs(open) do             -- 終わりの来なかった音はトラックの終わりで切る
      local ch = math.floor(key / 128)
      for _, st in ipairs(q) do
        if ch ~= 9 then tnotes[#tnotes + 1] = { s = st, e = tick, n = key % 128 } end
      end
    end
    if #tnotes > 0 then
      local nm = (tname and validUtf8(tname)) and tname or "?"
      trackList[#trackList + 1] = string.format("%d:%s(%d)", trk, nm, #tnotes)
      local sum = 0
      for _, nt in ipairs(tnotes) do sum = sum + nt.n end
      tracks[#tracks + 1] = { trk = trk, name = tname, notes = tnotes, mean = sum / #tnotes }
    end
    pos = stop
  end

  -- どのトラックを何番目の優先で鳴らすか（prio 1 がいちばん優先）
  local notes = {}
  local nprio = 1
  local want = opts.track
  if want == "all" then
    for _, tr in ipairs(tracks) do
      for _, nt in ipairs(tr.notes) do nt.p = 1; notes[#notes + 1] = nt end
    end
  elseif want then
    local p = 0
    for rawW in want:gmatch("[^,]+") do
      local w = rawW:match("^%s*(.-)%s*$")
      for _, tr in ipairs(tracks) do
        if tostring(tr.trk) == w or tr.name == w then
          p = p + 1
          for _, nt in ipairs(tr.notes) do nt.p = p; notes[#notes + 1] = nt end
        end
      end
    end
    nprio = math.max(p, 1)
  else
    local best = nil
    for _, tr in ipairs(tracks) do
      if #tr.notes >= 8 and (not best or tr.mean > best.mean) then best = tr end
    end
    best = best or tracks[1]
    if best then
      for _, nt in ipairs(best.notes) do nt.p = 1; notes[#notes + 1] = nt end
      trackList[#trackList + 1] = "→ 自動でトラック " .. best.trk
    end
  end
  if #notes == 0 then
    return nil, want and ("トラック " .. want .. " に音符がない") or "音符がない"
  end

  -- tick → 秒（テンポの変化をたどる）
  table.sort(tempos, function(a, b) return a.tick < b.tick end)
  local acc = {}
  local sec = 0
  for k, t in ipairs(tempos) do
    if k > 1 then
      local p = tempos[k - 1]
      sec = sec + (t.tick - p.tick) * p.us / 1e6 / div
    end
    acc[k] = sec
  end
  local function toSec(tick)
    local lo, hi = 1, #tempos
    while lo < hi do
      local mid = math.floor((lo + hi + 1) / 2)
      if tempos[mid].tick <= tick then lo = mid else hi = mid - 1 end
    end
    return acc[lo] + (tick - tempos[lo].tick) * tempos[lo].us / 1e6 / div
  end

  -- 区間ごとに、鳴っている中でいちばん優先のトラックの、いちばん高い音を決める（スカイライン）
  local ev = {}
  for _, nt in ipairs(notes) do
    if nt.e > nt.s then
      ev[#ev + 1] = { t = toSec(nt.s), d = 1, n = nt.n, p = nt.p }
      ev[#ev + 1] = { t = toSec(nt.e), d = -1, n = nt.n, p = nt.p }
    end
  end
  table.sort(ev, function(a, b)
    if a.t ~= b.t then return a.t < b.t end
    return a.d < b.d                        -- 同時なら先に止めてから鳴らす
  end)
  local count = {}
  for p = 1, nprio do
    count[p] = {}
    for n = 0, 127 do count[p][n] = 0 end
  end
  local segs = {}
  local k = 1
  local lastT = 0
  while k <= #ev do
    local t = ev[k].t
    local started = {}
    while k <= #ev and ev[k].t == t do
      local e = ev[k]
      count[e.p][e.n] = count[e.p][e.n] + e.d
      if e.d > 0 then started[e.n] = true end
      k = k + 1
    end
    local top, chord, prio = -1, {}, 0
    for p = 1, nprio do
      local c = count[p]
      for n = 127, 0, -1 do
        if c[n] > 0 then
          if top < 0 then top = n end
          if #chord < 4 then chord[#chord + 1] = n end
        end
      end
      if top >= 0 then prio = p; break end
    end
    if #segs > 0 then segs[#segs].t1 = t end
    segs[#segs + 1] = { t0 = t, t1 = t, note = top, prio = prio, attack = started[top] == true, chord = chord }
    lastT = t
  end
  -- 長さ 0 の区間を捨てる
  local out = {}
  for _, sg in ipairs(segs) do
    if sg.t1 > sg.t0 then out[#out + 1] = sg end
  end
  -- 優先のトラックが 0.3 秒未満しか休まない所は、下のトラックで埋めずに休みにする（音のすき間に伴奏が「ブッ」と混ざらない）
  local a = 1
  while a <= #out do
    if out[a].prio ~= 1 then
      local b = a
      while b + 1 <= #out and out[b + 1].prio ~= 1 do b = b + 1 end
      if out[b].t1 - out[a].t0 < 0.3 then
        for j = a, b do out[j].note = -1 end
      end
      a = b + 1
    else
      a = a + 1
    end
  end
  -- 曲頭の 1 秒を超える無音は飛ばす
  local start = 0
  for _, sg in ipairs(out) do
    if sg.note >= 0 then
      if sg.t0 > 1 then start = sg.t0 - 0.2 end
      break
    end
  end
  if title and not validUtf8(title) then title = nil end
  return { title = title, length = lastT, start = start, segs = out, tracks = table.concat(trackList, " ") }
end

---------------------------------------------------------------------------
-- ジュークボックス
---------------------------------------------------------------------------
local function midiHz(n) return 440 * 2 ^ ((n - 69) / 12) end
local loadSong, playSong, nextSong, drawList


-- bgm.txt:  2 = 曲名 ; track=MIKU   （; の後ろは省略できる。# から行末はコメント）
local function readConfig(path)
  local cfg = {}
  local text = readAll(path)
  if not text then return cfg end
  if text:sub(1, 3) == "\239\187\191" then text = text:sub(4) end   -- BOM
  for raw in text:gmatch("[^\r\n]+") do
    local line = raw:gsub("#.*$", "")
    local n, rest = line:match("^%s*(%d)%s*=%s*(.-)%s*$")
    if n then
      local c = {}
      local first = true
      for rawPart in (rest .. ";"):gmatch("(.-);") do
        local part = rawPart:match("^%s*(.-)%s*$")
        local k, v = part:match("^(%w+)%s*=%s*(.+)$")
        if k and not first then c[k] = v
        elseif first and part ~= "" then c.title = part end
        first = false
      end
      cfg[tonumber(n)] = c
    end
  end
  return cfg
end

function loadSong(self, i)
  if self.songs[i] ~= nil then return self.songs[i] end
  local c = self.config[i] or {}
  local ok, song, err = pcall(loadMidi, self.base .. i .. ".mid", { track = c.track })
  if ok and song and c.title and validUtf8(c.title) then song.title = c.title end
  if not ok then song, err = nil, tostring(song) end
  self.songs[i] = song or false
  self.errors[i] = err
  if song then
    log(string.format("[bgm] %d.mid: %s（%.1f 秒・%d 区間）トラック %s%s", i, song.title or "曲名なし",
      song.length, #song.segs, song.tracks, c.track and ("  → track=" .. c.track .. " を鳴らす") or ""))
  elseif err ~= "ファイルがない" then
    log(string.format("[bgm] %d.mid を読めません: %s", i, err))
  end
  return self.songs[i]
end

function playSong(self, i)
  if not self.songs[i] then self.cur = 0; return end
  self.cur, self.t, self.idx = i, self.songs[i].start, 1
end

function nextSong(self)
  if #self.queue > 0 then
    playSong(self, table.remove(self.queue, 1))
    return
  end
  for k = 1, 9 do                                  -- 予約が無ければ次の番号（空きは飛ばす）
    local j = (self.cur + k - 1) % 9 + 1
    if self.songs[j] then playSong(self, j); return end
  end
end

local function tapped(self, key)
  local d = keyDown(key)
  local was = self.keys[key]
  self.keys[key] = d
  return d and not was
end

function OnUpdate(self, dt)
  -- 操作
  local shift = keyDown("SHIFT")
  for i = 1, 9 do
    if tapped(self, tostring(i)) and self.songs[i] then
      if shift then
        if #self.queue < 9 then self.queue[#self.queue + 1] = i end
        if self.cur == 0 then nextSong(self) end
      else
        playSong(self, i)
      end
    end
  end
  if tapped(self, "0") then self.cur = 0; self.queue = {} end
  if tapped(self, "N") then nextSong(self) end

  local hz = 0
  local song = self.cur > 0 and self.songs[self.cur] or nil
  -- 時限爆弾が動いている間とトロンボーンが ON の間は止めて待つ
  local busy = loadNum("grove.bomb", 0) ~= 0 or loadNum("grove.trombone", 0) > 0 or loadNum("grove.safe", 0) > 0
  if song and not busy then
    self.t = self.t + dt * self.speed
    if self.t >= song.length then
      if #self.queue > 0 then
        nextSong(self)
        song = self.songs[self.cur]
      else
        self.t, self.idx = song.start + (self.t - song.length), 1   -- 予約が無ければくり返す
      end
    end
    local segs = song.segs
    while self.idx < #segs and segs[self.idx].t1 <= self.t do self.idx = self.idx + 1 end
    local sg = segs[self.idx]
    if sg and sg.note >= 0 and self.t >= sg.t0 then
      local n = sg.note
      if self.arpeggio and #sg.chord > 1 then
        self.arpT = self.arpT + dt
        local step = math.floor(self.arpT * 1000 / self.arpMs)
        n = sg.chord[step % #sg.chord + 1]
      end
      -- 同じ高さの音が続くときは、頭を少し切って打ち直したように聞かせる
      local prev = segs[self.idx - 1]
      local reattack = sg.attack and prev and prev.note == sg.note and self.t - sg.t0 < 0.025
      if not reattack then
        hz = midiHz(n + 12 * self.transpose)
        while hz > 4000 do hz = hz / 2 end
        while hz < self.lowestHz do hz = hz * 2 end
      end
    end
  end
  saveNum("grove.bgmHz", hz)
  saveNum("grove.bgmSlot", self.cur)

  if self.showList and loadNum("grove.safe", 0) == 0 then drawList(self, song) end
end

function drawList(self, song)
  local x, y = 580, 130
  ui:rect(x - 12, y - 10, 400, 300, 0.05, 0.06, 0.09, 0.78, 10)
  ui:text(x, y, "BGM  1〜9=切替  Shift+数字=予約  0=停止  N=次", 18, 0.85, 0.88, 0.95, 1)
  for i = 1, 9 do
    local yy = y + 6 + i * 24
    local s = self.songs[i]
    local name = s and (s.title or (i .. ".mid")) or "（空き）"
    local mark = "  "
    local r, g, b = 0.75, 0.78, 0.85
    if not s then r, g, b = 0.4, 0.42, 0.48 end
    for k, q in ipairs(self.queue) do
      if q == i then mark = "予" .. k; r, g, b = 1.0, 0.85, 0.35; break end
    end
    if i == self.cur then mark = "●"; r, g, b = 0.4, 1.0, 0.55 end
    ui:text(x, yy, string.format("%s %d  %s", mark, i, name), 18, r, g, b, 1)
  end
  local yb = y + 6 + 10 * 24 + 4
  if song then
    ui:rect(x, yb, 370, 8, 0.15, 0.17, 0.22, 0.9, 3)
    ui:rect(x, yb, 370 * clamp(self.t / song.length, 0, 1), 8, 0.4, 1.0, 0.55, 1, 3)
    if loadNum("grove.bomb", 0) ~= 0 then
      ui:text(x, yb + 12, "爆弾が動いている間は一時停止", 16, 1.0, 0.6, 0.3, 1)
    elseif loadNum("grove.trombone", 0) > 0 then
      ui:text(x, yb + 12, "トロンボーン中は一時停止（T で戻る）", 16, 1.0, 0.82, 0.35, 1)
    end
  end
end

function OnStart(self)
  local base = ASSETS or ""
  if base ~= "" and base:sub(-1) ~= "/" and base:sub(-1) ~= "\\" then base = base .. "/" end
  self.base = base .. "bgm/"
  self.config = readConfig(self.base .. "bgm.txt")
  self.songs = {}       -- [番号] = 読んだ曲 / false（読めなかった）
  self.errors = {}
  self.cur = 0          -- 流している番号（0 = 止まっている）
  self.t = 0
  self.idx = 1
  self.queue = {}
  self.keys = {}
  self.arpT = 0
  saveNum("grove.bgmHz", 0)
  -- 一覧に曲名を出すため、9 曲とも先に読んでおく（小さいファイルなので一瞬）
  for i = 1, 9 do loadSong(self, i) end
  if self.autoPlay > 0 then playSong(self, self.autoPlay) end
end
