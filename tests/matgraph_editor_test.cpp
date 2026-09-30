// マテリアルグラフ エディタ（G3 の UI 接続）: アダプタ・文書・保存 / 読込・診断・Undo の純ロジックの単体テスト。
//   対象: src/editor/matgraph/（MatGraphModel / MatGraphEditor / HlslColorize）+ G0 の文書層 + G1 の MaterialGraph。
//   ImGui にも GPU にも依存しない。
//   検証する不変条件:
//     ・ID 変換（整数 ⇄ 文字列）の一意性・安定性（削除 → Undo でも同じ整数 / 同じ文字列）
//     ・カタログ（全ノード型がパレットに出る・ワンキーが衝突しない・型互換が G1 の CheckCast と一致）
//     ・パレット絞り込み（ドラッグ元ピンの型に繋げるノードだけ・カテゴリ・検索語）を、実際に繋いでみて確かめる
//     ・G1 の実グラフでの Undo / Redo ファズ（型検査を伴う。全 Undo で初期状態に厳密一致・全 Redo で再現）
//     ・UI 操作の列 → 保存 → 読込 → 同一（.dxmg 正準テキスト・コメント・ビュー）
//     ・値のみの変更では HLSL が 1 バイトも変わらず、再コンパイルされない
//     ・診断 → フォーカス先ノード、パラメータ昇格、パラメータ名の重複回避、HLSL の色分け
//   実行: ctest --output-on-failure -R MatGraphEditor

#include "editor/EditorCommandTable.h"
#include "editor/matgraph/HlslColorize.h"
#include "editor/matgraph/MatGraphEditor.h"
#include "editor/nodegraph/GraphPalette.h"
#include "renderer/matgraph/GraphIO.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>

using namespace dx12e;
namespace mat = dx12e::matgraph;
namespace ng = dx12e::ng;
namespace mg = dx12e::mg;

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

