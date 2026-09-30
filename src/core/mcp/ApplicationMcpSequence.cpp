// ===========================================================================
// MCP: シーケンサー(.dxseq)  sequence_list / load / save / get / eval / scrub / play / stop / apply_op / autoplay
// ---------------------------------------------------------------------------
// 設計: docs/SEQUENCER_DESIGN.md §6、仕様: docs/DXSEQ_FORMAT.md §18〜§20、MCP の面: docs/MCP.md「シーケンサー」。
//   ・実体は core/SequencerHost(Application 非依存)。ここは JSON ⇔ ホストの薄い糊。
//   ・meta を直接渡す(McpDefine(names, McpMeta, fn)): 引数は中央検査され、describe_mcp_manifest に載り、再起動なしで dx12_call から使える。
//   ・時刻は秒(t)/ティック(tick)/フレーム(frame)のどれかで渡す。
//   ・sequence_eval は【非破壊】(何も書かない・guid も確定しない)。sequence_scrub はエディタ上で適用する(PreAnimatedState 管理下 =
//     閉じる / Play / 保存の前に必ず元へ戻る)。
//   ・dryRun:true(dx12_call / dx12_sequence)は apply_op / save / scrub / play のプレビューを返す(何も変えない)。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/SequencerHost.h"
#include "core/mcp/McpManifestBuild.h"
#include "core/mcp/McpSafety.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
using namespace appdetail;
using mcpdata::P;

// DX12E_MCP_HANDLER と同じ引数で、ローカルのヘルパ(host / needDoc)も捕まえるハンドラ。
#define SEQ_HANDLER                                                              [this, host, needDoc]([[maybe_unused]] const nlohmann::json& params,                               [[maybe_unused]] nlohmann::json&       resp,                                 [[maybe_unused]] const std::string&    method,                               [[maybe_unused]] McpDeferred&          deferred,                             [[maybe_unused]] bool&                 isDeferred,                           [[maybe_unused]] bool                  busyPlaying) -> void

