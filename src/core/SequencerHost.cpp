#include "core/SequencerHost.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/SequencerBinding.h"
#include "core/vfs/Vfs.h"
#include "renderer/PostProcessSettings.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

namespace dx12e
{
namespace seqhost
{

namespace fs = std::filesystem;

namespace
{
std::string Norm(std::string_view s)
{
    std::string o;
    for (const char c : s)
    {
        if (c == '_' || c == '-' || c == ' ') continue;
        o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return o;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void HookThunk(void* ctx, const entt::registry& reg)
{
    static_cast<SequencerHost*>(ctx)->RestoreForSave(reg);
}
} // namespace

SequencerHost::SequencerHost() = default;

SequencerHost::~SequencerHost()
{
    UninstallSaveHook();
}

void SequencerHost::SetAssetsDirOverride(std::string dir)
{
    for (char& c : dir) if (c == '\\') c = '/';
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    m_assetsOverride = std::move(dir);
}

void SequencerHost::InstallSaveHook()
{
    SceneSerializer::SetPreSerializeHook(&HookThunk, this);
    m_hookInstalled = true;
}

void SequencerHost::UninstallSaveHook()
{
    if (!m_hookInstalled) return;
    SceneSerializer::SetPreSerializeHook(nullptr, nullptr);
    m_hookInstalled = false;
}

std::string SequencerHost::AssetsDir() const
{
    return m_assetsOverride.empty() ? PathResolver::AssetsDir() : m_assetsOverride;
}

// ===========================================================================
// 文書
// ===========================================================================
std::string SequencerHost::RelPathFor(const std::string& nameOrRel)
{
    std::string s = nameOrRel;
    for (char& c : s) if (c == '\\') c = '/';
    if (s.rfind("assets/", 0) == 0) s = s.substr(7);
    if (EndsWith(s, ".dxseq")) return s;
    return "sequences/" + s + ".dxseq";
}

std::string SequencerHost::KeyFor(const std::string& nameOrRel)
{
    std::string rel = RelPathFor(nameOrRel);
    if (rel.rfind("sequences/", 0) == 0) rel = rel.substr(10);
    if (EndsWith(rel, ".dxseq")) rel.resize(rel.size() - 6);
    return rel;
}

DocPtr SequencerHost::FindDoc(const std::string& nameOrRel) const
{
    const std::string key = KeyFor(nameOrRel);
    for (const DocPtr& d : m_docs)
        if (d->name == key) return d;
    return nullptr;
}

std::string SequencerHost::ReadAssetText(const std::string& rel, bool& found) const
{
    found = false;
    if (!m_assetsOverride.empty())
    {
        std::ifstream f(m_assetsOverride + rel, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        found = true;
        return ss.str();
    }
    const std::vector<std::uint8_t> bytes = vfs::ReadAsset(rel);
    if (bytes.empty()) return {};
    found = true;
    return std::string(bytes.begin(), bytes.end());
}

DocPtr SequencerHost::MakeDoc(const std::string& name, const std::string& rel, seq::Sequence&& s, bool onDisk)
{
    auto d = std::make_shared<SeqDoc>();
    d->name = name;
    d->rel = rel;
    d->seq = std::move(s);
    d->ids = seq::IdAllocator(seq::IdAllocator::RandomSeed());
    seq::ReserveAllIds(d->seq, d->ids);
    d->onDisk = onDisk;
    d->savedText = seq::SerializeSequence(d->seq);
    d->dirty = false;
    return d;
}

DocPtr SequencerHost::LoadDoc(const std::string& nameOrRel, std::string& err, bool forceReload)
{
    const std::string key = KeyFor(nameOrRel);
    DocPtr existing = FindDoc(nameOrRel);
    if (existing && !forceReload) return existing;

    const std::string rel = RelPathFor(nameOrRel);
    bool found = false;
    const std::string text = ReadAssetText(rel, found);
    if (!found)
    {
        err = "シーケンスが見つからない: " + rel + "(assets/sequences/<名前>.dxseq)";
        return nullptr;
    }
    seq::Sequence s;
    const seq::ParseResult pr = seq::ParseSequence(text, s);
    if (!pr)
    {
        err = rel + ": " + pr.error;
        return nullptr;
    }
    for (const std::string& w : pr.warnings) Logger::Warn("シーケンス {}: {}", rel, w);
    if (existing)
    {
        // 同じ文書を読み直す(共有ポインタは保つ = 編集セッションの参照が切れない)
        existing->seq = std::move(s);
        existing->history.Clear();
        existing->ids = seq::IdAllocator(seq::IdAllocator::RandomSeed());
        seq::ReserveAllIds(existing->seq, existing->ids);
        existing->onDisk = true;
        existing->savedText = seq::SerializeSequence(existing->seq);
        existing->dirty = false;
        ++existing->revision;
        return existing;
    }
    DocPtr d = MakeDoc(key, rel, std::move(s), true);
    m_docs.push_back(d);
    return d;
}

DocPtr SequencerHost::NewDoc(const std::string& name, int fps, std::string& err)
{
    if (name.empty()) { err = "name が空"; return nullptr; }
    if (FindDoc(name)) { err = "同じ名前のシーケンスが既に開いている: " + name; return nullptr; }
    seq::Sequence s;
    s.name = KeyFor(name);
    s.frameRate = std::clamp(fps, 1, 1000);
    s.meta = seq::SeqValue::MakeObject();
    DocPtr d = MakeDoc(KeyFor(name), RelPathFor(name), std::move(s), false);
    d->dirty = true;   // ファイルがまだ無い
    m_docs.push_back(d);
    return d;
}

DocPtr SequencerHost::AddDocFromText(const std::string& name, const std::string& text, std::string& err)
{
    if (FindDoc(name)) { err = "同じ名前のシーケンスが既に開いている: " + name; return nullptr; }
    seq::Sequence s;
    const seq::ParseResult pr = seq::ParseSequence(text, s);
    if (!pr) { err = pr.error; return nullptr; }
    DocPtr d = MakeDoc(KeyFor(name), RelPathFor(name), std::move(s), false);
    m_docs.push_back(d);
    return d;
}

bool SequencerHost::SaveDoc(const DocPtr& doc, std::string& err, const std::string& relOverride)
{
    if (!doc) { err = "文書が無い"; return false; }
    if (m_assetsOverride.empty() && vfs::InGameMode()) { err = "配布ゲーム(pak)の中のシーケンスは保存できない"; return false; }
    std::string rel = relOverride.empty() ? doc->rel : RelPathFor(relOverride);
    if (rel.empty()) rel = RelPathFor(doc->name);
    const std::string full = AssetsDir() + rel;
    std::error_code ec;
    fs::create_directories(fs::path(full).parent_path(), ec);
    std::string e2;
    if (!seq::SaveSequenceFile(full, doc->seq, &e2)) { err = e2; return false; }
    if (rel != doc->rel)
    {
        doc->rel = rel;
        doc->name = KeyFor(rel);
    }
    doc->onDisk = true;
    doc->savedText = seq::SerializeSequence(doc->seq);
    doc->dirty = false;
    return true;
}

bool SequencerHost::CloseDoc(const std::string& nameOrRel, std::string& err)
{
    const DocPtr d = FindDoc(nameOrRel);
    if (!d) { err = "開いていないシーケンス: " + nameOrRel; return false; }
    if (m_ed.active && m_ed.doc == d) EditorEnd();   // (b) シーケンスを閉じる → 元へ戻す
    for (std::size_t i = m_players.size(); i-- > 0;)
        if (m_players[i]->name == d->name) m_players.erase(m_players.begin() + static_cast<std::ptrdiff_t>(i));
    m_docs.erase(std::remove(m_docs.begin(), m_docs.end(), d), m_docs.end());
    RebuildOutputs();
    return true;
}

std::vector<DocPtr> SequencerHost::OpenDocs() const { return m_docs; }

std::vector<std::string> SequencerHost::ListAssets() const
{
    std::vector<std::string> out;
    if (m_assetsOverride.empty() && vfs::InGameMode()) return out;
    const std::string base = AssetsDir();
    std::error_code ec;
    const fs::path dir = fs::path(base) / "sequences";
    if (!fs::exists(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".dxseq") continue;
        std::string rel = fs::relative(it->path(), fs::path(base), ec).generic_string();
        out.push_back(std::move(rel));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ===========================================================================
// 編集
// ===========================================================================
void SequencerHost::TouchDoc(const DocPtr& doc)
{
    ++doc->revision;
    doc->dirty = (seq::SerializeSequence(doc->seq) != doc->savedText);
}

void SequencerHost::NotifyDocChanged(const DocPtr& doc)
{
    if (doc) TouchDoc(doc);
}

bool SequencerHost::PreprocessOpsJson(const DocPtr& doc, std::string& text, std::string& err)
{
    using J = nlohmann::ordered_json;
    J j = J::parse(text, nullptr, false);
    if (j.is_discarded()) return true;   // JSON として読めない → 本物のパーサに任せて位置つきのエラーを出させる
    J* arr = nullptr;
    if (j.is_array()) arr = &j;
    else if (j.is_object() && j.contains("ops") && j["ops"].is_array()) arr = &j["ops"];
    if (!arr) return true;

    bool changed = false;
    std::map<std::string, std::string> addedNames;   // 同じ ops の中で先に addBinding した「名前 → id」(id を明示したものだけ)
    for (J& op : *arr)
    {
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) continue;
        const std::string name = Norm(op["op"].get<std::string>());

        // addBinding の {"entity": "名前" / エンティティ ID / {"guid": "…"}} → hint(guid / path / name)を現在のシーンから作る
        if (name == "addbinding" && op.contains("binding") && op["binding"].is_object() && op["binding"].contains("entity"))
        {
            J& b = op["binding"];
            if (!m_scene) { err = "entity 指定でバインディングを作るにはシーンが要る"; return false; }
            entt::registry& reg = m_scene->GetRegistry();
            entt::entity e = entt::null;
            const J& spec = b["entity"];
            std::string label;
            if (spec.is_number_integer())
            {
                const auto id = static_cast<entt::entity>(spec.get<std::uint32_t>());
                if (reg.valid(id)) e = id;
                label = std::to_string(spec.get<std::uint32_t>());
            }
            else if (spec.is_string())
            {
                label = spec.get<std::string>();
                for (auto [ent, tag] : reg.view<const NameTag>().each())
                    if (tag.name == label) { e = ent; break; }   // Scene::FindEntity と同じ(最後に作られたものが先頭)
            }
            else if (spec.is_object() && spec.contains("guid") && spec["guid"].is_string())
            {
                label = spec["guid"].get<std::string>();
                e = FindEntityByGuid(reg, ParseEntityGuidHex(label));
            }
            if (e == entt::null)
            {
                err = "ops: addBinding の entity \"" + label + "\" がシーンに見つからない(名前は完全一致・エンティティ ID は今のシーンのもの)";
                return false;
            }
            const seq::BindingHint h = seqhost::MakeBindingHint(reg, e, true);
            b.erase("entity");
            if (!b.contains("hint"))
            {
                J hj = J::object();
                if (!h.guid.empty()) hj["guid"] = h.guid;
                if (!h.path.empty()) hj["path"] = h.path;
                if (!h.name.empty()) hj["name"] = h.name;
                b["hint"] = std::move(hj);
            }
            if (!b.contains("name")) b["name"] = h.name;
            if (!b.contains("kind")) b["kind"] = "entity";
            changed = true;
        }
        if (name == "addbinding" && op.contains("binding") && op["binding"].is_object())
        {
            const J& b = op["binding"];
            if (b.contains("id") && b["id"].is_string() && b.contains("name") && b["name"].is_string())
                addedNames[b["name"].get<std::string>()] = b["id"].get<std::string>();
        }

        // カットの camera にバインディングの「名前」を書いたら id に直す(AI が id を知らなくてよい)
        if ((name == "addcut" || name == "setcut") && op.contains("cut") && op["cut"].is_object() &&
            op["cut"].contains("camera") && op["cut"]["camera"].is_string())
        {
            const std::string cam = op["cut"]["camera"].get<std::string>();
            if (const auto ai = addedNames.find(cam); ai != addedNames.end() && seq::FindBinding(doc->seq, cam) < 0)
            {
                op["cut"]["camera"] = ai->second;
                changed = true;
            }
            else if (seq::FindBinding(doc->seq, cam) < 0)
            {
                int hit = -1, n = 0;
                for (std::size_t i = 0; i < doc->seq.bindings.size(); ++i)
                    if (doc->seq.bindings[i].name == cam) { hit = static_cast<int>(i); ++n; }
                if (n == 1) { op["cut"]["camera"] = doc->seq.bindings[static_cast<std::size_t>(hit)].id; changed = true; }
            }
        }
    }
    if (changed) text = j.dump();
    return true;
}

EditResult SequencerHost::ApplyOps(const DocPtr& doc, std::string_view opsJson, bool dryRun)
{
    EditResult r;
    r.dryRun = dryRun;
    if (!doc) { r.error = "文書が無い"; return r; }
    std::string text(opsJson);
    std::string err;
    if (!PreprocessOpsJson(doc, text, err)) { r.error = err; return r; }

    std::vector<std::string> issued;
    const seq::IdIssuer ids = [&](char prefix) {
        std::string id = doc->ids.New(prefix);
        issued.push_back(id);
        return id;
    };
    seq::SeqTxn txn;
    const seq::OpParseResult pr = seq::ParseTxnJson(text, doc->seq, ids, txn);
    const auto releaseIds = [&]() { for (const std::string& id : issued) doc->ids.Release(id); };
    if (!pr) { releaseIds(); r.error = pr.error; return r; }
    if (txn.label.empty()) txn.label = "sequence_apply_op";
    r.label = txn.label;
    for (const seq::SeqOp& op : txn.ops) r.opNames.push_back(seq::OpName(op));

    if (dryRun)
    {
        seq::Sequence tmp = doc->seq;
        const seq::ApplyResult ar = seq::ApplyTxn(tmp, txn);
        releaseIds();
        if (!ar) { r.error = ar.error; return r; }
        r.ok = true;
        r.applied = static_cast<int>(txn.ops.size());
        return r;
    }
    const seq::ApplyResult ar = doc->history.Execute(doc->seq, txn);
    if (!ar) { releaseIds(); r.error = ar.error; return r; }
    TouchDoc(doc);
    r.ok = true;
    r.applied = static_cast<int>(txn.ops.size());
    return r;
}

EditResult SequencerHost::ApplyTxn(const DocPtr& doc, seq::SeqTxn txn)
{
    EditResult r;
    if (!doc) { r.error = "文書が無い"; return r; }
    r.label = txn.label;
    for (const seq::SeqOp& op : txn.ops) r.opNames.push_back(seq::OpName(op));
    const seq::ApplyResult ar = doc->history.Execute(doc->seq, std::move(txn));
    if (!ar) { r.error = ar.error; return r; }
    TouchDoc(doc);
    r.ok = true;
    r.applied = static_cast<int>(r.opNames.size());
    return r;
}

EditResult SequencerHost::Undo(const DocPtr& doc)
{
    EditResult r;
    if (!doc) { r.error = "文書が無い"; return r; }
    const seq::ApplyResult ar = doc->history.Undo(doc->seq);
    if (!ar) { r.error = ar.error; return r; }
    TouchDoc(doc);
    r.ok = true;
    return r;
}

EditResult SequencerHost::Redo(const DocPtr& doc)
{
    EditResult r;
    if (!doc) { r.error = "文書が無い"; return r; }
    const seq::ApplyResult ar = doc->history.Redo(doc->seq);
    if (!ar) { r.error = ar.error; return r; }
    TouchDoc(doc);
    r.ok = true;
    return r;
}

// ===========================================================================
// エディタ: 非破壊のスクラブ
// ===========================================================================
namespace
{
bool NeedRebind(const BoundSequence& b, bool boundOk, const entt::registry& reg, bool force, std::uint64_t frame, bool checkCount)
{
    if (!boundOk || force) return true;
    if (!BoundIsFresh(b, reg, checkCount)) return true;
    if (!b.set.AllResolved() && frame % 30 == 0) return true;   // 未解決がある間は時々やり直す(あとから作られたエンティティを拾う)
    return false;
}
} // namespace

void SequencerHost::ApplyEditorNow()
{
    if (!m_ed.active || !m_ed.doc || !m_scene || m_inApply) return;
    m_inApply = true;
    entt::registry& reg = m_scene->GetRegistry();
    const seq::Sequence& s = m_ed.doc->seq;

    const bool force = m_bindingsDirty || m_ed.boundRevision != m_ed.doc->revision || m_ed.boundToken != m_token;
    if (NeedRebind(m_ed.bound, m_ed.bound_ok, reg, force, m_frame, /*checkCount=*/true))
    {
        BindOptions bo;
        bo.ensureGuids = true;   // ★エディタは参照するエンティティへ guid を確定させる(元値の退避と Undo での作り直しに耐える)
        BindSequence(s, reg, bo, m_ed.bound);
        m_ed.bound_ok = true;
        m_ed.boundRevision = m_ed.doc->revision;
        m_ed.boundToken = m_token;
        m_bindingsDirty = false;
    }

    seq::EvalFilter f;
    f.bindingEnabled = m_ed.bound.mask;
    seq::Evaluate(s, m_ed.t, m_ed.res, &f);

    ApplyOptions opt;
    opt.pre = &m_pre;
    opt.physicsActive = false;
    m_ed.out.Clear();
    ApplySequence(s, m_ed.res, m_ed.bound, reg, opt, m_ed.out);
    m_ed.userDrift = m_ed.out.userDrift;
    m_ed.needsApply = false;
    ++m_applyCount;
    m_inApply = false;
    RebuildOutputs();
}

bool SequencerHost::EditorScrub(const DocPtr& doc, seq::Tick t, std::string& err)
{
    if (!doc) { err = "文書が無い"; return false; }
    if (!m_scene) { err = "シーンが無い(プロジェクトを開いていない)"; return false; }
    if (m_prevMode == HostMode::Playing) { err = "Play 中は sequence_scrub できない(Editor モードで。Play 中の再生は sequence_play)"; return false; }
    if (m_ed.active && m_ed.doc != doc) EditorEnd();
    if (!m_ed.active)
    {
        m_ed = EditorSession{};
        m_ed.doc = doc;
        m_ed.active = true;
    }
    m_ed.playing = false;
    m_ed.t = t;
    m_ed.needsApply = true;
    ApplyEditorNow();
    return true;
}

bool SequencerHost::EditorPlay(const DocPtr& doc, bool loop, double rate, std::string& err)
{
    if (!doc) { err = "文書が無い"; return false; }
    if (!m_scene) { err = "シーンが無い(プロジェクトを開いていない)"; return false; }
    if (m_prevMode == HostMode::Playing) { err = "Play 中の再生は PlayRuntime"; return false; }
    if (m_ed.active && m_ed.doc != doc) EditorEnd();
    if (!m_ed.active)
    {
        m_ed = EditorSession{};
        m_ed.doc = doc;
        m_ed.active = true;
        m_ed.t = 0;
    }
    m_ed.pb.Reset(doc->seq, loop ? seq::LoopMode::Loop : seq::LoopMode::Once);
    seq::Tick start = m_ed.t;
    if (!loop && start >= m_ed.pb.RangeEnd()) start = m_ed.pb.RangeStart();
    m_ed.pb.Seek(start);
    m_ed.pb.SetRate(rate);
    m_ed.pb.Play();
    m_ed.t = m_ed.pb.Position();
    m_ed.playing = true;
    m_ed.needsApply = true;
    ApplyEditorNow();
    return true;
}

void SequencerHost::EditorPause()
{
    m_ed.playing = false;
    m_ed.pb.Pause();
}

void SequencerHost::EditorEnd()
{
    if (!m_ed.active && m_pre.Empty()) return;
    if (m_scene) m_pre.RestoreAll(m_scene->GetRegistry());
    else m_pre.Clear();
    m_ed = EditorSession{};
    RebuildOutputs();
}

EditorStatus SequencerHost::GetEditorStatus() const
{
    EditorStatus s;
    s.active = m_ed.active;
    s.doc = m_ed.doc ? m_ed.doc->name : std::string();
    s.t = m_ed.t;
    s.playing = m_ed.playing;
    s.preAnimated = m_pre.Count();
    s.userDrift = m_ed.userDrift;
    s.warnings = m_ed.bound.warnings;
    for (const std::string& w : m_ed.out.warnings) s.warnings.push_back(w);
    s.cutActive = m_ed.out.cutCamera != entt::null;
    s.cutCamera = m_ed.out.cutCamera;
    return s;
}

void SequencerHost::RestoreForSave(const entt::registry& reg)
{
    if (m_pre.Empty() || !m_scene) return;
    if (&m_scene->GetRegistry() != &reg) return;   // 担当外のシーン(別の Scene インスタンス)は触らない
    m_pre.RestoreAll(const_cast<entt::registry&>(reg), /*keepMissing=*/true);   // 消えたエンティティの元値は持ち続ける(Undo で作り直されたら戻せる)
    if (m_ed.active) m_ed.needsApply = true;       // 保存後に(フレーム末で)再適用する
}

void SequencerHost::PostUpdate()
{
    if (m_ed.active && m_ed.needsApply && m_prevMode == HostMode::Editor) ApplyEditorNow();
}

// ===========================================================================
// 非破壊の評価
// ===========================================================================
bool SequencerHost::EvalAt(const DocPtr& doc, seq::Tick t, EvalReport& out, std::string& err) const
{
    if (!doc) { err = "文書が無い"; return false; }
    out = EvalReport{};
    out.t = t;
    out.sceneAvailable = (m_scene != nullptr);
    seq::EvalFilter f;
    if (m_scene)
    {
        BindOptions bo;   // 書かない: guid も確定しない
        BindSequence(doc->seq, m_scene->GetRegistry(), bo, out.bound);
        f.bindingEnabled = out.bound.mask;
    }
    seq::Evaluate(doc->seq, t, out.res, m_scene ? &f : nullptr);
    return true;
}

bool SequencerHost::DescribeBindings(const DocPtr& doc, BoundSequence& out) const
{
    if (!doc || !m_scene) return false;
    BindOptions bo;
    BindSequence(doc->seq, m_scene->GetRegistry(), bo, out);
    return true;
}

// ===========================================================================
// Play 中の再生
// ===========================================================================
int SequencerHost::PlayerIndexByName(const std::string& name) const
{
    for (std::size_t i = 0; i < m_players.size(); ++i)
        if (m_players[i]->name == name && !m_players[i]->finished) return static_cast<int>(i);
    return -1;
}

int SequencerHost::PlayRuntime(const DocPtr& doc, const SequencePlayOptions& opt, std::string& err)
{
    if (!doc) { err = "文書が無い"; return 0; }
    // 同じシーケンスが再生中なら入れ替える(頭から)
    for (std::size_t i = m_players.size(); i-- > 0;)
        if (m_players[i]->name == doc->name) m_players.erase(m_players.begin() + static_cast<std::ptrdiff_t>(i));

    auto p = std::make_unique<RuntimePlayer>();
    p->id = m_nextPlayerId++;
    p->name = doc->name;
    p->seq = std::make_shared<const seq::Sequence>(doc->seq);
    const seq::Sequence& s = *p->seq;

    bool loop = false;
    if (opt.loop >= 0) loop = opt.loop >= 1;
    else
    {
        if (const seq::SeqValue* v = s.meta.Find("loop"); v && v->IsBool()) loop = v->b;
        else if (const seq::SeqValue* lg = s.meta.Find("legacy"); lg && lg->IsObject()) loop = lg->GetBool("loop", false);
    }
    p->mode = opt.loop == 2 ? seq::LoopMode::PingPong : (loop ? seq::LoopMode::Loop : seq::LoopMode::Once);
    p->rate = opt.rate;
    p->restoreOnEnd = opt.restoreOnEnd;
    p->delayLeft = std::max(0.0, opt.startDelay);
    // timeScale トラックがあるシーケンスの時計は強制的に実時間(自分が書くスケールで自分の時計が変わる自己参照を避ける。設計書 §3.5)
    bool hasTimeScale = false;
    for (const seq::Binding& b : s.bindings)
        for (const seq::Track& t : b.tracks)
            if (t.type == seq::TrackType::TimeScale) hasTimeScale = true;
    p->clockGame = opt.clockGame && !hasTimeScale;

    p->pb.Reset(s, p->mode);
    p->pb.SetRate(opt.rate);
    p->pb.SetFireBackward(true);
    if (opt.from != 0.0) p->pb.Seek(seq::SecondsToTicks(opt.from, s.ticksPerSecond));
    p->pb.Play();
    const int id = p->id;
    m_players.push_back(std::move(p));
    return id;
}

bool SequencerHost::StopRuntime(const std::string& nameOrId)
{
    bool any = false;
    const bool all = nameOrId == "*";
    int wantId = 0;
    if (!nameOrId.empty() && nameOrId[0] == '#') wantId = std::atoi(nameOrId.c_str() + 1);
    const std::string key = (wantId > 0 || all) ? std::string() : KeyFor(nameOrId);
    for (std::size_t i = m_players.size(); i-- > 0;)
    {
        RuntimePlayer& p = *m_players[i];
        const bool hit = all || (wantId > 0 && p.id == wantId) || (!key.empty() && p.name == key);
        if (!hit) continue;
        // 止めた時も、書いたタイムスケールは戻す(スローモのまま残らない)
        if (p.wroteTimeScale && p.restoreOnEnd && m_cb.setTimeScale) m_cb.setTimeScale(p.timeScaleBefore);
        m_players.erase(m_players.begin() + static_cast<std::ptrdiff_t>(i));
        any = true;
    }
    if (any) RebuildOutputs();
    return any;
}

bool SequencerHost::PauseRuntime(const std::string& name, bool paused)
{
    const int i = PlayerIndexByName(KeyFor(name));
    if (i < 0) return false;
    m_players[static_cast<std::size_t>(i)]->paused = paused;
    return true;
}

bool SequencerHost::SeekRuntime(const std::string& name, double seconds)
{
    const int i = PlayerIndexByName(KeyFor(name));
    if (i < 0) return false;
    RuntimePlayer& p = *m_players[static_cast<std::size_t>(i)];
    p.pb.Seek(seq::SecondsToTicks(seconds, p.seq->ticksPerSecond));   // スクラブ = イベントは発火しない
    return true;
}

std::vector<PlayerInfo> SequencerHost::Players() const
{
    std::vector<PlayerInfo> v;
    for (const auto& up : m_players)
    {
        const RuntimePlayer& p = *up;
        PlayerInfo i;
        i.id = p.id;
        i.name = p.name;
        i.timeSec = seq::TicksToSeconds(p.pb.Position(), p.seq->ticksPerSecond);
        seq::Tick a = 0, b = 0;
        seq::GetRange(*p.seq, a, b);
        i.durationSec = seq::TicksToSeconds(b - a, p.seq->ticksPerSecond);
        i.paused = p.paused;
        i.loop = p.mode != seq::LoopMode::Once;
        i.rate = p.rate;
        i.clockGame = p.clockGame;
        i.finished = p.finished;
        v.push_back(std::move(i));
    }
    return v;
}

int SequencerHost::LuaPlay(const std::string& name, const SequencePlayOptions& opt, std::string& err)
{
    const DocPtr d = LoadDoc(name, err);
    if (!d) return 0;
    return PlayRuntime(d, opt, err);
}

bool SequencerHost::LuaIsPlaying(const std::string& name) const
{
    return PlayerIndexByName(KeyFor(name)) >= 0;
}

double SequencerHost::LuaDuration(const std::string& name)
{
    std::string err;
    const DocPtr d = LoadDoc(name, err);
    if (!d) return -1.0;
    seq::Tick a = 0, b = 0;
    seq::GetRange(d->seq, a, b);
    return seq::TicksToSeconds(b - a, d->seq.ticksPerSecond);
}

double SequencerHost::LuaTime(const std::string& name) const
{
    const int i = PlayerIndexByName(KeyFor(name));
    if (i < 0) return -1.0;
    const RuntimePlayer& p = *m_players[static_cast<std::size_t>(i)];
    return seq::TicksToSeconds(p.pb.Position(), p.seq->ticksPerSecond);
}

// ---- イベント ------------------------------------------------------------------
void SequencerHost::FireOne(const seq::Sequence&, const seq::EventItem& ev)
{
    if (ev.kind == "emit")
    {
        EngineEvent e;
        e.name = ev.name;
        const auto put = [&](const std::string& key, const seq::SeqValue& v) {
            if (v.IsNumber()) e.set(key, v.n);
            else if (v.IsBool()) e.set(key, v.b);
            else if (v.IsString()) e.set(key, v.s);
        };
        if (const seq::SeqValue* d = ev.params.Find("data"); d && d->IsObject())
            for (const auto& m : d->obj) put(m.key, m.value);
        else if (const seq::SeqValue* v = ev.params.Find("value"))
            put("value", *v);
        if (m_cb.emit) m_cb.emit(e);
    }
    else if (ev.kind == "lua")
    {
        std::string fn = ev.name;
        if (fn.empty()) fn = ev.params.GetString("fn");
        std::vector<EngineEvent::Value> args;
        if (const seq::SeqValue* a = ev.params.Find("args"); a && a->IsArray())
            for (const seq::SeqValue& v : a->arr)
            {
                if (v.IsNumber()) args.emplace_back(v.n);
                else if (v.IsBool()) args.emplace_back(v.b);
                else if (v.IsString()) args.emplace_back(v.s);
            }
        std::string err;
        if (fn.empty()) Logger::Warn("シーケンスのイベント {}: lua の関数名が空", ev.id);
        else if (!m_cb.callLua) Logger::Warn("シーケンスのイベント {}: Lua が無いので {} を呼べない", ev.id, fn);
        else if (!m_cb.callLua(fn, args, err)) Logger::Warn("シーケンスのイベント {}: Lua {} の呼び出しに失敗: {}", ev.id, fn, err);
    }
    else if (ev.kind == "log")
    {
        Logger::Info("[Sequence] {}", ev.name);
    }
    else if (ev.kind == "loadScene")
    {
        if (m_cb.loadScene) m_cb.loadScene(ev.name);
    }
    else if (m_warnedKinds.insert(ev.kind).second)
    {
        Logger::Warn("シーケンスのイベント kind \"{}\" は未対応(emit / lua / log / loadScene)", ev.kind);
    }
}

void SequencerHost::FireEvents(RuntimePlayer& p)
{
    for (const seq::EventFire& f : p.fired)
    {
        if (f.binding < 0 || static_cast<std::size_t>(f.binding) >= p.seq->bindings.size()) continue;
        const seq::Binding& b = p.seq->bindings[static_cast<std::size_t>(f.binding)];
        if (f.track < 0 || static_cast<std::size_t>(f.track) >= b.tracks.size()) continue;
        const seq::Track& t = b.tracks[static_cast<std::size_t>(f.track)];
        if (f.event < 0 || static_cast<std::size_t>(f.event) >= t.events.size()) continue;
        // 未解決バインディングのイベントトラックは発火しない(scene バインディングは常に解決済み)
        if (static_cast<std::size_t>(f.binding) < p.bound.mask.size() && !p.bound.mask[static_cast<std::size_t>(f.binding)]) continue;
        FireOne(*p.seq, t.events[static_cast<std::size_t>(f.event)]);
    }
    p.fired.clear();
}

void SequencerHost::FinishPlayer(RuntimePlayer& p)
{
    p.finished = true;
    if (p.wroteTimeScale && p.restoreOnEnd && m_cb.setTimeScale) m_cb.setTimeScale(p.timeScaleBefore);
    if (m_cb.emit)
    {
        EngineEvent e;
        e.name = p.name + ":done";   // 旧 sequence_author の Lua と同じ名前(互換)
        e.set("value", 1.0);
        m_cb.emit(e);
    }
}

// ===========================================================================
// フレーム更新
// ===========================================================================
void SequencerHost::OnEnterPlaying()
{
    EditorEnd();   // (c) Play の直前に元へ戻す(EnterPlayMode も明示的に呼ぶ。ここは保険)
    // <name>:play / <name>:stop(旧 sequence_author の Lua と同じイベント名)で操作できるようにする
    if (m_cb.subscribe)
    {
        std::vector<std::string> names;
        for (const std::string& rel : ListAssets()) names.push_back(KeyFor(rel));
        for (const DocPtr& d : m_docs) names.push_back(d->name);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const std::string& n : names)
        {
            m_cb.subscribe(n + ":play", [this, n](const EngineEvent&) {
                std::string err;
                SequencePlayOptions o;
                if (LuaPlay(n, o, err) == 0) Logger::Warn("{}:play: {}", n, err);
            });
            m_cb.subscribe(n + ":stop", [this, n](const EngineEvent&) { StopRuntime(n); });
        }
    }
    // シーンの自動再生設定
    if (m_scene)
    {
        const std::vector<SequenceAutoPlay> list = m_scene->GetSequenceAutoPlay();   // 再生中に Lua が書き換えても安全なようコピー
        for (const SequenceAutoPlay& sp : list)
        {
            std::string err;
            SequencePlayOptions o;
            o.loop = sp.loop ? 1 : 0;
            o.rate = sp.rate;
            o.startDelay = sp.startDelay;
            o.clockGame = sp.clockGame;
            if (LuaPlay(sp.sequence, o, err) == 0) Logger::Warn("シーンの自動再生 {}: {}", sp.sequence, err);
        }
    }
}

void SequencerHost::OnLeavePlaying()
{
    for (auto& up : m_players)
        if (up->wroteTimeScale && up->restoreOnEnd && m_cb.setTimeScale) m_cb.setTimeScale(up->timeScaleBefore);
    m_players.clear();
    m_pre.Clear();
    m_warnedKinds.clear();
    RebuildOutputs();
}

void SequencerHost::RebuildOutputs()
{
    m_out.Clear();
    const auto merge = [&](const ApplyOutput& o, bool cameraOnly) {
        if (o.cutCamera != entt::null) { m_out.cutCamera = o.cutCamera; m_out.cutIndex = o.cutIndex; }
        if (cameraOnly) return;
        m_out.post.insert(m_out.post.end(), o.post.begin(), o.post.end());
        m_out.shakes.insert(m_out.shakes.end(), o.shakes.begin(), o.shakes.end());
        if (o.hasTimeScale) { m_out.hasTimeScale = true; m_out.timeScale = o.timeScale; }
        m_out.transformsWritten += o.transformsWritten;
        m_out.fieldsWritten += o.fieldsWritten;
        m_out.skippedPhysics += o.skippedPhysics;
        m_out.skippedMissing += o.skippedMissing;
        for (const std::string& w : o.warnings) m_out.warnings.push_back(w);
    };
    if (m_ed.active) merge(m_ed.out, false);
    for (const auto& up : m_players)
    {
        if (up->finished && up->restoreOnEnd) continue;
        merge(up->out, /*cameraOnly=*/up->finished);
    }
}

void SequencerHost::UpdateEditor(float dt)
{
    if (!m_ed.active)
    {
        // セッション無しで退避が残っている = 取りこぼし。念のため戻す
        if (!m_pre.Empty() && m_scene) m_pre.RestoreAll(m_scene->GetRegistry());
        return;
    }
    if (!m_scene) return;
    bool need = m_ed.needsApply;
    if (m_ed.playing)
    {
        std::vector<seq::EventFire> discard;   // ★エディタのプレビューはイベントを発火しない(設計書 §3.4)
        m_ed.pb.Advance(m_ed.doc->seq, dt, discard);
        m_ed.t = m_ed.pb.Position();
        need = true;
        if (m_ed.pb.IsFinished()) m_ed.playing = false;
    }
    const entt::registry& reg = m_scene->GetRegistry();
    if (m_bindingsDirty || m_ed.boundRevision != m_ed.doc->revision || m_ed.boundToken != m_token ||
        NeedRebind(m_ed.bound, m_ed.bound_ok, reg, false, m_frame, true))
        need = true;
    if (need)
    {
        m_ed.needsApply = true;
        ApplyEditorNow();
    }
}

void SequencerHost::UpdateRuntime(float dt, bool paused)
{
    if (paused || !m_scene || m_players.empty()) return;
    entt::registry& reg = m_scene->GetRegistry();
    const std::size_t n = m_players.size();
    for (std::size_t i = 0; i < n && i < m_players.size(); ++i)
    {
        RuntimePlayer& p = *m_players[i];
        if (p.finished) continue;
        const seq::Sequence& s = *p.seq;

        if (NeedRebind(p.bound, p.boundOk, reg, m_bindingsDirty || p.boundToken != m_token, m_frame, /*checkCount=*/false))
        {
            BindOptions bo;   // Play 中は guid を確定させない(シーンを書き換えない)
            BindSequence(s, reg, bo, p.bound);
            p.boundOk = true;
            p.boundToken = m_token;
            for (const std::string& w : p.bound.warnings)
                if (m_warnedKinds.insert(p.name + "|" + w).second) Logger::Warn("シーケンス {}: {}", p.name, w);
        }

        double eff = dt;
        if (p.clockGame && m_cb.getTimeScale) eff *= static_cast<double>(m_cb.getTimeScale());
        if (p.paused) eff = 0.0;
        if (p.delayLeft > 0.0)
        {
            p.delayLeft -= eff;
            if (p.delayLeft > 0.0) continue;
            eff = 0.0;
        }

        p.fired.clear();
        p.pb.Advance(s, eff, p.fired);

        seq::EvalFilter f;
        f.bindingEnabled = p.bound.mask;
        seq::Evaluate(s, p.pb.Position(), p.res, &f);
        ApplyOptions opt;
        opt.pre = nullptr;               // Play の再生は元値を退避しない(結果を残す。Stop でシーンごと復元される)
        opt.physicsActive = true;
        p.out.Clear();
        ApplySequence(s, p.res, p.bound, reg, opt, p.out);
        ++m_applyCount;
        for (const std::string& w : p.out.warnings)
            if (m_warnedKinds.insert(p.name + "|" + w).second) Logger::Warn("シーケンス {}: {}", p.name, w);

        if (p.out.hasTimeScale && m_cb.setTimeScale)
        {
            if (!p.wroteTimeScale)
            {
                p.wroteTimeScale = true;
                p.timeScaleBefore = m_cb.getTimeScale ? m_cb.getTimeScale() : 1.0f;
            }
            m_cb.setTimeScale(p.out.timeScale);
        }
        FireEvents(p);
        if (p.pb.IsFinished()) FinishPlayer(p);
    }
    m_bindingsDirty = false;
    // 終わったプレイヤーを片付ける(restoreOnEnd=false は最後のカットを保つために残す)
    for (std::size_t i = m_players.size(); i-- > 0;)
        if (m_players[i]->finished && m_players[i]->restoreOnEnd) m_players.erase(m_players.begin() + static_cast<std::ptrdiff_t>(i));
    RebuildOutputs();
}

void SequencerHost::Update(float dt, HostMode mode, bool paused, std::uint64_t sceneToken)
{
    ++m_frame;
    const bool modeChanged = mode != m_prevMode;
    if (modeChanged)
    {
        m_prevMode = mode;
        if (mode == HostMode::Playing) OnEnterPlaying();
        else OnLeavePlaying();
    }
    if (sceneToken != m_token)
    {
        const bool first = (m_token == ~0ull);
        m_token = sceneToken;
        if (!first && !modeChanged)
        {
            if (mode == HostMode::Editor)
            {
                // open_scene / new_scene: エンティティが全部入れ替わる。退避は捨てるだけ(戻す先の実体が別物)
                m_pre.Clear();
                m_ed = EditorSession{};
                RebuildOutputs();
            }
            else
            {
                // ランタイムのシーン切替(loadScene): 前のシーンの再生を止め、新しいシーンの自動再生を始める
                for (auto& up : m_players)
                    if (up->wroteTimeScale && up->restoreOnEnd && m_cb.setTimeScale) m_cb.setTimeScale(up->timeScaleBefore);
                m_players.clear();
                OnEnterPlaying();
            }
        }
    }
    if (mode == HostMode::Editor) UpdateEditor(dt);
    else UpdateRuntime(dt, paused);
}

// ===========================================================================
// 描画側への出力
// ===========================================================================
bool SequencerHost::ApplyPostOverrides(PostProcessSettings& pp, bool cameraView) const
{
    if (m_out.post.empty()) return false;
    ApplyPostWrites(pp, m_out.post, cameraView);
    return true;
}

bool SequencerHost::ShakeFor(entt::entity camera, float& x, float& y, float& z) const
{
    bool any = false;
    x = y = z = 0.0f;
    for (const ShakeWrite& s : m_out.shakes)
    {
        if (s.entity != camera) continue;
        x += s.x; y += s.y; z += s.z;
        any = true;
    }
    return any;
}

} // namespace seqhost
} // namespace dx12e
