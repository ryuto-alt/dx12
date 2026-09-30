#pragma once

// ===== アセットブラウザの純ロジック（ImGui / D3D12 / Windows に依存しない）=====
// tests/asset_browser_logic_test.cpp が直接叩く。パネル（panels/AssetBrowserPanel.cpp）と
// 走査ワーカー（AssetIndex.cpp）はここの関数だけを使う。
//
//   ・拡張子の分類 / 種別ラベル
//   ・ディレクトリ走査（1 階層 or 名前検索の再帰。件数上限つき）と、走査結果の差分判定
//   ・自然順ソート（tex_2 < tex_10）/ 種別フィルタ / 名前トークン検索
//   ・グリッドの行列計算と可視行の算出（ImGuiListClipper 相当を ImGui 抜きで）
//   ・複数選択（Ctrl / Shift / 矢印）の状態遷移
//   ・パンくず / フォルダ移動の可否 / OS ドロップの取り込み計画（拡張子ホワイトリスト・名前衝突の連番）
//   ・サムネイルのキャッシュキー / .utb ファイル（ヘッダ付き RGBA）の読み書き

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dx12e::abl
{

// ============================================================ 種別

// 並びは AssetBrowserPanel::AssetType と完全に同じ（CommandPalette が int へキャストして使っている）。
enum class Kind : int
{
    Folder = 0, Model, Texture, Scene, Script, Audio, Prefab, Shader, Material,
    UiAnim, SpriteSheet, MaterialGraph, Other,
};
inline constexpr int kKindCount = 13;

inline std::string LowerAscii(std::string_view s)
{
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// ext は ".png" のように点つき。大文字小文字は問わない。
inline Kind ClassifyExtension(std::string_view extIn)
{
    const std::string ext = LowerAscii(extIn);
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj" || ext == ".vgeo") return Kind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || ext == ".tga" || ext == ".bmp") return Kind::Texture;
    if (ext == ".json")    return Kind::Scene;
    if (ext == ".lua")     return Kind::Script;
    if (ext == ".hlsl" || ext == ".hlsli") return Kind::Shader;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") return Kind::Audio;
    if (ext == ".prefab")  return Kind::Prefab;
    if (ext == ".dxmat")   return Kind::Material;
    if (ext == ".dxmg")    return Kind::MaterialGraph;
    if (ext == ".uianim")  return Kind::UiAnim;
    if (ext == ".spranim") return Kind::SpriteSheet;
    return Kind::Other;
}

// UI に出す種別名（日本語主・チップの文言）。
inline const char* KindLabel(Kind k)
{
    switch (k)
    {
    case Kind::Folder:        return "フォルダ";
    case Kind::Model:         return "3D モデル";
    case Kind::Texture:       return "テクスチャ";
    case Kind::Scene:         return "シーン";
    case Kind::Script:        return "スクリプト";
    case Kind::Audio:         return "オーディオ";
    case Kind::Prefab:        return "プレハブ";
    case Kind::Shader:        return "シェーダー";
    case Kind::Material:      return "マテリアル";
    case Kind::UiAnim:        return "UI アニメ";
    case Kind::SpriteSheet:   return "スプライト";
    case Kind::MaterialGraph: return "マテリアルグラフ";
    default:                  return "その他";
    }
}

// 短い種別名（ツールチップ / 詳細帯の [ ] 内。英語の識別名。従来の GetTypeIcon と同じ）。
inline const char* KindShortName(Kind k)
{
    switch (k)
    {
    case Kind::Folder:        return "Folder";
    case Kind::Model:         return "Model";
    case Kind::Texture:       return "Texture";
    case Kind::Scene:         return "Scene";
    case Kind::Script:        return "Script";
    case Kind::Audio:         return "Audio";
    case Kind::Prefab:        return "Prefab";
    case Kind::Shader:        return "Shader";
    case Kind::Material:      return "Material";
    case Kind::UiAnim:        return "UiAnim";
    case Kind::SpriteSheet:   return "SpriteSheet";
    case Kind::MaterialGraph: return "MatGraph";
    default:                  return "File";
    }
}

// ---- 種別フィルタ（ビットマスク。0 = すべて）----
inline constexpr uint32_t KindBit(Kind k) { return 1u << static_cast<int>(k); }

// チップに出す種別（フォルダ / その他は出さない）。マテリアルのチップは MaterialGraph も含める（従来の挙動）。
inline constexpr Kind kChipKinds[] = {
    Kind::Model, Kind::Texture, Kind::Material, Kind::Scene, Kind::Prefab, Kind::Script,
    Kind::Shader, Kind::Audio, Kind::UiAnim, Kind::SpriteSheet,
};
inline constexpr int kChipCount = static_cast<int>(sizeof(kChipKinds) / sizeof(kChipKinds[0]));

inline uint32_t ChipMask(Kind chip)
{
    uint32_t m = KindBit(chip);
    if (chip == Kind::Material) m |= KindBit(Kind::MaterialGraph);
    return m;
}

// 種別フィルタを通るか。フォルダは常に通る（辿れなくなるので）。mask==0 は全通し。
inline bool PassesKind(Kind k, bool isDir, uint32_t mask)
{
    if (mask == 0 || isDir) return true;
    return (mask & KindBit(k)) != 0;
}

// ============================================================ エントリ

struct Entry
{
    std::filesystem::path path;
    std::string           name;         // ファイル名（拡張子込み）
    Kind                  kind = Kind::Other;
    bool                  isDir = false;
    uint64_t              size = 0;      // バイト（フォルダは 0）
    int64_t               mtime = 0;     // last_write_time の ticks（比較専用。表示用の日時ではない）
};

// ============================================================ 並び替え

// 大文字小文字を無視した自然順比較。数字の連なりは値で比べる（tex_2 < tex_10。先頭の 0 は無視するので a01 と a1 は同順）。
inline int NaturalCompare(std::string_view a, std::string_view b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb))
        {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;   // 先頭の 0 は値として無視
            while (js + 1 < je && b[js] == '0') ++js;
            const size_t la = ie - is, lb = je - js;
            if (la != lb) return la < lb ? -1 : 1;
            const int c = std::memcmp(a.data() + is, b.data() + js, la);
            if (c != 0) return c < 0 ? -1 : 1;
            i = ie; j = je;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++i; ++j;
    }
    if (i < a.size()) return 1;    // a の方が長い
    if (j < b.size()) return -1;
    return 0;
}

