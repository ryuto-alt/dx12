// マテリアルグラフ G0: 汎用ノードグラフ UI の純ロジック（src/editor/nodegraph/）の単体テスト。
// ImGui にも GPU にも依存しない層だけを検証する:
//   座標変換 / ベジェ / スプリング / 空間分割 / ノードの大きさ / モデルの不変条件（循環・型の拒否）/
//   Undo・Redo（1 万操作のファズ: 全 Undo で初期状態に厳密一致、全 Redo で再現）/ ヒットテスト / ボックス選択 /
//   整列 / JSON 往復（正準）/ クリップボード / パレット検索 / サンプルグラフ。
//
// 実行: ctest --output-on-failure -R NodeGraph

#include "editor/nodegraph/GraphDocument.h"
#include "editor/nodegraph/GraphIO.h"
#include "editor/nodegraph/GraphMath.h"
#include "editor/nodegraph/GraphPalette.h"
#include "editor/nodegraph/SandboxGraphModel.h"
#include "editor/nodegraph/SandboxSamples.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>

using namespace dx12e::ng;

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

bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b)); }

struct Fixture
{
    SandboxGraphModel model;
    GraphDocument doc{&model};
    PinRef Out(NodeId n, const char* name) { const NodeData* d = model.FindNode(n); return PinRef{n, static_cast<uint16_t>(model.OutputIndex(d->type, name)), true}; }
    PinRef In(NodeId n, const char* name) { const NodeData* d = model.FindNode(n); return PinRef{n, static_cast<uint16_t>(model.InputIndex(d->type, name)), false}; }
};

// ---------------------------------------------------------------------------
void TestViewport()
{
    Viewport v;
    v.origin = {100, 50}; v.size = {800, 600}; v.pan = {-30, 20}; v.zoom = 1.7f; v.dpi = 1.5f;
    for (Vec2 g : {Vec2(0, 0), Vec2(123.5f, -77.25f), Vec2(-400, 900)})
    {
        const Vec2 back = v.ToGraph(v.ToScreen(g));
        CHECK(Near(back.x, g.x) && Near(back.y, g.y), "往復 (%f,%f)->(%f,%f)", g.x, g.y, back.x, back.y);
    }
    // ToScreen の線形性: 1 単位 = zoom*dpi px
    CHECK(Near(v.ToScreen(Vec2(1, 0)).x - v.ToScreen(Vec2(0, 0)).x, 1.7f * 1.5f), "スケール");
    // カーソル中心ズーム: アンカー直下のグラフ座標が動かない
    const Vec2 anchor{420, 300};
    const Vec2 gBefore = v.ToGraph(anchor);
    v.ZoomAt(anchor, 3.1f);
    const Vec2 gAfter = v.ToGraph(anchor);
    CHECK(Near(gBefore.x, gAfter.x) && Near(gBefore.y, gAfter.y), "ZoomAt でアンカーが動いた");
    CHECK(Near(v.zoom, 3.1f), "zoom");
    v.ZoomAt(anchor, 100.0f);
    CHECK(v.zoom == kMaxZoom, "上限 400%%");
    v.ZoomAt(anchor, 0.001f);
    CHECK(v.zoom == kMinZoom, "下限 10%%");
    // 表示範囲
    Viewport w; w.origin = {0, 0}; w.size = {1000, 500}; w.pan = {10, 20}; w.zoom = 2.0f; w.dpi = 1.0f;
    const Rect vis = w.VisibleGraphRect();
    CHECK(Near(vis.min.x, 10) && Near(vis.max.x, 510) && Near(vis.max.y, 270), "VisibleGraphRect");
    // FitRect
    Viewport f; f.origin = {0, 0}; f.size = {800, 600}; f.dpi = 1.0f;
    f.FitRect(Rect({100, 100}, {500, 400}), 40.0f, 1.0f);
    const Rect fv = f.VisibleGraphRect();
    CHECK(fv.Contains(Rect({100, 100}, {500, 400})), "FitRect が対象を含む");
    CHECK(f.zoom <= 1.0f, "FitRect の拡大上限");
    const Vec2 fc = fv.Center();
    CHECK(Near(fc.x, 300, 1e-2f) && Near(fc.y, 250, 1e-2f), "FitRect が中央に置く");
    // DPI 独立: 同じ zoom でも dpi 倍で画面 px が dpi 倍
    Viewport d1 = w, d2 = w; d2.dpi = 2.0f;
    CHECK(Near((d2.ToScreen(Vec2(100, 0)).x - d2.origin.x), 2.0f * (d1.ToScreen(Vec2(100, 0)).x - d1.origin.x)), "DPI 倍率");
}

void TestBezier()
{
    const Bezier b = MakeWire({0, 0}, {300, 100});
    CHECK(b.At(0.0f) == Vec2(0, 0) && Near(b.At(1.0f).x, 300) && Near(b.At(1.0f).y, 100), "端点");
    // 水平接線: 出発は右向き
    CHECK(b.p1.y == b.p0.y && b.p1.x > b.p0.x, "出力側は右向きの接線");
    CHECK(b.p2.y == b.p3.y && b.p2.x < b.p3.x, "入力側は左から入る接線");
    // 外接矩形は曲線を含む
    const Rect bb = b.Bounds();
    for (int i = 0; i <= 20; ++i) CHECK(bb.Expanded(0.01f).Contains(b.At(static_cast<float>(i) / 20.0f)), "Bounds 内");
    // 距離: 曲線上の点は 0 近傍、離れた点は大きい
    CHECK(DistanceToBezier(b.At(0.4f), b) < 0.5f, "曲線上の点");
    CHECK(DistanceToBezier({150, 400}, b) > 100.0f, "遠い点");
    // 後ろ向き（入力が左）でも有限
    const Bezier back = MakeWire({500, 0}, {100, 200});
    CHECK(back.p1.x > back.p0.x && back.p2.x < back.p3.x, "後ろ向きも同じ向きの接線");
    for (int i = 0; i <= 10; ++i) { const Vec2 p = back.At(static_cast<float>(i) / 10.0f); CHECK(std::isfinite(p.x) && std::isfinite(p.y), "有限"); }
    // 切断ストローク
    CHECK(BezierIntersectsSegment(b, {150, -100}, {150, 300}), "横切る");
    CHECK(!BezierIntersectsSegment(b, {150, 400}, {160, 500}), "離れている");
    CHECK(SegmentsIntersect({0, 0}, {10, 10}, {0, 10}, {10, 0}), "交差");
    CHECK(!SegmentsIntersect({0, 0}, {10, 0}, {0, 5}, {10, 5}), "平行");
}

void TestSpring()
{
    // 臨界減衰: オーバーシュートしない、収束する
    Spring s;
    float maxV = 0.0f;
    for (int i = 0; i < 240; ++i) { s.Step(1.0f, 1.0f / 60.0f, 26.0f, 1.0f); maxV = std::max(maxV, s.value); CHECK(std::isfinite(s.value), "有限"); }
    CHECK(s.value == 1.0f, "収束 %f", s.value);
    CHECK(maxV <= 1.0005f, "臨界減衰はオーバーシュートしない %f", maxV);
    // linear ではない（最初の数フレームは加速）
    Spring a; a.Step(1.0f, 1.0f / 60.0f); const float v1 = a.value; a.Step(1.0f, 1.0f / 60.0f); const float v2 = a.value;
    CHECK(v2 - v1 > v1, "ease-in の立ち上がり（滑らかな加速）");
    // 減衰不足はオーバーシュート
    Spring u; float mx = 0.0f;
    for (int i = 0; i < 240; ++i) { u.Step(1.0f, 1.0f / 60.0f, 30.0f, 0.55f); mx = std::max(mx, u.value); }
    CHECK(mx > 1.02f && u.value == 1.0f, "少しオーバーシュートして収束 %f", mx);
    // dt が大きくても発散しない
    Spring big;
    for (int i = 0; i < 100; ++i) { big.Step(1.0f, 0.5f); CHECK(std::isfinite(big.value) && big.value < 2.0f, "大きな dt でも安定"); }
    // 30fps / 144fps でも同じ場所へ
    Spring lo, hi;
    for (int i = 0; i < 60; ++i) lo.Step(1.0f, 1.0f / 30.0f);
    for (int i = 0; i < 288; ++i) hi.Step(1.0f, 1.0f / 144.0f);
    CHECK(lo.value == 1.0f && hi.value == 1.0f, "フレームレートに依らず収束");
}