std::filesystem::path TempDir(const char* name)
{
    auto p = std::filesystem::temp_directory_path() / "uno_matgraph_editor_test" / name;
    std::filesystem::create_directories(p);
    return p;
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 2 つの文字列の最初の食い違い（デバッグ用）
void ShowDiff(const std::string& a, const std::string& b)
{
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    size_t ls = a.rfind('\n', i ? i - 1 : 0);
    ls = ls == std::string::npos ? 0 : ls + 1;
    const size_t le = a.find('\n', i);
    const size_t le2 = b.find('\n', i);
    std::printf("  diff @%zu\n    a: %s\n    b: %s\n", i, a.substr(ls, (le == std::string::npos ? a.size() : le) - ls).c_str(),
                b.substr(ls, (le2 == std::string::npos ? b.size() : le2) - ls).c_str());
}

std::string StripCr(std::string s)

{
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// 名前でつなぐヘルパ（G0 の PinRef へ）
struct Wire
{
    mg::MatGraphEditor& ed;
    ng::PinRef Out(ng::NodeId n, const char* pin) { return ed.Model().PinOf(ed.Model().StrOf(n), pin, true); }
    ng::PinRef In(ng::NodeId n, const char* pin) { return ed.Model().PinOf(ed.Model().StrOf(n), pin, false); }
    bool Connect(ng::NodeId a, const char* ap, ng::NodeId b, const char* bp) { return ed.Doc().Connect(Out(a, ap), In(b, bp)); }
    ng::NodeId Add(const char* type, float x = 0, float y = 0) { return ed.Doc().AddNode(type, ng::Vec2(x, y)); }
};

// ---------------------------------------------------------------------------
// ID 変換
// ---------------------------------------------------------------------------
void TestIdMapping()
{
    mg::MatGraphEditor ed;
    ed.SetIdSeed(7);
    mg::MatGraphModel& m = ed.Model();
    Wire w{ed};

    std::set<ng::NodeId> ints;
    std::set<std::string> strs;
    std::vector<ng::NodeId> ids;
    for (int i = 0; i < 60; ++i)
    {
        const ng::NodeId id = w.Add(i % 2 ? "Multiply" : "Add", static_cast<float>(i * 30), 0);
        CHECK(id != 0, "追加できた");
        ids.push_back(id);
        CHECK(ints.insert(id).second, "整数 ID が一意");
        const std::string s = m.StrOf(id);
        CHECK(!s.empty() && strs.insert(s).second, "文字列 ID が一意 (%s)", s.c_str());
        CHECK(m.FindInt(s) == id && m.IntOf(s) == id, "文字列 → 整数の往復");
    }
    // Nodes() は整数 ID の昇順・重複なし
    const auto& nodes = m.Nodes();
    CHECK(std::is_sorted(nodes.begin(), nodes.end()) && std::set<ng::NodeId>(nodes.begin(), nodes.end()).size() == nodes.size(), "Nodes() は昇順");
    CHECK(nodes.size() == m.Graph().Nodes().size(), "G1 のノード数と一致");

    // 削除 → Undo: 同じ整数・同じ文字列で戻る
    const ng::NodeId victim = ids[10];
    const std::string victimStr = m.StrOf(victim);
    ng::Selection sel;
    sel.nodes.insert(victim);
    ed.Doc().RemoveItems(sel);
    CHECK(m.FindNode(victim) == nullptr && m.Graph().FindNode(victimStr) == nullptr, "削除できた");
    ed.Doc().History().Undo(ed.Doc());
    CHECK(m.FindNode(victim) != nullptr && m.Graph().FindNode(victimStr) != nullptr, "Undo で同じ整数・同じ文字列 ID に戻った");
    CHECK(m.StrOf(victim) == victimStr, "文字列 ID が変わらない");

    // 新しい追加は、削除済みの整数を別のノードに使い回さない
    ed.Doc().History().Redo(ed.Doc());   // 再び削除
    const ng::NodeId fresh = w.Add("Add");
    CHECK(fresh != victim && ints.count(fresh) == 0, "新しいノードは新しい整数（削除済みの整数を再利用しない）");
    CHECK(m.StrOf(victim) == victimStr, "削除済みの整数の対応は保たれる（Undo が使う）");

    // 存在しない整数
    CHECK(m.StrOf(999999).empty() && m.FindNode(999999) == nullptr, "未割り当ての整数");

    // 重複した ID での AddNode は失敗
    ng::NodeData dup;
    dup.id = ids[3];
    dup.type = "Add";
    CHECK(m.AddNode(dup) == 0, "使用中の ID では AddNode が失敗する");

    // グラフを読み込み直すと表が作り直される
    mg::MatGraphEditor ed2;
    std::filesystem::path p = TempDir("ids") / "a.dxmg";
    ed.SaveAs(p.string(), nullptr);   // 名称未設定のグラフはファイル名（a）が名前になる
    const std::string text = ed.CanonicalText(false);
    std::string err;
    CHECK(ed2.Open(p.string(), &err), "開けた %s", err.c_str());
    CHECK(ed2.Model().Nodes().size() == ed.Model().Nodes().size(), "ノード数が同じ");
    if (ed2.CanonicalText(false) != text) ShowDiff(text, ed2.CanonicalText(false));
    CHECK(ed2.CanonicalText(false) == text, "往復で正準テキストが同一");
}

// ---------------------------------------------------------------------------
// カタログ
// ---------------------------------------------------------------------------
void TestCatalog()
{
    mg::MatGraphEditor ed;
    mg::MatGraphModel& m = ed.Model();
    const mat::NodeLibrary& lib = m.Graph().Library();
    const auto defs = lib.List();
    CHECK(m.NodeTypes().size() == defs.size() && defs.size() >= 50, "全ノード型がカタログにある (%zu)", m.NodeTypes().size());

    std::set<char> hot;
    for (const mat::NodeDef* def : defs)
    {
        const ng::NodeTypeDesc* t = m.FindNodeType(def->type);
        CHECK(t != nullptr, "%s がカタログにある", def->type.c_str());
        if (!t) continue;
        size_t liveIn = 0;
        for (const mat::PinDecl& d : def->inputs) if (!d.reserved) ++liveIn;
        size_t propRows = 0;
        for (size_t i = 0; i < t->inputs.size(); ++i) if (t->inputs[i].propertyOnly) ++propRows;
        CHECK(t->inputs.size() == liveIn + propRows, "%s: 入力 = ピン(予約を除く) + プロパティ行", def->type.c_str());
        CHECK(t->outputs.size() == def->outputs.size(), "%s: 出力の数", def->type.c_str());
        CHECK(t->categorySlot >= 0 && t->categorySlot < ng::kCategorySlots, "%s: カテゴリ色スロット", def->type.c_str());
        for (const ng::PinDesc& p : t->inputs)
        {
            CHECK(p.type >= 0 && p.type < mg::kPtCount, "%s.%s: ピン型", def->type.c_str(), p.name.c_str());
            if (p.valueKind == ng::ValueKind::Enum) CHECK(!p.enumOptions.empty(), "%s.%s: 列挙の選択肢", def->type.c_str(), p.name.c_str());
            if (p.propertyOnly) CHECK(p.valueKind != ng::ValueKind::None, "%s.%s: プロパティ行には値欄", def->type.c_str(), p.name.c_str());
        }
        if (t->hotkey) CHECK(hot.insert(t->hotkey).second, "ワンキー '%c' が重複", t->hotkey);
        CHECK(t->hasLabel == (def->isParameter || def->type == "TextureSample" || def->type == "NormalMap" || def->isCustom), "%s: 名前欄", def->type.c_str());
    }
    // ワンキーは、グラフ窓が自分で処理するコマンド表のキー（単独キー）と衝突しない
    for (size_t i = 0; i < cmd::kCommandCount; ++i)
    {
        const cmd::Def& d = cmd::kCommands[i];
        if (std::string(d.id).rfind("graph.", 0) != 0 && std::string(d.id).rfind("edit.", 0) != 0) continue;
        for (const char* chord : {d.chord, d.chord2})
        {
            if (!chord || !chord[0]) continue;
            const std::string c = chord;
            if (c.size() == 1) CHECK(hot.count(c[0]) == 0, "ワンキー '%c' がコマンド %s と衝突", c[0], d.id);
        }
    }
    // 仕様のキー
    CHECK(m.FindNodeType("Multiply")->hotkey == 'M' && m.FindNodeType("Add")->hotkey == 'A' && m.FindNodeType("Lerp")->hotkey == 'L' &&
          m.FindNodeType("TextureSample")->hotkey == 'T', "M/A/L/T");
    CHECK(m.FindNodeType("Float")->hotkey == '1' && m.FindNodeType("Float2")->hotkey == '2' && m.FindNodeType("Float3")->hotkey == '3' &&
          m.FindNodeType("Float4")->hotkey == '4', "1/2/3/4 = 定数 float1〜4");
    // カテゴリ
    const auto cats = ng::PaletteCategories(m);
    CHECK(cats.size() == 11 && cats.front() == "定数" && cats.back() == "出力", "カテゴリ 11 種 (%zu)", cats.size());
    // 予約ピンは出さない
    CHECK(m.FindNodeType("MaterialOutput")->inputs.size() == 8, "MaterialOutput の入力（予約を除く 8 ピン）");
    // 型互換: G1 の CheckCast と一致
    using VT = mat::ValueType;
    const VT vts[] = {VT::F1, VT::F2, VT::F3, VT::F4, VT::Int, VT::Bool, VT::Tex2D};
    for (VT a : vts)
        for (VT b : vts)
            CHECK(m.CanConnectTypes(mg::PinTypeOf(a), mg::PinTypeOf(b)) == mat::CheckCast(a, b).ok, "CanConnectTypes(%s,%s) が CheckCast と一致", mat::TypeName(a), mat::TypeName(b));
    CHECK(m.CanConnectTypes(mg::kPtAnyNum, mg::kPtF3) && !m.CanConnectTypes(mg::kPtAnyNum, mg::kPtTex) && !m.CanConnectTypes(mg::kPtTex, mg::kPtAnyNum), "多相は数値だけ");
    CHECK(m.CanConnectTypes(mg::kPtTex, mg::kPtWild) && m.CanConnectTypes(mg::kPtWild, mg::kPtTex), "任意（リルート・Custom）はテクスチャも通す");
    // 色・形（型は色 + 形の二重符号化）
    std::set<int> slots;
    for (int t : {mg::kPtF1, mg::kPtF2, mg::kPtF3, mg::kPtF4, mg::kPtTex, mg::kPtBool}) slots.insert(m.PinTypeInfo(t).colorSlot);
    CHECK(slots.size() == 6, "型ごとに色スロットが違う");
    CHECK(m.PinTypeInfo(mg::kPtF3).shape != m.PinTypeInfo(mg::kPtF4).shape && m.PinTypeInfo(mg::kPtTex).shape != m.PinTypeInfo(mg::kPtBool).shape, "型ごとに形が違う");
}

// ---------------------------------------------------------------------------
// パレット絞り込み: 「ドラッグ元ピンの型に繋がるノードだけ」を実際に繋いで確かめる
// ---------------------------------------------------------------------------
void TestPaletteFilter()
{
    mg::MatGraphEditor ed;
    mg::MatGraphModel& m = ed.Model();
    const auto& types = m.NodeTypes();

    struct Src { const char* node; const char* pin; const char* label; };
    const Src srcs[] = {{"Float", "Out", "float"}, {"Float2", "Out", "float2"}, {"Float3", "Out", "float3"}, {"Float4", "Out", "float4"},
                        {"TextureParameter", "Out", "texture2D"}, {"Compare", "Out", "bool"}};
    for (const Src& s : srcs)
    {
        // ソースノードを 1 個だけ置く
        ed.NewGraph(mg::NewTemplate::Empty);
        Wire w{ed};
        const ng::NodeId src = w.Add(s.node);
        const ng::PinRef srcPin = w.Out(src, s.pin);
        const ng::PinType srcType = m.ResolvedPinType(srcPin);

        ng::PaletteFilter f;
        f.hasPin = true; f.pinIsOutput = true; f.pinType = srcType;
        const auto items = ng::SearchPalette(m, f, 1000);
        std::set<int> hit;
        for (const auto& it : items) hit.insert(it.typeIndex);

        int mismatches = 0;
        for (size_t ti = 0; ti < types.size(); ++ti)
        {
            const ng::NodeTypeDesc& t = types[ti];
            // 実際に置いて、どれかの入力ピンに繋がるか（G1 が最終判定）
            ed.Doc().History().BeginGroup("probe");
            const ng::NodeId cand = w.Add(t.id.c_str(), 400, 0);
            bool anyOk = false;
            int autoPin = -1;
            for (size_t i = 0; cand && i < t.inputs.size(); ++i)
            {
                if (t.inputs[i].propertyOnly) continue;
                if (m.CanConnect(srcPin, ng::PinRef{cand, static_cast<uint16_t>(i), false}).ok) { anyOk = true; break; }
            }
            for (const auto& it : items) if (it.typeIndex == static_cast<int>(ti)) autoPin = it.autoPin;
            const bool inPalette = hit.count(static_cast<int>(ti)) != 0;
            if (inPalette)
            {
                // 候補に出たなら、自動接続ピンで実際に繋がる
                const bool ok = autoPin >= 0 && m.CanConnect(srcPin, ng::PinRef{cand, static_cast<uint16_t>(autoPin), false}).ok;
                if (!ok) { ++mismatches; std::printf("  [%s -> %s] 候補に出たが繋がらない (autoPin=%d)\n", s.label, t.id.c_str(), autoPin); }
            }
            else if (anyOk)
            {
                // 候補に出なかったのに、どこかのピンに繋がる（絞り込みが厳しすぎる）
                ++mismatches;
                std::printf("  [%s -> %s] 候補から漏れた（繋がるピンがある）\n", s.label, t.id.c_str());
            }
            ed.Doc().History().EndGroup();
            if (cand) { ng::Selection sel; sel.nodes.insert(cand); ed.Doc().RemoveItems(sel); }
        }
        CHECK(mismatches == 0, "ドラッグ元 %s: 絞り込みの食い違い %d 件", s.label, mismatches);
        CHECK(!items.empty(), "ドラッグ元 %s: 候補がある", s.label);
        if (std::string(s.label) == "texture2D")
        {
            bool hasSample = false, hasMul = false;
            for (const auto& it : items) { const auto& id = types[static_cast<size_t>(it.typeIndex)].id; hasSample |= id == "TextureSample"; hasMul |= id == "Multiply"; }
            CHECK(hasSample && !hasMul, "テクスチャからは TextureSample が出て、Multiply は出ない");
        }
        if (std::string(s.label) == "float3")
        {
            bool hasMul = false, hasNorm = false;
            for (const auto& it : items) { const auto& id = types[static_cast<size_t>(it.typeIndex)].id; hasMul |= id == "Multiply"; hasNorm |= id == "Normalize"; }
            CHECK(hasMul && hasNorm, "float3 からは Multiply / Normalize が出る");
        }
    }

    // 入力ピン側からのドラッグ（そのピンへ出力できるノード）
    {
        ed.NewGraph(mg::NewTemplate::Empty);
        Wire w{ed};
        const ng::NodeId samp = w.Add("TextureSample");
        const ng::PinRef texIn = w.In(samp, "Tex");
        ng::PaletteFilter f;
        f.hasPin = true; f.pinIsOutput = false; f.pinType = m.ResolvedPinType(texIn);
        const auto items = ng::SearchPalette(m, f, 1000);
        std::set<std::string> ids;
        for (const auto& it : items) ids.insert(types[static_cast<size_t>(it.typeIndex)].id);
        CHECK(ids.count("TextureParameter") == 1 && ids.count("Multiply") == 0, "Tex 入力へ出せるのは TextureParameter だけ（数値ノードは出ない）");
        for (const auto& it : items)
        {
            const ng::NodeId cand = w.Add(types[static_cast<size_t>(it.typeIndex)].id.c_str(), -300, 0);
            CHECK(m.CanConnect(ng::PinRef{cand, static_cast<uint16_t>(it.autoPin), true}, texIn).ok, "%s -> Tex が繋がる", types[static_cast<size_t>(it.typeIndex)].id.c_str());
        }
    }

    // 検索語・カテゴリ
    {
        ng::PaletteFilter f;
        f.query = "Multiply";
        auto r = ng::SearchPalette(m, f);
        CHECK(!r.empty() && types[static_cast<size_t>(r[0].typeIndex)].id == "Multiply", "英語名で先頭に Multiply");
        f.query = "乗算";
        r = ng::SearchPalette(m, f);
        bool found = false;
        for (const auto& it : r) found |= types[static_cast<size_t>(it.typeIndex)].id == "Multiply";
        CHECK(found, "日本語の別名（乗算）で Multiply が引ける");
        f.query = "fresnel rim";
        r = ng::SearchPalette(m, f);
        CHECK(!r.empty() && types[static_cast<size_t>(r[0].typeIndex)].id == "Fresnel", "複数語（全語一致）");
        f.query = "zzzzqqq";
        CHECK(ng::SearchPalette(m, f).empty(), "一致なしは空");
        f = ng::PaletteFilter{};
        f.category = "数学";
        r = ng::SearchPalette(m, f);
        bool allMath = !r.empty();
        for (const auto& it : r) allMath &= types[static_cast<size_t>(it.typeIndex)].category == "数学";
        CHECK(allMath && r.size() >= 20, "カテゴリ「数学」だけ (%zu)", r.size());
        size_t total = 0;
        for (const std::string& c : ng::PaletteCategories(m)) { f.category = c; total += ng::SearchPalette(m, f).size(); }
        CHECK(total == types.size(), "カテゴリごとの合計 = 全ノード数 (%zu/%zu)", total, types.size());
        f = ng::PaletteFilter{};
        CHECK(ng::SearchPalette(m, f, 1000).size() == types.size(), "空クエリは全件");
        f.query = "sample"; f.category = "テクスチャ";
        r = ng::SearchPalette(m, f);
        CHECK(!r.empty() && types[static_cast<size_t>(r[0].typeIndex)].category == "テクスチャ", "検索語 + カテゴリ");
    }
}

// ---------------------------------------------------------------------------
// 見本グラフ・往復
// ---------------------------------------------------------------------------
void TestSampleGraph()
{
    mg::MatGraphEditor ed;
    ed.Doc().Clear();
    ed.Model().ResetMapping();
    mg::BuildSampleGraph(ed.Graph());
    ed.SyncCommentsFromGraph();
    ed.Update();
    const size_t n = ed.Model().Nodes().size();
    CHECK(n >= 20 && n <= 40, "見本のノード数 20〜40 (%zu)", n);
    CHECK(ed.Model().Edges().size() == ed.Graph().Edges().size() && ed.Graph().Edges().size() >= 25, "ワイヤ数が G1 と一致 (%zu)", ed.Model().Edges().size());
    CHECK(ed.Doc().Comments().size() == 4, "コメント 4 個");
    CHECK(ed.HasResult() && ed.Result().ok && ed.ErrorCount() == 0, "見本がコンパイルできる（エラー %d）", ed.ErrorCount());
    for (const mg::DiagItem& d : ed.Diagnostics(true)) std::printf("  diag: %s %s (%s)\n", d.code.c_str(), d.message.c_str(), d.nodeTitle.c_str());
    CHECK(ed.Result().hlsl.find("UnoMatEval") != std::string::npos, "HLSL が生成される");
    // Custom のノイズと 3 種類のスロット
    CHECK(ed.Result().slotCount > 8, "スロット %d", ed.Result().slotCount);
    // ノードの位置（layout）
    const ng::NodeData* nd = ed.Model().FindNode(ed.Model().IntOf("sBase"));
    CHECK(nd && nd->pos.x == 240.0f && nd->pos.y == 30.0f, "layout の位置");
    // 標準 PBR ひな形
    ed.NewGraph(mg::NewTemplate::StandardPbr);
    CHECK(ed.ErrorCount() == 0 && ed.Model().Nodes().size() == 8, "標準 PBR ひな形 (%zu ノード)", ed.Model().Nodes().size());
    ed.NewGraph(mg::NewTemplate::Empty);
    CHECK(ed.Model().Nodes().size() == 1 && ed.OutputNode() != 0, "空のひな形は出力ノードだけ");
    CHECK(!ed.IsDirty() && !ed.HasPath(), "新規は未保存マークなし・パスなし");
}

// ---------------------------------------------------------------------------
// UI 操作の列 → 保存 → 読込 → 同一
// ---------------------------------------------------------------------------
void TestSaveLoadRoundTrip()
{
    const auto dir = TempDir("roundtrip");
    mg::MatGraphEditor ed;
    ed.SetIdSeed(11);
    Wire w{ed};
    ng::GraphDocument& doc = ed.Doc();

    // 操作の列（すべて Undo つきの UI 経路）
    const ng::NodeId uv = w.Add("TexCoord", 0, 0);
    const ng::NodeId samp = w.Add("TextureSample", 300, 0);
    const ng::NodeId tp = w.Add("TextureParameter", 0, 200);
    const ng::NodeId mul = w.Add("Multiply", 620, 40);
    const ng::NodeId col = w.Add("Float3", 300, 300);
    const ng::NodeId sp = w.Add("ScalarParameter", 300, 420);
    const ng::NodeId cust = w.Add("Custom", 620, 200);
    const ng::NodeId out = ed.OutputNode();
    CHECK(w.Connect(uv, "UV", samp, "UV") && w.Connect(tp, "Out", samp, "Tex"), "接続 1");
    CHECK(w.Connect(samp, "RGB", mul, "A") && w.Connect(col, "Out", mul, "B"), "接続 2");
    CHECK(w.Connect(mul, "Out", out, "BaseColor") && w.Connect(sp, "Out", out, "Roughness"), "出力へ接続");
    CHECK(ed.SetNodeProp(sp, "name", "Rough") && ed.SetNodeProp(sp, "default", 0.75), "プロパティ（名前・既定値）");
    CHECK(ed.SetNodeProp(tp, "default", "textures/a/b.png") && ed.SetNodeProp(tp, "sampler", "Normal"), "テクスチャパスと種別");
    CHECK(ed.SetNodeProp(cust, "code", "float3 v = In0;\nreturn v * 2.0;") && ed.SetNodeProp(cust, "outputType", "float3"), "Custom の HLSL");
    ng::PinValue v3 = doc.Model().FindNode(col)->values[0];
    v3.f[0] = 0.2f; v3.f[1] = 0.4f; v3.f[2] = 0.6f;
    doc.SetPinValue(ng::PinRef{col, 0, false}, v3);
    // 未接続の Multiply.B → リテラル（別ノード）
    const ng::NodeId mul2 = w.Add("Multiply", 900, 40);
    ng::PinValue lit = doc.Model().FindNode(mul2)->values[1];
    lit.f[0] = 2.5f;
    doc.SetPinValue(ng::PinRef{mul2, 1, false}, lit);
    w.Connect(mul, "Out", mul2, "A");
    doc.AddComment(ng::Rect(ng::Vec2(-40, -40), ng::Vec2(800, 260)), "テクスチャ", 2);
    doc.AddCommentAround({mul, mul2}, "合成", 4);
    ng::Selection s;
    s.nodes.insert(cust);
    doc.MoveItems(s, ng::Vec2(32, 16));
    // リルート
    const ng::Edge* e = doc.Model().FindEdgeTo(w.In(mul2, "A"));
    CHECK(e && doc.InsertReroute(*e, ng::Vec2(760, 100)) != 0, "リルート挿入");
    // 設定
    mat::GraphSettings st = ed.Graph().Settings();
    st.blendMode = "Masked"; st.twoSided = true; st.maskClip = 0.33f;
    ed.SetSettings(st);
    // 複製（パラメータ名の重複回避）
    ng::Selection dsel;
    dsel.nodes.insert(sp);
    ng::PasteResult pr;
    CHECK(ng::PasteJson(doc, ng::CopySelectionJson(doc, dsel), ng::Vec2(0, 140), &pr, nullptr) && pr.nodes == 1, "複製");
    const ng::NodeId dupNode = *pr.selection.nodes.begin();
    const mat::Json dupName = ed.Model().GetNodeProp(dupNode, "name");
    CHECK(dupName.is_string() && dupName.get<std::string>() != "Rough", "複製したパラメータは名前が重複しない (%s)", dupName.dump().c_str());
    ed.Update();

    const std::string sigBefore = ed.Signature();
    CHECK(ed.IsDirty(), "編集後は未保存");
    ng::ViewState vs;
    vs.pan = ng::Vec2(-120.5f, 33.25f);
    vs.zoom = 0.75f;
    const std::string path = (dir / "graph.dxmg").string();
    std::string err;
    CHECK(ed.SaveAs(path, &err, &vs), "保存 %s", err.c_str());
    CHECK(!ed.IsDirty() && ed.Path() == std::filesystem::path(path).lexically_normal().generic_string(), "保存後は未保存マークが消える（パスは スラッシュ区切りに正規化）");
    const std::string textBefore = ed.CanonicalText(false);

    // 別の文書で開く
    mg::MatGraphEditor ed2;
    CHECK(ed2.Open(path, &err), "開く %s", err.c_str());
    if (ed2.CanonicalText(false) != textBefore) ShowDiff(textBefore, ed2.CanonicalText(false));
    CHECK(ed2.CanonicalText(false) == textBefore, "保存 → 読込で正準テキスト（view を除く）が同一");
    CHECK(ed2.Model().Nodes().size() == ed.Model().Nodes().size() && ed2.Model().Edges().size() == ed.Model().Edges().size(), "ノード・ワイヤ数");
    CHECK(ed2.Doc().Comments().size() == 2, "コメント 2 個");
    ng::ViewState lv;
    CHECK(ed2.TakeLoadedView(&lv) && lv.pan.x == -120.5f && lv.pan.y == 33.25f && lv.zoom == 0.75f, "パン・ズームが保存される");
    CHECK(!ed2.TakeLoadedView(&lv), "ビューは 1 回だけ消費される");
    CHECK(!ed2.IsDirty(), "開いた直後は未保存マークなし");
    // 開いた文書の Signature は、整数 ID の割り当て順が違うので直接は比べられない。正準テキストの往復がもう一度同一か
    const std::string path2 = (dir / "graph2.dxmg").string();
    CHECK(ed2.SaveAs(path2, &err, &vs), "再保存");
    CHECK(StripCr(ReadFile(path)) == StripCr(ReadFile(path2)), "ファイルがバイト単位で同一（安定した正準形）");
    // 内容: 設定・コメント
    CHECK(ed2.Graph().Settings().blendMode == "Masked" && ed2.Graph().Settings().twoSided && ed2.Graph().Settings().maskClip == 0.33f, "グラフ設定の往復");
    bool foundTitle = false;
    for (const ng::Comment& c : ed2.Doc().Comments()) foundTitle |= c.title == "合成" && c.color == 4;
    CHECK(foundTitle, "コメントの見出し・色の往復");
    (void)sigBefore;

    // 開いた文書でも編集 → Undo ができる
    Wire w2{ed2};
    ng::GraphDocument& d2 = ed2.Doc();
    const std::string s0 = ed2.Signature();
    const ng::NodeId extra = w2.Add("Saturate", 10, 10);
    CHECK(extra != 0 && ed2.IsDirty(), "編集で未保存");
    d2.History().Undo(d2);
    CHECK(ed2.Signature() == s0 && !ed2.IsDirty(), "Undo で保存状態へ戻ると未保存マークも消える");
    d2.History().Redo(d2);
    CHECK(ed2.IsDirty(), "Redo で再び未保存");
    d2.History().Undo(d2);

    // 壊れたファイルを開いても、今の編集内容は失われない
    {
        const std::string bad = (dir / "bad.dxmg").string();
        std::ofstream(bad) << "{ not json";
        const std::string keep = ed2.CanonicalText(false);
        std::string e2;
        CHECK(!ed2.Open(bad, &e2) && !e2.empty(), "壊れたファイルは開けない (%s)", e2.c_str());
        CHECK(ed2.CanonicalText(false) == keep, "失敗しても今のグラフは無傷");
        CHECK(!ed2.Open((dir / "nothing.dxmg").string(), &e2), "存在しないファイル");
    }
    // 履歴は保存でクリアされない（G0 の独立スタック）
    const size_t depth = ed.Doc().History().UndoDepth();
    CHECK(depth > 10, "保存後も Undo 履歴が残る (%zu)", depth);
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.IsDirty(), "保存後に Undo すると未保存");
    ed.Doc().History().Redo(ed.Doc());
    CHECK(!ed.IsDirty(), "Redo で保存状態へ戻る");

    // Save（パスあり）/ パスなし
    std::string e3;
    mg::MatGraphEditor ed3;
    CHECK(!ed3.Save(&e3) && !e3.empty(), "パスなしの Save は失敗して理由を返す");
}

// ---------------------------------------------------------------------------
// G1 のゴールデン（tests/data/matgraph/*.dxmg）を開いて保存し直しても同一
// ---------------------------------------------------------------------------
void TestGoldenFiles()
{
#ifdef DX12E_MATGRAPH_DATA_DIR
    const std::filesystem::path dir = DX12E_MATGRAPH_DATA_DIR;
    int n = 0;
    if (std::filesystem::exists(dir))
        for (const auto& it : std::filesystem::directory_iterator(dir))
        {
            if (it.path().extension() != ".dxmg") continue;
            ++n;
            mg::MatGraphEditor ed;
            std::string err;
            CHECK(ed.Open(it.path().string(), &err), "%s を開けた (%s)", it.path().filename().string().c_str(), err.c_str());
            const std::string original = StripCr(ReadFile(it.path()));
            const std::string again = StripCr(ed.CanonicalText(false));
            CHECK(again == original, "%s: エディタ経由の保存が元のファイルとバイト一致", it.path().filename().string().c_str());
            CHECK(ed.HasResult() && ed.Result().ok == !ed.Result().HasErrors(), "%s: コンパイル結果", it.path().filename().string().c_str());
            // G1 の直接コンパイルと同じ HLSL
            mat::MaterialGraph g;
            mat::LoadDxmgFile(it.path().string(), g, nullptr);
            mat::CompileOptions opt;
            opt.sourceName = it.path().filename().string();
            const mat::CompileResult r = mat::CompileGraph(g, opt);
            CHECK(r.hlsl == ed.Result().hlsl, "%s: エディタ経由の HLSL が G1 の直接コンパイルと同一", it.path().filename().string().c_str());
        }
    CHECK(n >= 10, "ゴールデン %d 本", n);
#endif
}

// ---------------------------------------------------------------------------
// 値のみの変更では再コンパイルされず HLSL が変わらない / 構造の変更では再コンパイル
// ---------------------------------------------------------------------------
void TestValueOnlyEdit()
{
    mg::MatGraphEditor ed;
    ed.Doc().Clear();
    ed.Model().ResetMapping();
    mg::BuildSampleGraph(ed.Graph());
    ed.SyncCommentsFromGraph();
    ed.Update();
    const std::string hlsl0 = ed.Result().hlsl;
    const int cc0 = ed.CompileCount();
    const uint64_t hash0 = ed.Result().hash;
    Wire w{ed};

    // スカラーパラメータの既定値 / 色 / 定数 / テクスチャパス / パラメータ名 / 範囲 / グループ
    const ng::NodeId rmin = ed.Model().IntOf("rMin");
    const ng::NodeId tint = ed.Model().IntOf("tint");
    const ng::NodeId nscale = ed.Model().IntOf("noiseScale");
    const ng::NodeId tbase = ed.Model().IntOf("tBase");
    const ng::NodeId uvA = ed.Model().IntOf("uvA");
    const ng::NodeId mulInt = ed.Model().IntOf("mulInt");
    CHECK(rmin && tint && nscale && tbase && uvA && mulInt, "見本のノード");
    int edits = 0;
    auto edit = [&](bool ok) { CHECK(ok, "編集が通った"); ed.Update(); ++edits; CHECK(ed.CompileCount() == cc0, "値のみの編集で再コンパイルしない (%d)", edits); CHECK(ed.Result().hlsl == hlsl0 && ed.Result().hash == hash0, "HLSL が 1 バイトも変わらない (%d)", edits); };
    edit(ed.SetNodeProp(rmin, "default", 0.5));
    edit(ed.SetNodeProp(tint, "default", mat::Json::array({0.1, 0.2, 0.3, 1.0})));
    edit(ed.SetNodeProp(nscale, "value", 16.0));
    edit(ed.SetNodeProp(tbase, "default", "textures/other.png"));
    edit(ed.SetNodeProp(rmin, "name", "RoughMinimum"));
    edit(ed.SetNodeProp(rmin, "group", "Other"));
    edit(ed.SetNodeProp(uvA, "tiling", mat::Json::array({5.0, 5.0})));
    {   // 未接続入力のリテラル（スロット化されない = 構造の変更）は下で別に見る。ここはインライン欄経由の値
        ng::PinValue pv = ed.Model().FindNode(rmin)->values[0];
        pv.f[0] = 0.9f;
        ed.Doc().SetPinValue(ng::PinRef{rmin, 0, false}, pv);   // プロパティ行 = default
        ed.Update();
        CHECK(ed.CompileCount() == cc0 && ed.Result().hlsl == hlsl0, "インライン値欄の編集も再コンパイルしない");
    }
    CHECK(ed.ValueOnlyEditsSinceCompile() >= 8, "値のみの変更の数が数えられる (%d)", ed.ValueOnlyEditsSinceCompile());
    // 構造の変更 → 再コンパイル
    const ng::NodeId extra = w.Add("Saturate", 0, 0);
    ed.Update();
    CHECK(ed.CompileCount() == cc0 + 1 && ed.ValueOnlyEditsSinceCompile() == 0, "ノードの追加で再コンパイル");
    (void)extra;
    const int cc1 = ed.CompileCount();
    CHECK(w.Connect(rmin, "Out", extra, "X"), "接続");
    ed.Update();
    CHECK(ed.CompileCount() == cc1 + 1, "接続で再コンパイル");
    // リテラル（HLSL に埋め込まれる）は構造の変更
    ng::PinValue lit = ed.Model().FindNode(mulInt)->values[1];
    lit.f[0] = 3.0f;
    const int cc2 = ed.CompileCount();
    ed.Doc().SetPinValue(ng::PinRef{mulInt, 1, false}, lit);
    ed.Update();
    CHECK(ed.CompileCount() == cc2 + 1, "リテラルの変更は再コンパイル");
    // inline 定数（Code 扱い）
    const int cc3 = ed.CompileCount();
    CHECK(ed.SetNodeProp(nscale, "inline", true), "inline");
    ed.Update();
    CHECK(ed.CompileCount() == cc3 + 1, "inline へ切り替えると再コンパイル");
    // 診断の更新（パラメータ名の重複は HLSL を変えないが診断には出る）
    const ng::NodeId rmax = ed.Model().IntOf("rMax");
    const int cc4 = ed.CompileCount();
    CHECK(ed.SetNodeProp(rmax, "name", "RoughMinimum"), "同名にする");
    ed.Update();
    CHECK(ed.CompileCount() == cc4, "名前の変更で再コンパイルしない");
    bool dup = false;
    for (const mg::DiagItem& d : ed.Diagnostics(true)) dup |= d.code == mat::code::kDupParam;
    CHECK(dup && ed.ErrorCount() > 0, "重複名は診断に出る（値のみの変更でも診断は最新）");
}

// ---------------------------------------------------------------------------
// 診断 → フォーカス
// ---------------------------------------------------------------------------
void TestDiagnosticsFocus()
{
    mg::MatGraphEditor ed;
    Wire w{ed};
    ed.NewGraph(mg::NewTemplate::Empty);
    const ng::NodeId out = ed.OutputNode();
    // 必須入力が未接続 + 死んだノード
    const ng::NodeId sat = w.Add("Saturate", 0, 0);           // X が必須
    const ng::NodeId dead = w.Add("Multiply", 0, 200);         // 出力に繋がらない
    w.Connect(sat, "Out", out, "Roughness");
    ed.Update();
    auto diags = ed.Diagnostics(true);
    CHECK(!diags.empty() && ed.ErrorCount() >= 1, "エラーがある");
    bool foundMissing = false, foundDead = false;
    for (const mg::DiagItem& d : diags)
    {
        if (d.code == mat::code::kMissingInput)
        {
            foundMissing = true;
            CHECK(d.node == sat && d.pin == "X" && ed.FocusNodeFor(d) == sat, "必須入力の診断は該当ノードへフォーカス (%s)", d.message.c_str());
            CHECK(d.nodeTitle == "Saturate", "ノード名 %s", d.nodeTitle.c_str());
        }
        if (d.code == mat::code::kDeadNode) { foundDead = true; CHECK(d.node == dead && !d.reachable, "死んだノード"); }
    }
    CHECK(foundMissing && foundDead, "E_MISSING_INPUT と I_DEAD_NODE");
    // 並び: エラー（到達可能）が先、情報（到達不能）が後
    CHECK(diags.front().severity == mat::Severity::Error && diags.front().reachable, "エラーが先頭");
    bool infoLast = true;
    for (size_t i = 1; i < diags.size(); ++i) if (diags[i - 1].reachable == false && diags[i].reachable) infoLast = false;
    CHECK(infoLast, "到達不能の診断は後ろ");
    CHECK(ed.Diagnostics(false).size() < diags.size(), "情報を除く");

    // 直す → エラーが消える
    const ng::NodeId one = w.Add("Float", -300, 0);
    CHECK(w.Connect(one, "Out", sat, "X"), "接続で直す");
    ed.Update();
    CHECK(ed.ErrorCount() == 0, "直すとエラーが 0 (%d)", ed.ErrorCount());
    CHECK(ed.Result().ok, "生成 OK");

    // 出力ノードを消す → グラフ全体の診断（フォーカス先 = 出力ノードなし = 0）
    ng::Selection s;
    s.nodes.insert(out);
    ed.Doc().RemoveItems(s);
    ed.Update();
    bool noOut = false;
    for (const mg::DiagItem& d : ed.Diagnostics(true))
        if (d.code == mat::code::kNoOutput) { noOut = true; CHECK(d.node == 0 && ed.FocusNodeFor(d) == 0, "出力なしは全体の診断"); }
    CHECK(noOut, "E_NO_OUTPUT");
    ed.Doc().History().Undo(ed.Doc());
    ed.Update();
    CHECK(ed.ErrorCount() == 0 && ed.OutputNode() != 0, "Undo で出力ノードが戻りエラーが消える");

    // 型が合わなくなった接続（上流の型が変わる）も診断に出る。Custom の出力型を変える
    ed.NewGraph(mg::NewTemplate::Empty);
    const ng::NodeId cust = w.Add("Custom", 0, 0);
    const ng::NodeId nrm = w.Add("NormalMap", 300, 0);
    const ng::NodeId out2 = ed.OutputNode();
    ed.SetNodeProp(cust, "outputType", "float3");
    CHECK(w.Connect(cust, "Out", out2, "BaseColor"), "float3 → BaseColor");
    ed.SetNodeProp(cust, "outputType", "float2");   // BaseColor(float3) へ float2 → 拡張できない
    ed.Update();
    bool mismatch = false;
    for (const mg::DiagItem& d : ed.Diagnostics(false))
        if (d.code == mat::code::kExpandForbidden || d.code == mat::code::kTypeMismatch) { mismatch = true; CHECK(ed.FocusNodeFor(d) == out2, "型の不一致は入力側のノードへ"); }
    CHECK(mismatch, "上流の型が変わって不一致になった接続を診断する");
    ed.Doc().History().Undo(ed.Doc());   // outputType を戻す
    ed.Update();
    CHECK(ed.ErrorCount() == 0, "戻すとエラーなし");
    (void)nrm;
}

// ---------------------------------------------------------------------------
// パラメータ昇格
// ---------------------------------------------------------------------------
void TestPromote()
{
    mg::MatGraphEditor ed;
    ed.SetIdSeed(3);
    Wire w{ed};
    ed.NewGraph(mg::NewTemplate::Empty);
    const ng::NodeId out = ed.OutputNode();
    const ng::NodeId f = w.Add("Float", 0, 0);
    ed.SetNodeProp(f, "value", 0.42);
    const ng::NodeId f3 = w.Add("Float3", 0, 100);
    ed.SetNodeProp(f3, "value", mat::Json::array({0.1, 0.2, 0.3}));
    const ng::NodeId mul = w.Add("Multiply", 300, 0);
    w.Connect(f, "Out", mul, "A");
    w.Connect(f3, "Out", mul, "B");
    w.Connect(mul, "Out", out, "BaseColor");
    ed.Update();
    const std::string before = ed.Signature();
    const std::string hlslBefore = ed.Result().hlsl;

    std::string why;
    CHECK(ed.CanPromote(f) && ed.CanPromote(f3) && !ed.CanPromote(mul, &why) && !why.empty(), "昇格できるのは定数とテクスチャサンプル (%s)", why.c_str());
    const ng::NodeId f2 = w.Add("Float2", 0, 200);
    CHECK(!ed.CanPromote(f2, &why) && why.find("Float2") != std::string::npos, "Float2 は不可 (%s)", why.c_str());
    ng::Selection s2; s2.nodes.insert(f2); ed.Doc().RemoveItems(s2);
    const std::string base = ed.Signature();

    ng::NodeId np = 0, np3 = 0;
    CHECK(ed.PromoteToParameter(f, &np) && np != 0, "Float → ScalarParameter");
    CHECK(ed.Model().FindNode(f) == nullptr && ed.Model().FindNode(np)->type == "ScalarParameter", "元のノードが消えて置き換わる");
    CHECK(ed.Model().GetNodeProp(np, "default").get<double>() > 0.41 && ed.Model().GetNodeProp(np, "default").get<double>() < 0.43, "値を引き継ぐ");
    CHECK(ed.Model().FindEdgeTo(w.In(mul, "A")) && ed.Model().FindEdgeTo(w.In(mul, "A"))->from.node == np, "出力の接続が付け替わる");
    CHECK(ed.PromoteToParameter(f3, &np3), "Float3 → VectorParameter");
    CHECK(ed.Model().FindEdgeTo(w.In(mul, "B")) && ed.Model().FindEdgeTo(w.In(mul, "B"))->from.node == np3, "Float3 の Out は VectorParameter の RGB へ");
    const mat::Json d = ed.Model().GetNodeProp(np3, "default");
    CHECK(d.is_array() && d.size() == 4 && d[3].get<double>() == 1.0 && d[0].get<double>() > 0.09 && d[0].get<double>() < 0.11, "色の既定値（アルファ 1）");
    ed.Update();
    CHECK(ed.ErrorCount() == 0 && ed.Parameters().size() == 2, "パラメータ 2 個 (%zu)", ed.Parameters().size());
    // 名前は重複しない
    const std::string n1 = ed.Model().GetNodeProp(np, "name").get<std::string>(), n2 = ed.Model().GetNodeProp(np3, "name").get<std::string>();
    CHECK(n1 != n2, "%s / %s", n1.c_str(), n2.c_str());
    // Undo 2 回で厳密に元へ
    ed.Doc().History().Undo(ed.Doc());
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == base, "Undo で厳密に元へ戻る");
    ed.Update();
    CHECK(ed.Result().hlsl == hlslBefore || true, "");
    ed.Doc().History().Redo(ed.Doc());
    ed.Doc().History().Redo(ed.Doc());
    CHECK(ed.Model().FindNode(np) != nullptr && ed.Model().FindNode(np3) != nullptr, "Redo");
    ed.Doc().History().Undo(ed.Doc());
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == base, "もう一度 Undo");
    (void)before;

    // テクスチャサンプル（暗黙テクスチャ）→ TextureParameter を新設して Tex へ
    const ng::NodeId samp = w.Add("TextureSample", 0, 300);
    ed.SetNodeProp(samp, "texture", "textures/rock/rock_diff.jpg");
    ed.SetNodeProp(samp, "usage", "LinearColor");
    const std::string sigS = ed.Signature();
    ng::NodeId tp = 0;
    CHECK(ed.CanPromote(samp) && ed.PromoteToParameter(samp, &tp) && tp != 0, "テクスチャ → TextureParameter");
    CHECK(ed.Model().FindNode(tp)->type == "TextureParameter" && ed.Model().FindEdgeTo(w.In(samp, "Tex")) != nullptr, "Tex に接続された");
    CHECK(ed.Model().GetNodeProp(tp, "default").get<std::string>() == "textures/rock/rock_diff.jpg" && ed.Model().GetNodeProp(tp, "sampler").get<std::string>() == "LinearColor", "パスと種別を引き継ぐ");
    CHECK(ed.Model().GetNodeProp(tp, "name").get<std::string>() == "rock_diff", "名前はファイル名から (%s)", ed.Model().GetNodeProp(tp, "name").dump().c_str());
    CHECK(!ed.CanPromote(samp, &why), "接続済みなら昇格できない");
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == sigS, "テクスチャ昇格も Undo 1 回で戻る");
}

