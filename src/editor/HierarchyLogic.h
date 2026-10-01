#pragma once

// ===== ヒエラルキーの純ロジック（フェーズ 1b）=====
// ImGui にも GPU にも依存しない（entt::registry と ecs/Components.h だけ）。tests/hierarchy_logic_test.cpp が直接呼ぶ。
//   ・型の分類（メッシュ / ライト / カメラ / UI / 物理 / スクリプト / エフェクト / 音 / フォルダ）とチップ用マスク
//   ・検索クエリ: 名前 + コンポーネント名 + タグ。接頭辞 `tag:` `c:` `t:` `n:` `is:`
//   ・親→子の索引（CSR。兄弟は (siblingOrder, id) の順）と構造の指紋（変わっていない間は使い回す）
//   ・範囲選択 / ドロップ位置（前 / 中 / 後）→ 親替え + 兄弟順の計画
//   ・エディタ専用フラグ（非表示 / ロック）の「これだけ表示（Alt+クリック）」の計画と状態遷移
// ★描画側に何も足さない（hidden / locked はエディタの見た目と選択だけに効く）。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/Components.h"
#include "ecs/EditorFlags.h"

namespace dx12e::hier
{

// ===================== 型 =====================
enum TypeBit : unsigned
{
    kTMesh    = 1u << 0,   // メッシュ / 地形 / スカルプト
    kTLight   = 1u << 1,
    kTCamera  = 1u << 2,
    kTUi      = 1u << 3,
    kTPhysics = 1u << 4,   // 剛体 / コライダー / キャラクターコントローラ
    kTScript  = 1u << 5,   // Lua / Brain
    kTEffect  = 1u << 6,   // パーティクル / トレイル / デカール
    kTAudio   = 1u << 7,
    kTFolder  = 1u << 8,   // 整理用フォルダ（EditorFolder）
};

struct TypeInfo
{
    unsigned    bit;
    const char* key;     // クエリ `t:` の値（英小文字）
    const char* label;   // 表示名（標準語）
    const char* alias;   // 別名（日本語など。`t:` でも引ける）
};

inline constexpr TypeInfo kTypes[] = {
    {kTMesh,    "mesh",    "メッシュ",       "モデル"},
    {kTLight,   "light",   "ライト",         "光"},
    {kTCamera,  "camera",  "カメラ",         "cam"},
    {kTUi,      "ui",      "UI",             "ゲームui"},
    {kTPhysics, "physics", "物理",           "collider"},
    {kTScript,  "script",  "スクリプト",     "lua"},
    {kTEffect,  "effect",  "エフェクト",     "particle"},
    {kTAudio,   "audio",   "オーディオ",     "sound"},
    {kTFolder,  "folder",  "フォルダ",       "group"},
};
inline constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));
inline constexpr unsigned kAllTypes = (1u << kTypeCount) - 1u;

// エンティティが持つ型ビットの合計（複数の型を持ちうる。何も無ければ 0）。
inline unsigned ClassifyTypes(const entt::registry& reg, entt::entity e)
{
    unsigned m = 0;
    if (reg.any_of<MeshRenderer, Terrain, SculptMesh>(e))                              m |= kTMesh;
    if (reg.any_of<PointLight, SpotLight, DirectionalLight>(e))                        m |= kTLight;
    if (reg.all_of<CameraComponent>(e))                                                m |= kTCamera;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton, UISlider, UIToggle,
                   UIScrollView, UILayout>(e))                                         m |= kTUi;
    if (reg.any_of<RigidBody, CharacterController, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, MeshCollider>(e))              m |= kTPhysics;
    if (reg.any_of<LuaScript, Brain>(e))                                               m |= kTScript;
    if (reg.any_of<ParticleEmitter, TrailRenderer, DecalComponent>(e))                 m |= kTEffect;
    if (reg.any_of<AudioSource, AudioReverbZone>(e))                                   m |= kTAudio;
    if (eflags::Self<EditorFolder>(reg, e))                                            m |= kTFolder;
    return m;
}