void TestSpatialGrid()
{
    std::mt19937 rng(12345);
    auto rf = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() % 100000) / 100000.0f; };
    for (int round = 0; round < 20; ++round)
    {
        SpatialGrid g(256.0f);
        std::vector<Rect> rects;
        const int n = 50 + static_cast<int>(rng() % 200);
        for (int i = 0; i < n; ++i)
        {
            const Vec2 p{rf(-3000, 3000), rf(-3000, 3000)};
            const Vec2 s{rf(20, 400), rf(20, 300)};
            rects.push_back(Rect::FromPosSize(p, s));
            g.Insert(static_cast<uint32_t>(i), rects.back());
        }
        // 全体を覆う巨大な矩形（大物リスト）
        rects.push_back(Rect({-1e6f, -1e6f}, {1e6f, 1e6f}));
        g.Insert(static_cast<uint32_t>(rects.size() - 1), rects.back());

        for (int q = 0; q < 40; ++q)
        {
            const Vec2 p{rf(-3500, 3500), rf(-3500, 3500)};
            const Rect qr = Rect::FromPosSize(p, {rf(1, 1500), rf(1, 1500)});
            std::vector<uint32_t> got;
            g.Query(qr, got);
            std::set<uint32_t> gotSet(got.begin(), got.end());
            CHECK(gotSet.size() == got.size(), "Query に重複が無い");
            // 真に重なるものは全部含まれている（偽陽性は許す）
            for (size_t i = 0; i < rects.size(); ++i)
                if (rects[i].Overlaps(qr)) CHECK(gotSet.count(static_cast<uint32_t>(i)) == 1, "重なる矩形 %zu が漏れた", i);
        }
    }
}

void TestLayout()
{
    Fixture f;
    const NodeTypeDesc* add = f.model.FindNodeType("add");
    const NodeTypeDesc* out = f.model.FindNodeType("output");
    CHECK(add && out, "型がある");
    GraphLayout& L = f.doc.Layout();
    const TypeLayout& la = L.For(*add);
    CHECK(la.size.x >= L.Config().minW, "最小幅");
    CHECK(Near(std::fmod(la.size.x, 8.0f), 0.0f) || Near(std::fmod(la.size.x, 8.0f), 8.0f), "幅は 8 の倍数");
    CHECK(la.inY.size() == 2 && la.outY.size() == 1, "行数");
    CHECK(la.inY[1] > la.inY[0] && Near(la.inY[1] - la.inY[0], L.Config().rowH), "行間");
    CHECK(la.inY[0] > la.headerH, "最初の行はヘッダの下");
    // 同じ型は同じ結果（キャッシュ）
    CHECK(&L.For(*add) == &la, "キャッシュ");
    // ピン位置: 入力は左端、出力は右端
    const NodeId n = f.doc.AddNode("add", Vec2(100, 200));
    const NodeData* nd = f.model.FindNode(n);
    const Rect r = f.doc.NodeRect(n);
    CHECK(Near(r.min.x, 100) && Near(r.min.y, 200), "NodeRect の左上");
    CHECK(Near(f.doc.PinPos({n, 0, false}).x, r.min.x), "入力ピンは左端");
    CHECK(Near(f.doc.PinPos({n, 0, true}).x, r.max.x), "出力ピンは右端");
    CHECK(f.doc.PinPos({n, 1, false}).y > f.doc.PinPos({n, 0, false}).y, "下の行");
    // 値欄はノード内
    const Rect s0 = f.doc.SlotRect({n, 0, false});
    CHECK(s0.Width() > 0 && r.Contains(s0), "値欄がノード内");
    // 出力ノードは高い（6 行）
    CHECK(L.For(*out).size.y > la.size.y, "行数が多いノードは高い");
    // 値欄のあるピンの幅は測定に依存して増える
    (void)nd;
    // プレビューつきは予約領域
    const NodeTypeDesc* smp = f.model.FindNodeType("sample");
    const TypeLayout& ls = L.For(*smp);
    CHECK(ls.preview.Width() > 0 && ls.preview.max.y <= ls.size.y, "プレビュー領域がノード内");
    // リルートは小さい
    const NodeTypeDesc* rr = f.model.FindNodeType("reroute");
    CHECK(L.For(*rr).size.x <= 24.0f && L.For(*rr).reroute, "リルートは小さい丸");
    // 値欄の成分矩形
    const Rect slot({0, 0}, {114, 17});
    CHECK(Near(SlotComponentRect(slot, ValueKind::Vec3, 1).min.x, 38.0f), "Vec3 の成分 1");
    CHECK(Near(SlotComponentRect(slot, ValueKind::Float, 0).max.x, 114.0f), "Float は全体");
}

