// アセットブラウザの純ロジック（editor/AssetBrowserLogic.h）と非同期インデックス（editor/AssetIndex.cpp）の単体テスト（フェーズ 1b A）。
//   ・拡張子の分類 / 種別フィルタ / 自然順ソート / 名前トークン検索
//   ・ディレクトリ走査（1 階層 / 再帰検索 / 上限 / "." 始まりの除外）と、走査結果の差分判定
//   ・グリッドの行列計算 / 可視行（クリッパー）/ 矢印キー移動
//   ・複数選択（Ctrl / Shift / 範囲 / 選択の掃除）
//   ・パンくず / 移動の可否 / 名前衝突の連番 / OS ドロップの取り込み計画（拡張子ホワイトリスト）
//   ・参照の検索（名前変更 / 移動の前の警告）
//   ・サムネイルのキャッシュキー / .utb ファイルの往復と鮮度判定 / 縮小サイズ
//   ・AssetIndex（ワーカースレッドの走査・変更監視・ツリーのサブフォルダ）
// GPU / ImGui は不要。一時フォルダは temp 配下に作って最後に消す。

#include "editor/AssetBrowserLogic.h"
#include "editor/AssetIndex.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace dx12e;
namespace fs = std::filesystem;
namespace abl = dx12e::abl;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

fs::path g_root;

void Touch(const fs::path& p, const std::string& content = "x")
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

void TestClassify()
{
    CHECK(abl::ClassifyExtension(".PNG") == abl::Kind::Texture, "大文字の拡張子");
    CHECK(abl::ClassifyExtension(".glb") == abl::Kind::Model, "glb");
    CHECK(abl::ClassifyExtension(".vgeo") == abl::Kind::Model, "vgeo");
    CHECK(abl::ClassifyExtension(".json") == abl::Kind::Scene, "json は Scene");
    CHECK(abl::ClassifyExtension(".dxmg") == abl::Kind::MaterialGraph, "dxmg");
    CHECK(abl::ClassifyExtension(".xyz") == abl::Kind::Other, "不明");
    CHECK(abl::ClassifyExtension("") == abl::Kind::Other, "空");
    // 並びが AssetBrowserPanel::AssetType と同じ（CommandPalette が int で使う）
    CHECK(static_cast<int>(abl::Kind::Folder) == 0 && static_cast<int>(abl::Kind::Model) == 1 && static_cast<int>(abl::Kind::Other) == 12, "enum の並び");
    CHECK(abl::kKindCount == 13, "種別数");

    // フィルタ: フォルダは常に通る / マテリアルのチップは MaterialGraph も含む
    CHECK(abl::PassesKind(abl::Kind::Folder, true, abl::KindBit(abl::Kind::Texture)), "フォルダは常に通る");
    CHECK(!abl::PassesKind(abl::Kind::Model, false, abl::KindBit(abl::Kind::Texture)), "モデルは落ちる");
    CHECK(abl::PassesKind(abl::Kind::Model, false, 0), "mask 0 は全通し");
    CHECK(abl::PassesKind(abl::Kind::MaterialGraph, false, abl::ChipMask(abl::Kind::Material)), "マテリアルのチップ = MaterialGraph 込み");
    CHECK(!abl::PassesKind(abl::Kind::MaterialGraph, false, abl::KindBit(abl::Kind::Material)), "素のビットでは含まない");
    CHECK(abl::kChipCount == 10, "チップ数");
}