// ===================== 文字列ユーティリティ =====================
inline std::string ToLowerAscii(std::string_view s)
{
    std::string r(s);
    for (char& c : r)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

inline bool ContainsLower(const std::string& haystackLower, const std::string& needleLower)
{
    return needleLower.empty() || haystackLower.find(needleLower) != std::string::npos;
}

// 大文字小文字を無視した部分一致（haystack 側は毎回小文字化する。名前は短いので十分軽い）。
inline bool ContainsCI(const std::string& haystack, const std::string& needleLower)
{
    if (needleLower.empty()) return true;
    if (haystack.size() < needleLower.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= haystack.size(); ++i)
    {
        size_t j = 0;
        for (; j < needleLower.size(); ++j)
        {
            char a = haystack[i + j];
            if (static_cast<unsigned char>(a) < 0x80) a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
            if (a != needleLower[j]) break;
        }
        if (j == needleLower.size()) return true;
    }
    return false;
}

// ===================== コンポーネント名の表（`c:` と、接頭辞なしの語の照合用）=====================
struct CompEntry
{
    const char* name;   // 小文字で照合する表示名
    bool (*has)(const entt::registry&, entt::entity);
};

#define DX12E_HIER_COMP(NAME, TYPE) {NAME, [](const entt::registry& r, entt::entity e) { return r.all_of<TYPE>(e); }}
inline const CompEntry* CompTable(int& count)
{
    static const CompEntry kTable[] = {
        DX12E_HIER_COMP("MeshRenderer", MeshRenderer),
        DX12E_HIER_COMP("Terrain", Terrain),
        DX12E_HIER_COMP("SculptMesh", SculptMesh),
        DX12E_HIER_COMP("PointLight", PointLight),
        DX12E_HIER_COMP("SpotLight", SpotLight),
        DX12E_HIER_COMP("DirectionalLight", DirectionalLight),
        DX12E_HIER_COMP("Camera", CameraComponent),
        DX12E_HIER_COMP("Sprite2D", Sprite2D),
        DX12E_HIER_COMP("UICanvas", UICanvas),
        DX12E_HIER_COMP("UIRect", UIRect),
        DX12E_HIER_COMP("UIImage", UIImage),
        DX12E_HIER_COMP("UIText", UIText),
        DX12E_HIER_COMP("UIButton", UIButton),
        DX12E_HIER_COMP("UISlider", UISlider),
        DX12E_HIER_COMP("UIToggle", UIToggle),
        DX12E_HIER_COMP("UIScrollView", UIScrollView),
        DX12E_HIER_COMP("UILayout", UILayout),
        DX12E_HIER_COMP("UIAnimator", UIAnimator),
        // ※ SkeletalAnimation は SkinningBuffer（不完全型）を unique_ptr で持つので、ここから引くと include の順序に依存する。表に入れない。
        DX12E_HIER_COMP("AnimatorController", AnimatorController),
        DX12E_HIER_COMP("RigidBody", RigidBody),
        DX12E_HIER_COMP("BoxCollider", BoxCollider),
        DX12E_HIER_COMP("SphereCollider", SphereCollider),
        DX12E_HIER_COMP("CapsuleCollider", CapsuleCollider),
        DX12E_HIER_COMP("ConvexHullCollider", ConvexHullCollider),
        DX12E_HIER_COMP("MeshCollider", MeshCollider),
        DX12E_HIER_COMP("CharacterController", CharacterController),
        DX12E_HIER_COMP("LuaScript", LuaScript),
        DX12E_HIER_COMP("Brain", Brain),
        DX12E_HIER_COMP("AudioSource", AudioSource),
        DX12E_HIER_COMP("AudioReverbZone", AudioReverbZone),
        DX12E_HIER_COMP("ParticleEmitter", ParticleEmitter),
        DX12E_HIER_COMP("TrailRenderer", TrailRenderer),
        DX12E_HIER_COMP("DecalComponent", DecalComponent),
        DX12E_HIER_COMP("Trigger", Trigger),
        DX12E_HIER_COMP("Gimmick", Gimmick),
        DX12E_HIER_COMP("PrefabLink", PrefabLink),
        DX12E_HIER_COMP("VirtualGeometry", VirtualGeometry),
        DX12E_HIER_COMP("FoliageLayer", FoliageLayer),
        DX12E_HIER_COMP("InstanceGroup", InstanceGroup),
        DX12E_HIER_COMP("NetworkIdentity", NetworkIdentity),
    };
    count = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}
#undef DX12E_HIER_COMP

// ===================== 検索クエリ =====================
// 空白区切りの語を AND で結ぶ。
//   `tag:enemy`  … Tag コンポーネントのタグに含む
//   `c:light`    … コンポーネント名に含む（PointLight / SpotLight / DirectionalLight）
//   `t:mesh`     … 型（kTypes の key / 表示名 / 別名）
//   `n:door`     … 名前だけ
//   `is:hidden` `is:locked` `is:folder` `is:disabled` … 状態
//   接頭辞なしの語 … 名前 or タグ or（2 文字以上ならコンポーネント名）に含む
struct Term
{
    enum class Kind : unsigned char { Any, Name, Tag, Comp, Type, Is } kind = Kind::Any;
    std::string      text;                 // 小文字
    unsigned         typeMask = 0;         // Kind::Type
    std::vector<int> compIdx;              // Kind::Comp / Any: text を含むコンポーネント名の表番号
    int              isFlag   = 0;         // Kind::Is: 1=hidden 2=locked 3=folder 4=disabled
};

struct Query
{
    std::vector<Term> terms;
    bool Empty() const { return terms.empty(); }
    // 表示用の署名（キャッシュの鍵）
    std::string Signature() const
    {
        std::string s;
        for (const Term& t : terms) { s += static_cast<char>('0' + static_cast<int>(t.kind)); s += t.text; s += '\x1f'; }
        return s;
    }
};

inline std::vector<int> FindCompsContaining(const std::string& lower)
{
    std::vector<int> out;
    int n = 0;
    const CompEntry* tbl = CompTable(n);
    for (int i = 0; i < n; ++i)
        if (ContainsLower(ToLowerAscii(tbl[i].name), lower)) out.push_back(i);
    return out;
}

inline unsigned ParseTypeMask(const std::string& lower)
{
    unsigned m = 0;
    if (lower.empty()) return 0;
    for (const TypeInfo& t : kTypes)
        if (lower == t.key || lower == ToLowerAscii(t.alias) || lower == t.label) m |= t.bit;
    // 前方一致も許す（`t:mes` → mesh）。1 文字は曖昧なので不可。
    if (m == 0 && lower.size() >= 2)
        for (const TypeInfo& t : kTypes)
            if (std::string_view(t.key).substr(0, lower.size()) == lower) m |= t.bit;
    return m;
}

inline Query ParseQuery(std::string_view text)
{
    Query q;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
        if (j > i)
        {
            std::string_view tok = text.substr(i, j - i);
            Term t;
            const auto colon = tok.find(':');
            std::string key = colon == std::string_view::npos ? std::string() : ToLowerAscii(tok.substr(0, colon));
            const std::string val = colon == std::string_view::npos ? std::string() : ToLowerAscii(tok.substr(colon + 1));
            if (colon != std::string_view::npos && !val.empty() && (key == "tag" || key == "c" || key == "t" || key == "n" || key == "is"))
            {
                t.text = val;
                if (key == "tag")      t.kind = Term::Kind::Tag;
                else if (key == "n")   t.kind = Term::Kind::Name;
                else if (key == "c")   { t.kind = Term::Kind::Comp; t.compIdx = FindCompsContaining(val); }
                else if (key == "t")   { t.kind = Term::Kind::Type; t.typeMask = ParseTypeMask(val); }
                else
                {
                    t.kind = Term::Kind::Is;
                    if (val == "hidden" || val == "hide")           t.isFlag = 1;
                    else if (val == "locked" || val == "lock")      t.isFlag = 2;
                    else if (val == "folder")                       t.isFlag = 3;
                    else if (val == "disabled" || val == "off")     t.isFlag = 4;
                }
            }
            else
            {
                t.kind = Term::Kind::Any;
                t.text = ToLowerAscii(tok);
                if (t.text.size() >= 2) t.compIdx = FindCompsContaining(t.text);
            }
            q.terms.push_back(std::move(t));
        }
        i = j;
    }
    return q;
}