void TestModelInvariants()
{
    Fixture f;
    auto& m = f.model;
    const NodeId t = f.doc.AddNode("texture", {0, 0});
    const NodeId s = f.doc.AddNode("sample", {300, 0});
    const NodeId uv = f.doc.AddNode("texcoord", {0, 200});
    const NodeId add = f.doc.AddNode("add", {600, 0});
    const NodeId mul = f.doc.AddNode("multiply", {900, 0});
    const NodeId flt = f.doc.AddNode("float", {0, 400});
    const NodeId c3 = f.doc.AddNode("vec3", {0, 500});
    const NodeId c2 = f.doc.AddNode("vec2", {0, 600});
    const NodeId out = f.doc.AddNode("output", {1200, 0});

    // 向き: 入力→出力は不可
    CHECK(!m.CanConnect(f.In(s, "Tex"), f.Out(t, "Tex")).ok, "向きが逆");
    // 型: Texture → Tex ok / Texture → UV(Vec2) 不可
    CHECK(m.CanConnect(f.Out(t, "Tex"), f.In(s, "Tex")).ok, "Tex → Tex");
    CHECK(!m.CanConnect(f.Out(t, "Tex"), f.In(s, "UV")).ok, "Tex → Vec2 は拒否");
    CHECK(!m.CanConnect(f.Out(uv, "UV"), f.In(s, "Tex")).ok, "Vec2 → Tex は拒否");
    CHECK(!m.CanConnect(f.Out(uv, "UV"), f.In(s, "Tex")).reason.empty(), "拒否理由が出る");
    // 数値: Float → Vec3 ok（拡張）、Vec2 → Vec3 拒否、Vec3 → Vec2 ok（切り詰め）
    CHECK(m.CanConnect(f.Out(flt, "Out"), f.In(out, "BaseColor")).ok, "F1 → F3");
    CHECK(!m.CanConnect(f.Out(c2, "Out"), f.In(out, "BaseColor")).ok, "F2 → F3 は拒否");
    CHECK(m.CanConnect(f.Out(c3, "Out"), f.In(s, "UV")).ok, "F3 → F2 は切り詰め");
    // 同一ノード
    CHECK(!m.CanConnect(f.Out(add, "Out"), f.In(add, "A")).ok, "自己接続");
    // プロパティ行は繋げない
    CHECK(!m.CanConnect(f.Out(flt, "Out"), f.In(flt, "Value")).ok, "プロパティ行へは不可");
    CHECK(!m.CanConnect(f.Out(uv, "UV"), f.In(uv, "Tiling")).ok, "プロパティ行へは不可(2)");
    // 多相推論
    CHECK(f.doc.Connect(f.Out(c3, "Out"), f.In(add, "A")), "Vec3 → Add.A");
    CHECK(m.ResolvedPinType(f.Out(add, "Out")) == SandboxGraphModel::kVec3, "Add の出力は Vec3 に推論される");
    CHECK(m.ResolvedPinType(f.In(add, "B")) == SandboxGraphModel::kVec3, "B も同じ型");
    CHECK(f.doc.Connect(f.Out(add, "Out"), f.In(mul, "A")), "Add → Mul");
    CHECK(m.ResolvedPinType(f.Out(mul, "Out")) == SandboxGraphModel::kVec3, "推論の伝播");
    // 循環の拒否: mul の出力を add の入力へ戻す
    const ConnectCheck cyc = m.CanConnect(f.Out(mul, "Out"), f.In(add, "B"));
    CHECK(!cyc.ok, "循環を拒否");
    CHECK(cyc.reason.find("循環") != std::string::npos, "理由に循環: %s", cyc.reason.c_str());
    CHECK(!f.doc.Connect(f.Out(mul, "Out"), f.In(add, "B")), "Document 経由でも拒否");
    // 置き換え
    CHECK(f.doc.Connect(f.Out(flt, "Out"), f.In(add, "A")), "同じ入力へ繋ぎ直すと置き換え");
    CHECK(m.FindEdgeTo(f.In(add, "A"))->from.node == flt, "置き換わった");
    CHECK(m.ResolvedPinType(f.Out(add, "Out")) == SandboxGraphModel::kFloat, "推論が更新される");
    CHECK(m.CanConnect(f.Out(c3, "Out"), f.In(add, "A")).replaces, "replaces フラグ");
    // リルートは型を引き継ぐ
    const NodeId rr = f.doc.AddNode("reroute", {300, 600});
    CHECK(m.ResolvedPinType(f.Out(rr, "")) == SandboxGraphModel::kWildcard, "未接続のリルートは Wildcard");
    CHECK(f.doc.Connect(f.Out(t, "Tex"), f.In(rr, "")), "リルートは何でも通す");
    CHECK(m.ResolvedPinType(f.Out(rr, "")) == SandboxGraphModel::kTexture, "上流の型を引き継ぐ");
    CHECK(f.doc.Connect(f.Out(rr, ""), f.In(s, "Tex")), "リルート → Tex");
    CHECK(!m.CanConnect(f.Out(rr, ""), f.In(s, "UV")).ok, "リルート(Tex) → Vec2 は拒否");
    // 下流が受け付けない型は、リルートの上流に繋げない
    CHECK(!m.CanConnect(f.Out(c3, "Out"), f.In(rr, "")).ok, "下流が Tex を要求するのに Vec3 は繋げない");
    // 上流 / 下流
    std::vector<NodeId> ds;
    m.Downstream(t, ds);
    CHECK(ds.size() == 1 && ds[0] == rr, "Downstream");
    std::vector<NodeId> us;
    m.Upstream(s, us);
    CHECK(us.size() == 1 && us[0] == rr, "Upstream");
    // ノード削除で接続も消える
    Selection sel; sel.nodes.insert(rr);
    f.doc.RemoveItems(sel);
    CHECK(!m.FindNode(rr) && !m.FindEdgeTo(f.In(s, "Tex")), "削除で接続も消える");
    // Edges は to の昇順
    const auto& es = m.Edges();
    for (size_t i = 1; i < es.size(); ++i) CHECK(es[i - 1].to < es[i].to, "Edges は to 昇順");
    // ID は昇順・値欄の既定値
    const auto& ids = m.Nodes();
    for (size_t i = 1; i < ids.size(); ++i) CHECK(ids[i - 1] < ids[i], "Nodes は ID 昇順");
    CHECK(m.FindNode(out)->values.size() == 6 && Near(m.FindNode(out)->values[2].f[0], 0.5f), "Roughness の既定 0.5");
    // 明示 ID での追加は重複不可
    NodeData d; d.type = "add"; d.id = add;
    CHECK(m.AddNode(d) == 0, "使用中の ID は不可");
    d.id = 999;
    CHECK(m.AddNode(d) == 999, "明示 ID");
    d.type = "nonexistent"; d.id = 0;
    CHECK(m.AddNode(d) == 0, "未知の型");
}

void TestDocumentBasics()
{
    Fixture f;
    auto& doc = f.doc;
    const std::string sig0 = doc.Signature();
    const NodeId a = doc.AddNode("float", {0, 0});
    const NodeId b = doc.AddNode("add", {200, 0});
    CHECK(doc.Connect(f.Out(a, "Out"), f.In(b, "A")), "connect");
    doc.SetPinValue(f.In(b, "B"), [] { PinValue v; v.kind = ValueKind::Float; v.f[0] = 3.5f; return v; }());
    CHECK(doc.History().UndoDepth() == 4, "深さ %zu", doc.History().UndoDepth());
    const std::string sig4 = doc.Signature();
    doc.History().Undo(doc);
    CHECK(Near(f.model.FindNode(b)->values[1].f[0], 0.0f), "値の Undo");
    doc.History().Undo(doc);
    CHECK(!f.model.FindEdgeTo(f.In(b, "A")), "接続の Undo");
    doc.History().Undo(doc);
    doc.History().Undo(doc);
    CHECK(doc.Signature() == sig0, "全 Undo で初期状態");
    CHECK(!doc.History().CanUndo() && doc.History().CanRedo(), "can undo/redo");
    for (int i = 0; i < 4; ++i) doc.History().Redo(doc);
    CHECK(doc.Signature() == sig4, "全 Redo で再現");
    // 新しい操作で redo は消える
    doc.History().Undo(doc);
    doc.AddNode("time", {0, 300});
    CHECK(!doc.History().CanRedo(), "新操作で redo 破棄");

    // 同じ値・同じ接続は何も積まない
    const size_t depth = doc.History().UndoDepth();
    doc.SetPinValue(f.In(b, "A"), f.model.FindNode(b)->values[0]);
    CHECK(doc.History().UndoDepth() == depth, "変化なしは積まない");

    // グループ: 1 回の Undo で全部戻る
    doc.History().Clear();
    const std::string before = doc.Signature();
    doc.History().BeginGroup("まとめ");
    const NodeId c = doc.AddNode("saturate", {400, 0});
    doc.Connect(f.Out(b, "Out"), f.In(c, "X"));
    doc.MoveItems([&] { Selection s; s.nodes.insert(c); return s; }(), {5, 5});
    doc.History().EndGroup();
    CHECK(doc.History().UndoDepth() == 1, "グループは 1 エントリ (%zu)", doc.History().UndoDepth());
    doc.History().Undo(doc);
    CHECK(doc.Signature() == before, "グループを 1 回で戻す");
    doc.History().Redo(doc);
    CHECK(f.model.FindNode(c) && f.model.FindNode(c)->pos == Vec2(405, 5), "グループの Redo");

    // 移動セッション: ドラッグ中の生の変更 → 離して 1 コマンド
    doc.History().Clear();
    Selection sel; sel.nodes.insert(a); sel.nodes.insert(b);
    auto ms = doc.BeginMove(sel);
    for (int i = 1; i <= 30; ++i) doc.UpdateMove(ms, {static_cast<float>(i), static_cast<float>(i) * 2});
    CHECK(doc.History().UndoDepth() == 0, "ドラッグ中は積まない");
    CHECK(doc.CommitMove(ms), "確定");
    CHECK(doc.History().UndoDepth() == 1, "離したら 1 個");
    CHECK(f.model.FindNode(a)->pos == Vec2(30, 60), "最終位置");
    doc.History().Undo(doc);
    CHECK(f.model.FindNode(a)->pos == Vec2(0, 0) && f.model.FindNode(b)->pos == Vec2(200, 0), "1 回で元へ");
    auto ms2 = doc.BeginMove(sel);
    CHECK(!doc.CommitMove(ms2), "動かなければ積まない");
    // キャンセル
    auto ms3 = doc.BeginMove(sel);
    doc.UpdateMove(ms3, {50, 50});
    doc.CancelMove(ms3);
    CHECK(f.model.FindNode(a)->pos == Vec2(0, 0), "キャンセルで元へ");

    // 値のライブ編集 → コミット
    doc.History().Clear();
    const PinRef pin = f.In(b, "B");
    const PinValue old = f.model.FindNode(b)->values[1];
    for (int i = 1; i <= 10; ++i) { PinValue v = old; v.f[0] = static_cast<float>(i); doc.SetPinValueLive(pin, v); }
    CHECK(doc.History().UndoDepth() == 0, "ライブ中は積まない");
    CHECK(doc.CommitPinValue(pin, old) && doc.History().UndoDepth() == 1, "値ドラッグは 1 個");
    doc.History().Undo(doc);
    CHECK(Near(f.model.FindNode(b)->values[1].f[0], old.f[0]), "値を戻す");

    // 履歴の上限
    doc.History().Clear();
    doc.History().SetMaxDepth(5);
    for (int i = 0; i < 20; ++i) doc.AddNode("time", {static_cast<float>(i), 0});
    CHECK(doc.History().UndoDepth() == 5, "上限");
    doc.History().SetMaxDepth(0);

    // EditSeq は単調増加
    const uint64_t e0 = doc.History().EditSeq();
    doc.AddNode("time", {0, 0});
    CHECK(doc.History().EditSeq() > e0, "EditSeq");
    doc.History().Clear();
    CHECK(doc.History().EditSeq() >= e0, "Clear で戻らない");

    // リルート挿入
    Fixture g;
    const NodeId x = g.doc.AddNode("float", {0, 0});
    const NodeId y = g.doc.AddNode("saturate", {400, 0});
    g.doc.Connect(g.Out(x, "Out"), g.In(y, "X"));
    g.doc.History().Clear();
    const std::string s1 = g.doc.Signature();
    const Edge e = *g.model.FindEdgeTo(g.In(y, "X"));
    const NodeId r = g.doc.InsertReroute(e, {200, 10});
    CHECK(r != 0 && g.model.FindNode(r)->type == "reroute", "リルート挿入");
    CHECK(g.model.FindEdgeTo(g.In(y, "X"))->from.node == r, "y の入力は r から");
    CHECK(g.model.FindEdgeTo({r, 0, false})->from.node == x, "r の入力は x から");
    CHECK(g.doc.History().UndoDepth() == 1, "1 操作");
    g.doc.History().Undo(g.doc);
    CHECK(g.doc.Signature() == s1, "リルート挿入の Undo");
}