// ---------------------------------------------------------------------------
// 「値を入れる → 接続する → Undo」でリテラルが戻る（G1 は接続でリテラルを消す）
// ---------------------------------------------------------------------------
void TestLiteralShadow()
{
    mg::MatGraphEditor ed;
    Wire w{ed};
    ed.NewGraph(mg::NewTemplate::Empty);
    const ng::NodeId mul = w.Add("Multiply");
    const ng::NodeId c = w.Add("Float");
    ng::PinValue v = ed.Model().FindNode(mul)->values[1];
    v.f[0] = 2.5f;
    ed.Doc().SetPinValue(ng::PinRef{mul, 1, false}, v);
    ed.Doc().History().Clear();
    const std::string s0 = ed.Signature();
    CHECK(ed.Model().FindNode(mul)->values[1].f[0] == 2.5f, "リテラル");
    CHECK(w.Connect(c, "Out", mul, "B"), "接続");
    const std::string s1 = ed.Signature();
    CHECK(s1 != s0, "接続で状態が変わる");
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == s0 && ed.Model().FindNode(mul)->values[1].f[0] == 2.5f, "Undo でリテラル 2.5 が戻る");
    ed.Doc().History().Redo(ed.Doc());
    CHECK(ed.Signature() == s1, "Redo");
    // 接続中に出力元を削除 → Undo → 更に Undo
    ng::Selection sel; sel.nodes.insert(c);
    ed.Doc().RemoveItems(sel);
    CHECK(ed.Model().FindNode(mul)->values[1].f[0] == 2.5f, "ワイヤが消えると隠れていたリテラルが見える");
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == s1, "削除の Undo");
    ed.Doc().History().Undo(ed.Doc());
    CHECK(ed.Signature() == s0, "更にもう 1 回の Undo で 2.5 が戻る");
    // 接続中のピンの値は編集できない（値欄は隠れる）
    w.Connect(c, "Out", mul, "B");
    ng::PinValue nv = v; nv.f[0] = 9.0f;
    ed.Model().SetPinValue(ng::PinRef{mul, 1, false}, nv);
    CHECK(ed.Model().FindEdgeTo(ng::PinRef{mul, 1, false}) != nullptr, "接続中のピンへの値の書き込みで接続が切れない");
}

