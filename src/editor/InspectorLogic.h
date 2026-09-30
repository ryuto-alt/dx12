#pragma once

// ===== インスペクタの純ロジック（フェーズ 1b）=====
// ImGui / entt に依存しない（tests/inspector_logic_test.cpp が単体で検証する）。
//   ・Add Component の目録（カテゴリ / 1 行説明 / 別名）と、検索 + 最近使ったのランキング
//   ・最近使った / 折りたたみ状態 の文字列往復（prefs 保存用）
//   ・タグ集合の操作（重複 / 空白 / サジェスト）
//   ・複数選択のバイト比較（値がバラバラ = Mixed / 既定値との差）
//   ・Transform の一括編集（触った軸だけ全員へ書く）
//   ・プロパティ検索の一致判定

#include "editor/FuzzyMatch.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace dx12e::insp
{

// 一括編集の対象にできる選択の上限。これを超える選択（全選択で 10 万体など）では、毎フレーム全員を走査するのを避けるため
// 共通コンポーネントの表示・一括編集をしない（案内だけ出す）。
inline constexpr size_t kMaxMultiEdit = 1000;

// ---------------------------------------------------------------------------
// Add Component の目録
// ---------------------------------------------------------------------------
struct ComponentInfo
{
    const char* id;         // 安定した識別子（最近使った / 固定 ID）。例 "PointLight"
    const char* label;      // メニュー表示（UI 自動テストが "Point Light" などの部分文字列で引く。英語ラベルは維持）
    const char* category;   // 見出し
    const char* desc;       // 1 行説明（日本語）
    const char* keywords;   // 検索用の別名（日本語 / 英語）
};

// 並びがそのまま「カテゴリ内の表示順」。カテゴリの並びは kCategoryOrder。
inline constexpr ComponentInfo kCatalog[] = {
    // ---- ライト ----
    {"PointLight",        "Point Light",        "ライト",        "全方向へ広がる点光源（電球・たいまつ）",           "light 光 電球 ライト point"},
    {"DirectionalLight",  "Directional Light",  "ライト",        "太陽のような平行光。影の向きを決める",              "light 太陽 sun 平行光 directional"},
    {"SpotLight",         "Spot Light",         "ライト",        "円錐状に照らす光（懐中電灯・スポット）",            "light スポット 懐中電灯 spot cone"},
    // ---- カメラ ----
    {"CameraComponent",   "Camera",             "カメラ",        "ゲームの視点。アクティブなカメラが絵を決める",      "camera カメラ 視点 fov"},
    // ---- 描画 ----
    {"Sprite2D",          "Sprite2D",           "描画",          "2D スプライト（HUD / ワールド配置）",               "sprite 2d スプライト 画像 image"},
    {"ParticleEmitter",   "Particle Emitter",   "描画",          "パーティクルの発生源（炎・煙・火花）",              "particle vfx effect パーティクル エフェクト 炎"},
    {"TrailRenderer",     "Trail Renderer",     "描画",          "動いた軌跡に帯を引く",                              "trail 軌跡 ライン ribbon"},
    {"DecalComponent",    "Decal",              "描画",          "面へ貼り付ける絵（傷・血痕・ひび割れ）",            "decal デカール 貼り付け"},
    {"VirtualGeometry",   "Virtual Geometry (.vgeo・P2 は統計のみ)", "描画", "仮想ジオメトリ（.vgeo）。P2 時点は統計のみ", "virtual geometry vgeo nanite"},
    {"FoliageLayer",      "Foliage Layer (植生・GPU カリング + 風)", "描画", "1 コンポーネント = 数十万〜百万のインスタンス（草・木）。エンティティにせず GPU で描く", "foliage vegetation grass tree wind 植生 草 木 風 instancing"},
    // ---- オーディオ ----
    {"AudioSource",       "Audio Source",       "オーディオ",    "音を鳴らす発生源（3D 定位 / ループ）",              "audio sound 音 サウンド bgm se"},
    {"AudioReverbZone",   "Audio Reverb Zone (部屋・廊下・洞窟の響き)", "オーディオ", "空間ごとの残響（部屋・廊下・洞窟）", "reverb 残響 リバーブ 響き"},
    // ---- 物理 ----
    {"Physics",           "Physics (剛体 + コライダー自動)", "物理", "剛体と、メッシュから作る凸包コライダーをまとめて付ける", "physics rigidbody 物理 剛体 重力 自動"},
    {"RigidBody",         "RigidBody",          "物理",          "剛体。重力・衝突で動く / 動かさない",               "rigidbody physics 剛体 物理 重力 質量"},
    {"BoxCollider",       "Box Collider",       "物理",          "箱形の当たり判定",                                  "collider 当たり判定 box 箱"},
    {"SphereCollider",    "Sphere Collider",    "物理",          "球形の当たり判定",                                  "collider 当たり判定 sphere 球"},
    {"CapsuleCollider",   "Capsule Collider",   "物理",          "カプセル形の当たり判定（キャラ向き）",              "collider 当たり判定 capsule カプセル"},
    {"CharacterController","Character Controller","物理",        "キャラクター移動用の物理コントローラ",              "character controller キャラ 移動 歩行"},
    // ---- UI ----
    {"UICanvas",          "UI Canvas",          "UI",            "ゲーム内 UI の土台（基準解像度・表示順）",          "ui canvas キャンバス hud"},
    {"UIRect",            "UI Rect",            "UI",            "UI 要素の位置・大きさ・アンカー",                   "ui rect 矩形 アンカー レイアウト"},
    {"UIImage",           "UI Image",           "UI",            "UI の画像",                                         "ui image 画像 アイコン"},
    {"UIText",            "UI Text",            "UI",            "UI の文字",                                         "ui text 文字 テキスト ラベル"},
    {"UIButton",          "UI Button",          "UI",            "押せるボタン（ホバー / 押下の色）",                 "ui button ボタン"},
    {"UISlider",          "UI Slider",          "UI",            "つまみで値を動かすスライダー",                      "ui slider スライダー ゲージ"},
    {"UIToggle",          "UI Toggle",          "UI",            "オン / オフの切替",                                 "ui toggle チェック スイッチ"},
    {"UIScrollView",      "UI Scroll View",     "UI",            "はみ出した中身をスクロールする枠",                  "ui scroll スクロール リスト"},
    {"UILayout",          "UI Layout (VBox/HBox/Grid)", "UI",    "子を縦 / 横 / 格子に自動で並べる",                  "ui layout vbox hbox grid 並べる"},
    {"UIAnimator",        "UI Animator",        "UI",            "UI のフェード・移動などの簡易アニメ",               "ui animator アニメ フェード"},
    // ---- アニメーション ----
    {"UIAnimPlayer",      "UI Anim Player (.uianim クリップ)", "アニメーション", ".uianim のタイムラインを再生する", "ui anim player uianim タイムライン"},
    {"SpriteAnimator",    "Sprite Animator (.spranim シート)", "アニメーション", ".spranim のスプライトシートを再生する", "sprite animator spranim パラパラ"},
    {"AnimatorController","Animator Controller (.animfsm ステートマシン)", "アニメーション", "状態遷移でアニメを切り替える（.animfsm）", "animator controller fsm ステートマシン"},
    {"FootIK",            "Foot IK (接地補正・Play 中のみ)", "アニメーション", "足を地面に合わせる接地補正（Play 中のみ）", "foot ik 接地 足"},
    // ---- ゲームプレイ ----
    {"Gimmick",           "Gimmick",            "ゲームプレイ",  "動く床・回転・往復などの仕掛け",                    "gimmick 仕掛け 動く床 回転"},
    {"Trigger",           "Trigger",            "ゲームプレイ",  "入ったら何かを起こす領域",                          "trigger 領域 イベント 範囲"},
    {"Brain",             "Brain (ゲーム AI の頭脳・Play 中のみ)", "ゲームプレイ", "ゲーム AI の頭脳（Play 中のみ）", "brain ai 頭脳 敵"},
    // ---- ネットワーク ----
    {"NetworkIdentity",   "Network Identity",   "ネットワーク",  "マルチプレイで同一物として扱う識別子",              "network identity マルチプレイ 同期"},
    {"NetworkTransform",  "Network Transform",  "ネットワーク",  "位置・回転をネットワーク同期する",                  "network transform 同期 補間"},
};
inline constexpr int kCatalogCount = static_cast<int>(sizeof(kCatalog) / sizeof(kCatalog[0]));

inline constexpr const char* kCategoryOrder[] = {
    "ライト", "カメラ", "描画", "オーディオ", "物理", "UI", "アニメーション", "ゲームプレイ", "ネットワーク",
};
inline constexpr int kCategoryCount = static_cast<int>(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));