enum class SortKey : int { Name = 0, Type, Modified, Size };
inline constexpr int kSortKeyCount = 4;

inline const char* SortKeyLabel(SortKey k)
{
    switch (k)
    {
    case SortKey::Name:     return "名前";
    case SortKey::Type:     return "種別";
    case SortKey::Modified: return "更新日";
    case SortKey::Size:     return "サイズ";
    }
    return "名前";
}

// フォルダは常に先頭。同順位は名前の自然順。asc=false でキーの向きだけ逆にする（フォルダ優先は変えない）。
inline bool EntryLess(const Entry& a, const Entry& b, SortKey key, bool asc)
{
    if (a.isDir != b.isDir) return a.isDir;
    int c = 0;
    switch (key)
    {
    case SortKey::Name:     c = 0; break;
    case SortKey::Type:     c = static_cast<int>(a.kind) - static_cast<int>(b.kind); break;
    case SortKey::Modified: c = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0; break;
    case SortKey::Size:     c = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0; break;
    }
    if (c != 0) return asc ? (c < 0) : (c > 0);
    const int n = NaturalCompare(a.name, b.name);
    if (n != 0) return asc ? (n < 0) : (n > 0);
    return false;
}

inline void SortEntries(std::vector<Entry>& v, SortKey key, bool asc)
{
    std::stable_sort(v.begin(), v.end(), [key, asc](const Entry& a, const Entry& b) { return EntryLess(a, b, key, asc); });
}

// ============================================================ 名前検索

// 検索語を小文字・空白区切りのトークンへ。全トークンを含む名前だけが一致（AND）。
inline std::vector<std::string> QueryTokens(std::string_view q)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : q)
    {
        if (c == ' ' || c == '\t' || c == '　')
        {
            if (!cur.empty()) { out.push_back(LowerAscii(cur)); cur.clear(); }
        }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(LowerAscii(cur));
    return out;
}