namespace
{
using json = nlohmann::json;
using seqhost::DocPtr;

[[noreturn]] void ThrowSeq(int code, const char* name, const std::string& msg, const std::string& hint, const std::string& cause,
                           std::vector<std::string> didYouMean = {})
{
    McpError err(code, msg, hint);
    err.name = name;
    err.cause = cause;
    err.didYouMean = std::move(didYouMean);
    throw err;
}

const char* IssueCodeName(seq::BindingIssue::Code c)
{
    switch (c)
    {
    case seq::BindingIssue::Code::Unresolved: return "Unresolved";
    case seq::BindingIssue::Code::FellBack: return "FellBack";
    case seq::BindingIssue::Code::AmbiguousPath: return "AmbiguousPath";
    case seq::BindingIssue::Code::AmbiguousName: return "AmbiguousName";
    case seq::BindingIssue::Code::SharedTarget: return "SharedTarget";
    case seq::BindingIssue::Code::SpawnableUnsupported: return "SpawnableUnsupported";
    }
    return "?";
}

double Round(double v, double scale = 1e6) { return std::round(v * scale) / scale; }

// 秒 t / ティック tick / フレーム frame → ティック(どれも無ければ 0)
seq::Tick TimeFromParams(const json& p, const seq::Sequence& s, const char* secKey = "t")
{
    if (p.contains("tick") && p["tick"].is_number()) return static_cast<seq::Tick>(std::llround(p["tick"].get<double>()));
    if (p.contains("frame") && p["frame"].is_number())
        return seq::FrameToTick(static_cast<std::int64_t>(std::llround(p["frame"].get<double>())), s.frameRate, s.ticksPerSecond);
    if (p.contains(secKey) && p[secKey].is_number()) return seq::SecondsToTicks(p[secKey].get<double>(), s.ticksPerSecond);
    return 0;
}

json HintJson(const seq::BindingHint& h)
{
    json j = json::object();
    if (!h.guid.empty()) j["guid"] = h.guid;
    if (!h.path.empty()) j["path"] = h.path;
    if (!h.name.empty()) j["name"] = h.name;
    return j;
}

json EntityRefJson(const entt::registry& reg, entt::entity e)
{
    if (e == entt::null || !reg.valid(e)) return nullptr;
    json j = {{"id", static_cast<std::uint32_t>(e)}};
    if (const auto* t = reg.try_get<NameTag>(e)) j["name"] = t->name;
    return j;
}

json BindingStatusJson(const seq::Sequence& s, const seqhost::BoundSequence& b, const entt::registry* reg)
{
    json arr = json::array();
    for (std::size_t i = 0; i < s.bindings.size(); ++i)
    {
        const seq::Binding& bd = s.bindings[i];
        json o = {{"id", bd.id}, {"name", bd.name}, {"kind", seq::BindingKindName(bd.kind)}};
        if (i < b.set.byBinding.size())
        {
            const seq::BindingResolution& r = b.set.byBinding[i];
            o["resolved"] = r.Resolved();
            o["via"] = seq::ResolveViaName(r.via);
            if (r.candidates > 1) o["candidates"] = r.candidates;
        }
        if (reg && i < b.entity.size()) o["entity"] = EntityRefJson(*reg, b.entity[i]);
        o["hint"] = HintJson(bd.hint);
        json warns = json::array();
        if (i < b.tracks.size())
            for (const seqhost::BoundTrack& bt : b.tracks[i])
                if (!bt.warning.empty()) warns.push_back(bt.warning);
        if (!warns.empty()) o["trackWarnings"] = std::move(warns);
        arr.push_back(std::move(o));
    }
    return arr;
}

json IssuesJson(const seqhost::BoundSequence& b)
{
    json arr = json::array();
    for (const seq::BindingIssue& is : b.set.issues)
        arr.push_back({{"code", IssueCodeName(is.code)}, {"binding", is.bindingId}, {"tracks", is.trackIds}, {"message", is.message}});
    return arr;
}

json TrackSummaryJson(const seq::Track& t)
{
    json o = {{"id", t.id}, {"type", seq::TrackTypeName(t.type)}};
    if (!t.name.empty()) o["name"] = t.name;
    if (!t.path.empty()) o["path"] = t.path;
    if (t.mute) o["mute"] = true;
    json chs = json::array();
    for (const seq::Channel& c : t.channels) chs.push_back({{"name", c.name}, {"keys", c.keys.size()}});
    if (!chs.empty()) o["channels"] = std::move(chs);
    if (!t.clips.empty()) o["clips"] = t.clips.size();
    if (!t.events.empty()) o["events"] = t.events.size();
    return o;
}

json DocSummaryJson(const DocPtr& d)
{
    const seq::Sequence& s = d->seq;
    seq::Tick a = 0, b = 0;
    seq::GetRange(s, a, b);
    std::size_t tracks = 0;
    for (const seq::Binding& bd : s.bindings) tracks += bd.tracks.size();
    json o = {{"name", d->name}, {"rel", d->rel}, {"dirty", d->dirty}, {"onDisk", d->onDisk},
              {"durationSec", Round(seq::TicksToSeconds(b - a, s.ticksPerSecond))},
              {"range", json::array({a, b})},
              {"fps", s.frameRate}, {"ticksPerSecond", s.ticksPerSecond},
              {"bindings", s.bindings.size()}, {"tracks", tracks}, {"cuts", s.cuts.size()}, {"markers", s.markers.size()},
              {"revision", d->revision}, {"canUndo", d->history.CanUndo()}, {"canRedo", d->history.CanRedo()}};
    return o;
}

json EvalToJson(const seq::Sequence& s, const seq::EvalResult& r)
{
    json j = {{"tick", r.t}, {"seconds", Round(seq::TicksToSeconds(r.t, s.ticksPerSecond))},
              {"frame", seq::TickToFrameFloor(r.t, s.frameRate, s.ticksPerSecond)}};
    // カット
    if (r.cutIndex >= 0 && static_cast<std::size_t>(r.cutIndex) < s.cuts.size())
    {
        const seq::Cut& c = s.cuts[static_cast<std::size_t>(r.cutIndex)];
        j["cut"] = {{"index", r.cutIndex}, {"id", c.id}, {"camera", c.camera}};
        if (r.cutBinding >= 0) j["cut"]["cameraName"] = s.bindings[static_cast<std::size_t>(r.cutBinding)].name;
    }
    else j["cut"] = nullptr;

    json values = json::array();
    // バインディングごとの読みやすい状態(Transform を [x,y,z] にまとめる)
    json state = json::object();
    for (const seq::ChannelSample& cs : r.channels)
    {
        const seq::Binding& b = s.bindings[static_cast<std::size_t>(cs.binding)];
        const seq::Track& t = b.tracks[static_cast<std::size_t>(cs.track)];
        const seq::Channel& ch = t.channels[static_cast<std::size_t>(cs.channel)];
        json v = {{"binding", b.id}, {"track", t.id}, {"type", seq::TrackTypeName(t.type)}, {"channel", ch.name}, {"value", Round(cs.value)}};
        if (!t.path.empty()) v["path"] = t.path;
        values.push_back(std::move(v));

        json& st = state[b.id];
        if (!st.is_object()) st = {{"name", b.name}};
        std::string key = ch.name;
        const std::size_t dot = ch.name.find('.');
        if (t.type == seq::TrackType::Transform && dot != std::string::npos && ch.name.size() == dot + 2)
        {
            const std::string grp = ch.name.substr(0, dot);
            const int comp = ch.name[dot + 1] == 'x' ? 0 : ch.name[dot + 1] == 'y' ? 1 : 2;
            json& arr = st[grp];
            if (!arr.is_array()) arr = json::array({nullptr, nullptr, nullptr});
            arr[static_cast<std::size_t>(comp)] = Round(cs.value);
        }
        else
        {
            const std::string k = t.path.empty() ? std::string(seq::TrackTypeName(t.type)) + "." + ch.name : t.path + (ch.name == "value" ? "" : "." + ch.name);
            st[k] = Round(cs.value);
        }
    }
    j["values"] = std::move(values);
    for (const seq::QuatSample& q : r.quats)
    {
        const seq::Binding& b = s.bindings[static_cast<std::size_t>(q.binding)];
        const DirectX::XMFLOAT3 eu = QuaternionToEulerDegrees(
            DirectX::XMFLOAT4(static_cast<float>(q.q.x), static_cast<float>(q.q.y), static_cast<float>(q.q.z), static_cast<float>(q.q.w)));
        json& st = state[b.id];
        if (!st.is_object()) st = {{"name", b.name}};
        st["rotation"] = json::array({Round(eu.x), Round(eu.y), Round(eu.z)});
        st["quaternion"] = json::array({Round(q.q.x), Round(q.q.y), Round(q.q.z), Round(q.q.w)});
    }
    json clips = json::array();
    for (const seq::ClipSample& c : r.clips)
    {
        const seq::Binding& b = s.bindings[static_cast<std::size_t>(c.binding)];
        const seq::Track& t = b.tracks[static_cast<std::size_t>(c.track)];
        clips.push_back({{"binding", b.id}, {"track", t.id}, {"type", seq::TrackTypeName(t.type)},
                         {"clip", t.clips[static_cast<std::size_t>(c.clip)].id},
                         {"localSec", Round(seq::TicksToSeconds(c.localTick, s.ticksPerSecond))}, {"weight", Round(c.weight)}});
    }
    j["clips"] = std::move(clips);
    j["state"] = std::move(state);
    return j;
}

json ApplyStatsJson(const seqhost::ApplyOutput& o)
{
    json j = {{"transformsWritten", o.transformsWritten}, {"fieldsWritten", o.fieldsWritten},
              {"skippedPhysics", o.skippedPhysics}, {"skippedMissing", o.skippedMissing}, {"userDrift", o.userDrift},
              {"postOverrides", o.post.size()}, {"shakes", o.shakes.size()}};
    if (!o.warnings.empty()) j["warnings"] = o.warnings;
    return j;
}
} // namespace