inline bool TagContains(const entt::registry& reg, entt::entity e, const std::string& lower)
{
    const auto* tg = reg.try_get<Tag>(e);
    if (!tg) return false;
    for (const std::string& s : tg->tags)
        if (ContainsCI(s, lower)) return true;
    return false;
}

inline bool CompHas(const entt::registry& reg, entt::entity e, const std::vector<int>& idx)
{
    if (idx.empty()) return false;
    int n = 0;
    const CompEntry* tbl = CompTable(n);
    for (int i : idx)
        if (i >= 0 && i < n && tbl[i].has(reg, e)) return true;
    return false;
}

inline bool MatchTerm(const entt::registry& reg, entt::entity e, const std::string& name, const Term& t)
{
    switch (t.kind)
    {
    case Term::Kind::Name: return ContainsCI(name, t.text);
    case Term::Kind::Tag:  return TagContains(reg, e, t.text);
    case Term::Kind::Comp: return CompHas(reg, e, t.compIdx);
    case Term::Kind::Type: return t.typeMask != 0 && (ClassifyTypes(reg, e) & t.typeMask) != 0;
    case Term::Kind::Is:
        switch (t.isFlag)
        {
        case 1: return eflags::IsHidden(reg, e);
        case 2: return eflags::IsLocked(reg, e);
        case 3: return eflags::Self<EditorFolder>(reg, e);
        case 4: return eflags::IsDisabled(reg, e);
        default: return false;
        }
    case Term::Kind::Any:
    default:
        return ContainsCI(name, t.text) || TagContains(reg, e, t.text) || CompHas(reg, e, t.compIdx);
    }
}