inline bool MatchesTokens(std::string_view name, const std::vector<std::string>& tokens)
{
    if (tokens.empty()) return true;
    const std::string lower = LowerAscii(name);
    for (const std::string& t : tokens)
        if (lower.find(t) == std::string::npos) return false;
    return true;
}

// ============================================================ 走査

struct ScanOptions
{
    std::filesystem::path dir;
    std::string           query;               // 空でなければ dir 以下を再帰して名前検索
    size_t                maxHits    = 400;    // 検索ヒットの上限
    size_t                maxVisit   = 20000;  // 検索で訪ねる要素数の上限（一致しない巨大ツリーで固まらない保険）
    bool                  includeHidden = false;   // "." で始まる名前（.thumbcache 等）
};

struct ScanResult
{
    std::vector<Entry> entries;
    bool   truncated = false;
    bool   dirMissing = false;
    size_t visited = 0;
};

inline int64_t ToTicks(std::filesystem::file_time_type t) { return static_cast<int64_t>(t.time_since_epoch().count()); }

inline Entry MakeEntry(const std::filesystem::directory_entry& de, std::error_code& ec)
{
    Entry e;
    e.path  = de.path();
    e.name  = e.path.filename().string();
    e.isDir = de.is_directory(ec);
    if (e.isDir)
    {
        e.kind = Kind::Folder;
    }
    else
    {
        e.kind = ClassifyExtension(e.path.extension().string());
        std::error_code ec2;
        const auto sz = de.file_size(ec2);
        e.size = ec2 ? 0 : static_cast<uint64_t>(sz);
    }
    std::error_code ec3;
    const auto mt = de.last_write_time(ec3);
    e.mtime = ec3 ? 0 : ToTicks(mt);
    return e;
}

// 並びは付けない（ソートは表示側）。
inline ScanResult ScanDirectory(const ScanOptions& opt)
{
    namespace fs = std::filesystem;
    ScanResult r;
    std::error_code ec;
    if (!fs::is_directory(opt.dir, ec)) { r.dirMissing = true; return r; }

    if (opt.query.empty())
    {
        for (fs::directory_iterator it(opt.dir, ec), end; !ec && it != end; it.increment(ec))
        {
            ++r.visited;
            std::error_code e2;
            Entry e = MakeEntry(*it, e2);
            if (!opt.includeHidden && !e.name.empty() && e.name[0] == '.') continue;   // .thumbcache 等
            r.entries.push_back(std::move(e));
        }
        return r;
    }

    const std::vector<std::string> tokens = QueryTokens(opt.query);
    for (fs::recursive_directory_iterator it(opt.dir, ec), end; it != end; it.increment(ec))
    {
        if (ec) break;
        if (++r.visited > opt.maxVisit) { r.truncated = true; break; }
        const std::string fileName = it->path().filename().string();
        if (!opt.includeHidden && !fileName.empty() && fileName[0] == '.')
        {
            std::error_code e2;
            if (it->is_directory(e2)) it.disable_recursion_pending();   // .thumbcache 等は覗かない
            continue;
        }
        if (!MatchesTokens(fileName, tokens)) continue;
        if (r.entries.size() >= opt.maxHits) { r.truncated = true; break; }
        std::error_code e2;
        r.entries.push_back(MakeEntry(*it, e2));
    }
    return r;
}

// 走査結果が「見た目に同じ」か（同じパス集合・同じ更新時刻・同じサイズ）。順序は問わない。
// 変化が無いときは UI へ結果を渡さない（選択・スクロール・サムネの再要求を起こさない）。
inline bool SameListing(const std::vector<Entry>& a, const std::vector<Entry>& b)
{
    if (a.size() != b.size()) return false;
    std::vector<const Entry*> pa, pb;
    pa.reserve(a.size()); pb.reserve(b.size());
    for (const Entry& e : a) pa.push_back(&e);
    for (const Entry& e : b) pb.push_back(&e);
    auto cmp = [](const Entry* x, const Entry* y) { return x->path < y->path; };
    std::sort(pa.begin(), pa.end(), cmp);
    std::sort(pb.begin(), pb.end(), cmp);
    for (size_t i = 0; i < pa.size(); ++i)
    {
        const Entry& x = *pa[i]; const Entry& y = *pb[i];
        if (x.path != y.path || x.mtime != y.mtime || x.size != y.size || x.isDir != y.isDir) return false;
    }
    return true;
}