inline int CategoryRank(const char* cat)
{
    for (int i = 0; i < kCategoryCount; ++i)
        if (std::strcmp(kCategoryOrder[i], cat) == 0) return i;
    return kCategoryCount;
}

inline int FindCatalog(std::string_view id)
{
    for (int i = 0; i < kCatalogCount; ++i)
        if (id == kCatalog[i].id) return i;
    return -1;
}

// 検索スコア（高いほど上）。不一致 = -1、空クエリ = 0。
// ラベルの一致を最優先し、次にカテゴリ、別名、説明の順（別名 / 説明だけの一致は大きく減点）。
inline int ScoreComponent(const ComponentInfo& c, std::string_view query)
{
    if (query.empty()) return 0;
    int best = -1;
    auto consider = [&](int s, int bonus)
    {
        if (s >= 0) best = (std::max)(best, s + bonus);
    };
    consider(fuzzy::Score(query, c.label), 1000);
    consider(fuzzy::Score(query, c.id), 900);
    consider(fuzzy::Score(query, c.category), 300);
    consider(fuzzy::Score(query, c.keywords), 200);
    consider(fuzzy::Score(query, c.desc), 0);
    return best;
}

// 最近使った id 列（先頭ほど新しい）。
using RecentList = std::vector<std::string>;