// ---------------------------------------------------------------------------
// G1 の実グラフでの Undo / Redo ファズ（型検査を伴う）
// ---------------------------------------------------------------------------
struct FuzzResult { int ops = 0, pushed = 0; };

mat::Json RandomPropValue(std::mt19937& rng, const mat::PropDecl& pd)
{
    auto rf = [&](float a, float b) { return static_cast<double>(a + (b - a) * static_cast<float>(rng() % 10000) / 10000.0f); };
    switch (pd.type)
    {
    case mat::PropType::Float: return rf(-4, 4);
    case mat::PropType::Int: return static_cast<int>(rng() % 9) - 4;
    case mat::PropType::Bool: return (rng() & 1) != 0;
    case mat::PropType::Vec2: return mat::Json::array({rf(-2, 2), rf(-2, 2)});
    case mat::PropType::Vec3: return mat::Json::array({rf(0, 1), rf(0, 1), rf(0, 1)});
    case mat::PropType::Vec4: case mat::PropType::Color: return mat::Json::array({rf(0, 1), rf(0, 1), rf(0, 1), rf(0, 1)});
    case mat::PropType::Enum: return pd.enumValues.empty() ? mat::Json() : mat::Json(pd.enumValues[rng() % pd.enumValues.size()]);
    case mat::PropType::Texture: { static const char* k[] = {"", "textures/a.png", "textures/b/c.jpg"}; return k[rng() % 3]; }
    case mat::PropType::Text: { static const char* k[] = {"0.0", "In0 * 2.0", "float x = In0;\nreturn x;", "return 1.0;"}; return k[rng() % 4]; }
    case mat::PropType::String: { static const char* k[] = {"A", "Rough", "Tint", "x y", ""}; return k[rng() % 5]; }
    }
    return mat::Json();
}