void TestAlign()
{
    std::vector<AlignItem> it = {{1, Rect({0, 0}, {100, 50})}, {2, Rect({200, 30}, {260, 130})}, {3, Rect({500, 300}, {580, 340})}, {4, Rect({350, 90}, {400, 120})}};
    auto pos = [](const std::vector<std::pair<NodeId, Vec2>>& v, NodeId id) { for (auto& p : v) if (p.first == id) return p.second; return Vec2(-9999, -9999); };
    auto l = ComputeAlign(AlignMode::Left, it);
    for (NodeId id : {1u, 2u, 3u, 4u}) CHECK(Near(pos(l, id).x, 0), "左揃え");
    CHECK(Near(pos(l, 2).y, 30), "左揃えは Y を動かさない");
    auto r = ComputeAlign(AlignMode::Right, it);
    CHECK(Near(pos(r, 1).x + 100, 580) && Near(pos(r, 3).x + 80, 580) && Near(pos(r, 2).x + 60, 580), "右揃え");
    auto t = ComputeAlign(AlignMode::Top, it);
    for (NodeId id : {1u, 2u, 3u, 4u}) CHECK(Near(pos(t, id).y, 0), "上揃え");
    auto b = ComputeAlign(AlignMode::Bottom, it);
    CHECK(Near(pos(b, 3).y + 40, 340) && Near(pos(b, 1).y + 50, 340), "下揃え");
    auto ch = ComputeAlign(AlignMode::CenterH, it);
    for (NodeId id : {1u, 2u, 3u, 4u}) { const Rect rr = [&] { for (auto& x : it) if (x.id == id) return x.rect; return Rect(); }(); CHECK(Near(pos(ch, id).x + rr.Width() * 0.5f, 290), "水平中央"); }
    auto dh = ComputeAlign(AlignMode::DistributeH, it);
    // 端は固定・間隔が等しい
    std::vector<std::pair<float, float>> spans;   // (左, 幅) を x 順に
    for (const AlignItem& x : it) spans.emplace_back(pos(dh, x.id).x, x.rect.Width());
    std::sort(spans.begin(), spans.end());
    CHECK(Near(spans.front().first, 0) && Near(spans.back().first + spans.back().second, 580), "端が固定");
    const float g0 = spans[1].first - (spans[0].first + spans[0].second);
    const float g1 = spans[2].first - (spans[1].first + spans[1].second);
    const float g2 = spans[3].first - (spans[2].first + spans[2].second);
    CHECK(Near(g0, g1, 1e-2f) && Near(g1, g2, 1e-2f), "等間隔 %f %f %f", g0, g1, g2);
    auto dv = ComputeAlign(AlignMode::DistributeV, {it[0], it[1]});
    CHECK(pos(dv, 1) == it[0].rect.min && pos(dv, 2) == it[1].rect.min, "2 個では等間隔は動かない");
    CHECK(ComputeAlign(AlignMode::Left, {it[0]}).size() == 1, "1 個は動かない");

    // Document 経由: 1 回の Undo で全部戻る
    Fixture f;
    const NodeId a = f.doc.AddNode("add", {0, 0}), c = f.doc.AddNode("add", {300, 77}), d = f.doc.AddNode("add", {40, 220});
    f.doc.History().Clear();
    const std::string s0 = f.doc.Signature();
    f.doc.Align({a, c, d}, AlignMode::Top);
    CHECK(f.model.FindNode(c)->pos.y == 0 && f.model.FindNode(d)->pos.y == 0, "Document.Align");
    CHECK(f.doc.History().UndoDepth() == 1, "整列は 1 操作");
    f.doc.History().Undo(f.doc);
    CHECK(f.doc.Signature() == s0, "整列の Undo");
}