// フォルダツリー用: dir の直下にあるサブフォルダ（"." 始まりは除く）。hasSub は「その中にさらにフォルダがあるか」。
struct DirInfo
{
    std::filesystem::path path;
    std::string           name;
    bool                  hasSub = false;
};

inline std::vector<DirInfo> ListSubDirs(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    std::vector<DirInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        DirInfo d;
        d.path = it->path();
        d.name = d.path.filename().string();
        if (d.name.empty() || d.name[0] == '.') continue;
        std::error_code e3;
        for (fs::directory_iterator sub(d.path, e3), sEnd; !e3 && sub != sEnd; sub.increment(e3))
        {
            std::error_code e4;
            if (sub->is_directory(e4)) { d.hasSub = true; break; }
        }
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const DirInfo& a, const DirInfo& b) { return NaturalCompare(a.name, b.name) < 0; });
    return out;
}

// ============================================================ グリッド / 可視範囲（クリッパー）

struct GridMetrics
{
    int   columns = 1;
    int   rows    = 0;
};

// availW: 使える幅。cellW: 1 セルの幅（間隔込み）。rows は itemCount を columns で割り上げ。
inline GridMetrics ComputeGrid(int itemCount, float availW, float cellW)
{
    GridMetrics g;
    g.columns = (cellW > 1.0f) ? (std::max)(1, static_cast<int>(availW / cellW)) : 1;
    g.rows = itemCount <= 0 ? 0 : (itemCount + g.columns - 1) / g.columns;
    return g;
}

struct RowRange { int first = 0; int last = 0; };   // [first, last)

// スクロール位置と表示高さから、描くべき行の範囲（上下に overscan 行のゆとり）。
inline RowRange VisibleRows(float scrollY, float viewH, float rowH, int rows, int overscan = 1)
{
    RowRange r;
    if (rows <= 0 || rowH <= 0.0f) return r;
    int first = static_cast<int>(scrollY / rowH) - overscan;
    int last  = static_cast<int>((scrollY + viewH) / rowH) + 1 + overscan;
    r.first = (std::max)(0, first);
    r.last  = (std::min)(rows, (std::max)(r.first, last));
    return r;
}

// 行範囲 → 項目番号範囲 [first, last)。
inline RowRange ItemRange(const RowRange& rows, int columns, int itemCount)
{
    RowRange r;
    r.first = (std::min)(itemCount, rows.first * columns);
    r.last  = (std::min)(itemCount, rows.last * columns);
    return r;
}

// 矢印キー移動: cursor の項目を delta 方向へ（列数 columns のグリッド。リスト表示は columns=1）。範囲外へは出ない。
inline int MoveCursor(int cursor, int columns, int dx, int dy, int itemCount)
{
    if (itemCount <= 0) return -1;
    if (cursor < 0) return 0;
    int n = cursor + dx + dy * (std::max)(1, columns);
    if (dx != 0 && n / (std::max)(1, columns) != cursor / (std::max)(1, columns) && columns > 1)
        n = cursor;   // 左右移動は行をまたがない
    return (std::max)(0, (std::min)(itemCount - 1, n));
}

// ============================================================ 複数選択

struct Selection
{
    std::unordered_set<std::string> set;   // 選択中のパス（string()）
    std::string primary;                   // 最後に選んだもの（詳細帯・改名の対象）
    std::string anchor;                    // Shift 範囲の起点

    bool Has(const std::string& p) const { return set.count(p) != 0; }
    bool Empty() const { return set.empty(); }
    void Clear() { set.clear(); primary.clear(); anchor.clear(); }
};