// id を先頭へ（重複は 1 個に畳む）。最大 maxN 件。
inline void PushRecent(RecentList& list, std::string_view id, size_t maxN = 8)
{
    if (id.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), std::string(id)), list.end());
    list.insert(list.begin(), std::string(id));
    if (list.size() > maxN) list.resize(maxN);
}

// ',' 区切りの文字列 <-> 配列（prefs 保存用。空要素は捨てる）。
inline std::string JoinList(const std::vector<std::string>& v, char sep = ',')
{
    std::string s;
    for (const std::string& x : v)
    {
        if (x.empty()) continue;
        if (!s.empty()) s += sep;
        s += x;
    }
    return s;
}
inline std::vector<std::string> SplitList(std::string_view s, char sep = ',')
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size())
    {
        size_t e = s.find(sep, pos);
        if (e == std::string_view::npos) e = s.size();
        if (e > pos) out.emplace_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return out;
}

// 表示に出す並び。空クエリ: 最近使った（未所持のもの）→ カテゴリ順。クエリあり: スコア降順（同点は目録順）。
// owned(id) が true のものは出さない（既に付いている）。戻り値は kCatalog の添字。
// outRecentCount: 先頭の何件が「最近使った」枠か（空クエリのときだけ > 0）。
template <class OwnedFn>
inline std::vector<int> RankCatalog(std::string_view query, const RecentList& recent, OwnedFn&& owned,
                                    int* outRecentCount = nullptr)
{
    std::vector<int> out;
    if (outRecentCount) *outRecentCount = 0;
    if (query.empty())
    {
        std::vector<char> used(static_cast<size_t>(kCatalogCount), 0);
        for (const std::string& id : recent)
        {
            const int i = FindCatalog(id);
            if (i < 0 || used[static_cast<size_t>(i)] || owned(kCatalog[i].id)) continue;
            used[static_cast<size_t>(i)] = 1;
            out.push_back(i);
        }
        if (outRecentCount) *outRecentCount = static_cast<int>(out.size());
        // 残りをカテゴリ順（同カテゴリは目録順）
        for (int c = 0; c <= kCategoryCount; ++c)
            for (int i = 0; i < kCatalogCount; ++i)
            {
                if (used[static_cast<size_t>(i)] || owned(kCatalog[i].id)) continue;
                if (CategoryRank(kCatalog[i].category) != c) continue;
                out.push_back(i);
            }
        return out;
    }
    std::vector<std::pair<int, int>> scored;   // (score, index)
    for (int i = 0; i < kCatalogCount; ++i)
    {
        if (owned(kCatalog[i].id)) continue;
        const int s = ScoreComponent(kCatalog[i], query);
        if (s >= 0) scored.emplace_back(s, i);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& p : scored) out.push_back(p.second);
    return out;
}