void TestNaturalSort()
{
    CHECK(abl::NaturalCompare("tex_2", "tex_10") < 0, "2 < 10");
    CHECK(abl::NaturalCompare("tex_10", "tex_2") > 0, "10 > 2");
    CHECK(abl::NaturalCompare("Abc", "abc") == 0, "大文字小文字を無視");
    CHECK(abl::NaturalCompare("a01", "a1") == 0, "先頭の 0 は無視");
    CHECK(abl::NaturalCompare("a", "ab") < 0, "短い方が先");
    CHECK(abl::NaturalCompare("", "a") < 0, "空が先");
    CHECK(abl::NaturalCompare("file9", "file10") < 0 && abl::NaturalCompare("file10", "file11") < 0, "連番");

    std::vector<abl::Entry> v;
    auto add = [&](const char* n, bool dir, uint64_t sz, int64_t mt, abl::Kind k) {
        abl::Entry e; e.name = n; e.path = n; e.isDir = dir; e.size = sz; e.mtime = mt; e.kind = k; v.push_back(e);
    };
    add("b.png", false, 300, 5, abl::Kind::Texture);
    add("a10.lua", false, 100, 9, abl::Kind::Script);
    add("a2.lua", false, 200, 1, abl::Kind::Script);
    add("zdir", true, 0, 3, abl::Kind::Folder);
    add("adir", true, 0, 7, abl::Kind::Folder);

    abl::SortEntries(v, abl::SortKey::Name, true);
    CHECK(v[0].name == "adir" && v[1].name == "zdir", "フォルダが先頭（名前順）");
    CHECK(v[2].name == "a2.lua" && v[3].name == "a10.lua" && v[4].name == "b.png", "名前の自然順");
    abl::SortEntries(v, abl::SortKey::Name, false);
    CHECK(v[0].isDir && v[1].isDir, "降順でもフォルダが先頭");
    CHECK(v[2].name == "b.png" && v[4].name == "a2.lua", "降順");
    abl::SortEntries(v, abl::SortKey::Size, true);
    CHECK(v[2].name == "a10.lua" && v[3].name == "a2.lua" && v[4].name == "b.png", "サイズ昇順");
    abl::SortEntries(v, abl::SortKey::Modified, false);
    CHECK(v[2].name == "a10.lua" && v[4].name == "a2.lua", "更新日の降順（新しい順）");
    abl::SortEntries(v, abl::SortKey::Type, true);
    CHECK(v[2].kind == abl::Kind::Texture, "種別順（Texture < Script の enum 順）");
}

void TestQuery()
{
    const auto t = abl::QueryTokens("  Foo  bar\tBAZ ");
    CHECK(t.size() == 3 && t[0] == "foo" && t[1] == "bar" && t[2] == "baz", "トークン化");
    CHECK(abl::QueryTokens("").empty() && abl::QueryTokens("   ").empty(), "空");
    CHECK(abl::MatchesTokens("My_Foo_Bar.png", abl::QueryTokens("foo bar")), "AND 一致");
    CHECK(!abl::MatchesTokens("My_Foo.png", abl::QueryTokens("foo bar")), "片方だけは不一致");
    CHECK(abl::MatchesTokens("anything", {}), "トークン無しは全一致");
    CHECK(abl::MatchesTokens("wall.PNG", abl::QueryTokens(".png")), "拡張子での検索");
}