// 検索クエリ + 型チップ（typeMask != 0 なら OR で 1 つでも該当すること）に合うか。
inline bool Matches(const entt::registry& reg, entt::entity e, const std::string& name,
                    const Query& q, unsigned typeMask)
{
    if (typeMask != 0 && (ClassifyTypes(reg, e) & typeMask) == 0) return false;
    for (const Term& t : q.terms)
        if (!MatchTerm(reg, e, name, t)) return false;
    return true;
}

// ===================== 親→子の索引（CSR）=====================
// 兄弟は (siblingOrder, id) の昇順。ルートは「全員 siblingOrder==0 なら registry の走査順のまま（従来どおり）、
// どれかが非 0 なら siblingOrder で安定ソート」。
struct Index
{
    std::vector<entt::entity> roots;
    std::vector<entt::entity> parents;      // 子を持つエンティティ（「すべて展開」用）
    std::vector<entt::entity> flat;         // 子の連結（親ごとに連続）
    std::vector<uint32_t>     begin, count; // 親のエンティティ番号（entt::to_entity）で引く
    uint64_t                  key = 0;      // 構築時の指紋
    bool                      valid = false;

    void Clear() { roots.clear(); parents.clear(); flat.clear(); begin.clear(); count.clear(); valid = false; }

    // 親 e の子（描画順）。子が無ければ空。
    std::pair<const entt::entity*, size_t> Children(entt::entity e) const
    {
        const uint32_t id = static_cast<uint32_t>(entt::to_entity(e));
        if (id >= count.size() || count[id] == 0) return {nullptr, 0};
        return {flat.data() + begin[id], count[id]};
    }
    size_t ChildCount(entt::entity e) const { return Children(e).second; }
    bool HasChildren(entt::entity e) const { return ChildCount(e) != 0; }

