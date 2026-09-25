-- AiDirector.lua — ルールで書く「ディレクター」のひな型（Brain の上に立つ演出係）。
--
-- Brain（ai.brain）は 1 体ぶんの頭脳で「今どう動くか」を決める。ディレクターはゲーム全体の
-- 【緊張の拍】を決めて、Brain たちに黒板で伝えるだけ。直接は動かさない（決めるのは各 Brain）。
--   拍（beat）は 4 つ:
--     silence  … 静寂。追跡の後の息つぎ。Brain には director.calm=true（狩りをやめて離れる合図）
--     presence … 気配。静寂が長すぎたら、一番近い Brain に「プレイヤーのまわり」を記憶させて寄せる
--                （brain:remember。正確な位置ではなく半径 zoneRadius の中の点＝通り過ぎることもある）
--     chase    … 追跡中（どれかの Brain が chaseAction を実行している）
--     hint     … 進展が無いまま hintAfter 秒たった。ゲームへ "director.hint" イベントを出す（光らせる等は
--                ゲーム側の仕事）
--   ★外部 AI は使わない。全部このファイルの if 文（決定論・オフライン）。乱数も使わない。
--
-- 出力:
--   各 Brain の黒板: director.beat（文字列）/ director.intensity（0..1）/ director.calm（真偽）
--     → Brain の考慮事項の input にそのまま使える（例: { input = "director.calm", curve = "step", invert = true }）
--   イベント: "director.beat" { beat, prev }（拍が変わった時）/ "director.hint" { waited }
--   saveNum("directorBeat", 0..3)（BGM などほかのスクリプトが読む。0=silence 1=presence 2=chase 3=hint）
-- 入力:
--   プレイヤー（player プロパティの名前）の位置と速さ / 各 Brain の今の行動と target.distance /
--   ゲームが出す "director.progress" イベント（謎を解いた・鍵を拾った など。hint の時計を戻す）/
--   プレイヤーが出した音（"ai.sound" で source がプレイヤーのもの）→ intensity を上げる
--
-- 使い方: 空のエンティティに貼る（1 シーンに 1 個）。Brain を持つ敵は別に置く。
--   敵の行動定義で director.calm を考慮事項に入れると「静寂の拍では引く」が効く。

properties = {
  { name = "player",       type = "string", default = "MainCamera", label = "プレイヤーの名前" },
  { name = "chaseAction",  type = "string", default = "chase",      label = "追跡とみなす行動名" },
  { name = "restTime",     type = "float",  default = 20.0, min = 0, max = 300, label = "追跡の後の静寂(秒)" },
  { name = "silenceMax",   type = "float",  default = 45.0, min = 5, max = 600, label = "静寂の上限(秒)→気配" },
  { name = "presenceTime", type = "float",  default = 25.0, min = 5, max = 300, label = "気配の長さ(秒)" },
  { name = "zoneRadius",   type = "float",  default = 12.0, min = 1, max = 100, label = "気配で渡す範囲の半径(m)" },
  { name = "hintAfter",    type = "float",  default = 120.0, min = 10, max = 1800, label = "進展が無ければヒント(秒)" },
  { name = "nearDist",     type = "float",  default = 15.0, min = 1, max = 100, label = "「近い」とみなす距離(m)" },
  { name = "debugLog",     type = "bool",   default = true, label = "拍の切り替えをログに出す" },
}

local BEAT_ID = { silence = 0, presence = 1, chase = 2, hint = 3 }

local function find(name) local e = scene:findEntity(name); return (e and e:isValid()) and e or nil end

-- 拍を変える（変わった時だけイベントとログ）
local function setBeat(self, beat)
    if self.beat == beat then return end
    local prev = self.beat
    self.beat, self.beatT = beat, 0
    saveNum("directorBeat", BEAT_ID[beat] or 0)
    events:emit("director.beat", { beat = beat, prev = prev or "" })
    if self.debugLog then log(string.format("Director: %s -> %s (intensity %.2f)", tostring(prev), beat, self.intensity)) end
end

function OnStart(self)
    self.beat, self.beatT, self.sinceChase, self.sinceProgress = nil, 0, 1e9, 0
    self.intensity = 0
    self.lp = nil
    setBeat(self, "silence")
    -- ゲームが「進展した」を知らせる口（hint の時計を戻す）
    events:on("director.progress", function() self.sinceProgress = 0 end)
    -- プレイヤーの出した音で緊張が上がる
    events:on("ai.sound", function(d)
        local pl = find(self.player)
        if pl and d.source == pl.id then self.intensity = math.min(1, self.intensity + 0.05) end
    end)
end

function OnUpdate(self, dt)
    local pl = find(self.player)
    if not pl then return end
    local pp = pl.transform.position
    self.beatT = self.beatT + dt
    self.sinceChase = self.sinceChase + dt
    self.sinceProgress = self.sinceProgress + dt

    -- ---- 敵の様子（一番近い Brain / 追跡中か）----
    local chasing, nearest, nearestD = false, nil, 1e9
    for _, e in ipairs(ai.brains()) do
        local b = ai.brain(e)
        if b:current() == self.chaseAction then chasing = true end
        local d = b:get("target.distance", 1e9)
        if d < nearestD then nearest, nearestD = b, d end
    end

    -- ---- 緊張（intensity）: 近い・追われていると上がり、何も無ければ下がる ----
    local up = chasing and 0.25 or ((nearestD < self.nearDist) and 0.08 or -0.03)
    self.intensity = math.max(0, math.min(1, self.intensity + up * dt))

    -- ---- 拍のルール（上から順に 1 つだけ当てはめる）----
    if chasing then
        setBeat(self, "chase")
        self.sinceChase = 0
    elseif self.beat == "chase" then
        setBeat(self, "silence")                       -- 追跡が終わったら必ず息つぎ
    elseif self.sinceProgress >= self.hintAfter and self.beat ~= "hint" then
        setBeat(self, "hint")
        events:emit("director.hint", { waited = self.sinceProgress })
    elseif self.beat == "hint" and self.beatT >= 10 then
        self.sinceProgress = 0                         -- ヒントは 1 回出したら時計を戻す
        setBeat(self, "silence")
    elseif self.beat == "silence" and self.sinceChase >= self.restTime and self.beatT >= self.silenceMax then
        setBeat(self, "presence")
        -- 一番近い Brain に「プレイヤーのまわり」を渡す（正確な位置は渡さない）
        if nearest then
            local t = nearest:randomPoint(pp, self.zoneRadius)
            if t then nearest:remember(t) end
        end
    elseif self.beat == "presence" and self.beatT >= self.presenceTime then
        setBeat(self, "silence")
    end

    -- ---- Brain たちへ伝える（黒板）----
    local calm = (self.beat == "silence" and self.sinceChase < self.restTime)
    for _, e in ipairs(ai.brains()) do
        local b = ai.brain(e)
        b:set("director.beat", self.beat)
        b:set("director.intensity", self.intensity)
        b:set("director.calm", calm)
    end
end