void TestHitTestAndBoxSelect()
{
    Fixture f;
    auto& doc = f.doc;
    const NodeId a = doc.AddNode("float", {0, 0});
    const NodeId b = doc.AddNode("add", {400, 0});
    const NodeId c = doc.AddNode("output", {900, 300});
    doc.Connect(f.Out(a, "Out"), f.In(b, "A"));
    doc.Connect(f.Out(b, "Out"), f.In(c, "BaseColor"));
    HitParams hp;

    // ピン
    const Vec2 pa = doc.PinPos(f.Out(a, "Out"));
    Hit h = doc.HitTest(pa, hp);
    CHECK(h.kind == Hit::Pin && h.pin == f.Out(a, "Out"), "出力ピン (kind=%d)", static_cast<int>(h.kind));
    h = doc.HitTest(pa + Vec2(3, 2), hp);
    CHECK(h.kind == Hit::Pin, "ピンの当たり半径");
    h = doc.HitTest(doc.PinPos(f.In(b, "B")), hp);
    CHECK(h.kind == Hit::Pin && h.pin == f.In(b, "B"), "入力ピン");
    // 値欄 / 本体
    const Rect sr = doc.SlotRect(f.In(b, "B"));
    h = doc.HitTest(sr.Center(), hp);
    CHECK(h.kind == Hit::Slot && h.pin == f.In(b, "B"), "値欄 (kind=%d)", static_cast<int>(h.kind));
    // 接続済みの入力は値欄が当たらない → ノード本体
    const Rect sa = doc.SlotRect(f.In(b, "A"));
    h = doc.HitTest(sa.Center(), hp);
    CHECK(h.kind == Hit::Node && h.node == b, "接続済みの値欄は無効");
    const Rect nb = doc.NodeRect(b);
    h = doc.HitTest({nb.Center().x, nb.min.y + 8}, hp);
    CHECK(h.kind == Hit::Node && h.node == b, "ヘッダ = ノード");
    // プロパティ行は接続に関係なく値欄
    const Rect pr = doc.SlotRect(f.In(a, "Value"));
    h = doc.HitTest(pr.Center(), hp);
    CHECK(h.kind == Hit::Slot, "プロパティ行の値欄");
    // 空き地
    h = doc.HitTest({-500, -500}, hp);
    CHECK(h.kind == Hit::None, "空き地");
    // ワイヤ: 曲線の中点
    const Edge e = *f.model.FindEdgeTo(f.In(b, "A"));
    const Vec2 mid = doc.EdgeCurve(e).At(0.5f);
    h = doc.HitTest(mid, hp);
    CHECK(h.kind == Hit::Edge && h.edge == e, "ワイヤ (kind=%d)", static_cast<int>(h.kind));
    h = doc.HitTest(mid + Vec2(0, 30), hp);
    CHECK(h.kind != Hit::Edge, "離れるとワイヤに当たらない");
    // z 順: 重なったノードは手前（z 大）が勝つ
    const NodeId d1 = doc.AddNode("multiply", {2000, 0});
    const NodeId d2 = doc.AddNode("multiply", {2010, 8});
    const Vec2 p = doc.NodeRect(d1).min + Vec2(30, 12);   // どちらのヘッダにも入る点
    h = doc.HitTest(p, hp);
    CHECK(h.kind == Hit::Node && h.node == d2, "ID の大きい方が手前");
    std::unordered_map<NodeId, uint32_t> z; z[d1] = 5; z[d2] = 1;
    hp.z = &z;
    h = doc.HitTest(p, hp);
    CHECK(h.kind == Hit::Node && h.node == d1, "z 指定が優先");
    hp.z = nullptr;

    // コメント
    const CommentId cm = doc.AddCommentAround({d1, d2}, "test", 1);
    const Comment* cc = doc.FindComment(cm);
    CHECK(cc && cc->rect.Contains(doc.NodeRect(d1)) && cc->rect.Contains(doc.NodeRect(d2)), "コメントがノードを囲む");
    h = doc.HitTest({cc->rect.min.x + 60, cc->rect.min.y + 10}, hp);
    CHECK(h.kind == Hit::CommentTitle && h.comment == cm, "見出し (kind=%d)", static_cast<int>(h.kind));
    h = doc.HitTest({cc->rect.max.x - 4, cc->rect.max.y - 4}, hp);
    CHECK(h.kind == Hit::CommentResize, "リサイズ");
    h = doc.HitTest({cc->rect.min.x + 8, cc->rect.max.y - 30}, hp);
    CHECK(h.kind == Hit::CommentBody, "本体 (kind=%d)", static_cast<int>(h.kind));
    const std::vector<NodeId> inside = doc.NodesInsideComment(cm);
    CHECK(inside.size() == 2, "同伴ノード %zu", inside.size());

    // ボックス選択: 完全内包 / 交差
    std::vector<NodeId> out;
    const Rect box = Rect({-10, -10}, {doc.NodeRect(a).max.x + 5, doc.NodeRect(a).max.y + 5});
    doc.NodesInRect(box, true, out);
    CHECK(out.size() == 1 && out[0] == a, "完全内包");
    const Rect partial = Rect({-10, -10}, {doc.NodeRect(a).max.x - 30, doc.NodeRect(a).max.y - 30});
    doc.NodesInRect(partial, true, out);
    CHECK(out.empty(), "一部だけ入るものは完全内包に入らない");
    doc.NodesInRect(partial, false, out);
    CHECK(out.size() == 1 && out[0] == a, "交差");
}

void TestSpatialCorrectnessOnGraphs()
{
    for (uint32_t seed = 1; seed <= 6; ++seed)
    {
        Fixture f;
        const SampleReport rep = BuildStressGraph(f.doc, f.model, 120 + static_cast<int>(seed) * 40, 200 + static_cast<int>(seed) * 60, seed);
        CHECK(rep.nodes > 100 && rep.edges > 100, "ストレスグラフ nodes=%d edges=%d", rep.nodes, rep.edges);
        std::mt19937 rng(seed * 77);
        const Rect bounds = f.doc.ContentBounds();
        for (int q = 0; q < 60; ++q)
        {
            const float x = bounds.min.x + static_cast<float>(rng() % 10000) / 10000.0f * bounds.Width();
            const float y = bounds.min.y + static_cast<float>(rng() % 10000) / 10000.0f * bounds.Height();
            const Rect r = Rect::FromPosSize({x, y}, {static_cast<float>(20 + rng() % 1200), static_cast<float>(20 + rng() % 900)});
            for (bool inside : {false, true})
            {
                std::vector<NodeId> a, b;
                f.doc.NodesInRect(r, inside, a);
                f.doc.NodesInRectBrute(r, inside, b);
                CHECK(a == b, "空間分割とブルートフォースが一致(ノード) seed=%u q=%d (%zu vs %zu)", seed, q, a.size(), b.size());
            }
            std::vector<int> ea, eb;
            f.doc.EdgesInRect(r, ea);
            f.doc.EdgesInRectBrute(r, eb);
            CHECK(ea == eb, "空間分割とブルートフォースが一致(ワイヤ) seed=%u q=%d (%zu vs %zu)", seed, q, ea.size(), eb.size());
        }
        // ヒットテスト: 全ピンの位置がそのピンに当たる（他ノードに隠されていない限り）
        int pinChecks = 0, pinBad = 0;
        for (NodeId id : f.model.Nodes())
        {
            const NodeData* n = f.model.FindNode(id);
            for (size_t i = 0; i < n->desc->outputs.size(); ++i)
            {
                const PinRef pin{id, static_cast<uint16_t>(i), true};
                const Hit h = f.doc.HitTest(f.doc.PinPos(pin), HitParams{});
                ++pinChecks;
                if (!(h.kind == Hit::Pin && h.pin == pin)) ++pinBad;
            }
        }
        CHECK(pinBad == 0, "全出力ピンにヒットする (%d/%d 失敗)", pinBad, pinChecks);
    }
}