    // GridPlane（内部用）は除外。parent が壊れている（無効な親）ものはルートとして扱う。
    void Build(const entt::registry& reg)
    {
        Clear();
        uint32_t maxId = 0;
        {
            const auto* s = reg.storage<entt::entity>();
            if (s) maxId = static_cast<uint32_t>(s->size());
        }
        begin.assign(maxId + 1, 0);
        count.assign(maxId + 1, 0);

        // ★走査は NameTag の単独プール（従来のヒエラルキーと同じ）。ルートは「プールの走査順の逆」を表示順にする
        //   （従来は走査順に積んだスタックから取り出していたので逆順＝作成順に並んでいた。それを保つ）。
        auto view = reg.view<const NameTag>();
        bool anyRootOrder = false;
        size_t nChildren = 0;
        for (auto [e, n] : view.each())
        {
            (void)n;
            if (reg.all_of<GridPlane>(e)) continue;
            const Transform* t = reg.try_get<Transform>(e);
            if (t && t->parent != entt::null && reg.valid(t->parent))
            {
                const uint32_t pid = static_cast<uint32_t>(entt::to_entity(t->parent));
                if (pid >= count.size()) { count.resize(pid + 1, 0); begin.resize(pid + 1, 0); }
                if (++count[pid] == 1) parents.push_back(t->parent);
                ++nChildren;
            }
            else
            {
                roots.push_back(e);
                if (t && t->siblingOrder != 0) anyRootOrder = true;
            }
        }
        std::reverse(roots.begin(), roots.end());
        // 各親の開始位置
        uint32_t acc = 0;
        for (size_t i = 0; i < count.size(); ++i) { begin[i] = acc; acc += count[i]; }
        flat.resize(nChildren);
        std::vector<uint32_t> fill(count.size(), 0);
        for (auto [e, n] : view.each())
        {
            (void)n;
            if (reg.all_of<GridPlane>(e)) continue;
            const Transform* t = reg.try_get<Transform>(e);
            if (!t || t->parent == entt::null || !reg.valid(t->parent)) continue;
            const uint32_t pid = static_cast<uint32_t>(entt::to_entity(t->parent));
            flat[begin[pid] + fill[pid]++] = e;
        }
        // 兄弟の並び: (siblingOrder, id)
        for (size_t p = 0; p < count.size(); ++p)
        {
            if (count[p] < 2) continue;
            auto b = flat.begin() + begin[p];
            std::sort(b, b + count[p], [&reg](entt::entity a, entt::entity c) {
                const int oa = reg.get<Transform>(a).siblingOrder, oc = reg.get<Transform>(c).siblingOrder;
                if (oa != oc) return oa < oc;
                return a < c;
            });
        }
        if (anyRootOrder)
            std::stable_sort(roots.begin(), roots.end(), [&reg](entt::entity a, entt::entity c) {
                return reg.get<Transform>(a).siblingOrder < reg.get<Transform>(c).siblingOrder;
            });
        valid = true;
    }
};

// 表示行（開いている親の子だけ）。明示スタックの DFS。
struct Row { entt::entity e; int depth; };

inline void BuildRows(const Index& idx, const std::unordered_set<entt::entity>& open, std::vector<Row>& rows,
                      bool openAll = false)
{
    rows.clear();
    std::vector<Row> stack;
    stack.reserve(64);
    for (auto it = idx.roots.rbegin(); it != idx.roots.rend(); ++it) stack.push_back({*it, 0});
    while (!stack.empty())
    {
        const Row r = stack.back();
        stack.pop_back();
        rows.push_back(r);
        if (!openAll && (open.empty() || !open.count(r.e))) continue;
        const auto [p, n] = idx.Children(r.e);
        for (size_t i = n; i-- > 0;) stack.push_back({p[i], r.depth + 1});
    }
}