void TestScan()
{
    const fs::path d = g_root / "scan";
    Touch(d / "a.png"); Touch(d / "b.lua", "hello"); Touch(d / ".hidden.txt"); Touch(d / ".thumbcache" / "x.utb");
    Touch(d / "sub" / "deep" / "target_wall.png"); Touch(d / "sub" / "wall2.png"); Touch(d / "sub" / ".secret" / "wall3.png");
    fs::create_directories(d / "emptydir");

    abl::ScanOptions o;
    o.dir = d;
    abl::ScanResult r = abl::ScanDirectory(o);
    CHECK(!r.dirMissing && !r.truncated, "通常走査");
    int dirs = 0, files = 0;
    for (const auto& e : r.entries) { if (e.isDir) ++dirs; else ++files; }
    CHECK(dirs == 2 && files == 2, "フォルダ 2（sub / emptydir）+ ファイル 2（隠しは除外）: dirs=%d files=%d", dirs, files);
    bool sawSize = false;
    for (const auto& e : r.entries) if (e.name == "b.lua") sawSize = (e.size == 5 && e.kind == abl::Kind::Script && e.mtime != 0);
    CHECK(sawSize, "サイズ / 種別 / 更新時刻が入る");

    o.includeHidden = true;
    r = abl::ScanDirectory(o);
    CHECK(r.entries.size() == 6, "includeHidden で 6 件（通常 4 + 隠し 2）: %zu", r.entries.size());

    // 再帰検索
    o = abl::ScanOptions{};
    o.dir = d; o.query = "wall";
    r = abl::ScanDirectory(o);
    CHECK(r.entries.size() == 2, "wall を含む 2 件（.secret の中は覗かない）: %zu", r.entries.size());
    o.query = "TARGET WALL";
    r = abl::ScanDirectory(o);
    CHECK(r.entries.size() == 1 && r.entries[0].name == "target_wall.png", "トークン AND の検索");
    o.query = "zzz";
    r = abl::ScanDirectory(o);
    CHECK(r.entries.empty() && !r.truncated, "一致なし");
    o.query = "png"; o.maxHits = 2;
    r = abl::ScanDirectory(o);
    CHECK(r.entries.size() == 2 && r.truncated, "ヒット上限で打ち切り");
    o.maxHits = 400; o.maxVisit = 3;
    r = abl::ScanDirectory(o);
    CHECK(r.truncated, "走査数の上限で打ち切り");

    o = abl::ScanOptions{};
    o.dir = g_root / "no_such_dir";
    CHECK(abl::ScanDirectory(o).dirMissing, "存在しないフォルダ");

    // 差分判定
    o = abl::ScanOptions{};
    o.dir = d;
    auto a = abl::ScanDirectory(o).entries;
    auto b = abl::ScanDirectory(o).entries;
    std::reverse(b.begin(), b.end());
    CHECK(abl::SameListing(a, b), "順序が違うだけなら同じ");
    Touch(d / "c.png");
    auto c = abl::ScanDirectory(o).entries;
    CHECK(!abl::SameListing(a, c), "ファイルが増えたら違う");
    auto c2 = c;
    c2[0].mtime += 1;
    CHECK(!abl::SameListing(c, c2), "更新時刻が違えば違う");
    auto c3 = c;
    c3[0].size += 1;
    CHECK(!abl::SameListing(c, c3), "サイズが違えば違う");

    // サブフォルダ一覧
    const auto subs = abl::ListSubDirs(d);
    CHECK(subs.size() == 2, "サブフォルダ 2: %zu", subs.size());
    bool subHas = false, emptyHas = true;
    for (const auto& s : subs) { if (s.name == "sub") subHas = s.hasSub; if (s.name == "emptydir") emptyHas = s.hasSub; }
    CHECK(subHas && !emptyHas, "hasSub（sub には deep がある / emptydir は空）");
}