void Application::RegisterMcpSequenceMethods()
{
    using json = nlohmann::json;

    const auto host = [this]() -> seqhost::SequencerHost& {
        if (!m_sequencer)
            throw McpError(McpErr::Internal, "sequencer is not created", "エンジンの起動が終わっていない。dx12_ping が通るまで待ってから呼ぶこと");
        m_sequencer->SetScene(m_scene.get());
        return *m_sequencer;
    };

    // 文書を引く(未読なら読む)。無ければ NotFound + 近い名前
    const auto needDoc = [this, host](const json& params, bool load) -> DocPtr {
        seqhost::SequencerHost& h = host();
        const std::string name = params.value("name", std::string());
        if (name.empty())
            throw McpError(McpErr::InvalidParam, "missing 'name'", "sequence_list で名前を確かめる(assets/sequences/<name>.dxseq)");
        DocPtr d = h.FindDoc(name);
        if (d) return d;
        std::vector<std::string> cand;
        for (const std::string& rel : h.ListAssets()) cand.push_back(seqhost::SequencerHost::KeyFor(rel));
        for (const DocPtr& o : h.OpenDocs()) cand.push_back(o->name);
        if (load)
        {
            std::string err;
            d = h.LoadDoc(name, err);
            if (d) return d;
            ThrowSeq(McpErr::NotFound, "E_NOT_FOUND", "sequence '" + name + "' not found: " + err,
                     "sequence_list で名前を確かめる。新しく作るなら sequence_load {name, create:true}", "このシーケンスは assets/sequences/ にも開いている文書にも無い",
                     McpSuggest(name, cand, 5));
        }
        ThrowSeq(McpErr::NotFound, "E_NOT_FOUND", "sequence '" + name + "' is not loaded", "先に sequence_load {name} で読む(sequence_list で名前を確かめる)",
                 "この名前の文書は開いていない", McpSuggest(name, cand, 5));
    };

    // -------------------------------------------------------------- sequence_list
    {
        McpMeta m;
        m.summary   = "シーケンス(.dxseq)の一覧: assets/sequences/ のファイルと開いている文書(未保存の印つき)。エディタのスクラブ状態と Play 中の再生も返す";
        m.keywords  = "sequence sequencer timeline cutscene シーケンス シーケンサー タイムライン カットシーン 一覧 list dxseq";
        m.category  = "sequence";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 8000;
        m.idempotent = true;
        m.next      = {{"sequence_load", "一覧の名前を読み込む"}, {"sequence_get", "中身を見る"}};
        McpDefine("sequence_list", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                json arr = json::array();
                std::vector<std::string> seen;
                for (const DocPtr& d : h.OpenDocs())
                {
                    json o = DocSummaryJson(d);
                    o["loaded"] = true;
                    arr.push_back(std::move(o));
                    seen.push_back(d->name);
                }
                for (const std::string& rel : h.ListAssets())
                {
                    const std::string key = seqhost::SequencerHost::KeyFor(rel);
                    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                    arr.push_back({{"name", key}, {"rel", rel}, {"loaded", false}, {"onDisk", true}});
                }
                const seqhost::EditorStatus es = h.GetEditorStatus();
                json players = json::array();
                for (const seqhost::PlayerInfo& p : h.Players())
                    players.push_back({{"id", p.id}, {"name", p.name}, {"timeSec", Round(p.timeSec)}, {"durationSec", Round(p.durationSec)},
                                       {"paused", p.paused}, {"loop", p.loop}, {"rate", p.rate}, {"clock", p.clockGame ? "game" : "real"}, {"finished", p.finished}});
                resp["ok"] = true;
                resp["result"] = {{"sequences", std::move(arr)},
                                  {"editor", {{"active", es.active}, {"doc", es.doc}, {"tick", es.t}, {"playing", es.playing}, {"preAnimated", es.preAnimated}}},
                                  {"players", std::move(players)}};
            });
    }

    // -------------------------------------------------------------- sequence_load
    {
        McpMeta m;
        m.summary   = "シーケンス(.dxseq)をメモリへ読む(既に開いていればそのまま返す)。create:true で無ければ空の文書を作る(ファイルは sequence_save で作る)。バインディングの解決状況を返す";
        m.keywords  = "sequence load open create new シーケンス 読み込み 作成 新規 dxseq";
        m.category  = "sequence";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 10000;
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シーケンス名(assets/sequences/<name>.dxseq)または assets 相対パス"),
                       P("create", "bool", false, nullptr, nullptr, nullptr, "false", "true: 無ければ空の文書を作る"),
                       P("fps", "int", false, nullptr, "1", "1000", "30", "create のときのフレームレート(整数 fps。23.976 / 29.97 は 24 / 30 で編集する)"),
                       P("reload", "bool", false, nullptr, nullptr, nullptr, "false", "true: 開いている文書を読み直す(未保存の編集は失う)")};
        m.next      = {{"sequence_apply_op", "トラック・キーを足す"}, {"sequence_scrub", "エディタで時刻を評価して確かめる"}};
        m.examples  = {{"{\"name\":\"Intro\",\"create\":true,\"fps\":30}", "空のシーケンス Intro を作る"}};
        McpDefine("sequence_load", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                const std::string name = params.value("name", std::string());
                DocPtr d = h.FindDoc(name);
                bool created = false;
                std::string err;
                if (d && params.value("reload", false)) d = h.LoadDoc(name, err, /*forceReload=*/true);
                else if (!d)
                {
                    d = h.LoadDoc(name, err);
                    if (!d && params.value("create", false))
                    {
                        d = h.NewDoc(name, params.value("fps", 30), err);
                        created = (d != nullptr);
                    }
                }
                if (!d)
                {
                    std::vector<std::string> cand;
                    for (const std::string& rel : h.ListAssets()) cand.push_back(seqhost::SequencerHost::KeyFor(rel));
                    ThrowSeq(McpErr::NotFound, "E_NOT_FOUND", "sequence '" + name + "': " + err,
                             "sequence_list で名前を確かめる。新しく作るなら create:true", "読み込めなかった", McpSuggest(name, cand, 5));
                }
                json r = DocSummaryJson(d);
                r["created"] = created;
                seqhost::BoundSequence b;
                if (h.DescribeBindings(d, b))
                {
                    r["bindingStatus"] = BindingStatusJson(d->seq, b, &m_scene->GetRegistry());
                    r["issues"] = IssuesJson(b);
                    r["unresolved"] = std::count_if(b.mask.begin(), b.mask.end(), [](std::uint8_t v) { return v == 0; });
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // -------------------------------------------------------------- sequence_save
    {
        McpMeta m;
        m.summary   = "開いているシーケンスを assets/sequences/<name>.dxseq へ保存する(アトミック。正準形 = git 差分が読める)。path で別名保存(以後その名前になる)。dryRun で書く先と大きさだけ返す";
        m.keywords  = "sequence save write シーケンス 保存 dxseq";
        m.category  = "sequence";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 10000;
        m.dryRun    = "preview";
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "開いているシーケンス名"),
                       P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "別名保存の名前(assets/sequences/<path>.dxseq)または assets 相対パス")};
        McpDefine("sequence_save", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                DocPtr d = needDoc(params, /*load=*/false);
                std::string err;
                if (!h.SaveDoc(d, err, params.value("path", std::string())))
                    ThrowSeq(McpErr::FileIo, "E_FILE_IO", "sequence_save: " + err, "書き込み先(assets/sequences/)を確かめる。配布ゲーム(pak)では保存できない", err);
                resp["ok"] = true;
                resp["result"] = {{"saved", true}, {"name", d->name}, {"rel", d->rel}, {"bytes", d->savedText.size()}};
            });
        mcpsafety::PreviewTable()["sequence_save"] = [this, host](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            seqhost::SequencerHost& h = host();
            const std::string name = p.is_object() ? p.value("name", std::string()) : std::string();
            const DocPtr d = h.FindDoc(name);
            if (!d) return json{{"error", "sequence '" + name + "' is not loaded"}}.dump();
            std::string rel = (p.is_object() && p.contains("path") && p["path"].is_string()) ? seqhost::SequencerHost::RelPathFor(p["path"].get<std::string>()) : d->rel;
            if (rel.empty()) rel = seqhost::SequencerHost::RelPathFor(d->name);
            return json{{"wouldWrite", rel}, {"bytes", seq::SerializeSequence(d->seq).size()}, {"overwrites", d->onDisk && rel == d->rel}, {"dirty", d->dirty}}.dump();
        };
    }

    // -------------------------------------------------------------- sequence_get
    {
        McpMeta m;
        m.summary   = "シーケンスの中身: summary = 構成の要約(バインディング・トラック・カット・マーカーと、現在のシーンへの解決状況)/ full = 正準形の全文(dxseq)と JSON";
        m.keywords  = "sequence get show inspect シーケンス 中身 取得 バインディング トラック キー";
        m.category  = "sequence";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 10000;
        m.idempotent = true;
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シーケンス名(未読なら読む)"),
                       P("detail", "enum", false, "summary|full", nullptr, nullptr, "\"summary\"", "summary(既定)= 要約 / full = 全文")};
        m.next      = {{"sequence_eval", "ある時刻の値を(書かずに)読む"}};
        McpDefine("sequence_get", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                DocPtr d = needDoc(params, /*load=*/true);
                json r = DocSummaryJson(d);
                json binds = json::array();
                for (const seq::Binding& b : d->seq.bindings)
                {
                    json bo = {{"id", b.id}, {"name", b.name}, {"kind", seq::BindingKindName(b.kind)}, {"hint", HintJson(b.hint)}};
                    json tr = json::array();
                    for (const seq::Track& t : b.tracks) tr.push_back(TrackSummaryJson(t));
                    bo["tracks"] = std::move(tr);
                    binds.push_back(std::move(bo));
                }
                r["bindingTable"] = std::move(binds);
                json cuts = json::array();
                for (const seq::Cut& c : d->seq.cuts) cuts.push_back({{"id", c.id}, {"start", c.start}, {"end", c.end}, {"camera", c.camera}});
                r["cutTable"] = std::move(cuts);
                json marks = json::array();
                for (const seq::Marker& mk : d->seq.markers) marks.push_back({{"id", mk.id}, {"t", mk.t}, {"name", mk.name}});
                r["markerTable"] = std::move(marks);
                seqhost::BoundSequence b;
                if (h.DescribeBindings(d, b))
                {
                    r["bindingStatus"] = BindingStatusJson(d->seq, b, &m_scene->GetRegistry());
                    r["issues"] = IssuesJson(b);
                }
                if (params.value("detail", std::string("summary")) == "full")
                {
                    const std::string text = seq::SerializeSequence(d->seq);
                    r["dxseq"] = text;
                    r["sequence"] = json::parse(text, nullptr, false);
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // -------------------------------------------------------------- sequence_eval
    {
        McpMeta m;
        m.summary   = "【非破壊】シーケンスを時刻 t で評価して値を返す(何も書かない・guid も確定しない)。カット・チャンネル値・有効クリップ・バインディングごとの状態。撮影前の数値確認用";
        m.keywords  = "sequence eval evaluate sample preview シーケンス 評価 サンプル 値 カット 時刻";
        m.category  = "sequence";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 10000;
        m.idempotent = true;
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シーケンス名(未読なら読む)"),
                       P("t", "number", false, nullptr, nullptr, nullptr, nullptr, "時刻(秒)。tick / frame でも指定できる(優先: tick > frame > t)"),
                       P("tick", "int", false, nullptr, nullptr, nullptr, nullptr, "時刻(ティック。既定 6000/秒)"),
                       P("frame", "int", false, nullptr, nullptr, nullptr, nullptr, "時刻(フレーム番号。シーケンスの fps)")};
        m.next      = {{"sequence_scrub", "エディタ上で適用して screenshot_game_view で見る"}};
        m.examples  = {{"{\"name\":\"Intro\",\"t\":2.5}", "2.5 秒の状態を数値で読む"}};
        McpDefine("sequence_eval", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                DocPtr d = needDoc(params, /*load=*/true);
                const seq::Tick t = TimeFromParams(params, d->seq);
                seqhost::EvalReport rep;
                std::string err;
                if (!h.EvalAt(d, t, rep, err)) throw McpError(McpErr::Internal, "sequence_eval: " + err, "sequence_get でシーケンスの状態を確かめる");
                json r = EvalToJson(d->seq, rep.res);
                r["nonDestructive"] = true;
                if (rep.sceneAvailable)
                {
                    r["unresolved"] = std::count_if(rep.bound.mask.begin(), rep.bound.mask.end(), [](std::uint8_t v) { return v == 0; });
                    r["bindingStatus"] = BindingStatusJson(d->seq, rep.bound, &m_scene->GetRegistry());
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // -------------------------------------------------------------- sequence_scrub
    {
        McpMeta m;
        m.summary   = "エディタ上で時刻 t に適用する(スクラブ)。対象の値は PreAnimatedState が先に退避し、end / Play / シーン保存の直前に必ず元へ戻る(保存にスクラブの値は混ざらない)。Editor モード限定。見た目は screenshot_game_view";
        m.keywords  = "sequence scrub seek preview apply シーケンス スクラブ 時刻 適用 プレビュー カメラワーク";
        m.category  = "sequence";
        m.effect    = McpEffect::Runtime;
        m.mode      = "editor";
        m.timeoutMs = 10000;
        m.dryRun    = "preview";
        m.params    = {P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "シーケンス名(未読なら読む)。end:true のときは不要"),
                       P("t", "number", false, nullptr, nullptr, nullptr, nullptr, "時刻(秒)。tick / frame でも指定できる(優先: tick > frame > t)"),
                       P("tick", "int", false, nullptr, nullptr, nullptr, nullptr, "時刻(ティック)"),
                       P("frame", "int", false, nullptr, nullptr, nullptr, nullptr, "時刻(フレーム番号)"),
                       P("end", "bool", false, nullptr, nullptr, nullptr, "false", "true: スクラブを終えて元の値へ戻す")};
        m.next      = {{"screenshot_game_view", "カットのカメラ視点で撮る"}, {"sequence_scrub", "end:true で元へ戻す"}};
        m.examples  = {{"{\"name\":\"Intro\",\"t\":2.5}", "2.5 秒に適用"}, {"{\"end\":true}", "元の値へ戻す"}};
        McpDefine("sequence_scrub", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                if (m_engineMode == EngineMode::Playing)
                    ThrowSeq(McpErr::ModeConflict, "E_WRONG_MODE", "sequence_scrub works in Editor mode only", "dx12_stop で Editor に戻してから。Play 中に流すなら sequence_play",
                             "Play 中はシーンの値を戻せない(Stop でシーンごと復元される)ので、スクラブは Editor 限定");
                if (params.value("end", false))
                {
                    const bool was = h.EditorActive();
                    const std::size_t n = h.PreAnimatedCount();
                    h.EditorEnd();
                    resp["ok"] = true;
                    resp["result"] = {{"ended", was}, {"restored", n}};
                    return;
                }
                DocPtr d = needDoc(params, /*load=*/true);
                const seq::Tick t = TimeFromParams(params, d->seq);
                std::string err;
                if (!h.EditorScrub(d, t, err)) throw McpError(McpErr::ModeConflict, "sequence_scrub: " + err, "Editor モードで、シーンが開いている状態で呼ぶ(dx12_stop / dx12_open_scene)");
                seqhost::EvalReport rep;
                h.EvalAt(d, t, rep, err);
                json r = EvalToJson(d->seq, rep.res);
                const seqhost::EditorStatus es = h.GetEditorStatus();
                r["applied"] = ApplyStatsJson(h.LastOutput());
                r["editor"] = {{"active", es.active}, {"preAnimated", es.preAnimated}, {"userDrift", es.userDrift}};
                if (h.CutCameraEntity() != entt::null)
                    r["cutCamera"] = EntityRefJson(m_scene->GetRegistry(), h.CutCameraEntity());
                r["bindingStatus"] = BindingStatusJson(d->seq, rep.bound, &m_scene->GetRegistry());
                r["note"] = "エディタ上で適用済み。元へ戻すのは sequence_scrub {end:true} / Play / シーンの保存(自動)。見た目は screenshot_game_view";
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
        mcpsafety::PreviewTable()["sequence_scrub"] = [this, host](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            seqhost::SequencerHost& h = host();
            const std::string name = p.is_object() ? p.value("name", std::string()) : std::string();
            std::string err;
            DocPtr d = h.FindDoc(name);
            if (!d) d = h.LoadDoc(name, err);
            if (!d) return json{{"error", "sequence '" + name + "': " + err}}.dump();
            seqhost::EvalReport rep;
            h.EvalAt(d, TimeFromParams(p, d->seq), rep, err);
            json r = EvalToJson(d->seq, rep.res);
            r["wouldApply"] = true;
            return r.dump();
        };
    }

    // -------------------------------------------------------------- sequence_play
    {
        McpMeta m;
        m.summary   = "シーケンスを再生する。Play 中 = 実時間(タイムスケール非適用)で流す(イベントは前進で 1 回発火)/ Editor 中 = プレビュー再生(値は非破壊・イベントは発火しない)";
        m.keywords  = "sequence play start cutscene シーケンス 再生 開始 カットシーン プレビュー";
        m.category  = "sequence";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 10000;
        m.dryRun    = "preview";
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シーケンス名(未読なら読む)"),
                       P("loop", "enum", false, "once|loop|pingpong", nullptr, nullptr, nullptr, "省略 = シーケンスの meta.loop(無ければ 1 回)"),
                       P("rate", "number", false, nullptr, nullptr, nullptr, "1", "再生速度(負 = 逆再生)"),
                       P("from", "number", false, nullptr, nullptr, nullptr, "0", "開始位置(秒)"),
                       P("clock", "enum", false, "real|game", nullptr, nullptr, "\"real\"", "real = 実時間(既定)/ game = ゲーム時間(タイムスケール適用。timeScale トラックがあると real 固定)"),
                       P("restoreOnEnd", "bool", false, nullptr, nullptr, nullptr, "true", "終了時にタイムスケール・カットの選択を戻す"),
                       P("delay", "number", false, nullptr, nullptr, nullptr, "0", "開始までの待ち(秒)")};
        m.next      = {{"sequence_stop", "止める"}, {"step_frames", "決定論で 1 フレームずつ進めて撮る"}};
        m.examples  = {{"{\"name\":\"Intro\"}", "頭から再生"}, {"{\"name\":\"Intro\",\"loop\":\"loop\",\"rate\":0.5}", "半速でループ"}};
        McpDefine("sequence_play", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                DocPtr d = needDoc(params, /*load=*/true);
                const std::string loop = params.value("loop", std::string());
                const double rate = params.value("rate", 1.0);
                std::string err;
                if (m_engineMode == EngineMode::Playing)
                {
                    SequencePlayOptions o;
                    o.loop = loop.empty() ? -1 : (loop == "loop" ? 1 : loop == "pingpong" ? 2 : 0);
                    o.rate = rate;
                    o.from = params.value("from", 0.0);
                    o.clockGame = params.value("clock", std::string("real")) == "game";
                    o.restoreOnEnd = params.value("restoreOnEnd", true);
                    o.startDelay = params.value("delay", 0.0);
                    const int id = h.PlayRuntime(d, o, err);
                    if (id <= 0) throw McpError(McpErr::Internal, "sequence_play: " + err, "sequence_get でシーケンスの状態を確かめる");
                    resp["ok"] = true;
                    resp["result"] = {{"mode", "Playing"}, {"id", id}, {"name", d->name}, {"clock", o.clockGame ? "game" : "real"}};
                    return;
                }
                if (params.contains("from"))
                {
                    if (!h.EditorScrub(d, seq::SecondsToTicks(params.value("from", 0.0), d->seq.ticksPerSecond), err))
                        throw McpError(McpErr::ModeConflict, "sequence_play: " + err, "Editor モードで、シーンが開いている状態で呼ぶ(Play 中の再生は dx12_play の後に sequence_play)");
                }
                if (!h.EditorPlay(d, loop == "loop" || loop == "pingpong", rate, err)) throw McpError(McpErr::ModeConflict, "sequence_play: " + err, "Editor モードで、シーンが開いている状態で呼ぶ(Play 中の再生は dx12_play の後に sequence_play)");
                resp["ok"] = true;
                resp["result"] = {{"mode", "Editor"}, {"preview", true}, {"name", d->name},
                                  {"note", "エディタのプレビュー再生(イベントは発火しない・値は非破壊)。止めるのは sequence_stop。Play 中の再生は dx12_play してから sequence_play"}};
            });
        mcpsafety::PreviewTable()["sequence_play"] = [this, host](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            seqhost::SequencerHost& h = host();
            const std::string name = p.is_object() ? p.value("name", std::string()) : std::string();
            std::string err;
            DocPtr d = h.FindDoc(name);
            if (!d) d = h.LoadDoc(name, err);
            if (!d) return json{{"error", "sequence '" + name + "': " + err}}.dump();
            seq::Tick a = 0, b = 0;
            seq::GetRange(d->seq, a, b);
            return json{{"wouldPlay", d->name}, {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor(プレビュー)"},
                        {"durationSec", Round(seq::TicksToSeconds(b - a, d->seq.ticksPerSecond))}}.dump();
        };
    }

    // -------------------------------------------------------------- sequence_stop
    {
        McpMeta m;
        m.summary   = "シーケンスの再生を止める。Play 中 = 名前(省略で全部)の再生を止める(値は戻さない。Stop でシーン復元)/ Editor 中 = プレビューを止めて元の値へ戻す";
        m.keywords  = "sequence stop pause シーケンス 停止 止める";
        m.category  = "sequence";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 8000;
        m.params    = {P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "シーケンス名(省略 = 全部)"),
                       P("restore", "bool", false, nullptr, nullptr, nullptr, "true", "Editor のとき: 元の値へ戻す(false でプレビュー位置に留める)")};
        McpDefine("sequence_stop", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                const std::string name = params.value("name", std::string("*"));
                if (m_engineMode == EngineMode::Playing)
                {
                    const bool any = h.StopRuntime(name);
                    resp["ok"] = true;
                    resp["result"] = {{"mode", "Playing"}, {"stopped", any}};
                    return;
                }
                const bool was = h.EditorActive();
                h.EditorPause();
                if (params.value("restore", true)) h.EditorEnd();
                resp["ok"] = true;
                resp["result"] = {{"mode", "Editor"}, {"stopped", was}, {"restored", params.value("restore", true)}};
            });
    }

    // -------------------------------------------------------------- sequence_apply_op
    {
        McpMeta m;
        m.summary   = "シーケンスを宣言的に編集する(SeqOp の JSON。1 回の呼び出し = Undo 1 ステップ)。バインディング/トラック/キー/クリップ/イベント/カット/マーカーの追加・変更・削除。ID は省略可。addBinding は entity:\"名前\" で対象を指せる(guid を自動確定)。dryRun で検査だけ";
        m.keywords  = "sequence edit op key keyframe track binding cut marker シーケンス 編集 キー キーフレーム トラック バインディング カット マーカー 追加 undo";
        m.category  = "sequence";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 15000;
        m.dryRun    = "preview";
        m.params    = {P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "シーケンス名(未読なら読む)"),
                       P("ops", "array", false, nullptr, nullptr, nullptr, nullptr, "op の配列。各要素 {\"op\":\"addKey\", …}。名前・フィールドは docs/DXSEQ_FORMAT.md §18(op 名は大文字小文字 / _ 無視)"),
                       P("label", "string", false, nullptr, nullptr, nullptr, nullptr, "Undo の表示名"),
                       P("undo", "bool", false, nullptr, nullptr, nullptr, "false", "true: 直前の編集を取り消す(ops は不要)"),
                       P("redo", "bool", false, nullptr, nullptr, nullptr, "false", "true: 取り消した編集をやり直す")};
        m.next      = {{"sequence_eval", "編集の結果を非破壊で確かめる"}, {"sequence_save", "ファイルへ保存"}};
        m.examples  = {
            {"{\"name\":\"Intro\",\"ops\":[{\"op\":\"addBinding\",\"binding\":{\"id\":\"b_cam\",\"entity\":\"Camera\","
             "\"tracks\":[{\"id\":\"t_tr\",\"type\":\"transform\",\"channels\":{\"position.x\":{\"keys\":[[0,0,\"a\"],[30000,10,\"a\"]]}}}]}}]}", "Camera を対象にバインディングと位置トラックを足す(時刻はティック。6000/秒)"},
            {"{\"name\":\"Intro\",\"ops\":[{\"op\":\"addKey\",\"trackId\":\"t_tr\",\"channel\":\"position.x\",\"key\":{\"sec\":8,\"v\":4,\"ip\":\"e:outQuad\"}}]}", "キーを秒指定で足す"}};
        McpDefine("sequence_apply_op", McpMeta(m), SEQ_HANDLER
            {
                seqhost::SequencerHost& h = host();
                DocPtr d = needDoc(params, /*load=*/true);
                seqhost::EditResult er;
                if (params.value("undo", false)) er = h.Undo(d);
                else if (params.value("redo", false)) er = h.Redo(d);
                else
                {
                    if (!params.contains("ops") || !params["ops"].is_array() || params["ops"].empty())
                        ThrowSeq(McpErr::InvalidParam, "E_MISSING_PARAM", "sequence_apply_op: 'ops' (non-empty array) is required", "例: ops:[{\"op\":\"addKey\",…}](op 名の一覧は docs/DXSEQ_FORMAT.md §18)", "ops が無い");
                    json body = {{"ops", params["ops"]}};
                    if (params.contains("label")) body["label"] = params["label"];
                    er = h.ApplyOps(d, body.dump(), /*dryRun=*/false);
                }
                if (!er.ok)
                {
                    std::vector<std::string> ops;
                    for (const std::string& n : seq::OpJsonNames()) ops.push_back(n);
                    ThrowSeq(McpErr::InvalidParam, "E_INVALID_OP", "sequence_apply_op: " + er.error,
                             "エラー文の位置(ops[i])を直して撃ち直す。何も変更していない(全部成功か全部巻き戻し)", "op を適用できなかった。シーケンスは変更されていない");
                }
                h.NotifyDocChanged(d);
                json r = DocSummaryJson(d);
                r["applied"] = er.applied;
                r["label"] = er.label;
                r["ops"] = er.opNames;
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
        mcpsafety::PreviewTable()["sequence_apply_op"] = [this, host](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            seqhost::SequencerHost& h = host();
            const std::string name = p.is_object() ? p.value("name", std::string()) : std::string();
            std::string err;
            DocPtr d = h.FindDoc(name);
            if (!d) d = h.LoadDoc(name, err);
            if (!d) return json{{"ok", false}, {"error", "sequence '" + name + "': " + err}}.dump();
            if (!p.contains("ops") || !p["ops"].is_array()) return json{{"ok", false}, {"error", "ops (array) is required"}}.dump();
            json body = {{"ops", p["ops"]}};
            const seqhost::EditResult er = h.ApplyOps(d, body.dump(), /*dryRun=*/true);
            json r = {{"ok", er.ok}, {"wouldApply", er.applied}, {"ops", er.opNames}};
            if (!er.ok) r["error"] = er.error;
            return r.dump();
        };
    }

    // -------------------------------------------------------------- sequence_autoplay
    {
        McpMeta m;
        m.summary   = "シーンの自動再生設定(Play 開始時に再生するシーケンス。シーン JSON の sequencePlayers に保存される)。list / set(全置換)/ add / remove";
        m.keywords  = "sequence autoplay auto play scene シーケンス 自動再生 シーン Play開始 オープニング";
        m.category  = "sequence";
        m.effect    = McpEffect::WriteScene;
        m.mode      = "editor";
        m.timeoutMs = 8000;
        m.params    = {P("op", "enum", false, "list|set|add|remove|clear", nullptr, nullptr, "\"list\"", "操作(既定 list)"),
                       P("sequence", "string", false, nullptr, nullptr, nullptr, nullptr, "add / remove のシーケンス名"),
                       P("loop", "bool", false, nullptr, nullptr, nullptr, "false", "add: ループ"),
                       P("rate", "number", false, nullptr, nullptr, nullptr, "1", "add: 再生速度"),
                       P("startDelay", "number", false, nullptr, nullptr, nullptr, "0", "add: Play 開始からの待ち(秒)"),
                       P("clock", "enum", false, "real|game", nullptr, nullptr, "\"real\"", "add: 時計(既定 real = 実時間)"),
                       P("players", "array", false, nullptr, nullptr, nullptr, nullptr, "set: [{sequence, loop?, rate?, startDelay?, clock?}, …] で全置換")};
        McpDefine("sequence_autoplay", McpMeta(m), SEQ_HANDLER
            {
                host();
                if (!m_scene) throw McpError(McpErr::Internal, "no scene", "プロジェクトを開いてから呼ぶ");
                auto& list = m_scene->GetSequenceAutoPlay();
                const std::string op = params.value("op", std::string("list"));
                const auto fromJson = [](const json& o) {
                    SequenceAutoPlay sp;
                    sp.sequence = o.value("sequence", std::string());
                    sp.loop = o.value("loop", false);
                    sp.rate = o.value("rate", 1.0f);
                    sp.startDelay = o.value("startDelay", 0.0f);
                    sp.clockGame = o.value("clock", std::string("real")) == "game";
                    return sp;
                };
                bool changed = false;
                if (op == "set")
                {
                    list.clear();
                    if (params.contains("players") && params["players"].is_array())
                        for (const json& o : params["players"]) if (o.is_object()) list.push_back(fromJson(o));
                    changed = true;
                }
                else if (op == "add")
                {
                    SequenceAutoPlay sp = fromJson(params);
                    if (sp.sequence.empty()) throw McpError(McpErr::InvalidParam, "sequence_autoplay add: 'sequence' is required", "sequence_list で名前を確かめる");
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const SequenceAutoPlay& x) { return x.sequence == sp.sequence; }), list.end());
                    list.push_back(std::move(sp));
                    changed = true;
                }
                else if (op == "remove")
                {
                    const std::string n = params.value("sequence", std::string());
                    const std::size_t before = list.size();
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const SequenceAutoPlay& x) { return x.sequence == n; }), list.end());
                    changed = list.size() != before;
                }
                else if (op == "clear") { changed = !list.empty(); list.clear(); }
                if (changed && m_editorCtx) m_editorCtx->undoSystem.MarkEdited();
                json arr = json::array();
                for (const SequenceAutoPlay& sp : list)
                    arr.push_back({{"sequence", sp.sequence}, {"loop", sp.loop}, {"rate", sp.rate}, {"startDelay", sp.startDelay}, {"clock", sp.clockGame ? "game" : "real"}});
                resp["ok"] = true;
                resp["result"] = {{"players", std::move(arr)}, {"changed", changed},
                                  {"note", "シーン JSON の sequencePlayers に保存される(save_scene)。Play 開始時に再生される"}};
            });
    }
}

} // namespace dx12e