// 構造の指紋。この値が変わらない間は Index を作り直さない（10 万体でも毎フレームの O(N) を避ける）。
// poolSizes: Transform / NameTag の数。editSeq / undoDepth / redoDepth: Undo 系（Undo/Redo は editSeq を進めないので深さも混ぜる）。
inline uint64_t StructureKey(size_t transforms, size_t names, uint64_t editSeq, size_t undoDepth, size_t redoDepth, uint64_t localVersion)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 1099511628211ull; };
    mix(transforms); mix(names); mix(editSeq); mix(undoDepth); mix(redoDepth); mix(localVersion);
    return h;
}

// ===================== 範囲選択 =====================
// rows（今の見えている並び）の上で a〜b の閉区間。a か b が rows に無ければ false（呼び出し側は単独選択へ退避）。
inline bool RangeBetween(const std::vector<Row>& rows, entt::entity a, entt::entity b, std::vector<entt::entity>& out)
{
    size_t ia = rows.size(), ib = rows.size();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].e == a) ia = i;
        if (rows[i].e == b) ib = i;
    }
    if (ia == rows.size() || ib == rows.size()) return false;
    if (ia > ib) std::swap(ia, ib);
    out.clear();
    for (size_t i = ia; i <= ib; ++i) out.push_back(rows[i].e);
    return true;
}

// ===================== ドロップ位置 → 親替え + 兄弟順 =====================
enum class DropZone : unsigned char { Before, Onto, After };

// 行の縦位置（0..1）から。上 25% = 前 / 下 25% = 後 / 中央 = 子にする。allowOnto=false（子にできない行）なら 2 分割。
inline DropZone ZoneFromRatio(float t, bool allowOnto = true)
{
    if (!allowOnto) return t < 0.5f ? DropZone::Before : DropZone::After;
    if (t < 0.25f) return DropZone::Before;
    if (t > 0.75f) return DropZone::After;
    return DropZone::Onto;
}

struct OrderChange { entt::entity e; int order; };

struct DropPlan
{
    bool                      ok = false;
    entt::entity              newParent = entt::null;
    std::vector<entt::entity> moving;          // 実際に動かす（祖先も掴んでいる子・循環は除外済み）
    std::vector<OrderChange>  orders;          // siblingOrder を書き換える（moving + 番号が変わる兄弟）
};

inline constexpr int kOrderStep = 16;

// 祖先（e から親を辿って a に当たるか）
inline bool IsAncestor(const entt::registry& reg, entt::entity a, entt::entity e)
{
    entt::entity cur = e;
    for (int depth = 0; cur != entt::null && reg.valid(cur) && depth < 4096; ++depth)
    {
        if (cur == a) return true;
        const auto* t = reg.try_get<Transform>(cur);
        cur = t ? t->parent : entt::null;
    }
    return false;
}