void TestGrid()
{
    auto g = abl::ComputeGrid(100, 1000.0f, 100.0f);
    CHECK(g.columns == 10 && g.rows == 10, "10x10");
    g = abl::ComputeGrid(101, 1000.0f, 100.0f);
    CHECK(g.rows == 11, "端数は行を足す");
    g = abl::ComputeGrid(5, 50.0f, 100.0f);
    CHECK(g.columns == 1 && g.rows == 5, "幅が足りなくても最低 1 列");
    g = abl::ComputeGrid(0, 500.0f, 100.0f);
    CHECK(g.rows == 0, "0 件");

    // 可視行: 10000 件・10 列 = 1000 行・行高 100・表示高 300 → 数行だけ
    const abl::RowRange vr = abl::VisibleRows(5000.0f, 300.0f, 100.0f, 1000, 1);
    CHECK(vr.first == 49 && vr.last == 55, "スクロール 5000 の可視行 [%d,%d)", vr.first, vr.last);
    const abl::RowRange ir = abl::ItemRange(vr, 10, 10000);
    CHECK(ir.first == 490 && ir.last == 550, "項目範囲 [%d,%d)", ir.first, ir.last);
    CHECK(ir.last - ir.first <= 100, "描くセルは 100 個以下（1 万件でも）");
    const abl::RowRange top = abl::VisibleRows(0.0f, 300.0f, 100.0f, 1000, 1);
    CHECK(top.first == 0, "先頭で負にならない");
    const abl::RowRange bot = abl::VisibleRows(99900.0f, 300.0f, 100.0f, 1000, 1);
    CHECK(bot.last == 1000, "末尾で行数を超えない");
    const abl::RowRange none = abl::VisibleRows(0, 100, 100, 0);
    CHECK(none.first == 0 && none.last == 0, "0 行");

    CHECK(abl::MoveCursor(0, 5, 1, 0, 20) == 1, "右");
    CHECK(abl::MoveCursor(4, 5, 1, 0, 20) == 4, "右端で行をまたがない");
    CHECK(abl::MoveCursor(5, 5, -1, 0, 20) == 5, "左端で行をまたがない");
    CHECK(abl::MoveCursor(3, 5, 0, 1, 20) == 8, "下");
    CHECK(abl::MoveCursor(3, 5, 0, -1, 20) == 0, "先頭行の上は 0 へ（範囲内に収める）");
    CHECK(abl::MoveCursor(8, 5, 0, -1, 20) == 3, "上");
    CHECK(abl::MoveCursor(18, 5, 0, 1, 20) == 19, "最終行を超えたら最後の項目");
    CHECK(abl::MoveCursor(-1, 5, 1, 0, 20) == 0, "未選択から動かすと先頭");
    CHECK(abl::MoveCursor(0, 5, 1, 0, 0) == -1, "0 件");
}

void TestSelection()
{
    const std::vector<std::string> order = {"a", "b", "c", "d", "e", "f"};
    abl::Selection s;
    abl::ApplyClick(s, order, 1, false, false);
    CHECK(s.set.size() == 1 && s.Has("b") && s.primary == "b" && s.anchor == "b", "単独選択");
    abl::ApplyClick(s, order, 4, false, true);
    CHECK(s.set.size() == 4 && s.Has("b") && s.Has("e") && s.primary == "e" && s.anchor == "b", "Shift で b〜e（anchor は動かない）");
    abl::ApplyClick(s, order, 0, false, true);
    CHECK(s.set.size() == 2 && s.Has("a") && s.Has("b") && !s.Has("e"), "Shift で anchor を軸に反対側へ");
    abl::ApplyClick(s, order, 3, true, false);
    CHECK(s.set.size() == 3 && s.Has("d") && s.anchor == "d", "Ctrl でトグル追加");
    abl::ApplyClick(s, order, 3, true, false);
    CHECK(!s.Has("d") && s.set.size() == 2, "Ctrl で外す");
    abl::ApplyClick(s, order, 5, true, true);
    CHECK(s.Has("a") && s.Has("f"), "Ctrl+Shift は既存の選択に範囲を足す");
    abl::ApplyClick(s, order, 2, false, false);
    CHECK(s.set.size() == 1 && s.Has("c"), "通常クリックで単独へ戻る");
    abl::ApplyClick(s, order, 99, false, false);
    CHECK(s.set.size() == 1, "範囲外のクリックは無視");

    // 選択の掃除
    abl::Selection t;
    for (const char* p : {"a", "b", "zzz"}) t.set.insert(p);
    t.primary = "zzz"; t.anchor = "zzz";
    abl::PruneSelection(t, order);
    CHECK(t.set.size() == 2 && !t.Has("zzz"), "消えた物を外す");
    CHECK(t.set.count(t.primary) == 1, "primary が生きている物になる");
    abl::Selection u;
    abl::PruneSelection(u, order);
    CHECK(u.Empty() && u.primary.empty(), "空でも壊れない");
}