// クリック 1 回の状態遷移。order は今の表示順のパス一覧（フィルタ・ソート後）、index はクリックした位置。
//   通常 = 単独 / Ctrl = トグル / Shift = anchor からの範囲（Ctrl+Shift は既存の選択へ足す）。
inline void ApplyClick(Selection& s, const std::vector<std::string>& order, int index, bool ctrl, bool shift)
{
    if (index < 0 || index >= static_cast<int>(order.size())) return;
    const std::string& p = order[static_cast<size_t>(index)];
    if (shift && !s.anchor.empty())
    {
        int ia = -1;
        for (int i = 0; i < static_cast<int>(order.size()); ++i) if (order[static_cast<size_t>(i)] == s.anchor) { ia = i; break; }
        if (ia >= 0)
        {
            if (!ctrl) s.set.clear();
            const int lo = (std::min)(ia, index), hi = (std::max)(ia, index);
            for (int i = lo; i <= hi; ++i) s.set.insert(order[static_cast<size_t>(i)]);
            s.primary = p;
            return;   // anchor は動かさない
        }
    }
    if (ctrl)
    {
        if (s.set.count(p)) { s.set.erase(p); if (s.primary == p) s.primary = s.set.empty() ? std::string() : *s.set.begin(); }
        else { s.set.insert(p); s.primary = p; }
    }
    else
    {
        s.set.clear();
        s.set.insert(p);
        s.primary = p;
    }
    s.anchor = p;
}

// 選択のうち、今の一覧に残っているものだけへ絞る（フォルダ移動・削除・再走査の後）。
inline void PruneSelection(Selection& s, const std::vector<std::string>& order)
{
    std::unordered_set<std::string> alive(order.begin(), order.end());
    for (auto it = s.set.begin(); it != s.set.end();)
        it = alive.count(*it) ? std::next(it) : s.set.erase(it);
    if (!alive.count(s.primary)) s.primary = s.set.empty() ? std::string() : *s.set.begin();
    if (!alive.count(s.anchor)) s.anchor = s.primary;
}

// ============================================================ パンくず / 移動 / 取り込み

struct Crumb
{
    std::string           label;
    std::filesystem::path path;
};

// root（ラベル rootLabel）から cur までの各階層。cur が root の外なら root だけ。
inline std::vector<Crumb> Breadcrumbs(const std::filesystem::path& root, const std::string& rootLabel,
                                      const std::filesystem::path& cur)
{
    namespace fs = std::filesystem;
    std::vector<Crumb> out;
    out.push_back({rootLabel, root});
    std::error_code ec;
    const fs::path rel = fs::relative(cur, root, ec);
    if (ec || rel.empty() || rel == ".") return out;
    fs::path acc = root;
    for (const fs::path& part : rel)
    {
        if (part == "..") return { Crumb{rootLabel, root} };   // root の外
        if (part == ".") continue;
        acc /= part;
        out.push_back({part.string(), acc});
    }
    return out;
}

// path が root の中（または root 自身）か（lexical。実在は見ない）。
inline bool IsInside(const std::filesystem::path& path, const std::filesystem::path& root)
{
    const auto a = path.lexically_normal();
    const auto b = root.lexically_normal();
    auto ia = a.begin(), ib = b.begin();
    for (; ib != b.end(); ++ia, ++ib)
    {
        if (ib->empty()) continue;   // 末尾の区切りが作る空要素
        if (ia == a.end() || *ia != *ib) return false;
    }
    return true;
}

enum class MoveCheck : int { Ok = 0, IntoSelf, IntoDescendant, SameFolder, NameConflict, SourceMissing, DestMissing };

// src をフォルダ destDir へ移せるか。existsFn は destDir/名前 の存在確認（テストで差し替える）。
inline MoveCheck CheckMove(const std::filesystem::path& src, const std::filesystem::path& destDir,
                           const std::function<bool(const std::filesystem::path&)>& existsFn)
{
    if (!existsFn(src)) return MoveCheck::SourceMissing;
    if (!existsFn(destDir)) return MoveCheck::DestMissing;
    const auto s = src.lexically_normal();
    const auto d = destDir.lexically_normal();
    if (s == d) return MoveCheck::IntoSelf;
    if (IsInside(d, s)) return MoveCheck::IntoDescendant;
    if (s.parent_path() == d) return MoveCheck::SameFolder;
    if (existsFn(d / s.filename())) return MoveCheck::NameConflict;
    return MoveCheck::Ok;
}