// grabbed（掴んだ行。選択に含まれていれば選択全部）を target の前 / 後 / 子として動かす計画を立てる（副作用なし）。
//   siblings … newParent の子を今の描画順に並べたもの（ルートなら roots。Onto のときは target の子）。
inline DropPlan PlanDrop(const entt::registry& reg, const std::vector<entt::entity>& grabbed, entt::entity target,
                         DropZone zone, const Index& idx)
{
    DropPlan plan;
    if (target == entt::null || !reg.valid(target) || !reg.all_of<Transform>(target)) return plan;

    // 動かす物: 祖先も一緒に掴んでいる子は除く。target 自身・target の祖先（循環）も除く。
    std::vector<entt::entity> moving;
    for (entt::entity d : grabbed)
    {
        if (d == target || !reg.valid(d) || !reg.all_of<Transform>(d)) continue;
        if (IsAncestor(reg, d, target)) continue;   // d が target の祖先 → d を target の下（や隣）に入れると循環
        bool underOther = false;
        for (entt::entity o : grabbed)
            if (o != d && IsAncestor(reg, o, d)) { underOther = true; break; }
        if (!underOther) moving.push_back(d);
    }
    if (moving.empty()) return plan;

    const Transform& tt = reg.get<Transform>(target);
    plan.newParent = zone == DropZone::Onto ? target
                   : (tt.parent != entt::null && reg.valid(tt.parent)) ? tt.parent : entt::null;
    plan.moving = moving;

    // 新しい親の子の並び（moving を抜いて、target の前 / 後 / 末尾に挿入）
    std::vector<entt::entity> seq;
    if (plan.newParent == entt::null)
        seq = idx.roots;
    else
    {
        const auto [p, n] = idx.Children(plan.newParent);
        seq.assign(p, p + n);
    }
    auto isMoving = [&](entt::entity e) { return std::find(moving.begin(), moving.end(), e) != moving.end(); };
    seq.erase(std::remove_if(seq.begin(), seq.end(), isMoving), seq.end());
    size_t at = seq.size();   // Onto は末尾へ
    if (zone != DropZone::Onto)
    {
        const auto it = std::find(seq.begin(), seq.end(), target);
        at = static_cast<size_t>(it - seq.begin()) + (zone == DropZone::After ? 1 : 0);
    }
    seq.insert(seq.begin() + static_cast<std::ptrdiff_t>(at), moving.begin(), moving.end());

    // 番号の振り直し: 今の値が新しい並びに対して狭義単調増加なら moving だけ隙間へ、そうでなければ全員 i*kOrderStep で振り直す。
    auto curOrder = [&](entt::entity e) { return reg.get<Transform>(e).siblingOrder; };
    bool monotoneRest = true;   // moving を除いた並びが今の番号で狭義単調か
    {
        bool first = true; int prev = 0;
        for (entt::entity e : seq)
        {
            if (isMoving(e)) continue;
            const int o = curOrder(e);
            if (!first && o <= prev) { monotoneRest = false; break; }
            prev = o; first = false;
        }
    }
    bool gapsOk = false;
    std::vector<int> assigned(seq.size(), 0);
    if (monotoneRest)
    {
        // moving の連続区間 [at, at+k) を、前後の番号の間へ等間隔で入れられるか
        const size_t k = moving.size();
        const bool hasPrev = at > 0, hasNext = at + k < seq.size();
        const long long lo = hasPrev ? curOrder(seq[at - 1]) : (hasNext ? static_cast<long long>(curOrder(seq[at + k])) - static_cast<long long>(kOrderStep) * static_cast<long long>(k + 1) : -static_cast<long long>(kOrderStep));
        const long long hi = hasNext ? curOrder(seq[at + k]) : lo + static_cast<long long>(kOrderStep) * static_cast<long long>(k + 1);
        if (hi - lo >= static_cast<long long>(k) + 1 && hi - lo < 2000000000ll)
        {
            gapsOk = true;
            for (size_t i = 0; i < k; ++i)
                assigned[at + i] = static_cast<int>(lo + (hi - lo) * static_cast<long long>(i + 1) / static_cast<long long>(k + 1));
            for (size_t i = 0; i < seq.size(); ++i)
                if (i < at || i >= at + k) assigned[i] = curOrder(seq[i]);
        }
    }
    if (!gapsOk)
        for (size_t i = 0; i < seq.size(); ++i) assigned[i] = static_cast<int>(i) * kOrderStep;

    for (size_t i = 0; i < seq.size(); ++i)
        if (isMoving(seq[i]) || assigned[i] != curOrder(seq[i]))
            plan.orders.push_back({seq[i], assigned[i]});
    plan.ok = true;
    return plan;
}

// ===================== エディタ専用フラグ: 切替・Alt+クリック =====================
// 1 回のクリックの対象: 押した行が選択に含まれていれば選択ぜんぶ、そうでなければその行だけ。
inline std::vector<entt::entity> ToggleTargets(const std::vector<entt::entity>& selection, entt::entity clicked)
{
    if (std::find(selection.begin(), selection.end(), clicked) != selection.end()) return selection;
    return {clicked};
}