void TestPaths()
{
    const fs::path root = "C:/proj/assets";
    auto bc = abl::Breadcrumbs(root, "assets", root / "models" / "props");
    CHECK(bc.size() == 3 && bc[0].label == "assets" && bc[1].label == "models" && bc[2].label == "props", "パンくず 3 段");
    CHECK(bc[2].path == root / "models" / "props" && bc[0].path == root, "各段のパス");
    bc = abl::Breadcrumbs(root, "assets", root);
    CHECK(bc.size() == 1, "root だけ");
    bc = abl::Breadcrumbs(root, "assets", fs::path("D:/elsewhere"));
    CHECK(bc.size() == 1, "root の外は root だけ");

    CHECK(abl::IsInside("C:/proj/assets/a/b.png", root), "中");
    CHECK(abl::IsInside(root, root), "自身");
    CHECK(!abl::IsInside("C:/proj/assets2/a.png", root), "前方一致だけの別フォルダは外");
    CHECK(!abl::IsInside("C:/proj", root), "親は外");
    CHECK(abl::IsInside("C:/proj/assets/./x/../y.png", root), "正規化");

    std::unordered_set<std::string> existing = {"C:/proj/assets/a", "C:/proj/assets/a/sub", "C:/proj/assets/b", "C:/proj/assets/b/a", "C:/proj/assets/c.png", "C:/proj/assets/b/c.png"};
    auto ex = [&](const fs::path& p) { return existing.count(p.lexically_normal().generic_string()) != 0; };
    CHECK(abl::CheckMove("C:/proj/assets/a", "C:/proj/assets/a", ex) == abl::MoveCheck::IntoSelf, "自分自身へ");
    CHECK(abl::CheckMove("C:/proj/assets/a", "C:/proj/assets/a/sub", ex) == abl::MoveCheck::IntoDescendant, "自分の子孫へ");
    CHECK(abl::CheckMove("C:/proj/assets/b/a", "C:/proj/assets/b", ex) == abl::MoveCheck::SameFolder, "同じフォルダ");
    CHECK(abl::CheckMove("C:/proj/assets/c.png", "C:/proj/assets/b", ex) == abl::MoveCheck::NameConflict, "同名あり");
    CHECK(abl::CheckMove("C:/proj/assets/a", "C:/proj/assets/b", ex) == abl::MoveCheck::NameConflict, "b/a が既にある");
    CHECK(abl::CheckMove("C:/proj/assets/nothing", "C:/proj/assets/b", ex) == abl::MoveCheck::SourceMissing, "移動元なし");
    CHECK(abl::CheckMove("C:/proj/assets/c.png", "C:/proj/assets/nothing", ex) == abl::MoveCheck::DestMissing, "移動先なし");
    existing.erase("C:/proj/assets/b/c.png");
    CHECK(abl::CheckMove("C:/proj/assets/c.png", "C:/proj/assets/b", ex) == abl::MoveCheck::Ok, "衝突が無ければ OK");
    CHECK(std::string(abl::MoveCheckMessage(abl::MoveCheck::NameConflict)).size() > 0, "メッセージ");
}

void TestUniqueAndImport()
{
    std::unordered_set<std::string> ex = {"a.png", "a_1.png", "b.png", "dir", "dir_1"};
    auto e = [&](const std::string& n) { return ex.count(n) != 0; };
    CHECK(abl::UniqueName("c.png", e) == "c.png", "衝突なし");
    CHECK(abl::UniqueName("a.png", e) == "a_2.png", "連番を飛ばして a_2");
    CHECK(abl::UniqueName("dir", e) == "dir_2", "拡張子なし");
    CHECK(abl::UniqueName("b.png", e, "_copy") == "b_copy.png", "複製は _copy");
    ex.insert("b_copy.png");
    CHECK(abl::UniqueName("b.png", e, "_copy") == "b_copy2.png", "_copy2");

    CHECK(abl::IsImportableExt(".PNG") && abl::IsImportableExt(".glb") && abl::IsImportableExt(".lua") && abl::IsImportableExt(".bin"), "許可");
    CHECK(!abl::IsImportableExt(".exe") && !abl::IsImportableExt(".dll") && !abl::IsImportableExt(".bat") && !abl::IsImportableExt(""), "実行ファイルなどは拒否");

    // 取り込み計画: 拡張子の拒否 / 衝突の連番 / 同じ計画内の衝突（同名 2 つを別々に）
    std::unordered_set<std::string> disk = {"C:/dest/tex.png"};
    auto exists = [&](const fs::path& p) { return disk.count(p.lexically_normal().generic_string()) != 0; };
    std::vector<abl::ImportSource> srcs = {
        {"D:/x/tex.png", {}}, {"D:/y/tex.png", {}}, {"D:/x/virus.exe", {}}, {"D:/x/model.glb", {}}, {"D:/x/folder/inner.png", "folder"},
    };
    const auto ops = abl::PlanImport(srcs, "C:/dest", exists);
    CHECK(ops.size() == 5, "5 件");
    CHECK(!ops[0].skipped && ops[0].dst.filename() == "tex_1.png", "既存と衝突 → tex_1: %s", ops[0].dst.string().c_str());
    CHECK(!ops[1].skipped && ops[1].dst.filename() == "tex_2.png", "計画内の同名 → tex_2: %s", ops[1].dst.string().c_str());
    CHECK(ops[2].skipped && !ops[2].reason.empty(), ".exe はスキップ");
    CHECK(!ops[3].skipped && ops[3].dst.filename() == "model.glb", "そのまま");
    CHECK(!ops[4].skipped && ops[4].dst.lexically_normal().generic_string() == "C:/dest/folder/inner.png", "フォルダ階層を保つ: %s", ops[4].dst.generic_string().c_str());
}