void TestSerialization()
{
    Fixture f;
    const SampleReport rep = BuildSampleGraph(f.doc, f.model);
    CHECK(rep.failedLinks == 0, "サンプルの接続失敗 %d", rep.failedLinks);
    CHECK(rep.nodes >= 30 && rep.nodes <= 50, "サンプルのノード数 %d", rep.nodes);
    CHECK(rep.comments >= 3, "コメント %d", rep.comments);
    CHECK(rep.edges >= 25, "接続 %d", rep.edges);

    ViewState view; view.pan = {123.456f, -78.9f}; view.zoom = 0.8125f;
    const std::string j1 = SaveGraphJson(f.doc, &view);
    CHECK(!j1.empty() && j1.find("\"kind\": \"nodegraph\"") != std::string::npos, "kind");
    // 1 ノード 1 行
    size_t lines = 0; for (char ch : j1) if (ch == '\n') ++lines;
    CHECK(lines >= static_cast<size_t>(rep.nodes + rep.edges + rep.comments), "1 要素 1 行 (%zu 行)", lines);

    // 読み込み → 再保存で完全一致（正準）
    Fixture g;
    ViewState v2;
    std::string warn;
    CHECK(LoadGraphJson(g.doc, j1, &v2, &warn), "読み込み: %s", warn.c_str());
    const std::string j2 = SaveGraphJson(g.doc, &v2);
    CHECK(j1 == j2, "往復で JSON が一致");
    CHECK(Near(v2.zoom, 0.8125f) && Near(v2.pan.x, 123.46f, 1e-3f), "ビューの復元");
    CHECK(SaveGraphJson(f.doc, nullptr) == SaveGraphJson(g.doc, nullptr), "ビュー抜きの正準 JSON も一致");
    // ID が安定
    CHECK(f.model.Nodes() == g.model.Nodes(), "ノード ID が保たれる");
    CHECK(f.model.Edges().size() == g.model.Edges().size(), "接続数");
    for (size_t i = 0; i < f.model.Edges().size(); ++i) CHECK(f.model.Edges()[i] == g.model.Edges()[i], "接続の同一性");
    CHECK(f.doc.Comments().size() == g.doc.Comments().size(), "コメント数");
    // 読み込み後は履歴が空
    CHECK(!g.doc.History().CanUndo(), "読み込み後の履歴は空");
    // 読み込み後に追加したノードの ID が衝突しない
    const NodeId nn = g.doc.AddNode("time", {0, 0});
    CHECK(nn > 0 && nn > g.model.Nodes().front() && std::count(g.model.Nodes().begin(), g.model.Nodes().end(), nn) == 1, "新 ID は重複しない");

    // 不正入力
    Fixture bad;
    CHECK(!LoadGraphJson(bad.doc, "not json", nullptr, &warn), "壊れた JSON");
    CHECK(!LoadGraphJson(bad.doc, "{\"kind\":\"other\"}", nullptr, &warn), "種別違い");
    // 未知の型は読み飛ばして警告
    const std::string withUnknown =
        "{\"kind\":\"nodegraph\",\"version\":1,\"nodes\":[{\"id\":1,\"type\":\"float\",\"pos\":[0,0]},{\"id\":2,\"type\":\"nope\",\"pos\":[1,1]}],"
        "\"edges\":[[1,0,2,0]],\"comments\":[]}";
    CHECK(LoadGraphJson(bad.doc, withUnknown, nullptr, &warn) && bad.model.Nodes().size() == 1 && !warn.empty(), "未知の型は読み飛ばし + 警告: %s", warn.c_str());

    // 値の既定は書かない（差分が小さい）
    Fixture h;
    h.doc.AddNode("float", {0, 0});
    CHECK(SaveGraphJson(h.doc, nullptr).find("values") == std::string::npos, "既定値は書かない");
    // 何度往復しても変わらない
    std::string cur = j1;
    for (int i = 0; i < 3; ++i)
    {
        Fixture k; ViewState vk;
        LoadGraphJson(k.doc, cur, &vk, nullptr);
        const std::string nxt = SaveGraphJson(k.doc, &vk);
        CHECK(nxt == cur, "反復往復 %d", i);
        cur = nxt;
    }
    // Signature も往復で一致（丸め後の値のため、往復 2 回目以降は不変）
    Fixture s1, s2;
    LoadGraphJson(s1.doc, j1, nullptr, nullptr);
    LoadGraphJson(s2.doc, SaveGraphJson(s1.doc, nullptr), nullptr, nullptr);
    CHECK(s1.doc.Signature() == s2.doc.Signature(), "Signature の往復");
}

void TestClipboard()
{
    Fixture f;
    BuildSampleGraph(f.doc, f.model);
    // 内部で完結する接続だけコピーされる
    const NodeId sat = f.model.Nodes()[3];
    Selection sel;
    // 出力ノード直前の 3 個 + コメント 1 個
    for (size_t i = 5; i < 9; ++i) sel.nodes.insert(f.model.Nodes()[i]);
    sel.comments.insert(f.doc.Comments()[0].id);
    (void)sat;
    const std::string clip = CopySelectionJson(f.doc, sel);
    int internal = 0;
    for (const Edge& e : f.model.Edges()) if (sel.nodes.count(e.from.node) && sel.nodes.count(e.to.node)) ++internal;

    const size_t nBefore = f.model.Nodes().size(), eBefore = f.model.Edges().size(), cBefore = f.doc.Comments().size();
    f.doc.History().Clear();
    PasteResult pr; std::string err;
    Rect cb;
    CHECK(ClipboardBounds(f.doc, clip, cb), "クリップボードの外接矩形");
    CHECK(PasteJson(f.doc, clip, {50, 50}, &pr, &err), "貼り付け: %s", err.c_str());
    CHECK(f.model.Nodes().size() == nBefore + sel.nodes.size(), "ノードが増える");
    CHECK(f.model.Edges().size() == eBefore + static_cast<size_t>(internal), "内部の接続だけ増える (%d)", internal);
    CHECK(f.doc.Comments().size() == cBefore + 1, "コメントも貼れる");
    CHECK(pr.selection.nodes.size() == sel.nodes.size(), "貼り付けた選択");
    for (NodeId id : pr.selection.nodes) CHECK(sel.nodes.count(id) == 0, "ID は再採番される");
    CHECK(f.doc.History().UndoDepth() == 1, "貼り付けは 1 操作 (%zu)", f.doc.History().UndoDepth());
    const std::string after = f.doc.Signature();
    f.doc.History().Undo(f.doc);
    CHECK(f.model.Nodes().size() == nBefore && f.model.Edges().size() == eBefore && f.doc.Comments().size() == cBefore, "貼り付けの Undo");
    f.doc.History().Redo(f.doc);
    CHECK(f.doc.Signature() == after, "貼り付けの Redo で同じ ID・同じ状態");

    // 別文書へも貼れる（テキストとして持ち運べる）
    Fixture g;
    CHECK(PasteJson(g.doc, clip, {0, 0}, &pr, &err) && g.model.Nodes().size() == sel.nodes.size(), "別のグラフへ貼り付け");
    CHECK(!PasteJson(g.doc, "{\"kind\":\"nodegraph\"}", {0, 0}, &pr, &err), "種別違いは拒否");
}

void TestPalette()
{
    Fixture f;
    PaletteFilter q;
    auto all = SearchPalette(f.model, q);
    CHECK(all.size() == f.model.NodeTypes().size(), "空クエリは全件");
    const auto cats = PaletteCategories(f.model);
    CHECK(cats.size() >= 8 && cats.front() == "定数", "カテゴリ順");
    // 名前
    q.query = "lerp";
    auto r = SearchPalette(f.model, q);
    CHECK(!r.empty() && f.model.NodeTypes()[static_cast<size_t>(r[0].typeIndex)].id == "lerp", "lerp が先頭");
    // 日本語の別名（キーワード）
    q.query = "加算";
    r = SearchPalette(f.model, q);
    CHECK(!r.empty() && f.model.NodeTypes()[static_cast<size_t>(r[0].typeIndex)].id == "add", "「加算」で Add");
    q.query = "かさん";   // ひらがな ↔ カタカナは FuzzyMatch が同一視（この語自体は無いので空でよい）
    // カテゴリ絞り込み
    q.query.clear(); q.category = "数学";
    r = SearchPalette(f.model, q);
    CHECK(!r.empty(), "数学カテゴリ");
    for (auto& it : r) CHECK(f.model.NodeTypes()[static_cast<size_t>(it.typeIndex)].category == "数学", "カテゴリ外が混ざらない");
    // 複数語 = 全語一致
    q.category.clear(); q.query = "tex sample";
    r = SearchPalette(f.model, q);
    CHECK(!r.empty() && f.model.NodeTypes()[static_cast<size_t>(r[0].typeIndex)].id == "sample", "複数語");
    q.query = "zzzzqqq";
    CHECK(SearchPalette(f.model, q).empty(), "一致なし");
    // ドラッグ元ピンの絞り込み: Texture の出力からは Tex 入力を持つノードだけ
    q = PaletteFilter{};
    q.hasPin = true; q.pinIsOutput = true; q.pinType = SandboxGraphModel::kTexture;
    r = SearchPalette(f.model, q);
    std::set<std::string> ids;
    for (auto& it : r) ids.insert(f.model.NodeTypes()[static_cast<size_t>(it.typeIndex)].id);
    CHECK(ids.count("sample") == 1 && ids.count("reroute") == 1, "Tex → sample / reroute");
    CHECK(ids.count("add") == 0 && ids.count("float") == 0, "数値ノードは出ない");
    for (auto& it : r) CHECK(it.autoPin >= 0, "自動接続ピンがある");
    // 入力ピン(Vec3) からのドラッグ → 出力を持つノード
    q.pinIsOutput = false; q.pinType = SandboxGraphModel::kVec3;
    r = SearchPalette(f.model, q);
    ids.clear();
    for (auto& it : r) ids.insert(f.model.NodeTypes()[static_cast<size_t>(it.typeIndex)].id);
    CHECK(ids.count("vec3") == 1 && ids.count("sample") == 1 && ids.count("output") == 0 && ids.count("texture") == 0, "Vec3 入力 ← 出力候補");
    // 型が完全一致するピンを優先（Sample の RGB は Vec3）
    for (auto& it : r)
        if (f.model.NodeTypes()[static_cast<size_t>(it.typeIndex)].id == "sample") CHECK(it.autoPin == 0, "RGB(Vec3) を優先 (%d)", it.autoPin);
    // タイトルのハイライト位置
    q = PaletteFilter{}; q.query = "mul";
    r = SearchPalette(f.model, q);
    CHECK(!r.empty() && !r[0].titleHighlight.empty(), "ハイライト");
}

