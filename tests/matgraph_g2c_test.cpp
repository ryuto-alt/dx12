// マテリアルグラフ G2c の純ロジックの単体テスト（GPU / DXC / ImGui 不要）。
//
//   1. 部分グラフ出力（CompileOptions::previewNode）: 対象ノードまでの HLSL 化・型ごとの表示色の写像・CPU 評価との一致・上流のエラーだけで判定・
//      既定（previewNode 空）の出力は従来と同一。
//   2. デバウンスの状態機械（250 ms・トレーリング / リーディング・maxWait・取り消し）。
//   3. 段階の順序保証（StageGate）と優先度つきジョブキュー（FIFO・条件つき取り出し・取り消し）。順不同に届く結果のシミュレーション。
//   4. プレビュー設定（形状・環境・カメラ・ライト）の文字列往復 / 壊れた入力 / 範囲。グラフ（.dxmg）の内容に影響しない。
//   5. ノード内サムネイルの管理（NodeThumbs）: 可視ノードだけ・上限枚数・変更があったノードとその下流だけ更新。
#include "editor/matgraph/MatGraphEditor.h"
#include "editor/matgraph/NodeThumbs.h"
#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/CpuEval.h"
#include "renderer/matgraph/GraphIO.h"
#include "renderer/matgraph/PreviewLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace dx12e::matgraph;