inline const char* MoveCheckMessage(MoveCheck c)
{
    switch (c)
    {
    case MoveCheck::Ok:             return "";
    case MoveCheck::IntoSelf:       return "自分自身の中には移動できません";
    case MoveCheck::IntoDescendant: return "自分の中のフォルダには移動できません";
    case MoveCheck::SameFolder:     return "すでにそのフォルダにあります";
    case MoveCheck::NameConflict:   return "同じ名前のファイルがあります";
    case MoveCheck::SourceMissing:  return "移動元が見つかりません";
    case MoveCheck::DestMissing:    return "移動先が見つかりません";
    }
    return "";
}

// 名前が衝突したときの連番: "a.png" → "a_1.png" → "a_2.png"…。suffix は "_" 以外へ替えられる（複製は "_copy"）。
inline std::string UniqueName(const std::string& fileName, const std::function<bool(const std::string&)>& existsFn,
                              const char* suffix = "_")
{
    if (!existsFn(fileName)) return fileName;
    const std::filesystem::path p(fileName);
    const std::string stem = p.stem().string();
    const std::string ext  = p.extension().string();
    for (int i = 1; i < 100000; ++i)
    {
        std::string cand;
        if (std::strcmp(suffix, "_copy") == 0) cand = stem + (i == 1 ? "_copy" : "_copy" + std::to_string(i)) + ext;
        else cand = stem + suffix + std::to_string(i) + ext;
        if (!existsFn(cand)) return cand;
    }
    return fileName;
}

// OS ドロップ / インポートで受け付ける拡張子（エディタが扱える形式だけ。実行ファイルや不明な物は取り込まない）。
inline bool IsImportableExt(std::string_view extIn)
{
    const std::string ext = LowerAscii(extIn);
    static const char* const kAllowed[] = {
        ".png", ".jpg", ".jpeg", ".dds", ".tga", ".bmp", ".hdr",
        ".gltf", ".glb", ".fbx", ".obj", ".bin", ".vgeo",
        ".wav", ".mp3", ".ogg",
        ".lua", ".hlsl", ".hlsli",
        ".prefab", ".dxmat", ".dxmg", ".uianim", ".spranim", ".json",
    };
    for (const char* a : kAllowed) if (ext == a) return true;
    return false;
}

struct ImportSource
{
    std::filesystem::path abs;       // 取り込み元（ファイル）
    std::filesystem::path relDir;    // 取り込み先フォルダの下での相対フォルダ（フォルダごとのドロップで階層を保つ。通常は空）
};

struct CopyOp
{
    std::filesystem::path src;
    std::filesystem::path dst;
    bool   skipped = false;
    std::string reason;              // skipped のとき: 拡張子が対象外 / 元が見つからない など
};

// 取り込み計画: 拡張子ホワイトリスト → 名前衝突は連番。existsFn は dst の存在確認（同じ計画内の先行 op も「在る」と見なす）。
inline std::vector<CopyOp> PlanImport(const std::vector<ImportSource>& srcs, const std::filesystem::path& destDir,
                                      const std::function<bool(const std::filesystem::path&)>& existsFn)
{
    std::vector<CopyOp> ops;
    std::unordered_set<std::string> planned;
    for (const ImportSource& s : srcs)
    {
        CopyOp op;
        op.src = s.abs;
        const std::filesystem::path dir = s.relDir.empty() ? destDir : destDir / s.relDir;
        const std::string name = s.abs.filename().string();
        if (!IsImportableExt(s.abs.extension().string()))
        {
            op.skipped = true;
            op.reason = "対象外の形式";
            op.dst = dir / name;
            ops.push_back(std::move(op));
            continue;
        }
        auto exists = [&](const std::string& n) {
            const std::filesystem::path c = dir / n;
            return existsFn(c) || planned.count(c.lexically_normal().string()) != 0;
        };
        const std::string unique = UniqueName(name, exists);
        op.dst = dir / unique;
        planned.insert(op.dst.lexically_normal().string());
        ops.push_back(std::move(op));
    }
    return ops;
}