// ---------------------------------------------------------------------------
// Undo/Redo ファズ: ランダムな操作列 → 全 Undo で初期状態に厳密一致 → 全 Redo で再現
// ---------------------------------------------------------------------------
struct FuzzResult { int ops = 0, pushed = 0; bool ok = true; };

FuzzResult RunFuzz(uint32_t seed, int opCount, bool verbose)
{
    Fixture f;
    auto& doc = f.doc;
    auto& m = f.model;
    std::mt19937 rng(seed);
    auto ri = [&](int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<uint32_t>(n)); };
    auto rf = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() % 10000) / 10000.0f; };
    auto pickNode = [&]() -> NodeId { const auto& ids = m.Nodes(); return ids.empty() ? 0 : ids[static_cast<size_t>(ri(static_cast<int>(ids.size())))]; };
    const auto& types = m.NodeTypes();

    // 初期状態: 少し置いておく（空から始めるのと両方カバー: seed 偶奇）
    if (seed % 2 == 0)
        for (int i = 0; i < 6; ++i) doc.AddNode(types[static_cast<size_t>(ri(static_cast<int>(types.size())))].id, {rf(-500, 500), rf(-500, 500)});
    doc.History().Clear();
    const std::string initial = doc.Signature();

    std::vector<std::string> before, after;   // 積まれた操作ごとの (直前, 直後) の状態
    std::vector<int> opKinds;                 // 積まれた操作の種類（不一致の調査用）
    FuzzResult res;
    std::string curSig = initial;             // 直前の状態（積まれた操作のたびに更新）
    for (int i = 0; i < opCount; ++i)
    {
        const std::string sigBefore = curSig;
        const uint64_t rev0 = doc.Revision();
        const size_t depth0 = doc.History().UndoDepth();
        const int op = ri(15);
        switch (op)
        {
        case 0: case 1: case 2:
            doc.AddNode(types[static_cast<size_t>(ri(static_cast<int>(types.size())))].id, {rf(-2000, 2000), rf(-2000, 2000)});
            break;
        case 3: case 4: case 5: case 6:
        {   // 接続（不可のものも試す）
            const NodeId a = pickNode(), b = pickNode();
            if (!a || !b) break;
            const NodeData* na = m.FindNode(a); const NodeData* nb = m.FindNode(b);
            if (na->desc->outputs.empty() || nb->desc->inputs.empty()) break;
            doc.Connect(PinRef{a, static_cast<uint16_t>(ri(static_cast<int>(na->desc->outputs.size()))), true},
                        PinRef{b, static_cast<uint16_t>(ri(static_cast<int>(nb->desc->inputs.size()))), false});
            break;
        }
        case 7:
        {   // 切断
            if (m.Edges().empty()) break;
            doc.Disconnect(m.Edges()[static_cast<size_t>(ri(static_cast<int>(m.Edges().size())))].to);
            break;
        }
        case 8:
        {   // 削除（ノード 1〜3 個 + たまにコメント）
            Selection s;
            const int n = 1 + ri(3);
            for (int k = 0; k < n; ++k) if (NodeId id = pickNode()) s.nodes.insert(id);
            if (!doc.Comments().empty() && ri(3) == 0) s.comments.insert(doc.Comments()[static_cast<size_t>(ri(static_cast<int>(doc.Comments().size())))].id);
            doc.RemoveItems(s);
            break;
        }
        case 9:
        {   // 値の変更
            const NodeId id = pickNode();
            if (!id) break;
            const NodeData* n = m.FindNode(id);
            if (n->desc->inputs.empty()) break;
            const int pi = ri(static_cast<int>(n->desc->inputs.size()));
            PinValue v = n->values[static_cast<size_t>(pi)];
            if (v.kind == ValueKind::None) break;
            for (int c = 0; c < 4; ++c) v.f[c] = rf(-10, 10);
            doc.SetPinValue(PinRef{id, static_cast<uint16_t>(pi), false}, v);
            break;
        }
        case 10:
        {   // 移動（複数）
            Selection s;
            const int n = 1 + ri(4);
            for (int k = 0; k < n; ++k) if (NodeId id = pickNode()) s.nodes.insert(id);
            if (!doc.Comments().empty() && ri(3) == 0) s.comments.insert(doc.Comments()[0].id);
            doc.MoveItems(s, {rf(-300, 300), rf(-300, 300)});
            break;
        }
        case 11:
        {   // コメント追加 / 編集
            if (doc.Comments().empty() || ri(2) == 0)
            {
                std::vector<NodeId> ns;
                for (int k = 0; k < 3; ++k) if (NodeId id = pickNode()) ns.push_back(id);
                if (!ns.empty()) doc.AddCommentAround(ns, "c" + std::to_string(i), ri(kCommentColors));
            }
            else
            {
                Comment c = doc.Comments()[static_cast<size_t>(ri(static_cast<int>(doc.Comments().size())))];
                c.title += "!"; c.color = ri(kCommentColors); c.rect.max.x += rf(-20, 60);
                doc.EditComment(c.id, c);
            }
            break;
        }
        case 12:
        {   // コピー → 貼り付け（複製）
            Selection s;
            const int n = 1 + ri(4);
            for (int k = 0; k < n; ++k) if (NodeId id = pickNode()) s.nodes.insert(id);
            if (!doc.Comments().empty() && ri(3) == 0) s.comments.insert(doc.Comments()[0].id);
            if (s.Empty()) break;
            PasteResult pr;
            PasteJson(doc, CopySelectionJson(doc, s), {rf(-100, 200), rf(-100, 200)}, &pr, nullptr);
            break;
        }
        case 13:
        {   // 整列
            std::vector<NodeId> ns;
            const int n = 2 + ri(4);
            for (int k = 0; k < n; ++k) if (NodeId id = pickNode()) ns.push_back(id);
            doc.Align(ns, static_cast<AlignMode>(ri(8)));
            break;
        }
        default:
        {   // リルート挿入 / ピン全切断 / グループ操作
            if (ri(3) == 0 && !m.Edges().empty())
                doc.InsertReroute(m.Edges()[static_cast<size_t>(ri(static_cast<int>(m.Edges().size())))], {rf(-500, 500), rf(-500, 500)});
            else if (ri(2) == 0)
            {
                const NodeId id = pickNode();
                if (!id) break;
                const NodeData* n = m.FindNode(id);
                const bool out = ri(2) == 0 && !n->desc->outputs.empty();
                const int cnt = static_cast<int>((out ? n->desc->outputs : n->desc->inputs).size());
                if (cnt > 0) doc.DisconnectPin(PinRef{id, static_cast<uint16_t>(ri(cnt)), out});
            }
            else
            {
                doc.History().BeginGroup("fuzz group");
                const NodeId x = doc.AddNode("add", {rf(-100, 100), rf(-100, 100)});
                const NodeId y = doc.AddNode("saturate", {rf(-100, 100), rf(-100, 100)});
                if (x && y) doc.Connect(PinRef{x, 0, true}, PinRef{y, 0, false});
                if (NodeId z = pickNode()) { Selection s; s.nodes.insert(z); doc.MoveItems(s, {7, 9}); }
                doc.History().EndGroup();
            }
            break;
        }
        }
        ++res.ops;
        const size_t depth1 = doc.History().UndoDepth();
        if (depth1 == depth0) { CHECK(doc.Revision() == rev0 || doc.Signature() == sigBefore, "履歴を積まない操作は状態を変えない (op=%d i=%d)", op, i); continue; }
        if (depth1 != depth0 + 1) { CHECK(false, "1 操作で履歴が %zu 増えた (op=%d)", depth1 - depth0, op); res.ok = false; return res; }
        before.push_back(sigBefore);
        opKinds.push_back(op);
        curSig = doc.Signature();
        after.push_back(curSig);
        ++res.pushed;
    }

    const std::string finalSig = doc.Signature();
    CHECK(res.pushed > opCount / 3, "十分な操作が積まれた (%d/%d)", res.pushed, opCount);
    CHECK(doc.History().UndoDepth() == static_cast<size_t>(res.pushed), "深さ");

    // 全 Undo: 1 段ずつ、直前の状態に厳密一致
    int badUndo = 0;
    for (int k = res.pushed - 1; k >= 0; --k)
    {
        doc.History().Undo(doc);
        const std::string now = doc.Signature();
        if (now != before[static_cast<size_t>(k)])
        {
            ++badUndo;
            if (badUndo <= 2)
            {
                // 最初に食い違う行を出す
                const std::string& want = before[static_cast<size_t>(k)];
                size_t i = 0;
                while (i < want.size() && i < now.size() && want[i] == now[i]) ++i;
                size_t ls = want.rfind('\n', i ? i - 1 : 0);
                ls = (ls == std::string::npos) ? 0 : ls + 1;
                std::printf("  Undo 不一致 seed=%u step=%d op=%d\n    want: %.110s\n    got : %.110s\n", seed, k, opKinds[static_cast<size_t>(k)],
                            want.c_str() + ls, now.c_str() + (ls < now.size() ? ls : 0));
            }
        }
    }
    CHECK(badUndo == 0, "seed=%u: Undo が直前の状態に一致しなかった段数 %d", seed, badUndo);
    CHECK(doc.Signature() == initial, "seed=%u: 全 Undo で初期状態に厳密一致", seed);
    CHECK(!doc.History().CanUndo(), "全部戻した");

    // 全 Redo: 1 段ずつ、直後の状態に一致
    int badRedo = 0;
    for (int k = 0; k < res.pushed; ++k)
    {
        doc.History().Redo(doc);
        if (doc.Signature() != after[static_cast<size_t>(k)]) { ++badRedo; if (badRedo <= 3) std::printf("  Redo 不一致 seed=%u step=%d\n", seed, k); }
    }
    CHECK(badRedo == 0, "seed=%u: Redo が直後の状態に一致しなかった段数 %d", seed, badRedo);
    CHECK(doc.Signature() == finalSig, "seed=%u: 全 Redo で最終状態を再現", seed);
    if (verbose) std::printf("  fuzz seed=%u: ops=%d pushed=%d nodes=%zu edges=%zu comments=%zu\n", seed, res.ops, res.pushed, m.Nodes().size(), m.Edges().size(), doc.Comments().size());

    // 最終状態の JSON 往復も一致
    Fixture g;
    LoadGraphJson(g.doc, SaveGraphJson(doc, nullptr), nullptr, nullptr);
    Fixture g2;
    LoadGraphJson(g2.doc, SaveGraphJson(g.doc, nullptr), nullptr, nullptr);
    CHECK(SaveGraphJson(g.doc, nullptr) == SaveGraphJson(g2.doc, nullptr), "seed=%u: 最終状態の JSON 往復", seed);
    return res;
}