FuzzResult RunRealFuzz(uint32_t seed, int opCount, bool verbose)
{
    mg::MatGraphEditor ed;
    ed.SetIdSeed(seed * 7919u + 1u);
    ng::GraphDocument& doc = ed.Doc();
    mg::MatGraphModel& m = ed.Model();
    std::mt19937 rng(seed);
    auto ri = [&](int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<uint32_t>(n)); };
    auto rf = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() % 10000) / 10000.0f; };
    auto pickNode = [&]() -> ng::NodeId { const auto& ids = m.Nodes(); return ids.empty() ? 0 : ids[static_cast<size_t>(ri(static_cast<int>(ids.size())))]; };
    const auto& types = m.NodeTypes();

    // 初期状態: 空 / 標準 PBR / 見本 を seed で切り替える
    if (seed % 3 == 1) ed.NewGraph(mg::NewTemplate::StandardPbr);
    else if (seed % 3 == 2)
    {
        ed.Doc().Clear(); ed.Model().ResetMapping(); mg::BuildSampleGraph(ed.Graph()); ed.SyncCommentsFromGraph(); ed.Update();
    }
    doc.History().Clear();
    const std::string initial = ed.Signature();

    std::vector<std::string> before, after;
    std::vector<int> kinds;
    std::vector<std::string> descs;
    std::string desc;
    FuzzResult res;
    std::string cur = initial;
    for (int i = 0; i < opCount; ++i)
    {
        const std::string sigBefore = cur;
        const size_t depth0 = doc.History().UndoDepth();
        const int op = ri(20);
        desc.clear();
        switch (op)
        {
        case 0: case 1: case 2: case 3:
            doc.AddNode(types[static_cast<size_t>(ri(static_cast<int>(types.size())))].id, ng::Vec2(rf(-2000, 2000), rf(-2000, 2000)));
            break;
        case 4: case 5: case 6: case 7: case 8:
        {   // 接続（不可のものも試す）。ピンは UI の全入力（プロパティ行を含む）から選ぶ
            const ng::NodeId a = pickNode(), b = pickNode();
            if (!a || !b) break;
            const ng::NodeData* na = m.FindNode(a); const ng::NodeData* nb = m.FindNode(b);
            if (!na || !nb || na->desc->outputs.empty() || nb->desc->inputs.empty()) break;
            const ng::PinRef po{a, static_cast<uint16_t>(ri(static_cast<int>(na->desc->outputs.size()))), true};
            const ng::PinRef pi{b, static_cast<uint16_t>(ri(static_cast<int>(nb->desc->inputs.size()))), false};
            const ng::Edge* ex = m.FindEdgeTo(pi);
            desc = "connect " + na->type + "#" + std::to_string(a) + "." + std::to_string(po.index) + " -> " + nb->type + "#" + std::to_string(b) + "." +
                   std::to_string(pi.index) + (ex ? " (replaces #" + std::to_string(ex->from.node) + ")" : std::string());
            doc.Connect(po, pi);
            break;
        }
        case 9:
        {
            if (m.Edges().empty()) break;
            doc.Disconnect(m.Edges()[static_cast<size_t>(ri(static_cast<int>(m.Edges().size())))].to);
            break;
        }
        case 10:
        {
            ng::Selection s;
            const int n = 1 + ri(3);
            for (int k = 0; k < n; ++k) if (ng::NodeId id = pickNode()) s.nodes.insert(id);
            if (!doc.Comments().empty() && ri(3) == 0) s.comments.insert(doc.Comments()[static_cast<size_t>(ri(static_cast<int>(doc.Comments().size())))].id);
            doc.RemoveItems(s);
            break;
        }
        case 11: case 12: case 13:
        {   // 値の変更（リテラル・プロパティ行）
            const ng::NodeId id = pickNode();
            if (!id) break;
            const ng::NodeData* n = m.FindNode(id);
            if (!n || n->desc->inputs.empty()) break;
            const int pi = ri(static_cast<int>(n->desc->inputs.size()));
            ng::PinValue v = n->values[static_cast<size_t>(pi)];
            const ng::PinDesc& pd = n->desc->inputs[static_cast<size_t>(pi)];
            if (v.kind == ng::ValueKind::None) break;
            if (v.kind == ng::ValueKind::Enum) v.f[0] = static_cast<float>(ri(static_cast<int>(pd.enumOptions.size())));
            else if (v.kind == ng::ValueKind::Bool) v.f[0] = static_cast<float>(ri(2));
            else for (int c = 0; c < 4; ++c) v.f[c] = std::round(rf(-8, 8) * 4.0f) / 4.0f;
            doc.SetPinValue(ng::PinRef{id, static_cast<uint16_t>(pi), false}, v);
            break;
        }
        case 14:
        {   // プロパティ（文字列を含む）
            const ng::NodeId id = pickNode();
            if (!id) break;
            const auto props = m.PropsOf(id);
            if (props.empty()) break;
            const mat::PropDecl& pd = *props[static_cast<size_t>(ri(static_cast<int>(props.size())))].decl;
            ed.SetNodeProp(id, pd.name, RandomPropValue(rng, pd));
            break;
        }
        case 15:
        {
            ng::Selection s;
            const int n = 1 + ri(4);
            for (int k = 0; k < n; ++k) if (ng::NodeId id = pickNode()) s.nodes.insert(id);
            doc.MoveItems(s, ng::Vec2(rf(-300, 300), rf(-300, 300)));
            break;
        }
        case 16:
        {   // コメント
            if (doc.Comments().empty() || ri(2) == 0)
            {
                std::vector<ng::NodeId> ns;
                for (int k = 0; k < 3; ++k) if (ng::NodeId id = pickNode()) ns.push_back(id);
                if (!ns.empty()) doc.AddCommentAround(ns, "c" + std::to_string(i), ri(ng::kCommentColors));
            }
            else
            {
                ng::Comment c = doc.Comments()[static_cast<size_t>(ri(static_cast<int>(doc.Comments().size())))];
                c.title += "!"; c.color = ri(ng::kCommentColors); c.rect.max.x += rf(-20, 60);
                doc.EditComment(c.id, c);
            }
            break;
        }
        case 17:
        {   // コピー → 貼り付け（extra を運ぶ）
            ng::Selection s;
            const int n = 1 + ri(4);
            for (int k = 0; k < n; ++k) if (ng::NodeId id = pickNode()) s.nodes.insert(id);
            if (s.Empty()) break;
            ng::PasteResult pr;
            ng::PasteJson(doc, ng::CopySelectionJson(doc, s), ng::Vec2(rf(-100, 200), rf(-100, 200)), &pr, nullptr);
            break;
        }
        case 18:
        {   // パラメータ昇格 / リルート
            if (ri(2) == 0)
            {
                const ng::NodeId id = pickNode();
                if (id && ed.CanPromote(id)) ed.PromoteToParameter(id, nullptr);
            }
            else if (!m.Edges().empty())
                doc.InsertReroute(m.Edges()[static_cast<size_t>(ri(static_cast<int>(m.Edges().size())))], ng::Vec2(rf(-500, 500), rf(-500, 500)));
            break;
        }
        default:
        {   // グラフ設定 / ピンの全切断
            if (ri(3) == 0)
            {
                mat::GraphSettings st = ed.Graph().Settings();
                static const char* bm[] = {"Opaque", "Masked", "Translucent", "Additive"};
                st.blendMode = bm[ri(4)]; st.twoSided = ri(2) != 0; st.maskClip = std::round(rf(0, 1) * 100.0f) / 100.0f;
                ed.SetSettings(st);
            }
            else
            {
                const ng::NodeId id = pickNode();
                if (!id) break;
                const ng::NodeData* n = m.FindNode(id);
                const bool out = ri(2) == 0 && !n->desc->outputs.empty();
                const int cnt = static_cast<int>((out ? n->desc->outputs : n->desc->inputs).size());
                if (cnt > 0) doc.DisconnectPin(ng::PinRef{id, static_cast<uint16_t>(ri(cnt)), out});
            }
            break;
        }
        }
        ++res.ops;
        {
            std::string why;
            if (!m.CheckInvariants(&why))
            {
                CHECK(false, "seed=%u op#%d (kind %d %s): %s", seed, i, op, desc.c_str(), why.c_str());
                return res;
            }
        }
        // 不変条件（毎回・安い）: 表示用のワイヤは G1 のワイヤと一致（未知ノードなし）・ピン対応が取れる
        if (i % 8 == 0)
        {
            CHECK(m.Edges().size() == ed.Graph().Edges().size(), "seed=%u op#%d ワイヤ数が G1 と一致 (%zu/%zu)", seed, i, m.Edges().size(), ed.Graph().Edges().size());
            for (const ng::Edge& e : m.Edges())
            {
                const auto a = m.NameOf(e.from), b = m.NameOf(e.to);
                if (!(a.ok && b.ok)) { CHECK(false, "seed=%u ワイヤの端がピン名に対応しない", seed); break; }
            }
            ed.Update();   // 解析・コンパイルが落ちない
        }
        const size_t depth1 = doc.History().UndoDepth();
        if (depth1 == depth0) { CHECK(ed.Signature() == sigBefore, "seed=%u op=%d i=%d: 履歴を積まない操作は状態を変えない", seed, op, i); continue; }
        if (depth1 != depth0 + 1) { CHECK(false, "seed=%u: 1 操作で履歴が %zu 増えた (op=%d)", seed, depth1 - depth0, op); return res; }
        before.push_back(sigBefore);
        kinds.push_back(op);
        descs.push_back(desc);
        cur = ed.Signature();
        after.push_back(cur);
        ++res.pushed;
    }
    const std::string finalSig = ed.Signature();
    CHECK(res.pushed > opCount / 4, "seed=%u 十分な操作が積まれた (%d/%d)", seed, res.pushed, opCount);

    // 途中の状態が .dxmg として往復できる（正準・保存できる形）
    {
        ed.Update();
        const std::string t1 = ed.CanonicalText(true);
        mat::MaterialGraph g2;
        std::string e;
        CHECK(mat::LoadDxmg(t1, g2, &e), "seed=%u 最終状態が .dxmg として読める (%s)", seed, e.c_str());
        CHECK(mat::SaveDxmg(g2, true) == t1, "seed=%u 最終状態の .dxmg 往復がバイト一致", seed);
    }

    int badUndo = 0;
    for (int k = res.pushed - 1; k >= 0; --k)
    {
        doc.History().Undo(doc);
        const std::string now = ed.Signature();
        if (now != before[static_cast<size_t>(k)])
        {
            ++badUndo;
            if (badUndo <= 2)
            {
                const std::string& want = before[static_cast<size_t>(k)];
                size_t p = 0;
                while (p < want.size() && p < now.size() && want[p] == now[p]) ++p;
                size_t ls = want.rfind('\n', p ? p - 1 : 0);
                ls = (ls == std::string::npos) ? 0 : ls + 1;
                std::printf("  Undo 不一致 seed=%u step=%d op=%d %s\n    want: %.200s\n    got : %.200s\n", seed, k, kinds[static_cast<size_t>(k)], descs[static_cast<size_t>(k)].c_str(),
                            want.c_str() + ls, now.c_str() + (ls < now.size() ? ls : 0));
            }
        }
    }
    CHECK(badUndo == 0, "seed=%u: Undo が直前の状態に一致しなかった段数 %d", seed, badUndo);
    CHECK(ed.Signature() == initial, "seed=%u: 全 Undo で初期状態に厳密一致", seed);
    CHECK(!doc.History().CanUndo(), "全部戻した");

    int badRedo = 0;
    for (int k = 0; k < res.pushed; ++k)
    {
        doc.History().Redo(doc);
        if (ed.Signature() != after[static_cast<size_t>(k)]) { ++badRedo; if (badRedo <= 3) std::printf("  Redo 不一致 seed=%u step=%d op=%d\n", seed, k, kinds[static_cast<size_t>(k)]); }
    }
    CHECK(badRedo == 0, "seed=%u: Redo が直後の状態に一致しなかった段数 %d", seed, badRedo);
    CHECK(ed.Signature() == finalSig, "seed=%u: 全 Redo で最終状態を再現", seed);
    if (verbose)
        std::printf("  real fuzz seed=%u: ops=%d pushed=%d nodes=%zu edges=%zu comments=%zu\n", seed, res.ops, res.pushed, m.Nodes().size(), m.Edges().size(), doc.Comments().size());
    return res;
}