// ============================================================ 参照の検索（名前変更 / 移動の前の警告用）

// アセットは「assets からの相対パスの文字列」で参照される（シーン JSON・プレハブ・マテリアル・Lua 等）。
// GUID 化されていない現状では、名前変更 / 移動で参照が黙って切れる。実行前に「どのファイルが参照しているか」を調べて警告する。
struct RefScanResult
{
    std::vector<std::string> files;   // 参照しているファイル（roots からの相対。区切りは /）
    bool   truncated = false;         // 上限（ファイル数 / 時間）で打ち切った
    size_t scanned = 0;
};

inline bool IsReferenceHolderExt(std::string_view extIn)
{
    const std::string ext = LowerAscii(extIn);
    return ext == ".json" || ext == ".prefab" || ext == ".dxmat" || ext == ".dxmg" || ext == ".uianim" || ext == ".spranim"
        || ext == ".lua" || ext == ".hlsl" || ext == ".hlsli";
}

// roots 以下のテキスト系アセットを読み、needles（"models/a.glb" のような相対パス文字列。フォルダは末尾 "/"）を含む物を集める。
// excluded が true を返すファイル（移動する本人など）は数えない。"." 始まりの場所は覗かない。
inline RefScanResult FindReferences(const std::vector<std::filesystem::path>& roots, const std::vector<std::string>& needles,
                                    const std::function<bool(const std::filesystem::path&)>& excluded,
                                    size_t maxFiles = 20000, size_t maxHits = 30, double budgetMs = 1500.0)
{
    namespace fs = std::filesystem;
    RefScanResult r;
    if (needles.empty()) return r;
    const auto t0 = std::chrono::steady_clock::now();
    for (const fs::path& root : roots)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
        {
            if (ec) break;
            const std::string name = it->path().filename().string();
            std::error_code e2;
            if (!name.empty() && name[0] == '.') { if (it->is_directory(e2)) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(e2)) continue;
            if (!IsReferenceHolderExt(it->path().extension().string())) continue;
            if (++r.scanned > maxFiles) { r.truncated = true; return r; }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budgetMs) { r.truncated = true; return r; }
            if (excluded && excluded(it->path())) continue;
            const auto sz = it->file_size(e2);
            if (e2 || sz > 8ull * 1024 * 1024) continue;
            std::ifstream ifs(it->path(), std::ios::binary);
            if (!ifs) continue;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            for (const std::string& n : needles)
            {
                if (!n.empty() && content.find(n) != std::string::npos)
                {
                    std::error_code e3;
                    r.files.push_back(fs::relative(it->path(), root, e3).generic_string());
                    break;
                }
            }
            if (r.files.size() >= maxHits) { r.truncated = true; return r; }
        }
    }
    return r;
}

// ============================================================ サムネイルのキャッシュ

inline uint64_t Fnv1a64(std::string_view s, uint64_t h = 1469598103934665603ull)
{
    for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
    return h;
}

// パスの表記ゆれ（区切り / 大小）を吸収した正規形。Windows のパスは大小を区別しない。
inline std::string NormalizeKeyPath(const std::string& path)
{
    std::string s = LowerAscii(path);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

inline constexpr uint32_t kThumbVersion = 2;   // 描画側（ライティング等）を変えたら上げる = 既存キャッシュを無効にする（2: サムネイルの packedTint 修正）
inline constexpr uint32_t kThumbDim     = 128; // サムネイルの長辺 px

// キャッシュキー = パス + 更新時刻 + サイズ + 生成バージョン。どれかが変われば別のキー。
inline uint64_t ThumbKey(const std::string& path, int64_t mtime, uint64_t size, uint32_t version = kThumbVersion)
{
    uint64_t h = Fnv1a64(NormalizeKeyPath(path));
    h = Fnv1a64(std::string_view(reinterpret_cast<const char*>(&mtime), sizeof(mtime)), h);
    h = Fnv1a64(std::string_view(reinterpret_cast<const char*>(&size), sizeof(size)), h);
    h = Fnv1a64(std::string_view(reinterpret_cast<const char*>(&version), sizeof(version)), h);
    return h;
}

// ディスク上のファイル名はパスだけで決める（同じ物の古い版を上書きできる = 溜まらない）。中身のヘッダで鮮度を確かめる。
inline std::string ThumbFileName(const std::string& path)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.utb", static_cast<unsigned long long>(Fnv1a64(NormalizeKeyPath(path))));
    return buf;
}