// ---------------------------------------------------------------------------
// タグ集合
// ---------------------------------------------------------------------------
inline std::string TrimTag(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    std::string t(s.substr(b, e - b));
    // 全角空白（U+3000）も両端から落とす
    const char* ws = "\xE3\x80\x80";
    while (t.size() >= 3 && t.compare(0, 3, ws) == 0) t.erase(0, 3);
    while (t.size() >= 3 && t.compare(t.size() - 3, 3, ws) == 0) t.erase(t.size() - 3);
    return t;
}

// 追加。空 / 重複（大小区別あり）は false。
inline bool AddTag(std::vector<std::string>& tags, std::string_view raw)
{
    const std::string t = TrimTag(raw);
    if (t.empty()) return false;
    if (std::find(tags.begin(), tags.end(), t) != tags.end()) return false;
    tags.push_back(t);
    return true;
}

inline bool RemoveTag(std::vector<std::string>& tags, std::string_view t)
{
    const auto it = std::find(tags.begin(), tags.end(), std::string(t));
    if (it == tags.end()) return false;
    tags.erase(it);
    return true;
}

// シーン内の全タグ（allTags は重複ありでよい）から、既に付いているものを除いて query に合う候補を返す。
// 空クエリ = 出現頻度の高い順。クエリあり = ファジースコア順。最大 maxN 件。
inline std::vector<std::string> SuggestTags(const std::vector<std::string>& allTags,
                                            const std::vector<std::string>& existing,
                                            std::string_view query, size_t maxN = 8)
{
    std::vector<std::pair<std::string, int>> counts;   // (tag, 出現数)
    for (const std::string& t : allTags)
    {
        if (t.empty() || std::find(existing.begin(), existing.end(), t) != existing.end()) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& p) { return p.first == t; });
        if (it == counts.end()) counts.emplace_back(t, 1);
        else ++it->second;
    }
    std::vector<std::pair<int, size_t>> ranked;   // (score, counts index)
    for (size_t i = 0; i < counts.size(); ++i)
    {
        int s = 0;
        if (!query.empty())
        {
            s = fuzzy::Score(query, counts[i].first);
            if (s < 0) continue;
        }
        ranked.emplace_back(s * 1000 + counts[i].second, i);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (const auto& r : ranked)
    {
        if (out.size() >= maxN) break;
        out.push_back(counts[r.second].first);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 複数選択のバイト比較
// ---------------------------------------------------------------------------
// primary の [offset, offset+n) が他のどれかと違うか（= 値がバラバラ）。
inline bool BytesMixed(const unsigned char* primary, const unsigned char* const* others, int nOthers,
                       size_t n)
{
    for (int i = 0; i < nOthers; ++i)
        if (others[i] && std::memcmp(primary, others[i], n) != 0) return true;
    return false;
}

// 既定値と違うか。def が null なら「違わない」扱い（印を出さない）。
inline bool BytesDiffer(const unsigned char* cur, const unsigned char* def, size_t n)
{
    return def != nullptr && std::memcmp(cur, def, n) != 0;
}

// p が [base, base+size) に n バイトぶん収まっているなら offset を返す。収まらなければ -1。
inline long OffsetIn(const void* base, size_t size, const void* p, size_t n)
{
    if (!base || !p) return -1;
    const auto b = reinterpret_cast<std::ptrdiff_t>(base);
    const auto q = reinterpret_cast<std::ptrdiff_t>(p);
    const std::ptrdiff_t off = q - b;
    if (off < 0 || static_cast<size_t>(off) + n > size) return -1;
    return static_cast<long>(off);
}

// ---------------------------------------------------------------------------
// Transform の一括編集
// ---------------------------------------------------------------------------
// 方針（Unity / UE と同じ）: 触った軸だけを「その値」で全員へ上書きする。
//   触っていない軸は各自の値のまま（位置 X だけ動かしても Y / Z は揃わない）。
//   edited = プライマリの編集後の値、before = 編集前の値。before と違う軸が「触った軸」。
struct Vec3 { float x, y, z; };

inline bool AxisTouched(float before, float after) { return before != after; }

// dst の触った軸だけを edited の値にする。戻り値 = 1 つでも書き換えたか。
inline bool ApplyTouchedAxes(Vec3& dst, const Vec3& before, const Vec3& edited)
{
    bool ch = false;
    if (AxisTouched(before.x, edited.x) && dst.x != edited.x) { dst.x = edited.x; ch = true; }
    if (AxisTouched(before.y, edited.y) && dst.y != edited.y) { dst.y = edited.y; ch = true; }
    if (AxisTouched(before.z, edited.z) && dst.z != edited.z) { dst.z = edited.z; ch = true; }
    return ch;
}

// 相対編集（差分を各自へ足す。Alt を押しながらの操作などに割り当てる想定）。
inline bool ApplyDeltaAxes(Vec3& dst, const Vec3& before, const Vec3& edited)
{
    const float dx = edited.x - before.x, dy = edited.y - before.y, dz = edited.z - before.z;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return false;
    dst.x += dx; dst.y += dy; dst.z += dz;
    return true;
}

// ---------------------------------------------------------------------------
// プロパティ検索
// ---------------------------------------------------------------------------
// filter が空 = 全部一致。ラベルは「日本語 English」の 2 言語混在なのでファジー（部分列）で見る。
inline bool PropertyMatches(std::string_view filter, std::string_view label)
{
    if (filter.empty()) return true;
    return fuzzy::Score(filter, label) >= 0;
}

// ---------------------------------------------------------------------------
// 折りたたみ状態（コンポーネント名 -> 開いているか）
// ---------------------------------------------------------------------------
// "Transform=1;MeshRenderer=0" 形式。名前に ';' '=' は入らない前提（コンポーネント名）。
struct FoldEntry { std::string name; bool open; };
using FoldList = std::vector<FoldEntry>;

inline void SetFold(FoldList& f, std::string_view name, bool open)
{
    for (FoldEntry& e : f)
        if (e.name == name) { e.open = open; return; }
    f.push_back({std::string(name), open});
}
inline int GetFold(const FoldList& f, std::string_view name)   // -1 = 記録なし / 0 = 閉 / 1 = 開
{
    for (const FoldEntry& e : f)
        if (e.name == name) return e.open ? 1 : 0;
    return -1;
}
inline std::string EncodeFold(const FoldList& f)
{
    std::string s;
    for (const FoldEntry& e : f)
    {
        if (!s.empty()) s += ';';
        s += e.name; s += '='; s += e.open ? '1' : '0';
    }
    return s;
}
inline FoldList DecodeFold(std::string_view s)
{
    FoldList out;
    for (const std::string& item : SplitList(s, ';'))
    {
        const size_t eq = item.rfind('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) continue;
        out.push_back({item.substr(0, eq), item[eq + 1] == '1'});
    }
    return out;
}

// ---------------------------------------------------------------------------
// 値のコピー / ペースト（行の右クリック。型タグ付きテキスト）
// ---------------------------------------------------------------------------
// 形式: "dx12v:f:1.5,2,3" / "dx12v:i:5" / "dx12v:b:1"。他のテキストは貼り付け不可（false）。
inline std::string EncodeFloats(const float* v, int n)
{
    std::string s = "dx12v:f:";
    char buf[48];
    for (int i = 0; i < n; ++i)
    {
        if (i) s += ',';
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v[i]));
        s += buf;
    }
    return s;
}
inline std::string EncodeInt(int v) { return "dx12v:i:" + std::to_string(v); }
inline std::string EncodeBool(bool v) { return std::string("dx12v:b:") + (v ? "1" : "0"); }

// 数値を n 個（float / int どちらの型タグでも可）読む。個数が合わない / 壊れていれば false。
inline bool DecodeNumbers(std::string_view text, int n, double* out)
{
    constexpr std::string_view kf = "dx12v:f:", ki = "dx12v:i:", kb = "dx12v:b:";
    std::string_view body;
    if (text.substr(0, kf.size()) == kf) body = text.substr(kf.size());
    else if (text.substr(0, ki.size()) == ki) body = text.substr(ki.size());
    else if (text.substr(0, kb.size()) == kb) body = text.substr(kb.size());
    else return false;
    int got = 0;
    size_t pos = 0;
    while (pos <= body.size() && got < n)
    {
        size_t e = body.find(',', pos);
        if (e == std::string_view::npos) e = body.size();
        const std::string tok(body.substr(pos, e - pos));
        if (tok.empty()) return false;
        char* endp = nullptr;
        const double d = std::strtod(tok.c_str(), &endp);
        if (endp == tok.c_str() || *endp != '\0') return false;
        out[got++] = d;
        pos = e + 1;
    }
    return got == n && pos > body.size();
}

} // namespace dx12e::insp