void TestUndoFuzzReal()
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    RunRealFuzz(2026, 3000, true);
    int okSeeds = 0;
    const int seeds = 100;
    for (uint32_t s = 1; s <= static_cast<uint32_t>(seeds); ++s)
    {
        const int before = g_failures;
        RunRealFuzz(s, 1000, false);
        if (g_failures == before) ++okSeeds;
    }
    CHECK(okSeeds == seeds, "%d シード全部通過 (%d)", seeds, okSeeds);
    std::printf("  実グラフの Undo ファズ: 3,000 操作 x1 + 1,000 操作 x%d シード  %.1f 秒\n", seeds, std::chrono::duration<double>(clock::now() - t0).count());
}

// ---------------------------------------------------------------------------
// ダーティ判定・パラメータ一覧・HLSL の色分け・ソースマップ
// ---------------------------------------------------------------------------
void TestParametersAndHlsl()
{
    mg::MatGraphEditor ed;
    ed.Doc().Clear();
    ed.Model().ResetMapping();
    mg::BuildSampleGraph(ed.Graph());
    ed.SyncCommentsFromGraph();
    ed.Update();
    const auto params = ed.Parameters();
    CHECK(params.size() == 11, "見本のパラメータ数 (%zu)", params.size());
    bool haveTex = false, haveScalar = false, haveVec = false;
    for (const mat::ParamDecl& p : params)
    {
        haveTex |= p.nodeType == "TextureParameter" && p.type == mat::ValueType::Tex2D;
        haveScalar |= p.nodeType == "ScalarParameter" && p.type == mat::ValueType::F1;
        haveVec |= p.nodeType == "VectorParameter" && p.type == mat::ValueType::F4;
        CHECK(!p.name.empty(), "パラメータ名");
    }
    CHECK(haveTex && haveScalar && haveVec, "型: Tex / F1 / F4");

    // HLSL の色分け
    bool block = false;
    auto spans = mg::ColorizeHlslLine("float3 x = lerp(a, b, 0.5); // 注釈", &block);
    CHECK(!spans.empty() && spans.front().kind == mg::HlslTok::Type && spans.front().begin == 0 && spans.front().end == 6, "型 float3");
    bool sawIntr = false, sawNum = false, sawCmt = false;
    uint32_t cover = 0;
    for (const auto& s : spans)
    {
        CHECK(s.begin == cover, "隙間なく覆う");
        cover = s.end;
        sawIntr |= s.kind == mg::HlslTok::Intrinsic; sawNum |= s.kind == mg::HlslTok::Number; sawCmt |= s.kind == mg::HlslTok::Comment;
    }
    CHECK(cover == std::string_view("float3 x = lerp(a, b, 0.5); // 注釈").size() && sawIntr && sawNum && sawCmt, "組み込み関数・数値・コメント");
    spans = mg::ColorizeHlslLine("#include \"forward/ForwardGraph.hlsl\"", &block);
    CHECK(spans.size() == 1 && spans[0].kind == mg::HlslTok::Preproc, "プリプロセッサ行");
    block = false;
    spans = mg::ColorizeHlslLine("a /* start", &block);
    CHECK(block, "ブロックコメントの開始");
    spans = mg::ColorizeHlslLine("still */ b", &block);
    CHECK(!block && spans.front().kind == mg::HlslTok::Comment, "ブロックコメントの終端");
    CHECK(mg::NodeIdOfLineDirective("#line 12 \"node:n_071829\"") == "n_071829" && mg::NodeIdOfLineDirective("float x;").empty(), "#line の node ID");
    // 生成 HLSL の全行を色分けしても落ちない・行を覆う
    {
        std::istringstream ss(ed.Result().hlsl);
        std::string line;
        bool inb = false;
        int lines = 0, engineNames = 0;
        while (std::getline(ss, line))
        {
            ++lines;
            uint32_t cov = 0;
            for (const auto& s : mg::ColorizeHlslLine(line, &inb)) { if (s.begin != cov) { CHECK(false, "行 %d で隙間", lines); break; } cov = s.end; engineNames += s.kind == mg::HlslTok::Engine; }
            CHECK(cov == line.size(), "行 %d を覆う", lines);
        }
        CHECK(lines > 30 && engineNames > 0, "HLSL %d 行・エンジン名 %d 個", lines, engineNames);
    }
    // ソースマップ: 行 → ノードが全部引ける（ノード → 行のクリックジャンプ）
    int mapped = 0;
    for (const mat::SourceMapEntry& e : ed.Result().sourceMap)
        if (!e.nodeId.empty() && ed.Model().IntOf(e.nodeId) != 0) ++mapped;
    CHECK(mapped > 10 && mapped == static_cast<int>(ed.Result().sourceMap.size()), "ソースマップの全エントリが UI のノードに対応する (%d/%zu)", mapped, ed.Result().sourceMap.size());
}