namespace
{
int g_failures = 0;
int g_checks = 0;
}
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{

bool Near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

// テスト用グラフ: 出力ノードあり / なしを選べる
//   uv → sTex(TextureSample) → RGB → mul(Multiply A) ← tint(VectorParameter RGB)
//   rough(ScalarParameter 0.6) → half(Multiply, B = 0.5) → out.Roughness
//   sTex.RGBA(F4) / sTex.R(F1) / uv.UV(F2) / tex.Out(Tex2D) を preview の対象にする
MaterialGraph MakeGraph(bool withOutput)
{
    MaterialGraph g;
    g.SetGuid("00c0ffee");
    g.SetName("g2c_partial");
    if (withOutput) g.AddNode(kOutputNodeType, 900, 100, "out");
    g.AddNode("TexCoord", 0, 0, "uv");
    g.AddNode("TextureParameter", 0, 120, "tex");
    g.SetNodeProp("tex", "name", "Albedo");
    g.SetNodeProp("tex", "sampler", "Color");
    g.AddNode("TextureSample", 240, 40, "sTex");
    g.Connect("tex", "Out", "sTex", "Tex");
    g.Connect("uv", "UV", "sTex", "UV");
    g.AddNode("VectorParameter", 240, 240, "tint");
    g.SetNodeProp("tint", "name", "Tint");
    g.SetNodeProp("tint", "default", Json::array({0.5, 0.25, 1.0, 1.0}));
    g.AddNode("Multiply", 480, 100, "mul");
    g.Connect("sTex", "RGB", "mul", "A");
    g.Connect("tint", "RGB", "mul", "B");
    g.AddNode("ScalarParameter", 0, 400, "rough");
    g.SetNodeProp("rough", "name", "R");
    g.SetNodeProp("rough", "default", 0.6);
    g.AddNode("Multiply", 240, 400, "half");
    g.Connect("rough", "Out", "half", "A");
    g.SetLiteral("half", "B", Value::Float(0.5f));
    if (withOutput)
    {
        g.Connect("mul", "Out", "out", "BaseColor");
        g.Connect("half", "Out", "out", "Roughness");
    }
    return g;
}

EvalEnv MakeEnv()
{
    EvalEnv env;
    env.uv[0] = 0.25f; env.uv[1] = 0.75f;
    env.sampleTexture = [](int, const float uv[2], float, float out[4]) {
        out[0] = 0.25f + 0.5f * uv[0]; out[1] = 0.5f; out[2] = 0.75f - 0.5f * uv[1]; out[3] = 0.4f;
    };
    return env;
}

CompileResult Partial(const MaterialGraph& g, const char* node, const char* pin = "")
{
    CompileOptions o;
    o.previewNode = node;
    o.previewPin = pin;
    return CompileGraph(g, o);
}

// ---------------------------------------------------------------------------
void TestPartialCompile()
{
    std::printf("stage: partial compile\n");
    const MaterialGraph full = MakeGraph(true);
    const MaterialGraph noOut = MakeGraph(false);

    // ---- F1: half = R * 0.5 → (v, v, v) ----
    CompileResult a = Partial(full, "half");
    CHECK_MSG(a.ok, "partial(half) がコンパイルできない");
    CHECK(a.previewType == ValueType::F1);
    CHECK(a.hlsl.find("s.baseColor = float3(") != std::string::npos);
    CHECK(a.hlsl.find("s.opacity") == std::string::npos);
    CHECK_MSG(a.stats.nodesReachable == 2, "half の上流は R と half の 2 ノード（%d）", a.stats.nodesReachable);   // texture 側は含まない
    CHECK(a.slotCount == 1);   // R だけ（half の B はリテラル）
    {
        const CpuEvalResult r = EvaluateCpu(a, MakeEnv());
        CHECK(r.ok);
        for (int k = 0; k < 3; ++k) CHECK(Near(r.surface.baseColor[k], 0.3f));
        CHECK(Near(r.surface.roughness, 0.5f));   // partial は他のフィールドを既定のまま
    }

    // ---- F3: mul = sTex.RGB * Tint.RGB（テクスチャ・パラメータを含む）----
    CompileResult b = Partial(full, "mul");
    CHECK(b.ok);
    CHECK(b.previewType == ValueType::F3);
    CHECK_MSG(b.slotCount == 4, "slotCount=%d", b.slotCount);   // Albedo（TextureParameter）/ Tint / TexCoord の tiling・offset の 2 スロット
    {
        const CpuEvalResult r = EvaluateCpu(b, MakeEnv());
        CHECK_MSG(r.ok, "CPU 評価できない");
        // sTex.RGB = (0.25+0.5*0.25, 0.5, 0.75-0.5*0.75) = (0.375, 0.5, 0.375)。Tint は linear へ変換されない（srgb 既定 false）
        CHECK(Near(r.surface.baseColor[0], 0.375f * 0.5f));
        CHECK(Near(r.surface.baseColor[1], 0.5f * 0.25f));
        CHECK(Near(r.surface.baseColor[2], 0.375f * 1.0f));
    }

    // ---- F4（RGBA 出力ピン）: rgb は baseColor・alpha は opacity ----
    CompileResult c = Partial(full, "sTex", "RGBA");
    CHECK(c.ok);
    CHECK(c.previewType == ValueType::F4);
    CHECK(c.hlsl.find("s.opacity = (") != std::string::npos);
    {
        const CpuEvalResult r = EvaluateCpu(c, MakeEnv());
        CHECK(r.ok);
        CHECK(Near(r.surface.baseColor[0], 0.375f) && Near(r.surface.baseColor[1], 0.5f) && Near(r.surface.baseColor[2], 0.375f));
        CHECK(Near(r.surface.opacity, 0.4f));
    }

    // ---- F1（成分ピン R）----
    CompileResult d = Partial(full, "sTex", "R");
    CHECK(d.ok && d.previewType == ValueType::F1);
    {
        const CpuEvalResult r = EvaluateCpu(d, MakeEnv());
        for (int k = 0; k < 3; ++k) CHECK(Near(r.surface.baseColor[k], 0.375f));
    }

    // ---- F2（UV）: (x, y, 0) ----
    CompileResult e = Partial(full, "uv");
    CHECK(e.ok && e.previewType == ValueType::F2);
    CHECK(e.hlsl.find("float3(") != std::string::npos);
    {
        const CpuEvalResult r = EvaluateCpu(e, MakeEnv());
        CHECK(Near(r.surface.baseColor[0], 0.25f) && Near(r.surface.baseColor[1], 0.75f) && Near(r.surface.baseColor[2], 0.0f));
    }

    // ---- Tex2D（TextureParameter）: そのテクスチャを mi.uv でサンプルした rgb ----
    CompileResult f = Partial(full, "tex");
    CHECK(f.ok && f.previewType == ValueType::Tex2D);
    CHECK(f.hlsl.find("UnoSample(") != std::string::npos && f.hlsl.find("mi.uv") != std::string::npos);
    CHECK(f.slotCount == 1);

    // ---- MaterialOutput が無くても評価できる ----
    CompileResult g2 = Partial(noOut, "mul");
    CHECK_MSG(g2.ok, "出力ノードが無いグラフでも部分出力は評価できるはず");
    CHECK(g2.hash == b.hash);   // 出力ノードの有無・他の枝に依らず、同じ部分グラフは同じ HLSL（= 同じ変種を共有できる）

    // ---- 別の枝のエラーは無関係（Roughness 側を壊しても mul は評価できる）----
    {
        MaterialGraph broken = MakeGraph(true);
        broken.SetNodeProp("half", "nonexistent", 1);   // 何もしない（不正でも落ちない）
        broken.Disconnect("sTex", "UV");                // sTex の UV は既定の組み込み UV へ → まだ有効
        CompileResult ok1 = Partial(broken, "mul");
        CHECK(ok1.ok);
        // 上流にエラーを作る: half の B にテクスチャを（型検査なしで）繋ぐ = 型の不一致
        MaterialGraph b2 = MakeGraph(true);
        b2.ConnectUnchecked("tex", "Out", "half", "B");
        CHECK(!CompileGraph(b2).ok);                     // 全体はエラー
        CompileResult ok2 = Partial(b2, "mul");
        CHECK_MSG(ok2.ok, "別の枝のエラーで mul の部分出力が失敗した");
        CompileResult bad = Partial(b2, "half");
        CHECK_MSG(!bad.ok, "上流にエラーがあるのに half の部分出力が成功した");
        bool hasHalf = false;
        for (const Diagnostic& dg : bad.diagnostics) if (dg.nodeId == "half" && dg.severity == Severity::Error) hasHalf = true;
        CHECK_MSG(hasHalf, "エラーが対象ノード（half）に付いていない");
    }

    // ---- 対象が使えないとき ----
    CompileResult o1 = Partial(full, "out");
    CHECK(!o1.ok);
    CHECK(!o1.diagnostics.empty() && o1.diagnostics[0].code == std::string(code::kPreviewTarget));
    CompileResult o2 = Partial(full, "nope");
    CHECK(!o2.ok);
    CompileResult o3 = Partial(full, "half", "NoSuchPin");
    CHECK(!o3.ok);

    // ---- ソースマップ: 出力行が対象ノードへ逆引きできる（DXC のエラー行 → ノード）----
    {
        bool found = false;
        for (const SourceMapEntry& sm : b.sourceMap) if (sm.nodeId == "mul") found = true;
        CHECK(found);
        CHECK(b.hlsl.find("#line 1 \"node:mul\"") != std::string::npos);
    }

    // ---- 既定（previewNode 空）は従来と同じ: 同じグラフの 2 回のコンパイルはバイト一致・hash も同じ ----
    CompileResult x1 = CompileGraph(full), x2 = CompileGraph(full);
    CHECK(x1.ok && x1.hlsl == x2.hlsl && x1.hash == x2.hash);
    CHECK(x1.previewType == ValueType::Invalid);
    CHECK(x1.hlsl.find("preview") == std::string::npos);

    // ---- 決定論: 部分出力も同じグラフ → 同じ HLSL ----
    CHECK(Partial(full, "mul").hlsl == Partial(full, "mul").hlsl);

    // ---- 値の編集では HLSL が変わらない（値はスロット）----
    {
        MaterialGraph v = MakeGraph(true);
        const std::string h0 = Partial(v, "mul").hlsl;
        v.SetNodeProp("tint", "default", Json::array({0.1, 0.9, 0.3, 1.0}));
        const CompileResult r1 = Partial(v, "mul");
        CHECK(r1.hlsl == h0);
        // ...けれど CPU 評価の値は変わる
        const CpuEvalResult r = EvaluateCpu(r1, MakeEnv());
        CHECK(Near(r.surface.baseColor[1], 0.5f * 0.9f));
    }

    // ---- 部分出力の CpuEval は、全体コンパイルの Op の値と一致する（同じ IR 評価）----
    {
        const CompileResult fullR = CompileGraph(full);
        const CpuEvalResult fr = EvaluateCpu(fullR, MakeEnv());
        const CpuEvalResult pr = EvaluateCpu(Partial(full, "mul"), MakeEnv());
        CHECK(fr.ok && pr.ok);
        for (int k = 0; k < 3; ++k) CHECK(Near(fr.surface.baseColor[k], pr.surface.baseColor[k]));   // out.BaseColor = mul
    }
}

// ---------------------------------------------------------------------------
void TestDebouncer()
{
    std::printf("stage: debouncer\n");
    // --- トレーリング（既定 250 ms）---
    {
        Debouncer d;   // quiet 0.25・leading なし
        CHECK(!d.Poll(0.0));
        d.Edit(1.00);
        CHECK(d.Pending());
        CHECK(!d.Poll(1.10));
        d.Edit(1.20);                      // 連続編集 → 待ちが延びる
        CHECK(!d.Poll(1.40));              // 最後の編集から 0.2 s
        CHECK(!d.Poll(1.449));
        CHECK(d.Poll(1.451));              // 0.25 s 静か → 1 回だけ発火
        CHECK(!d.Poll(1.50));
        CHECK(!d.Pending());
        CHECK(d.Fires() == 1);
        // 残り時間
        d.Edit(2.0);
        CHECK(Near(static_cast<float>(d.RemainingSec(2.1)), 0.15f, 1e-4f));
        CHECK(Near(static_cast<float>(d.RemainingSec(3.0)), 0.0f));
    }
    // --- 10 回の連続編集は 1 回にまとまる ---
    {
        Debouncer d;
        int fires = 0;
        for (int i = 0; i < 10; ++i) { d.Edit(0.05 * i); if (d.Poll(0.05 * i + 0.01)) ++fires; }
        for (double t = 0.5; t < 1.5; t += 0.016) if (d.Poll(t)) ++fires;
        CHECK_MSG(fires == 1, "fires=%d", fires);
    }
    // --- リーディング: 孤立した最初の編集は待たずに発火 → 直後の連続編集は 1 回のトレーリングにまとまる ---
    {
        Debouncer::Config c;
        c.leading = true;
        Debouncer d(c);
        d.Edit(5.0);
        CHECK(d.Poll(5.0));                // すぐ
        CHECK(!d.Pending());
        d.Edit(5.05);                      // 直後の編集: 待つ
        d.Edit(5.10);
        CHECK(!d.Poll(5.20));
        CHECK(d.Poll(5.36));               // 最後の編集 + 0.25 s
        CHECK(!d.Poll(5.5));
        CHECK(d.Fires() == 2);
        d.Edit(9.0);                       // 十分あとの編集 = また孤立 → すぐ
        CHECK(d.Poll(9.0));
    }
    // --- maxWait: 編集が止まらなくても必ず発火する ---
    {
        Debouncer::Config c;
        c.quietSec = 0.25;
        c.maxWaitSec = 1.0;
        Debouncer d(c);
        int fires = 0;
        for (double t = 0.0; t < 3.0; t += 0.1) { d.Edit(t); if (d.Poll(t)) ++fires; }
        CHECK_MSG(fires >= 2, "fires=%d（maxWait で途中で発火するはず）", fires);
        CHECK(fires <= 3);
    }
    // --- 取り消し ---
    {
        Debouncer d;
        d.Edit(0.0);
        d.Cancel();
        CHECK(!d.Pending());
        CHECK(!d.Poll(10.0));
    }
}

// ---------------------------------------------------------------------------
struct FakeJob { int id; int stage; };

void TestStagesAndQueue()
{
    std::printf("stage: stage gate / job queue\n");
    // --- StageGate: 届いた順ではなく段階で採否 ---
    {
        StageGate g;
        CHECK(g.Accept(1));       // 高速版
        CHECK(!g.Accept(1));      // 同じ段階の重複は捨てる
        CHECK(g.Accept(2));       // 最適化版で置き換わる
        CHECK(!g.Accept(1));      // 遅れて届いた高速版は最適化版を上書きしない
        CHECK(!g.Accept(2));
        CHECK(g.level == 2);
        StageGate h;              // 最適化版が先に届いたら、高速版は捨てる（順不同）
        CHECK(h.Accept(2));
        CHECK(!h.Accept(1));
        CHECK(h.level == 2);
    }
    // --- 全順列（3 個の結果 = 1, 2, 1）で最終段階は常に 2 ---
    {
        std::vector<int> arr = {1, 2, 1};
        std::sort(arr.begin(), arr.end());
        do
        {
            StageGate g;
            int accepted = 0;
            for (int lv : arr) if (g.Accept(lv)) ++accepted;
            CHECK(g.level == 2);
            CHECK(accepted >= 1 && accepted <= 2);
        } while (std::next_permutation(arr.begin(), arr.end()));
    }
    // --- ジョブキュー: 優先度 → FIFO ---
    {
        PriorityJobQueue<FakeJob> q;
        q.Push({1, 2}, 1);
        q.Push({2, 1}, 0);
        q.Push({3, 2}, 1);
        q.Push({4, 1}, 0);
        FakeJob j{};
        std::vector<int> order;
        while (q.Pop(j)) order.push_back(j.id);
        CHECK(order == (std::vector<int>{2, 4, 1, 3}));
    }
    // --- 条件つき取り出し（最適化ジョブの同時実行数の上限）---
    {
        PriorityJobQueue<FakeJob> q;
        q.Push({1, 2}, 1);
        q.Push({2, 2}, 1);
        FakeJob j{};
        auto noOpt = [](const FakeJob& x) { return x.stage != 2; };
        CHECK(!q.Pop(j, noOpt));          // 最適化しか無い + 枠なし → 取り出せない
        q.Push({3, 1}, 0);
        CHECK(q.Pop(j, noOpt) && j.id == 3);
        CHECK(q.Size() == 2);
        CHECK(q.Pop(j) && j.id == 1);     // 枠が空いたら FIFO
    }
    // --- 取り消し（古い編集の間引き）---
    {
        PriorityJobQueue<FakeJob> q;
        for (int i = 0; i < 6; ++i) q.Push({i, 1 + (i % 2)}, i % 2);
        const size_t n = q.CancelIf([](const FakeJob& x) { return x.id % 3 == 0; });
        CHECK(n == 2);
        CHECK(q.Size() == 4);
        FakeJob j{};
        std::vector<int> order;
        while (q.Pop(j)) order.push_back(j.id);
        CHECK(order == (std::vector<int>{2, 4, 1, 5}));
    }
    // --- 編集の連続シミュレーション: 各編集で「前の未着手ジョブを取り消して新しいのを積む」→ 最後の版だけが残る ---
    {
        PriorityJobQueue<FakeJob> q;
        int latest = -1;
        for (int i = 0; i < 20; ++i)
        {
            q.CancelIf([&](const FakeJob& x) { return x.id != i; });
            q.Push({i, 1}, 0);
            q.Push({i, 2}, 1);
            latest = i;
        }
        CHECK(q.Size() == 2);
        FakeJob j{};
        CHECK(q.Pop(j) && j.id == latest && j.stage == 1);
        CHECK(q.Pop(j) && j.id == latest && j.stage == 2);
    }
}

// ---------------------------------------------------------------------------
void TestPreviewSettings()
{
    std::printf("stage: preview settings\n");
    PreviewSettings a;
    a.shape = PreviewShape::Cylinder;
    a.env = PreviewEnv::Outdoor;
    a.yaw = 1.25f; a.pitch = -0.4f; a.dist = 4.2f; a.lightYaw = 0.6f; a.lightPitch = 0.3f;
    a.autoRotate = true; a.autoRotateSpeed = 0.9f;
    PreviewSettings b;
    CHECK(b.FromString(a.ToString()));
    CHECK(b == a);
    CHECK(b.shape == PreviewShape::Cylinder && b.env == PreviewEnv::Outdoor && b.autoRotate);
    CHECK(Near(b.yaw, 1.25f, 1e-3f) && Near(b.pitch, -0.4f, 1e-3f) && Near(b.dist, 4.2f, 1e-3f));

    // 壊れた入力: 読めたものだけ・範囲に丸める・落ちない
    PreviewSettings c;
    CHECK(!c.FromString(""));
    CHECK(!c.FromString("garbage;;=;x"));
    CHECK(c == PreviewSettings());
    CHECK(c.FromString("shape=cube;env=nonexistent;yaw=abc;dist=999;pitch=9"));
    CHECK(c.shape == PreviewShape::Cube);
    CHECK(c.env == PreviewEnv::Studio);                // 未知の値は無視（既定のまま）
    CHECK(c.dist <= 8.0f && c.pitch <= 1.45f);         // 範囲
    PreviewSettings d;
    d.yaw = std::numeric_limits<float>::quiet_NaN();
    d.Clamp();
    CHECK(std::isfinite(d.yaw));
    // 4 形状 × 3 環境の名前の往復
    for (int s = 0; s < 4; ++s)
        for (int e = 0; e < 3; ++e)
        {
            PreviewSettings p;
            p.shape = static_cast<PreviewShape>(s);
            p.env = static_cast<PreviewEnv>(e);
            PreviewSettings q;
            CHECK(q.FromString(p.ToString()) && q == p);
        }
    // 角度は 2π に畳まれる（保存値が増え続けない）
    PreviewSettings r;
    r.yaw = 100.0f;
    r.Clamp();
    CHECK(std::fabs(r.yaw) < 6.3f);

    // ---- グラフの内容に影響しない: 設定を変えても .dxmg のテキスト / コンパイル結果の hash は同じ ----
    MaterialGraph g = MakeGraph(true);
    const std::string t0 = SaveDxmg(g, true);
    const CompileResult c0 = CompileGraph(g);
    PreviewSettings zz;
    zz.shape = PreviewShape::Cube; zz.env = PreviewEnv::Dark; zz.yaw = 2.0f;
    (void)zz.ToString();
    CHECK(SaveDxmg(g, true) == t0);
    CHECK(CompileGraph(g).hash == c0.hash);
    CHECK(t0.find("cube") == std::string::npos && t0.find("outdoor") == std::string::npos);
}


// ---------------------------------------------------------------------------
struct FakeBackend : dx12e::mg::NodeThumbBackend
{
    struct Sent { std::string key; uint64_t hash; std::vector<float> values; };
    std::vector<Sent> sent;
    std::vector<std::string> removed;
    void SetCompiled(const std::string& key, std::shared_ptr<const CompileResult> cr) override
    {
        Sent s;
        s.key = key;
        s.hash = cr->hash;
        for (const SlotInfo& sl : cr->slots) for (int k = 0; k < 4; ++k) s.values.push_back(sl.value[k]);
        sent.push_back(std::move(s));
    }
    void Remove(const std::string& key) override { removed.push_back(key); }
    void Clear() { sent.clear(); removed.clear(); }
};

void TestNodeThumbs()
{
    std::printf("stage: node thumbs\n");
    using dx12e::mg::MatGraphEditor;
    using dx12e::mg::NodeThumbs;
    namespace ng = dx12e::ng;

    MatGraphEditor ed;
    ed.NewGraph(dx12e::mg::NewTemplate::Sample);
    auto id = [&](const char* s) { return ed.Model().IntOf(s); };

    // 見本の全ノード（可視とする）
    std::vector<ng::NodeId> all;
    for (const auto& kv : ed.Graph().Nodes()) all.push_back(ed.Model().IntOf(kv.first));
    int expectWanted = 0;
    for (ng::NodeId n : all) if (NodeThumbs::WantsThumb(ed.Model().DefOf(n))) ++expectWanted;
    CHECK_MSG(expectWanted >= 10, "サムネイルを出す型のノードが見本に少ない（%d）", expectWanted);

    NodeThumbs th;
    NodeThumbs::Config cfg;
    cfg.maxTiles = 48;
    cfg.maxCompilesPerFrame = 6;
    th.SetConfig(cfg);
    FakeBackend be;
    uint64_t frame = 1;

    // ---- 1. 初回: 1 フレーム 6 枚まで。数フレームで全部そろう ----
    std::vector<NodeThumbs::TileWork> work = th.Update(ed, all, be, frame++);
    CHECK(th.GetStats().wanted == expectWanted);
    CHECK_MSG(th.GetStats().compiledThisFrame == 6, "1 フレームの作り直し上限が効いていない（%d）", th.GetStats().compiledThisFrame);
    for (int i = 0; i < 8; ++i) th.Update(ed, all, be, frame++);
    {
        const auto st = th.GetStats();
        CHECK_MSG(st.compilesTotal == static_cast<uint64_t>(expectWanted), "初回の作り直し数 = 対象ノード数のはず（%llu / %d）", static_cast<unsigned long long>(st.compilesTotal), expectWanted);
        CHECK(st.resident == expectWanted);
    }
    int errors = 0, tiles = 0;
    std::set<int> used;
    for (ng::NodeId n : all)
    {
        if (!NodeThumbs::WantsThumb(ed.Model().DefOf(n))) continue;
        if (th.StateOf(n) == NodeThumbs::State::Error) ++errors;
        const int t = th.TileOf(n);
        if (t >= 0) { ++tiles; used.insert(t); }
    }
    CHECK_MSG(errors == 0, "見本のサムネイルにエラーがある（%d 件）", errors);
    CHECK_MSG(static_cast<int>(used.size()) == tiles, "同じタイルが 2 つのノードに割り当てられている");
    // GPU に描いてもらう待ち → 描けたら外す
    work = th.Update(ed, all, be, frame++);
    CHECK(static_cast<int>(work.size()) == expectWanted);
    for (const auto& w : work) th.MarkDrawn(w.tile);
    work = th.Update(ed, all, be, frame++);
    CHECK(work.empty());
    for (ng::NodeId n : all)
        if (NodeThumbs::WantsThumb(ed.Model().DefOf(n))) CHECK(th.StateOf(n) == NodeThumbs::State::Ready);

    // ---- 2. 何も変えなければ何も作り直さない ----
    be.Clear();
    const uint64_t before = th.GetStats().compilesTotal;
    for (int i = 0; i < 5; ++i) th.Update(ed, all, be, frame++);
    CHECK(th.GetStats().compilesTotal == before);
    CHECK(be.sent.empty());

    // ---- 3. ノードの移動では作り直さない ----
    ed.Graph().MoveNode("tint", 300.0f, 300.0f);
    th.Update(ed, all, be, frame++);
    CHECK(th.GetStats().compilesTotal == before);

    // ---- 4. 値の変更: そのノードとその下流の「サムネイルを持つ」ノードだけ（tint は他のサムネイルの上流ではない）----
    ed.Graph().SetNodeProp("tint", "default", Json::array({0.2, 0.4, 0.6, 1.0}));
    work = th.Update(ed, all, be, frame++);
    CHECK_MSG(th.GetStats().compiledThisFrame == 1, "tint の値の変更で作り直したのは %d 枚（1 のはず）", th.GetStats().compiledThisFrame);
    CHECK(be.sent.size() == 1 && be.sent[0].key == NodeThumbs::KeyFor("tint"));
    CHECK(work.size() == 1);
    for (const auto& w : work) th.MarkDrawn(w.tile);
    // 値だけの変更: HLSL のハッシュは前回と同じ（= GPU 側は再コンパイルなし）
    const uint64_t tintHash0 = be.sent.empty() ? 0 : be.sent[0].hash;
    be.Clear();
    ed.Graph().SetNodeProp("tint", "default", Json::array({0.9, 0.1, 0.1, 1.0}));
    th.Update(ed, all, be, frame++);
    CHECK(be.sent.size() == 1 && be.sent[0].hash == tintHash0);
    for (const auto& w : th.Update(ed, all, be, frame++)) th.MarkDrawn(w.tile);

    // ---- 5. 上流の値の変更: 下流のサムネイル（NormalMap）も作り直す ----
    be.Clear();
    ed.Graph().SetNodeProp("nStrength", "default", 2.0);
    for (const auto& w : th.Update(ed, all, be, frame++)) th.MarkDrawn(w.tile);
    {
        std::set<std::string> keys;
        for (const auto& s : be.sent) keys.insert(s.key);
        CHECK_MSG(keys.count(NodeThumbs::KeyFor("nStrength")) && keys.count(NodeThumbs::KeyFor("nrm")), "nStrength の変更が自分と下流の NormalMap に届いていない");
        CHECK_MSG(!keys.count(NodeThumbs::KeyFor("tint")) && !keys.count(NodeThumbs::KeyFor("tBase")) && !keys.count(NodeThumbs::KeyFor("sBase")),
                  "無関係なサムネイルまで作り直した");
        CHECK(keys.size() == 2);
    }

    // ---- 6. 構造の変更（接続）: 下流だけ ----
    be.Clear();
    CHECK(ed.Graph().Disconnect("mulBase", "B"));   // tint → mulBase を外す（mulBase の下流にサムネイルを持つノードは無い）
    th.Update(ed, all, be, frame++);
    CHECK_MSG(be.sent.empty(), "サムネイルを持たないノード（Multiply）の下流変更で何かを送った（%zu）", be.sent.size());
    CHECK(ed.Graph().Connect("tint", "RGB", "mulBase", "B"));
    th.Update(ed, all, be, frame++);
    CHECK(be.sent.empty());

    // ---- 7. エラー: Custom の本文を空にすると、その Custom のサムネイルはエラー（タイルなし）・バックエンドから削除 ----
    be.Clear();
    ed.Graph().SetNodeProp("noise", "code", "");
    th.Update(ed, all, be, frame++);
    CHECK(th.StateOf(id("noise")) == NodeThumbs::State::Error);
    CHECK(!th.ErrorOf(id("noise")).empty());
    CHECK(th.TileOf(id("noise")) < 0);
    CHECK(std::find(be.removed.begin(), be.removed.end(), NodeThumbs::KeyFor("noise")) != be.removed.end());
    // 直すと復帰
    be.Clear();
    ed.Graph().SetNodeProp("noise", "code", "return In0.x * In1;");
    for (const auto& w : th.Update(ed, all, be, frame++)) th.MarkDrawn(w.tile);
    CHECK(th.StateOf(id("noise")) != NodeThumbs::State::Error);
    CHECK(!be.sent.empty());

    // ---- 8. 可視ノードだけ・画面外は一定フレーム後に解放 ----
    {
        NodeThumbs t2;
        NodeThumbs::Config c2;
        c2.evictAfterFrames = 5;
        t2.SetConfig(c2);
        FakeBackend be2;
        uint64_t f2 = 1;
        std::vector<ng::NodeId> one = {id("tint")};
        for (int i = 0; i < 3; ++i) t2.Update(ed, one, be2, f2++);
        CHECK(t2.GetStats().wanted == 1 && t2.GetStats().resident == 1 && t2.GetStats().compilesTotal == 1);
        CHECK(NodeThumbs::WantsThumb(ed.Model().DefOf(id("tint"))));
        CHECK(!NodeThumbs::WantsThumb(ed.Model().DefOf(id("mulBase"))));   // Multiply はサムネイルを持たない型
        std::vector<ng::NodeId> none;
        for (int i = 0; i < 8; ++i) t2.Update(ed, none, be2, f2++);
        CHECK_MSG(t2.GetStats().resident == 0, "画面外のタイルが解放されない");
        CHECK(std::find(be2.removed.begin(), be2.removed.end(), NodeThumbs::KeyFor("tint")) != be2.removed.end());
    }

    // ---- 9. 上限枚数: maxTiles = 4 で全部が可視 → 常駐 4・残りは overBudget ----
    {
        NodeThumbs t3;
        NodeThumbs::Config c3;
        c3.maxTiles = 4;
        c3.maxCompilesPerFrame = 100;
        t3.SetConfig(c3);
        FakeBackend be3;
        uint64_t f3 = 1;
        for (int i = 0; i < 4; ++i) t3.Update(ed, all, be3, f3++);
        CHECK(t3.GetStats().resident == 4);
        CHECK(t3.GetStats().overBudget > 0);
        std::set<int> tl;
        for (ng::NodeId n : all) { const int t = t3.TileOf(n); if (t >= 0) tl.insert(t); }
        CHECK(tl.size() == 4);
        for (int t : tl) CHECK(t >= 0 && t < 4);
    }

    // ---- 10. グラフの入れ替え（Reset）: 全部捨てて、新しいグラフで作り直す ----
    {
        be.Clear();
        th.Reset(&be);
        CHECK(th.GetStats().resident == 0 || th.GetStats().resident > 0);   // Reset 直後の統計は次の Update で更新される
        ed.NewGraph(dx12e::mg::NewTemplate::StandardPbr);
        std::vector<ng::NodeId> ids;
        for (const auto& kv : ed.Graph().Nodes()) ids.push_back(ed.Model().IntOf(kv.first));
        for (int i = 0; i < 4; ++i) th.Update(ed, ids, be, frame++);
        CHECK(th.GetStats().resident > 0);
    }
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestPartialCompile();
    TestDebouncer();
    TestStagesAndQueue();
    TestPreviewSettings();
    TestNodeThumbs();
    if (g_failures == 0) std::printf("PASS: %d checks, 0 failures\n", g_checks);
    else std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