#pragma pack(push, 1)
struct ThumbHeader
{
    char     magic[4] = {'U', 'T', 'B', '1'};
    uint32_t version  = kThumbVersion;
    uint32_t width    = 0;
    uint32_t height   = 0;
    int64_t  srcMtime = 0;
    uint64_t srcSize  = 0;
    uint64_t reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(ThumbHeader) == 40, "ThumbHeader は 40 バイト固定（ディスク形式）");

inline bool WriteThumbFile(const std::filesystem::path& file, uint32_t w, uint32_t h, int64_t srcMtime, uint64_t srcSize,
                           const uint8_t* rgba)
{
    if (w == 0 || h == 0 || !rgba) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    ThumbHeader hd;
    hd.width = w; hd.height = h; hd.srcMtime = srcMtime; hd.srcSize = srcSize;
    // 途中で落ちても壊れたファイルを残さない: 一時名へ書いて rename
    const std::filesystem::path tmp = file.wstring() + L".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(&hd), sizeof(hd));
        ofs.write(reinterpret_cast<const char*>(rgba), static_cast<std::streamsize>(w) * h * 4);
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

// 鮮度（mtime / size / version）が一致するときだけ true。旧形式（ヘッダ無しの 65536 バイト）は magic が合わず false。
inline bool ReadThumbFile(const std::filesystem::path& file, int64_t srcMtime, uint64_t srcSize,
                          uint32_t& outW, uint32_t& outH, std::vector<uint8_t>& outRgba)
{
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) return false;
    ThumbHeader hd;
    ifs.read(reinterpret_cast<char*>(&hd), sizeof(hd));
    if (ifs.gcount() != static_cast<std::streamsize>(sizeof(hd))) return false;
    if (std::memcmp(hd.magic, "UTB1", 4) != 0 || hd.version != kThumbVersion) return false;
    if (hd.srcMtime != srcMtime || hd.srcSize != srcSize) return false;
    if (hd.width == 0 || hd.height == 0 || hd.width > 1024 || hd.height > 1024) return false;
    const size_t bytes = static_cast<size_t>(hd.width) * hd.height * 4;
    outRgba.resize(bytes);
    ifs.read(reinterpret_cast<char*>(outRgba.data()), static_cast<std::streamsize>(bytes));
    if (ifs.gcount() != static_cast<std::streamsize>(bytes)) { outRgba.clear(); return false; }
    outW = hd.width; outH = hd.height;
    return true;
}

// 長辺を maxDim に収める縮小サイズ（縦横比を保つ。既に収まっていれば等倍）。
inline void FitThumbSize(uint32_t srcW, uint32_t srcH, uint32_t maxDim, uint32_t& outW, uint32_t& outH)
{
    if (srcW == 0 || srcH == 0) { outW = outH = 0; return; }
    if (srcW <= maxDim && srcH <= maxDim) { outW = srcW; outH = srcH; return; }
    if (srcW >= srcH) { outW = maxDim; outH = (std::max)(1u, static_cast<uint32_t>((static_cast<uint64_t>(srcH) * maxDim + srcW / 2) / srcW)); }
    else              { outH = maxDim; outW = (std::max)(1u, static_cast<uint32_t>((static_cast<uint64_t>(srcW) * maxDim + srcH / 2) / srcH)); }
}

// ============================================================ 表示用の整形

inline std::string FormatSize(uint64_t bytes)
{
    char buf[32];
    if (bytes < 1024)                    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024)     std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else                                 std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

} // namespace dx12e::abl