void TestReferences()
{
    const fs::path d = g_root / "refs";
    Touch(d / "assets" / "scenes" / "main.json", "{\"model\":\"models/a.glb\"}");
    Touch(d / "assets" / "scenes" / "other.json", "{\"model\":\"models/b.glb\"}");
    Touch(d / "assets" / "materials" / "m.dxmat", "{\"albedo\":\"textures/wall.png\"}");
    Touch(d / "assets" / "models" / "a.glb", "models/a.glb");   // .glb は参照を持つ形式ではないので走査しない
    Touch(d / "assets" / ".thumbcache" / "zz.json", "models/a.glb");
    Touch(d / "scripts" / "p.lua", "load('textures/wall.png') -- models/a.glb");

    auto none = [](const fs::path&) { return false; };
    auto r = abl::FindReferences({d / "assets", d / "scripts"}, {"models/a.glb"}, none);
    CHECK(r.files.size() == 2, "a.glb を参照するのは main.json / p.lua の 2 件（.thumbcache と .glb 自身は数えない）: %zu", r.files.size());
    auto ex = [&](const fs::path& p) { return p.filename() == "main.json"; };
    r = abl::FindReferences({d / "assets", d / "scripts"}, {"models/a.glb"}, ex);
    CHECK(r.files.size() == 1 && r.files[0] == "p.lua", "除外関数で main.json を外すと 1 件: %zu", r.files.size());
    r = abl::FindReferences({d / "assets"}, {"textures/wall.png"}, none);
    CHECK(r.files.size() == 1 && r.files[0] == "materials/m.dxmat", "マテリアルからの参照");
    r = abl::FindReferences({d / "assets"}, {"nothing/here.png"}, none);
    CHECK(r.files.empty() && !r.truncated, "参照なし");
    r = abl::FindReferences({d / "assets"}, {"models/"}, none);
    CHECK(r.files.size() >= 2, "フォルダ（末尾 /）の参照");
    r = abl::FindReferences({d / "assets", d / "scripts"}, {"models/"}, none, 20000, 1);
    CHECK(r.files.size() == 1 && r.truncated, "ヒット上限");
    r = abl::FindReferences({d / "assets"}, {"models/a.glb"}, none, 1);
    CHECK(r.truncated, "ファイル数の上限");
    CHECK(abl::FindReferences({d / "assets"}, {}, none).files.empty(), "needle 空");
}