// e とその子孫と祖先（= 「これだけ」の残す集合 S）を集める。
inline void CollectKeepSet(const entt::registry& reg, const Index& idx, entt::entity e, std::unordered_set<entt::entity>& keep)
{
    keep.clear();
    for (entt::entity cur = e; cur != entt::null && reg.valid(cur); )
    {
        if (!keep.insert(cur).second) break;
        const auto* t = reg.try_get<Transform>(cur);
        cur = t ? t->parent : entt::null;
    }
    std::vector<entt::entity> st{e};
    while (!st.empty())
    {
        entt::entity c = st.back(); st.pop_back();
        keep.insert(c);
        const auto [p, n] = idx.Children(c);
        for (size_t i = 0; i < n; ++i) st.push_back(p[i]);
    }
}

// 「e だけ残す（他は全部フラグを立てる）」の計画。T = EditorHidden なら「これだけ表示」、EditorLocked なら「これ以外をロック」。
//   立てる  … S に入らない各兄弟（S の祖先を含む階層全体の、S 以外の子とルート）。祖先に立てれば子孫は実効で立つので最小で済む。
//   降ろす  … S（e / 祖先 / e の子孫）のうち自分に立っている物。
template <class T>
struct FlagPlan
{
    std::vector<entt::entity> set, clear;
};

template <class T>
inline FlagPlan<T> PlanIsolate(const entt::registry& reg, const Index& idx, entt::entity e)
{
    FlagPlan<T> plan;
    std::unordered_set<entt::entity> keep;
    CollectKeepSet(reg, idx, e, keep);
    for (entt::entity k : keep)
        if (eflags::Self<T>(reg, k)) plan.clear.push_back(k);
    auto consider = [&](entt::entity c) {
        if (!keep.count(c) && !eflags::Self<T>(reg, c)) plan.set.push_back(c);
    };
    for (entt::entity r : idx.roots) consider(r);
    for (entt::entity a : keep)
    {
        // S の祖先（e の子孫でない）のうち、子を持つものだけ子を見る
        const auto [p, n] = idx.Children(a);
        for (size_t i = 0; i < n; ++i) consider(p[i]);
    }
    return plan;
}

// 「e だけ残している状態」か。e が実効で立っておらず、S の外の全エンティティが実効で立っている。
template <class T>
inline bool IsIsolated(const entt::registry& reg, const Index& idx, entt::entity e)
{
    if (eflags::SelfOrAncestor<T>(reg, e)) return false;
    std::unordered_set<entt::entity> keep;
    CollectKeepSet(reg, idx, e, keep);
    bool any = false;
    for (auto [x, n] : reg.view<const NameTag>().each())
    {
        (void)n;
        if (reg.all_of<GridPlane>(x)) continue;
        if (keep.count(x)) continue;
        any = true;
        if (!eflags::SelfOrAncestor<T>(reg, x)) return false;
    }
    return any;   // 他に 1 つも無い（e だけのシーン）なら「切り替える意味が無い」ので false
}

// 全部降ろす（「すべて表示 / すべてロック解除」）。
template <class T>
inline std::vector<entt::entity> PlanClearAll(const entt::registry& reg)
{
    std::vector<entt::entity> out;
    if (const auto* s = reg.storage<T>())
        for (entt::entity e : *s) out.push_back(e);
    return out;
}

// Alt+クリックの状態遷移: 今「e だけ」なら全部降ろす / そうでなければ e だけ残す。
enum class AltAction : unsigned char { Isolate, ClearAll };
template <class T>
inline AltAction AltClickAction(const entt::registry& reg, const Index& idx, entt::entity e)
{
    return IsIsolated<T>(reg, idx, e) ? AltAction::ClearAll : AltAction::Isolate;
}

// 数（ヘッダの小表示用）。自分に立っている数。
template <class T>
inline size_t CountSelf(const entt::registry& reg)
{
    const auto* s = reg.storage<T>();
    return s ? s->size() : 0;
}

} // namespace dx12e::hier