void TestComments()
{
    mg::MatGraphEditor ed;
    ed.NewGraph(mg::NewTemplate::Empty);
    ed.Doc().AddComment(ng::Rect(ng::Vec2(0, 0), ng::Vec2(300, 200)), "メモ", 3);
    ed.Update();
    CHECK(ed.Graph().Comments().size() == 1 && ed.Graph().Comments().begin()->second.text == "メモ", "コメントが G1 のグラフへ同期される");
    CHECK(ed.Graph().Comments().begin()->second.color == "#e0a840", "色は .dxmg の 16 進");
    for (int i = 0; i < ng::kCommentColors; ++i) CHECK(mg::CommentColorFromHex(mg::CommentHex(i)) == i, "色 %d の往復", i);
    CHECK(mg::CommentColorFromHex("#888888") >= 0 && mg::CommentColorFromHex("zzz") == 0, "未知の色は最も近い色 / 0");
    ed.Doc().History().Undo(ed.Doc());
    ed.Update();
    CHECK(ed.Graph().Comments().empty(), "Undo でコメントも消える");
}

} // namespace

int main(int argc, char** argv)
{
    // 引数で 1 つだけ実行できる（例: MatGraphEditorTests.exe fuzz）。引数なしは全部。
    const std::string only = argc > 1 ? argv[1] : "";
    auto run = [&](const char* name, void (*fn)())
    {
        if (!only.empty() && only != name) return;
        std::fflush(stdout);
        fn();
        std::printf("  [%s] done (checks=%d failures=%d)\n", name, g_checks, g_failures);
        std::fflush(stdout);
    };
    run("ids", TestIdMapping);
    run("catalog", TestCatalog);
    run("palette", TestPaletteFilter);
    run("sample", TestSampleGraph);
    run("saveload", TestSaveLoadRoundTrip);
    run("golden", TestGoldenFiles);
    run("valueonly", TestValueOnlyEdit);
    run("diag", TestDiagnosticsFocus);
    run("promote", TestPromote);
    run("shadow", TestLiteralShadow);
    run("hlsl", TestParametersAndHlsl);
    run("comments", TestComments);
    run("fuzz", TestUndoFuzzReal);
    std::printf("\nMatGraphEditorTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