void TestThumbs()
{
    // キー: パス（区切り / 大小は無視）+ 更新時刻 + サイズ + バージョン
    const uint64_t k0 = abl::ThumbKey("C:/a/B.png", 100, 200);
    CHECK(k0 == abl::ThumbKey("c:\\a\\b.PNG", 100, 200), "パス表記ゆれは同じキー");
    CHECK(k0 != abl::ThumbKey("C:/a/B.png", 101, 200), "更新時刻が違えば別キー");
    CHECK(k0 != abl::ThumbKey("C:/a/B.png", 100, 201), "サイズが違えば別キー");
    CHECK(k0 != abl::ThumbKey("C:/a/B.png", 100, 200, abl::kThumbVersion + 1), "バージョンが違えば別キー");
    CHECK(k0 != abl::ThumbKey("C:/a/C.png", 100, 200), "パスが違えば別キー");
    CHECK(abl::ThumbFileName("C:/a/B.png") == abl::ThumbFileName("c:\\A\\b.png"), "ファイル名はパスだけで決まる");
    CHECK(abl::ThumbFileName("C:/a/B.png").size() == 20 && abl::ThumbFileName("C:/a/B.png").substr(16) == ".utb", "16 桁 hex + .utb");

    // .utb の往復と鮮度
    const fs::path f = g_root / "thumbs" / abl::ThumbFileName("C:/x/y.png");
    std::vector<uint8_t> px(8 * 4 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(i * 7);
    CHECK(abl::WriteThumbFile(f, 8, 4, 12345, 678, px.data()), "書き込み");
    CHECK(!fs::exists(f.wstring() + L".tmp"), "一時ファイルが残らない");
    uint32_t w = 0, h = 0; std::vector<uint8_t> back;
    CHECK(abl::ReadThumbFile(f, 12345, 678, w, h, back) && w == 8 && h == 4 && back == px, "往復");
    CHECK(!abl::ReadThumbFile(f, 12346, 678, w, h, back), "更新時刻が違えば無効");
    CHECK(!abl::ReadThumbFile(f, 12345, 679, w, h, back), "サイズが違えば無効");
    CHECK(!abl::ReadThumbFile(g_root / "thumbs" / "missing.utb", 1, 1, w, h, back), "ファイルなし");
    // 旧形式（ヘッダ無しの 128x128x4 生データ）は読まない
    const fs::path legacy = g_root / "thumbs" / "legacy.raw";
    { std::ofstream o(legacy, std::ios::binary); std::string z(65536, '\x7f'); o.write(z.data(), static_cast<std::streamsize>(z.size())); }
    CHECK(!abl::ReadThumbFile(legacy, 0, 0, w, h, back), "旧形式は無効（作り直し）");
    // 途中で切れたファイル
    { std::ofstream o(g_root / "thumbs" / "trunc.utb", std::ios::binary); abl::ThumbHeader hd; hd.width = 8; hd.height = 4; o.write(reinterpret_cast<const char*>(&hd), sizeof(hd)); o << "abc"; }
    CHECK(!abl::ReadThumbFile(g_root / "thumbs" / "trunc.utb", 0, 0, w, h, back), "切れたファイルは無効");
    CHECK(!abl::WriteThumbFile(g_root / "thumbs" / "z.utb", 0, 4, 0, 0, px.data()), "幅 0 は書かない");

    uint32_t ow = 0, oh = 0;
    abl::FitThumbSize(4096, 2048, 128, ow, oh);
    CHECK(ow == 128 && oh == 64, "横長を縮小");
    abl::FitThumbSize(100, 400, 128, ow, oh);
    CHECK(ow == 32 && oh == 128, "縦長を縮小");
    abl::FitThumbSize(64, 32, 128, ow, oh);
    CHECK(ow == 64 && oh == 32, "小さい画像は等倍");
    abl::FitThumbSize(4096, 1, 128, ow, oh);
    CHECK(ow == 128 && oh == 1, "極端な縦横比でも 1 以上");
    abl::FitThumbSize(0, 10, 128, ow, oh);
    CHECK(ow == 0 && oh == 0, "0 は 0");

    CHECK(abl::FormatSize(512) == "512 B" && abl::FormatSize(2048) == "2.0 KB" && abl::FormatSize(5ull * 1024 * 1024) == "5.0 MB", "サイズの整形");
}

void TestIndex()
{
    const fs::path d = g_root / "index";
    Touch(d / "one.png");
    fs::create_directories(d / "kid" / "grand");
    Touch(d / "kid" / "k.lua");

    AssetIndex idx;
    idx.SetWatchRoots({d});
    abl::ScanOptions o;
    o.dir = d;
    idx.Request(o);

    auto waitResult = [&](abl::ScanResult& out, int ms) {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms)
        {
            if (idx.Poll(out)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    abl::ScanResult r;
    CHECK(waitResult(r, 5000), "初回の走査結果が来る");
    CHECK(r.entries.size() == 2, "one.png + kid: %zu", r.entries.size());
    CHECK(idx.ScanCount() >= 1, "走査回数");

    // ツリー用: 最初は false（要求を積む）→ 少し待つと取れる
    std::vector<abl::DirInfo> subs;
    bool got = idx.SubDirs(d, subs);
    for (int i = 0; i < 300 && !got; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); got = idx.SubDirs(d, subs); }
    CHECK(got && subs.size() == 1 && subs[0].name == "kid" && subs[0].hasSub, "サブフォルダ（kid は grand を持つ）");

    // 変更監視: ファイルを足すと、要求しなくても新しい結果が来る（250ms のまとめ待ち込みで数秒以内）
    Touch(d / "two.png");
    // 直前のセットアップ由来の変更通知（フォルダの更新時刻）で 2 件のままの結果が先に来ることがあるので、3 件になるまで待つ
    bool got3 = false;
    for (int i = 0; i < 20 && !got3; ++i)
        if (waitResult(r, 500) && r.entries.size() == 3) got3 = true;
    CHECK(got3, "変更を検知して再走査され、two.png が増えて 3 件になる（最後の結果 %zu 件）", r.entries.size());

    // "." 始まりの場所の変更では再走査しない（サムネイルの書き込みで回り続けない）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    idx.Poll(r);   // 直前の変更に伴う余分な結果があれば捨てておく
    const uint64_t before = idx.ScanCount();
    Touch(d / ".thumbcache" / "aaa.utb");
    Touch(d / "tmpfile.tmp");
    fs::remove(d / "tmpfile.tmp");   // 一覧は変わらない（作って消しただけ）
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    abl::ScanResult none;
    CHECK(!idx.Poll(none), ".thumbcache / .tmp の変更では結果が来ない");
    CHECK(idx.ScanCount() == before, ".thumbcache / .tmp の変更では走査しない（%llu → %llu）", static_cast<unsigned long long>(before), static_cast<unsigned long long>(idx.ScanCount()));

    // 手動の再走査（内容が同じなら UI へ渡さない）
    idx.ForceRescan();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!idx.Poll(none), "内容が同じ再走査は結果を渡さない");

    // 新しい要求が来たら古い結果は捨てる
    abl::ScanOptions o2; o2.dir = d / "kid";
    idx.Request(o);
    idx.Request(o2);
    CHECK(waitResult(r, 5000), "最新の要求の結果が来る");
    CHECK(r.entries.size() == 2 && (r.entries[0].name == "k.lua" || r.entries[1].name == "k.lua"), "kid の中身（最新の要求）");

    // 検索（再帰）
    abl::ScanOptions o3; o3.dir = d; o3.query = "k.lua";
    idx.Request(o3);
    CHECK(waitResult(r, 5000) && r.entries.size() == 1, "再帰検索");

    idx.Stop();
    idx.Stop();   // 二重 Stop は安全
}
} // namespace

int main()
{
    std::printf("asset_browser_logic_test\n");
    g_root = fs::temp_directory_path() / ("dx12e_abl_test_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(g_root);

    TestClassify();
    TestNaturalSort();
    TestQuery();
    TestScan();
    TestGrid();
    TestSelection();
    TestPaths();
    TestUniqueAndImport();
    TestReferences();
    TestThumbs();
    TestIndex();

    std::error_code ec;
    fs::remove_all(g_root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