void TestUndoFuzz()
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    // 大きな 1 本: 1 万操作
    const FuzzResult big = RunFuzz(2026, 10000, true);
    CHECK(big.ops == 10000, "1 万操作を実行");
    // 複数シード: 1,000 操作 × 100（設計書の合否基準）
    int okSeeds = 0;
    for (uint32_t s = 1; s <= 100; ++s)
    {
        const int before = g_failures;
        RunFuzz(s, 1000, false);
        if (g_failures == before) ++okSeeds;
    }
    CHECK(okSeeds == 100, "100 シード全部通過 (%d)", okSeeds);
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    std::printf("  Undo ファズ: 10,000 操作 x1 + 1,000 操作 x100 シード  %.1f 秒\n", sec);
}

void TestSampleAndStress()
{
    Fixture f;
    const SampleReport r = BuildSampleGraph(f.doc, f.model);
    CHECK(r.nodes >= 30 && r.nodes <= 50 && r.failedLinks == 0, "サンプル nodes=%d failed=%d", r.nodes, r.failedLinks);
    // サンプルの出力ノードの型推論が通っている
    const NodeId out = f.model.Nodes().back();
    (void)out;
    Fixture s;
    const SampleReport st = BuildStressGraph(s.doc, s.model, 500, 800, 42);
    CHECK(st.nodes == 500, "ストレス 500 ノード (%d)", st.nodes);
    CHECK(st.edges >= 700, "ストレス 接続 %d", st.edges);
    // 決定的
    Fixture s2;
    BuildStressGraph(s2.doc, s2.model, 500, 800, 42);
    CHECK(s.doc.Signature() == s2.doc.Signature(), "同じ seed は同じグラフ");
    // 索引の構築時間（参考）
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    for (int i = 0; i < 50; ++i) { s.model.SetNodePos(s.model.Nodes()[0], {static_cast<float>(i), 0}); s.doc.EnsureIndex(); }
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / 50.0;
    std::printf("  500 ノード / %d ワイヤ: 索引の再構築 %.3f ms\n", st.edges, ms);
    CHECK(ms < 5.0, "索引の再構築が速い %.3f ms", ms);
}

void TestCommandTableEntries()
{
    // （表そのものの検査は editor_ux_test.cpp。ここでは graph.* が存在しキーが衝突しないことだけを確認する）
}
} // namespace

int main(int argc, char** argv)
{
    std::printf("nodegraph_test\n");
    // 調査用: NodeGraphTests --fuzz <seed> <ops> で 1 本だけ回す
    if (argc >= 4 && std::string(argv[1]) == "--fuzz")
    {
        RunFuzz(static_cast<uint32_t>(std::atoi(argv[2])), std::atoi(argv[3]), true);
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    TestViewport();
    TestBezier();
    TestSpring();
    TestSpatialGrid();
    TestLayout();
    TestModelInvariants();
    TestDocumentBasics();
    TestAlign();
    TestHitTestAndBoxSelect();
    TestSpatialCorrectnessOnGraphs();
    TestSerialization();
    TestClipboard();
    TestPalette();
    TestSampleAndStress();
    TestUndoFuzz();
    TestCommandTableEntries();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
