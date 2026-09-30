// マテリアルグラフ G1（src/renderer/matgraph/）の単体テスト。GPU / DXC 不要。
//
//   型規則 / モデル API / 診断 / コード生成（決定論・CSE・スロット）/ CPU 評価 / .dxmg 往復 / ゴールデン。
//   DXC でのコンパイルと WARP での数値比較は matgraph_dxc_test.cpp（ctest ラベル dxc）。
//
//   ゴールデンの更新: 環境変数 MATGRAPH_UPDATE_GOLDEN=1 で実行すると、tests/data/matgraph/*.dxmg を正準形に
//   書き直し、*.hlsl.expected を再生成する（差分は git で目視すること）。
//   CLI: MatGraphTests --compile <file.dxmg>   生成 HLSL と診断を標準出力へ。

#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/CpuEval.h"
#include "renderer/matgraph/GraphAnalysis.h"
#include "renderer/matgraph/GraphIO.h"
#include "renderer/matgraph/GraphModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace dx12e::matgraph;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;
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

bool Near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b)); }

// テスト用の include 行（参照契約ヘッダ）
CompileOptions TestOpt()
{
    CompileOptions o;
    o.includeLine = "#include \"UnoMatContractRef.hlsli\"";
    return o;
}

// ---- グラフ組み立ての道具 -----------------------------------------------------------
struct GB
{
    MaterialGraph g;
    GB() { g.SetIdSeed(1); g.SetGuid("00000001"); g.SetName("test"); }

    NodeId N(const char* type, const char* id, std::initializer_list<std::pair<const char*, Json>> props = {})
    {
        const NodeId nid = g.AddNode(type, 0, 0, id);
        CHECK_MSG(!nid.empty(), "AddNode(%s,%s) failed", type, id);
        for (const auto& p : props)
            CHECK_MSG(g.SetNodeProp(nid, p.first, p.second), "SetProp %s.%s failed", id, p.first);
        return nid;
    }
    // "a.Out" → "b.A"
    bool L(const std::string& from, const std::string& to, ConnectCheck* why = nullptr)
    {
        const size_t f = from.find('.'), t = to.find('.');
        return g.Connect(from.substr(0, f), from.substr(f + 1), to.substr(0, t), to.substr(t + 1), why);
    }
    void MustL(const std::string& from, const std::string& to)
    {
        ConnectCheck why;
        const bool ok = L(from, to, &why);
        CHECK_MSG(ok, "connect %s -> %s: %s", from.c_str(), to.c_str(), why.message.c_str());
    }
    void Lit(const char* node, const char* pin, const Value& v)
    {
        CHECK_MSG(g.SetLiteral(node, pin, v), "SetLiteral %s.%s failed", node, pin);
    }
};

// JSON テキストからグラフ（不正なグラフを作るのにも使う。接続の検査は通らない）
MaterialGraph FromText(const std::string& text)
{
    MaterialGraph g;
    std::string err;
    const bool ok = LoadDxmg(text, g, &err);
    CHECK_MSG(ok, "LoadDxmg: %s", err.c_str());
    return g;
}

bool HasDiag(const std::vector<Diagnostic>& ds, const char* code, const std::string& node = "*", const std::string& pin = "*")
{
    for (const Diagnostic& d : ds)
        if (d.code == code && (node == "*" || d.nodeId == node) && (pin == "*" || d.pin == pin)) return true;
    return false;
}

int CountSeverity(const std::vector<Diagnostic>& ds, Severity s, bool reachableOnly = true)
{
    int n = 0;
    for (const Diagnostic& d : ds)
        if (d.severity == s && (!reachableOnly || d.reachable)) ++n;
    return n;
}

std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> v;
    std::string cur;
    for (char c : s)
    {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

int CountDiffLines(const std::string& a, const std::string& b)
{
    const auto la = SplitLines(a), lb = SplitLines(b);
    if (la.size() != lb.size()) return 1000000;
    int n = 0;
    for (size_t i = 0; i < la.size(); ++i)
        if (la[i] != lb[i]) ++n;
    return n;
}

bool Contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// ---------------------------------------------------------------------------
// 1. 型規則
// ---------------------------------------------------------------------------
void TestTypes()
{
    std::printf("[types]\n");
    CHECK(CheckCast(ValueType::F3, ValueType::F3).kind == CastKind::Identity);
    CHECK(CheckCast(ValueType::F1, ValueType::F3).kind == CastKind::Splat);
    CHECK(CheckCast(ValueType::F1, ValueType::F3).ok && !CheckCast(ValueType::F1, ValueType::F3).warn);
    {
        const CastResult r = CheckCast(ValueType::F4, ValueType::F2);
        CHECK(r.ok && r.warn && r.kind == CastKind::Truncate);
    }
    {
        const CastResult r = CheckCast(ValueType::F2, ValueType::F3);
        CHECK(!r.ok && Contains(r.reason, "Append"));
    }
    CHECK(!CheckCast(ValueType::F2, ValueType::F4).ok);
    CHECK(CheckCast(ValueType::Int, ValueType::F3).ok && CheckCast(ValueType::Int, ValueType::F3).kind == CastKind::IntToFloat);
    CHECK(CheckCast(ValueType::Bool, ValueType::F1).ok);
    CHECK(CheckCast(ValueType::Bool, ValueType::Int).ok);
    CHECK(!CheckCast(ValueType::F1, ValueType::Int).ok);
    CHECK(!CheckCast(ValueType::F1, ValueType::Bool).ok);
    CHECK(!CheckCast(ValueType::Int, ValueType::Bool).ok);
    {
        const CastResult r = CheckCast(ValueType::Tex2D, ValueType::F3);
        CHECK(!r.ok && Contains(r.reason, "TextureSample"));
    }
    CHECK(!CheckCast(ValueType::F1, ValueType::Tex2D).ok);
    CHECK(CheckCast(ValueType::Tex2D, ValueType::Tex2D).ok);
    CHECK(!CheckCast(ValueType::Mat4, ValueType::F4).ok);
    CHECK(!CheckCast(ValueType::Sampler, ValueType::F1).ok);
    CHECK(!CheckCast(ValueType::Invalid, ValueType::F1).ok);
    // 不変条件: 同じ型は必ず通る。オブジェクト型は他へ変換できない
    const ValueType all[] = {ValueType::F1, ValueType::F2, ValueType::F3, ValueType::F4, ValueType::Int, ValueType::Bool,
                             ValueType::Tex2D, ValueType::Sampler, ValueType::Mat3, ValueType::Mat4};
    for (ValueType a : all)
        for (ValueType b : all)
        {
            const CastResult r = CheckCast(a, b);
            if (a == b) CHECK(r.ok && r.kind == CastKind::Identity);
            const bool objA = a == ValueType::Tex2D || a == ValueType::Sampler || a == ValueType::Mat3 || a == ValueType::Mat4;
            const bool objB = b == ValueType::Tex2D || b == ValueType::Sampler || b == ValueType::Mat3 || b == ValueType::Mat4;
            if ((objA || objB) && a != b) CHECK(!r.ok);
            if (!r.ok) CHECK(!r.reason.empty());
        }
    ValueType t;
    CHECK(ParseTypeName("float3", t) && t == ValueType::F3);
    CHECK(!ParseTypeName("half3", t));

    CHECK(FormatFloatShort(0.6f) == "0.6");
    CHECK(FormatHlslFloat(1.0f) == "1.0");
    CHECK(FormatHlslFloat(-0.5f) == "(-0.5)");
    CHECK(FormatHlslFloat(0.0f) == "0.0");
    CHECK(FormatHlslFloat(-0.0f) == "0.0");
    CHECK(FormatHlslValue(Value::Float3(1, 0, 0.5f)) == "float3(1.0, 0.0, 0.5)");
    CHECK(FormatHlslValue(Value::Bool1(true)) == "true");
    CHECK(FormatHlslValue(Value::Int32(-3)) == "-3");
    CHECK(FormatFloatShort(NAN) == "0");
}

// ---------------------------------------------------------------------------
// 2. モデル API
// ---------------------------------------------------------------------------
void TestModel()
{
    std::printf("[model]\n");
    GB b;
    std::vector<GraphChange> events;
    const auto lid = b.g.AddListener([&](const GraphChange& c) { events.push_back(c); });

    const uint64_t v0 = b.g.Version();
    b.N("Float", "f");
    CHECK(b.g.Version() > v0);
    CHECK(events.size() == 1 && events[0].kind == ChangeKind::NodeAdded && events[0].node == "f");
    b.N("Multiply", "m");
    b.N("MaterialOutput", "out");
    CHECK(b.g.AddNode("Float", 0, 0, "f").empty());          // 重複 ID は失敗
    CHECK(b.g.AddNode("Float", 0, 0, "a.b").empty());        // '.' は使えない
    CHECK(!b.g.GenerateNodeId().empty() && b.g.GenerateNodeId().rfind("n_", 0) == 0);

    // 接続 / 通知 / バージョン
    events.clear();
    const uint64_t sv = b.g.StructureVersion();
    CHECK(b.g.Connect("f", "Out", "m", "A"));
    CHECK(b.g.StructureVersion() > sv);
    CHECK(events.size() == 1 && events[0].kind == ChangeKind::LinkChanged && events[0].pin == "A" && events[0].affectsStructure);
    CHECK(b.g.Connect("m", "Out", "out", "Roughness"));
    CHECK(b.g.Edges().size() == 2);
    CHECK(b.g.EdgesFrom("f").size() == 1 && b.g.EdgesTo("m").size() == 1);

    // 構造を変えない編集: 移動 / スロット値 / メタ
    events.clear();
    const uint64_t sv2 = b.g.StructureVersion(), v2 = b.g.Version();
    CHECK(b.g.MoveNode("f", 10, 20));
    CHECK(b.g.SetNodeProp("f", "value", Json(0.75)));
    CHECK(b.g.Version() > v2);
    CHECK_MSG(b.g.StructureVersion() == sv2, "移動とスロット値の編集は構造バージョンを進めない");
    CHECK(events.size() == 2 && !events[0].affectsStructure && !events[1].affectsStructure);
    // 同じ値の再設定は通知しない
    events.clear();
    CHECK(b.g.SetNodeProp("f", "value", Json(0.75)));
    CHECK(events.empty());
    // inline を立てると Code 扱い（以後の値の編集も構造変更）
    CHECK(b.g.SetNodeProp("f", "inline", Json(true)));
    const uint64_t sv3 = b.g.StructureVersion();
    CHECK(b.g.SetNodeProp("f", "value", Json(0.5)));
    CHECK(b.g.StructureVersion() > sv3);
    CHECK(b.g.SetNodeProp("f", "inline", Json(false)));

    // プロパティの検証
    CHECK(!b.g.SetNodeProp("f", "value", Json("abc")));
    CHECK(b.g.GetNodeProp("f", "value").get<double>() == 0.5);
    CHECK(b.g.GetNodeProp("m", "safe").is_null());           // Multiply には safe が無い
    b.N("Divide", "dv");
    CHECK(b.g.GetNodeProp("dv", "safe").get<bool>() == false);
    b.N("TextureSample", "ts");
    CHECK(!b.g.SetNodeProp("ts", "samplerState", Json("Bogus")));
    CHECK(b.g.SetNodeProp("ts", "samplerState", Json("LinearClamp")));

    // 型の問い合わせ（Multiply は接続元 float から float を推論）
    CHECK(b.g.PinType("m", "Out", true) == ValueType::F1);
    CHECK(b.g.PinType("m", "A", false) == ValueType::F1);
    {
        const PinInfo pi = b.g.QueryPin("m", "A", false);
        CHECK(pi.exists && pi.connected && !pi.isOutput && pi.inDecl != nullptr);
        const PinInfo po = b.g.QueryPin("f", "Out", true);
        CHECK(po.exists && po.connected && po.outDecl != nullptr);
        CHECK(!b.g.QueryPin("f", "Nope", true).exists);
        CHECK(b.g.QueryPin("out", "Specular", false).reserved);
    }

    // 接続可否
    {
        b.N("TextureParameter", "tp");
        b.N("Float2", "f2");
        b.N("Float3", "f3");
        b.N("Float4", "f4");
        b.N("Cross", "cr");
        ConnectCheck c = b.g.CanConnect("tp", "Out", "m", "A");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && Contains(c.message, "TextureSample"));
        c = b.g.CanConnect("f2", "Out", "cr", "A");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && c.code == code::kExpandForbidden && Contains(c.message, "Append"));
        c = b.g.CanConnect("f4", "Out", "out", "BaseColor");
        CHECK(c.verdict == ConnectCheck::Verdict::Warn && c.code == code::kTruncate && c.ok());
        c = b.g.CanConnect("f", "Out", "out", "BaseColor");   // 拡張（splat）は無警告
        CHECK(c.verdict == ConnectCheck::Verdict::Ok);
        c = b.g.CanConnect("f2", "Out", "out", "Specular");   // 予約ピン
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && c.code == code::kReservedPin);
        c = b.g.CanConnect("m", "Out", "m", "B");             // 自己ループ
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && c.code == code::kCycle);
        c = b.g.CanConnect("m", "Out", "f", "Nope");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject);
        c = b.g.CanConnect("nobody", "Out", "m", "A");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject);
        CHECK(!b.g.Connect("tp", "Out", "m", "A"));           // 拒否されたら何も変わらない
        CHECK(b.g.Edges().size() == 2);

        // 多相の次元不一致（A=float3 に B=float2）は接続の時点で拒否
        b.N("Add", "ad");
        CHECK(b.g.Connect("f3", "Out", "ad", "A"));
        c = b.g.CanConnect("f2", "Out", "ad", "B");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && c.code == code::kDimMismatch);
        CHECK(b.g.CanConnect("f", "Out", "ad", "B").verdict == ConnectCheck::Verdict::Ok);     // float は拡張できる
        CHECK(b.g.CanConnect("f3", "Out", "ad", "B").verdict == ConnectCheck::Verdict::Ok);

        // 循環: m → out へは繋がっている。m の入力に out の下流を繋ぐ
        b.N("Saturate", "sat");
        CHECK(b.g.Connect("m", "Out", "sat", "X"));
        c = b.g.CanConnect("sat", "Out", "m", "B");
        CHECK(c.verdict == ConnectCheck::Verdict::Reject && c.code == code::kCycle);
        CHECK(!b.g.Connect("sat", "Out", "m", "A"));
    }

    // ConnectUnchecked: 型の検査は通さない（Undo 用）。存在と予約だけ確かめる
    {
        b.N("Float2", "u2");
        CHECK(!b.g.Connect("u2", "Out", "cr", "B"));                 // 検査つきは拒否（float2 → float3）
        CHECK(b.g.ConnectUnchecked("u2", "Out", "cr", "B"));         // 履歴の復元は通る
        CHECK(HasDiag(b.g.Analysis().diags, code::kExpandForbidden, "cr", "B"));   // 状態は診断でエラーとして見える
        CHECK(!b.g.ConnectUnchecked("u2", "Nope", "cr", "B"));       // 出力ピンが無い
        CHECK(!b.g.ConnectUnchecked("u2", "Out", "cr", "Zzz"));      // 入力ピンが無い
        CHECK(!b.g.ConnectUnchecked("u2", "Out", "out", "Specular")); // 予約ピン
        CHECK(!b.g.ConnectUnchecked("ghost", "Out", "cr", "B"));
        CHECK(b.g.Disconnect("cr", "B"));
        b.g.RemoveNode("u2");
    }

    // リテラル
    CHECK(b.g.SetLiteral("m", "B", Value::Float(2.0f)));
    CHECK(b.g.Binding("m", "B") && b.g.Binding("m", "B")->literal && !b.g.Binding("m", "B")->link);
    CHECK(!b.g.SetLiteral("out", "Specular", Value::Float(1.0f)));    // 予約ピンは不可
    CHECK(!b.g.SetLiteral("cr", "A", Value::Float2(1, 2)));           // float2 → float3 は拡張不可
    CHECK(b.g.SetLiteral("cr", "A", Value::Float3(1, 2, 3)));
    CHECK(b.g.ClearLiteral("m", "B"));
    CHECK(b.g.Binding("m", "B") == nullptr);

    // ノード削除と復元（Undo）
    {
        const auto edgesBefore = b.g.Edges();
        RemovedNode rm;
        CHECK(b.g.RemoveNode("m", &rm));
        CHECK(b.g.FindNode("m") == nullptr);
        CHECK(b.g.Edges().size() < edgesBefore.size());
        CHECK(!b.g.RemoveNode("m"));
        CHECK(b.g.RestoreNode(rm));
        CHECK(b.g.Edges() == edgesBefore);
        CHECK(!b.g.RestoreNode(rm));    // 二重復元は不可
    }

    // コメント
    {
        Comment c; c.text = "テスト"; c.x = 1; c.y = 2; c.w = 30; c.h = 40;
        const std::string cid = b.g.AddComment(c);
        CHECK(!cid.empty() && b.g.Comments().count(cid) == 1);
        c = b.g.Comments().at(cid);
        c.text = "変更";
        CHECK(b.g.SetComment(c) && b.g.Comments().at(cid).text == "変更");
        CHECK(b.g.RemoveComment(cid) && b.g.Comments().empty());
    }

    b.g.RemoveListener(lid);
    events.clear();
    b.g.MoveNode("f", 1, 1);
    CHECK(events.empty());

    // 未知の型でも追加できる（ファイル往復のため）
    CHECK(!b.g.AddNode("FutureNode", 0, 0, "fut").empty());

    // ID 採番の決定性
    MaterialGraph a, c;
    a.SetIdSeed(42); c.SetIdSeed(42);
    CHECK(a.GenerateNodeId() == c.GenerateNodeId());
}

// ---------------------------------------------------------------------------
// 3. 診断
// ---------------------------------------------------------------------------
const char* kOutNode = R"("out": { "type": "MaterialOutput", "in": %s })";

// ノード定義の断片から .dxmg を作る（"nodes" の中身だけ書く）
std::string Dxmg(const std::string& nodes)
{
    return "{ \"version\": 1, \"kind\": \"material\", \"guid\": \"t\", \"name\": \"t\", \"nodes\": {" + nodes + "} }";
}

void TestDiagnostics()
{
    std::printf("[diagnostics]\n");
    (void)kOutNode;

    // 1. 未知のノード型
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "x": { "type": "NoSuchNode" },
            "out": { "type": "MaterialOutput", "in": { "BaseColor": "x.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok);
        CHECK(HasDiag(r.diagnostics, code::kUnknownNode, "x"));
        CHECK_MSG(CountSeverity(r.diagnostics, Severity::Error) == 1, "未知ノードの下流に連鎖エラーを出さない (%d)", CountSeverity(r.diagnostics, Severity::Error));
    }
    // 2. 必須入力が未接続
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "s": { "type": "Saturate" },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "s.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kMissingInput, "s", "X"));
        CHECK(CountSeverity(r.diagnostics, Severity::Error) == 1);
    }
    // 3. 型不一致（テクスチャを float の入力へ）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "t": { "type": "TextureParameter" },
            "m": { "type": "Multiply", "in": { "A": "t.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "m.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kTypeMismatch, "m", "A"));
    }
    // 4. 次元不一致（Add の A=float3, B=float2）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float3" }, "b": { "type": "Float2" },
            "ad": { "type": "Add", "in": { "A": "a.Out", "B": "b.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Emissive": "ad.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kDimMismatch, "ad", "B"));
        for (const Diagnostic& d : r.diagnostics)
            if (d.code == code::kDimMismatch) CHECK(Contains(d.message, "A=float3") && Contains(d.message, "B=float2"));
    }
    // 5. 拡張の禁止（Cross.A ← float2）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float2" }, "b": { "type": "Float3" },
            "c": { "type": "Cross", "in": { "A": "a.Out", "B": "b.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Emissive": "c.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kExpandForbidden, "c", "A"));
    }
    // 6. 切り詰めは警告（コンパイルは通る）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float4" },
            "out": { "type": "MaterialOutput", "in": { "BaseColor": "a.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(r.ok && HasDiag(r.diagnostics, code::kTruncate, "out", "BaseColor"));
        CHECK(CountSeverity(r.diagnostics, Severity::Warning) == 1 && CountSeverity(r.diagnostics, Severity::Error) == 0);
        CHECK(Contains(r.hlsl, "s.baseColor = v0.xyz;"));
    }
    // 7. 循環
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Add", "in": { "A": "b.Out" } },
            "b": { "type": "Add", "in": { "A": "a.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "a.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok);
        CHECK(HasDiag(r.diagnostics, code::kCycle, "a") || HasDiag(r.diagnostics, code::kCycle, "b"));
        for (const Diagnostic& d : r.diagnostics)
            if (d.code == code::kCycle) CHECK(Contains(d.message, "→"));
    }
    // 8. 接続元ノードが無い / 出力ピンが無い / 入力名が無い
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "f": { "type": "Float" },
            "a": { "type": "Add", "in": { "A": "ghost.Out", "B": "f.Nope", "Zzz": 1 } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "a.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok);
        CHECK(HasDiag(r.diagnostics, code::kDanglingLink, "a", "A"));
        CHECK(HasDiag(r.diagnostics, code::kUnknownPin, "a", "B"));
        CHECK(HasDiag(r.diagnostics, code::kUnknownPin, "a", "Zzz"));
    }
    // 9. Append の合計が 4 を超える / ComponentMask
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float3" }, "b": { "type": "Float2" },
            "ap": { "type": "Append", "in": { "A": "a.Out", "B": "b.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Emissive": "ap.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kAppendOverflow, "ap"));
    }
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float2" },
            "mk": { "type": "ComponentMask", "in": { "X": "a.Out" }, "props": { "r": true, "g": true, "b": true, "a": false } },
            "out": { "type": "MaterialOutput", "in": { "Emissive": "mk.Out" } })"));
        CHECK(HasDiag(CompileGraph(g, TestOpt()).diagnostics, code::kMaskRange, "mk"));
        MaterialGraph g2 = FromText(Dxmg(R"(
            "a": { "type": "Float2" },
            "mk": { "type": "ComponentMask", "in": { "X": "a.Out" }, "props": { "r": false, "g": false, "b": false, "a": false } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "mk.Out" } })"));
        CHECK(HasDiag(CompileGraph(g2, TestOpt()).diagnostics, code::kMaskEmpty, "mk"));
    }
    // 10. Split の存在しない成分（消費側に出る）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "a": { "type": "Float2" },
            "sp": { "type": "Split", "in": { "X": "a.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "sp.B" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kMaskRange, "out", "Roughness"));
    }
    // 11. 予約ピン
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "f": { "type": "Float" },
            "out": { "type": "MaterialOutput", "in": { "ClearCoat": "f.Out", "Anisotropy": 0.5 } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kReservedPin, "out", "ClearCoat") && HasDiag(r.diagnostics, code::kReservedPin, "out", "Anisotropy"));
    }
    // 12. 出力ノードが無い / 2 個ある
    {
        MaterialGraph g = FromText(Dxmg(R"("f": { "type": "Float" })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kNoOutput));
        MaterialGraph g2 = FromText(Dxmg(R"("o1": { "type": "MaterialOutput" }, "o2": { "type": "MaterialOutput" })"));
        const CompileResult r2 = CompileGraph(g2, TestOpt());
        CHECK(!r2.ok && HasDiag(r2.diagnostics, code::kMultiOutput, "o2"));
    }
    // 13. パラメータ名の重複 / 空
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "p1": { "type": "ScalarParameter", "props": { "name": "Rough" } },
            "p2": { "type": "ScalarParameter", "props": { "name": "Rough" } },
            "p3": { "type": "ScalarParameter", "props": { "name": "" } },
            "m": { "type": "Multiply", "in": { "A": "p1.Out", "B": "p2.Out" } },
            "m2": { "type": "Multiply", "in": { "A": "m.Out", "B": "p3.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "m2.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kDupParam, "p2") && HasDiag(r.diagnostics, code::kDupParam, "p3"));
    }
    // 14. Custom の本文が空 / 不正なプロパティ値
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "c": { "type": "Custom", "props": { "code": "  " } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "c.Out" } })"));
        CHECK(HasDiag(CompileGraph(g, TestOpt()).diagnostics, code::kCustomEmpty, "c"));
        MaterialGraph g2 = FromText(Dxmg(R"(
            "t": { "type": "TextureSample", "props": { "samplerState": "Nope" } },
            "out": { "type": "MaterialOutput", "in": { "BaseColor": "t.RGB" } })"));
        CHECK(HasDiag(CompileGraph(g2, TestOpt()).diagnostics, code::kBadProp, "t"));
    }
    // 15. テクスチャを Free ピン（Append.A）へ
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "t": { "type": "TextureParameter" },
            "ap": { "type": "Append", "in": { "A": "t.Out", "B": 1.0 } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "ap.Out" } })"));
        CHECK(HasDiag(CompileGraph(g, TestOpt()).diagnostics, code::kTypeMismatch, "ap", "A"));
    }
    // 16. 出力に繋がらないノードのエラーはコンパイルを止めない
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "dead": { "type": "Saturate" },
            "f": { "type": "Float", "props": { "value": 0.3 } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "f.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(r.ok);
        bool sawDeadErr = false, sawInfo = false;
        for (const Diagnostic& d : r.diagnostics)
        {
            if (d.code == code::kMissingInput && d.nodeId == "dead") { sawDeadErr = true; CHECK(!d.reachable); }
            if (d.code == code::kDeadNode && d.nodeId == "dead") sawInfo = true;
        }
        CHECK(sawDeadErr && sawInfo);
        CHECK(!Contains(r.hlsl, "saturate"));
        CHECK(r.stats.nodesTotal == 3 && r.stats.nodesReachable == 2);
    }
    // 17. 上流のエラーは 1 回だけ（下流に連鎖しない）
    {
        MaterialGraph g = FromText(Dxmg(R"(
            "s": { "type": "Saturate" },
            "m": { "type": "Multiply", "in": { "A": "s.Out", "B": 2.0 } },
            "n": { "type": "Normalize", "in": { "X": "m.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "n.Out" } })"));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(CountSeverity(r.diagnostics, Severity::Error) == 1 && HasDiag(r.diagnostics, code::kMissingInput, "s", "X"));
    }
    // 18. Int ピン / Bool の型規則（登録表を拡張したテスト用ノード）
    {
        NodeLibrary lib = NodeLibrary::Builtin();
        NodeDef d;
        d.type = "TestIntUser"; d.displayName = "TestIntUser"; d.category = "数学";
        PinDecl p; p.name = "N"; p.mode = PinMode::Fixed; p.type = ValueType::Int; p.required = true;
        d.inputs = {p};
        OutPinDecl o; o.name = "Out"; o.mode = PinMode::Fixed; o.type = ValueType::F1;
        d.outputs = {o};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F1;
        d.hlsl = "float({N})";
        lib.Register(d);
        MaterialGraph g(&lib);
        std::string err;
        CHECK(LoadDxmg(Dxmg(R"(
            "f": { "type": "Float" },
            "i": { "type": "Int", "props": { "value": 4 } },
            "u1": { "type": "TestIntUser", "in": { "N": "f.Out" } },
            "u2": { "type": "TestIntUser", "in": { "N": "i.Out" } },
            "u3": { "type": "TestIntUser", "in": { "N": 3 } },
            "a": { "type": "Add", "in": { "A": "u1.Out", "B": "u2.Out" } },
            "a2": { "type": "Add", "in": { "A": "a.Out", "B": "u3.Out" } },
            "out": { "type": "MaterialOutput", "in": { "Roughness": "a2.Out" } })"), g, &err));
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(!r.ok && HasDiag(r.diagnostics, code::kTypeMismatch, "u1", "N"));
        CHECK(!HasDiag(r.diagnostics, code::kTypeMismatch, "u2") && !HasDiag(r.diagnostics, code::kTypeMismatch, "u3"));
    }
}

// ---------------------------------------------------------------------------
// 4. コード生成
// ---------------------------------------------------------------------------
void TestCodegen()
{
    std::printf("[codegen]\n");

    // 最小のグラフ: 出力の形を厳密に確認
    {
        GB b;
        b.N("Float3", "col", {{"value", Json::array({1.0, 0.5, 0.25})}});
        b.N("Float", "rough", {{"value", Json(0.3)}});
        b.N("MaterialOutput", "out");
        b.MustL("col.Out", "out.BaseColor");
        b.MustL("rough.Out", "out.Roughness");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok && !r.HasErrors());
        const std::string expect =
            "// @sm 6_6\n"
            "// GENERATED by Uno MaterialGraph. DO NOT EDIT.  graph=" + std::string(r.hlsl.substr(r.hlsl.find("graph=") + 6, 16)) + "  codegen=1\n"
            "#include \"UnoMatContractRef.hlsli\"\n"
            "\n"
            "void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s)\n"
            "{\n"
            "    s = UnoSurfaceDefault();\n"
            "#line 1 \"node:col\"\n"
            "    float3 v0 = pool.F3(0);\n"
            "#line 1 \"node:rough\"\n"
            "    float v1 = pool.F1(1);\n"
            "#line 1 \"node:out\"\n"
            "    s.baseColor = v0;\n"
            "    s.roughness = v1;\n"
            "}\n";
        CHECK_MSG(r.hlsl == expect, "\n--- got ---\n%s", r.hlsl.c_str());
        CHECK(r.slotCount == 2 && r.slots.size() == 2);
        CHECK(r.slots[0].kind == SlotKind::Constant && r.slots[0].type == ValueType::F3 && r.slots[0].nodeId == "col");
        CHECK(r.slots[0].value[1] == 0.5f);
        CHECK(r.slots[1].type == ValueType::F1 && r.slots[1].value[0] == 0.3f);
        CHECK(r.stats.ops == 2 && r.stats.slots == 2 && r.stats.textures == 0);
    }

    // 決定論: 位置 / コメント / ID 採番 / 値スロット / JSON のキー順が変わっても HLSL は同じ
    {
        const std::string text = Dxmg(R"(
            "a": { "type": "Float3", "props": { "value": [0.2, 0.4, 0.6] } },
            "b": { "type": "Float", "props": { "value": 2.0 } },
            "m": { "type": "Multiply", "in": { "A": "a.Out", "B": "b.Out" } },
            "out": { "type": "MaterialOutput", "in": { "BaseColor": "m.Out" } })");
        MaterialGraph g1 = FromText(text);
        const CompileResult r1 = CompileGraph(g1, TestOpt());
        CHECK(r1.ok);
        // 同じテキストをもう一度
        MaterialGraph g2 = FromText(text);
        CHECK(CompileGraph(g2, TestOpt()).hlsl == r1.hlsl);
        // キー順を入れ替えたテキスト
        const std::string shuffled = Dxmg(R"(
            "out": { "in": { "BaseColor": "m.Out" }, "type": "MaterialOutput" },
            "m": { "in": { "B": "b.Out", "A": "a.Out" }, "type": "Multiply" },
            "b": { "props": { "value": 2.0 }, "type": "Float" },
            "a": { "props": { "value": [0.2, 0.4, 0.6] }, "type": "Float3" })");
        MaterialGraph g3 = FromText(shuffled);
        CHECK(CompileGraph(g3, TestOpt()).hlsl == r1.hlsl);
        // 位置とコメント
        g1.MoveNode("a", 123, 456);
        Comment c; c.text = "コメント";
        g1.AddComment(c);
        CHECK(CompileGraph(g1, TestOpt()).hlsl == r1.hlsl);
        // 値の編集（スロット）でも HLSL は同じ。スロットの既定値だけ変わる
        CHECK(g1.SetNodeProp("b", "value", Json(7.5)));
        CHECK(g1.SetNodeProp("a", "value", Json::array({0.9, 0.9, 0.9})));
        const CompileResult r4 = CompileGraph(g1, TestOpt());
        CHECK_MSG(r4.hlsl == r1.hlsl, "値の編集で HLSL が変わった（再コンパイルが必要になってしまう）");
        CHECK(r4.hash == r1.hash);
        CHECK(r4.slots[1].value[0] == 7.5f && r4.slots[0].value[0] == 0.9f);
        // 構造の変更（inline）では変わる
        CHECK(g1.SetNodeProp("b", "inline", Json(true)));
        const CompileResult r5 = CompileGraph(g1, TestOpt());
        CHECK(r5.hlsl != r1.hlsl && Contains(r5.hlsl, "7.5"));
        CHECK(r5.slotCount == 1);
        // 別の ID 採番でも同じ形（ID は #line に出るだけ。ここでは同じ ID を使うので同一）
    }

    // CSE: 同じ入力の同じノードは 1 回だけ評価。スロットを持つノードは合流しない。inline 同値は合流する
    {
        GB b;
        b.N("Float3", "c1", {{"value", Json::array({1.0, 1.0, 1.0})}});
        b.N("Float3", "c2", {{"value", Json::array({1.0, 1.0, 1.0})}});           // 同値だが別スロット（独立に編集できる）
        b.N("Multiply", "m1"); b.N("Multiply", "m2"); b.N("Add", "sum");
        b.MustL("c1.Out", "m1.A"); b.MustL("c1.Out", "m2.A");
        b.Lit("m1", "B", Value::Float(2.0f)); b.Lit("m2", "B", Value::Float(2.0f));    // m1 と m2 は同じ式
        b.MustL("m1.Out", "sum.A"); b.MustL("m2.Out", "sum.B");
        b.N("Multiply", "m3"); b.MustL("c2.Out", "m3.A"); b.Lit("m3", "B", Value::Float(2.0f));  // c2 由来なので別
        b.N("Add", "sum2"); b.MustL("sum.Out", "sum2.A"); b.MustL("m3.Out", "sum2.B");
        b.N("MaterialOutput", "out"); b.MustL("sum2.Out", "out.Emissive");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        int multiplies = 0;
        for (const IrOp& op : r.ir.ops) if (op.opcode == "Multiply") ++multiplies;
        CHECK_MSG(multiplies == 2, "Multiply は 2 個（m1 = m2 が合流、m3 は別）: %d", multiplies);
        CHECK(r.stats.cseHits >= 1);
        CHECK(r.slotCount == 2);   // c1 と c2 は別スロット

        // inline の同値定数は合流
        GB b2;
        b2.N("Float", "k1", {{"value", Json(0.5)}, {"inline", Json(true)}});
        b2.N("Float", "k2", {{"value", Json(0.5)}, {"inline", Json(true)}});
        b2.N("Add", "a"); b2.MustL("k1.Out", "a.A"); b2.MustL("k2.Out", "a.B");
        b2.N("MaterialOutput", "out"); b2.MustL("a.Out", "out.Roughness");
        const CompileResult r2 = CompileGraph(b2.g, TestOpt());
        CHECK(r2.ok && r2.slotCount == 0);
        CHECK_MSG(r2.stats.ops == 2, "inline 同値定数 2 個 + Add = 2 Op（%d）", r2.stats.ops);
        CHECK(Contains(r2.hlsl, "(v0 + v0)"));
    }

    // 出力に繋がらないノードは生成されない
    {
        GB b;
        b.N("Float", "used", {{"value", Json(0.1)}});
        b.N("Float", "unused", {{"value", Json(0.9)}});
        b.N("Sin", "deadSin"); b.MustL("unused.Out", "deadSin.X");
        b.N("MaterialOutput", "out"); b.MustL("used.Out", "out.Metallic");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok && r.slotCount == 1 && !Contains(r.hlsl, "sin(") && !Contains(r.hlsl, "unused"));
    }

    // Reroute は生成コードに現れず、型を引き継ぐ
    {
        GB b;
        b.N("Float3", "c", {{"value", Json::array({0.1, 0.2, 0.3})}});
        b.N("Reroute", "rr1"); b.N("Reroute", "rr2");
        b.MustL("c.Out", "rr1.In"); b.MustL("rr1.Out", "rr2.In");
        b.N("MaterialOutput", "out"); b.MustL("rr2.Out", "out.BaseColor");
        CHECK(b.g.PinType("rr2", "Out", true) == ValueType::F3);
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok && r.stats.ops == 1 && Contains(r.hlsl, "s.baseColor = v0;"));
    }

    // キャストの出力: splat / truncate / int→float
    {
        GB b;
        b.N("Float", "f", {{"value", Json(0.5)}});
        b.N("Float4", "f4", {{"value", Json::array({1.0, 2.0, 3.0, 4.0})}});
        b.N("Int", "i", {{"value", Json(3)}});
        b.N("MaterialOutput", "out");
        b.MustL("f.Out", "out.BaseColor");          // splat
        b.MustL("f4.Out", "out.Emissive");          // truncate（警告）
        b.MustL("i.Out", "out.Roughness");          // int → float
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        CHECK(Contains(r.hlsl, "s.baseColor = v0.xxx;"));
        CHECK(Contains(r.hlsl, "s.emissive = v2.xyz;"));
        CHECK(Contains(r.hlsl, "int v1 = 3;") && Contains(r.hlsl, "s.roughness = ((float)v1);"));
    }

    // Normal を繋ぐと hasNormal が立つ
    {
        GB b;
        b.N("Float3", "n", {{"value", Json::array({0.0, 0.0, 1.0})}});
        b.N("MaterialOutput", "out"); b.MustL("n.Out", "out.Normal");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok && Contains(r.hlsl, "s.normalTS = v0;\n    s.hasNormal = true;"));
    }

    // ソースマップ: 各エントリの先頭行は `#line 1 "node:<id>"`
    {
        GB b;
        b.N("Float", "a", {{"value", Json(1.0)}}); b.N("Sin", "s"); b.MustL("a.Out", "s.X");
        b.N("MaterialOutput", "out"); b.MustL("s.Out", "out.Roughness");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        const auto lines = SplitLines(r.hlsl);
        CHECK(r.sourceMap.size() == 3);
        for (const SourceMapEntry& e : r.sourceMap)
        {
            CHECK(e.firstLine >= 1 && static_cast<size_t>(e.lastLine) <= lines.size());
            CHECK(lines[static_cast<size_t>(e.firstLine) - 1] == "#line 1 \"node:" + e.nodeId + "\"");
        }
        CHECK(static_cast<int>(lines.size()) == r.stats.hlslLines);
    }
}

// パラメータ / テクスチャ / スロット
void TestSlots()
{
    std::printf("[slots]\n");
    GB b;
    b.N("ScalarParameter", "pr", {{"name", Json("Roughness")}, {"default", Json(0.6)}, {"range", Json::array({0.0, 2.0})}, {"group", Json("Surface")}, {"priority", Json(3)}});
    b.N("VectorParameter", "pt", {{"name", Json("Tint")}, {"default", Json::array({0.5, 0.25, 1.0, 1.0})}, {"srgb", Json(true)}});
    b.N("TextureParameter", "ta", {{"name", Json("Albedo")}, {"default", Json("textures/a.png")}, {"sampler", Json("Color")}});
    b.N("TextureParameter", "tn", {{"name", Json("Normal")}, {"default", Json("textures/n.png")}, {"sampler", Json("Normal")}});
    b.N("TexCoord", "uv", {{"tiling", Json::array({2.0, 3.0})}});
    b.N("TextureSample", "s1"); b.MustL("ta.Out", "s1.Tex"); b.MustL("uv.UV", "s1.UV");
    b.N("TextureSample", "s2"); b.MustL("ta.Out", "s2.Tex"); b.MustL("uv.UV", "s2.UV");     // s1 と同じ → 合流
    b.N("Multiply", "tintMul"); b.MustL("s1.RGB", "tintMul.A"); b.MustL("pt.RGB", "tintMul.B");
    b.N("NormalMap", "nm"); b.MustL("tn.Out", "nm.Tex"); b.MustL("uv.UV", "nm.UV"); b.Lit("nm", "Strength", Value::Float(0.8f));
    b.N("TextureSample", "s3", {{"texture", Json("textures/orm.png")}, {"usage", Json("Mask")}});   // Tex 未接続 → 暗黙テクスチャ
    b.N("MaterialOutput", "out");
    b.MustL("tintMul.Out", "out.BaseColor");
    b.MustL("s2.A", "out.Opacity");
    b.MustL("nm.Out", "out.Normal");
    b.MustL("pr.Out", "out.Roughness");
    b.MustL("s3.G", "out.Metallic");
    const CompileResult r = CompileGraph(b.g, TestOpt());
    CHECK_MSG(r.ok, "%s", r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
    if (!r.ok) return;

    // スロット表: 連続 / 一意
    CHECK(r.slotCount == static_cast<int>(r.slots.size()));
    std::set<int> seen;
    for (size_t i = 0; i < r.slots.size(); ++i)
    {
        CHECK(r.slots[i].slot == static_cast<int>(i));
        seen.insert(r.slots[i].slot);
    }
    CHECK(seen.size() == r.slots.size());

    const SlotInfo* rough = r.FindSlotByName("Roughness");
    CHECK(rough && rough->kind == SlotKind::Scalar && rough->type == ValueType::F1 && rough->value[0] == 0.6f);
    CHECK(rough && rough->hasRange && rough->rangeMax == 2.0f && rough->group == "Surface" && rough->priority == 3 && rough->nodeId == "pr");
    const SlotInfo* tint = r.FindSlotByName("Tint");
    CHECK(tint && tint->kind == SlotKind::Vector && tint->type == ValueType::F4 && tint->srgb);
    const SlotInfo* alb = r.FindSlotByName("Albedo");
    CHECK(alb && alb->kind == SlotKind::Texture && alb->type == ValueType::Tex2D && alb->texturePath == "textures/a.png" && alb->textureUsage == "Color");
    const SlotInfo* nrm = r.FindSlotByName("Normal");
    CHECK(nrm && nrm->textureUsage == "Normal");
    int implicitCount = 0, nodeProp = 0;
    for (const SlotInfo& s : r.slots)
    {
        if (s.kind == SlotKind::ImplicitTexture) { ++implicitCount; CHECK(s.nodeId == "s3" && s.texturePath == "textures/orm.png" && s.textureUsage == "Mask"); }
        if (s.kind == SlotKind::NodeProp) ++nodeProp;
    }
    CHECK(implicitCount == 1);
    CHECK_MSG(nodeProp == 2, "TexCoord の tiling / offset = 2 スロット (%d)", nodeProp);
    CHECK(r.stats.textures == 3);

    // CSE: 同じテクスチャ + 同じ UV のサンプルは 1 回
    int samples = 0;
    for (const IrOp& op : r.ir.ops) if (op.opcode == "TextureSample") ++samples;
    CHECK_MSG(samples == 2, "TextureSample は s1=s2 が合流して 2 個（s1 と s3）: %d", samples);
    CHECK(Contains(r.hlsl, "MG_NormalUnpack"));
    CHECK(Contains(r.hlsl, "UNO_SAMP_ANISO_WRAP"));
    CHECK(Contains(r.hlsl, "pool.Tex("));

    // パラメータレコード
    ParamOverrides ov;
    ov["Roughness"].v[0] = 0.9f;
    ParamOverride tv; tv.isTexture = true; tv.texture = "textures/other.png";
    ov["Albedo"] = tv;
    std::vector<std::string> asked;
    const TextureResolver res = [&](const std::string& path, const std::string& usage) {
        asked.push_back(path + "|" + usage);
        TextureBinding tb;
        tb.srvIndex = 100u + static_cast<uint32_t>(asked.size());
        tb.width = 512; tb.height = 256;
        return tb;
    };
    const std::vector<float> rec = PackParamRecord(r, ov, res);
    CHECK(rec.size() == static_cast<size_t>(r.slotCount) * 4);
    CHECK(rec[static_cast<size_t>(rough->slot) * 4] == 0.9f);
    // sRGB → リニア（0.5 → 0.2140...、1.0 → 1.0、A は変換しない）
    CHECK(Near(rec[static_cast<size_t>(tint->slot) * 4 + 0], SrgbToLinear(0.5f)));
    CHECK(Near(rec[static_cast<size_t>(tint->slot) * 4 + 0], 0.21404f, 1e-3f));
    CHECK(Near(rec[static_cast<size_t>(tint->slot) * 4 + 2], 1.0f));
    CHECK(rec[static_cast<size_t>(tint->slot) * 4 + 3] == 1.0f);
    CHECK(SrgbToLinear(0.0f) == 0.0f && Near(SrgbToLinear(1.0f), 1.0f) && Near(SrgbToLinear(0.04f), 0.04f / 12.92f));
    // テクスチャスロット: asuint(x) = SRV 添字、y = 1/幅、z = 1/高さ
    uint32_t bits;
    std::memcpy(&bits, &rec[static_cast<size_t>(alb->slot) * 4], sizeof(bits));
    CHECK(bits >= 101u && Near(rec[static_cast<size_t>(alb->slot) * 4 + 1], 1.0f / 512.0f) && Near(rec[static_cast<size_t>(alb->slot) * 4 + 2], 1.0f / 256.0f));
    bool askedOther = false, askedNormal = false;
    for (const std::string& a : asked) { if (a == "textures/other.png|Color") askedOther = true; if (a == "textures/n.png|Normal") askedNormal = true; }
    CHECK(askedOther && askedNormal);

    // パラメータ宣言の一覧（priority → group → name の順）
    {
        const std::vector<ParamDecl> pd = CollectParameters(b.g);
        CHECK(pd.size() == 4);
        if (pd.size() == 4)
        {
            CHECK(pd[0].name == "Albedo" || pd[0].name == "Normal" || pd[0].name == "Tint");   // priority 0 の 3 件が先
            CHECK(pd[3].name == "Roughness" && pd[3].priority == 3 && pd[3].type == ValueType::F1 && pd[3].group == "Surface");
            for (const ParamDecl& d : pd) CHECK(d.reachable);
            for (const ParamDecl& d : pd)
                if (d.name == "Normal") CHECK(d.type == ValueType::Tex2D && d.usage == "Normal" && d.defaultValue.get<std::string>() == "textures/n.png");
        }
        // 出力に繋がっていないパラメータも一覧には出る（reachable=false）
        b.g.AddNode("ScalarParameter", 0, 0, "orphan");
        b.g.SetNodeProp("orphan", "name", Json("Orphan"));
        bool sawOrphan = false;
        for (const ParamDecl& d : CollectParameters(b.g)) if (d.name == "Orphan") { sawOrphan = true; CHECK(!d.reachable); }
        CHECK(sawOrphan);
        b.g.RemoveNode("orphan");
    }

    // 値だけの変更（既定値・名前・範囲）では HLSL は変わらない。パラメータ名を変えても同じ HLSL（スロット表の名前だけ変わる）
    const std::string hlsl0 = r.hlsl;
    CHECK(b.g.SetNodeProp("pr", "name", Json("Rough2")));
    CHECK(b.g.SetNodeProp("pr", "default", Json(0.1)));
    CHECK(b.g.SetNodeProp("ta", "default", Json("textures/z.png")));
    const CompileResult r2 = CompileGraph(b.g, TestOpt());
    CHECK(r2.hlsl == hlsl0 && r2.FindSlotByName("Rough2") && !r2.FindSlotByName("Roughness"));
}

// Custom ノード
void TestCustom()
{
    std::printf("[custom]\n");
    GB b;
    b.N("Float3", "c", {{"value", Json::array({1.0, 2.0, 3.0})}});
    b.N("Custom", "cu", {{"code", Json("In0 * 2.0 + In1")}, {"outputType", Json("float3")}});
    b.MustL("c.Out", "cu.In0");
    b.Lit("cu", "In1", Value::Float3(0.5f, 0.5f, 0.5f));
    b.N("Custom", "cf", {{"code", Json("float x = In0;\nfloat y = x * x;\nreturn y + 1.0;")}, {"outputType", Json("float")}});
    b.N("Float", "k", {{"value", Json(3.0)}}); b.MustL("k.Out", "cf.In0");
    b.N("Add", "sum"); b.MustL("cu.Out", "sum.A"); b.MustL("cf.Out", "sum.B");
    b.N("MaterialOutput", "out"); b.MustL("sum.Out", "out.Emissive");
    const CompileResult r = CompileGraph(b.g, TestOpt());
    CHECK(r.ok);
    CHECK(Contains(r.hlsl, "float3 MG_Custom0(float3 In0, float3 In1)"));
    CHECK(Contains(r.hlsl, "    return (In0 * 2.0 + In1);"));
    CHECK(Contains(r.hlsl, "float MG_Custom1(float In0)"));
    CHECK(Contains(r.hlsl, "    float y = x * x;"));
    CHECK(Contains(r.hlsl, "MG_Custom0(v0, float3(0.5, 0.5, 0.5))"));
    // 本文の先頭行は `#line 1 "node:cf"` の次の行 = 本文の 1 行目
    const auto lines = SplitLines(r.hlsl);
    bool found = false;
    for (size_t i = 0; i + 1 < lines.size(); ++i)
        if (lines[i] == "#line 1 \"node:cf\"" && lines[i + 1] == "    float x = In0;") found = true;
    CHECK(found);
    // CPU では評価できない（診断で報告）
    const CpuEvalResult cr = EvaluateCpu(r, EvalEnv{});
    CHECK(!cr.ok && HasDiag(cr.diagnostics, code::kCpuUnsupported, "cu"));
    // DXC ログの逆引き
    const std::string log =
        "node:cf:2:9: error: use of undeclared identifier 'zz'\n"
        "    float y = zz * x;\n"
        "warning: some noise\n"
        "node:cu:1:3: warning: implicit truncation of vector type\n"
        "matgraph.hlsl:40:5: error: unknown type name 'foo'\n";
    const std::vector<Diagnostic> ds = MapDxcLog(log);
    CHECK(ds.size() == 3);
    if (ds.size() == 3)
    {
        CHECK(ds[0].nodeId == "cf" && ds[0].line == 2 && ds[0].severity == Severity::Error && ds[0].code == code::kDxc);
        CHECK(Contains(ds[0].message, "undeclared identifier"));
        CHECK(ds[1].nodeId == "cu" && ds[1].severity == Severity::Warning);
        CHECK(ds[2].nodeId.empty() && ds[2].line == 40);
    }
}

// ---------------------------------------------------------------------------
// 5. CPU 評価
// ---------------------------------------------------------------------------
float EvalEmissiveX(const MaterialGraph& g, const EvalEnv& env = {})
{
    const CompileResult r = CompileGraph(g, TestOpt());
    CHECK_MSG(r.ok, "compile");
    const CpuEvalResult e = EvaluateCpu(r, env);
    CHECK(e.ok);
    return e.surface.emissive[0];
}

void TestCpuEval()
{
    std::printf("[cpu-eval]\n");

    // 単項: X = float3(a,b,c) → Node → Emissive（3 成分をそれぞれ検査）
    struct Un { const char* type; std::function<float(float)> f; std::vector<float> xs; };
    const std::vector<Un> unary = {
        {"Saturate", [](float x) { return std::min(std::max(x, 0.0f), 1.0f); }, {-0.5f, 0.3f, 1.7f}},
        {"OneMinus", [](float x) { return 1.0f - x; }, {-0.5f, 0.3f, 1.7f}},
        {"Abs",      [](float x) { return std::fabs(x); }, {-0.5f, 0.3f, -1.7f}},
        {"Frac",     [](float x) { return x - std::floor(x); }, {-0.25f, 0.3f, 2.7f}},
        {"Floor",    [](float x) { return std::floor(x); }, {-0.25f, 0.3f, 2.7f}},
        {"Ceil",     [](float x) { return std::ceil(x); }, {-0.25f, 0.3f, 2.7f}},
        {"Sqrt",     [](float x) { return std::sqrt(std::max(x, 0.0f)); }, {-4.0f, 2.0f, 9.0f}},
        {"Sin",      [](float x) { return std::sin(x); }, {-1.0f, 0.5f, 2.0f}},
        {"Cos",      [](float x) { return std::cos(x); }, {-1.0f, 0.5f, 2.0f}},
    };
    for (const Un& u : unary)
    {
        GB b;
        b.N("Float3", "v", {{"value", Json::array({u.xs[0], u.xs[1], u.xs[2]})}});
        b.N(u.type, "n"); b.MustL("v.Out", "n.X");
        b.N("MaterialOutput", "out"); b.MustL("n.Out", "out.Emissive");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK_MSG(r.ok, "%s", u.type);
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(e.ok);
        for (int k = 0; k < 3; ++k)
            CHECK_MSG(Near(e.surface.emissive[k], u.f(u.xs[static_cast<size_t>(k)])), "%s[%d]: %g vs %g", u.type, k, e.surface.emissive[k], u.f(u.xs[static_cast<size_t>(k)]));
    }

    // 二項（A は float3、B は float にして splat も同時に検査）
    struct Bin { const char* type; const char* pa; const char* pb; std::function<float(float, float)> f; };
    const std::vector<Bin> binary = {
        {"Add",      "A", "B", [](float a, float b) { return a + b; }},
        {"Subtract", "A", "B", [](float a, float b) { return a - b; }},
        {"Multiply", "A", "B", [](float a, float b) { return a * b; }},
        {"Divide",   "A", "B", [](float a, float b) { return a / b; }},
        {"Min",      "A", "B", [](float a, float b) { return std::min(a, b); }},
        {"Max",      "A", "B", [](float a, float b) { return std::max(a, b); }},
        {"Power",    "Base", "Exp", [](float a, float e) { return std::pow(std::max(a, 0.0f), e); }},
        {"Step",     "X", "Edge", [](float x, float e) { return x >= e ? 1.0f : 0.0f; }},
    };
    const float av[3] = {0.25f, 1.5f, -2.0f};
    for (const Bin& bn : binary)
    {
        GB b;
        b.N("Float3", "a", {{"value", Json::array({av[0], av[1], av[2]})}});
        b.N("Float", "c", {{"value", Json(0.75)}});
        b.N(bn.type, "n"); b.MustL("a.Out", std::string("n.") + bn.pa); b.MustL("c.Out", std::string("n.") + bn.pb);
        b.N("MaterialOutput", "out"); b.MustL("n.Out", "out.Emissive");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK_MSG(r.ok, "%s", bn.type);
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        for (int k = 0; k < 3; ++k)
            CHECK_MSG(Near(e.surface.emissive[k], bn.f(av[k], 0.75f)), "%s[%d]: %g vs %g", bn.type, k, e.surface.emissive[k], bn.f(av[k], 0.75f));
    }

    // Divide の safe
    {
        GB b;
        b.N("Float", "one", {{"value", Json(1.0)}}); b.N("Float", "zero", {{"value", Json(0.0)}});
        b.N("Divide", "d", {{"safe", Json(true)}}); b.MustL("one.Out", "d.A"); b.MustL("zero.Out", "d.B");
        b.N("MaterialOutput", "out"); b.MustL("d.Out", "out.Emissive");
        CHECK(Near(EvalEmissiveX(b.g), 1.0f / 1e-6f));
    }

    // Lerp / Clamp / Smoothstep
    {
        GB b;
        b.N("Float3", "a", {{"value", Json::array({1.0, 0.0, 0.0})}});
        b.N("Float3", "c", {{"value", Json::array({0.0, 0.0, 1.0})}});
        b.N("Lerp", "l", {}); b.MustL("a.Out", "l.A"); b.MustL("c.Out", "l.B"); b.Lit("l", "Alpha", Value::Float(0.25f));
        b.N("MaterialOutput", "out"); b.MustL("l.Out", "out.BaseColor");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(Near(e.surface.baseColor[0], 0.75f) && Near(e.surface.baseColor[1], 0.0f) && Near(e.surface.baseColor[2], 0.25f));
    }
    {
        GB b;
        b.N("Float", "x", {{"value", Json(5.0)}});
        b.N("Clamp", "c"); b.MustL("x.Out", "c.X"); b.Lit("c", "Min", Value::Float(-1.0f)); b.Lit("c", "Max", Value::Float(2.0f));
        b.N("Smoothstep", "s"); b.Lit("s", "Min", Value::Float(0.0f)); b.Lit("s", "Max", Value::Float(4.0f)); b.Lit("s", "X", Value::Float(1.0f));
        b.N("MaterialOutput", "out"); b.MustL("c.Out", "out.Metallic"); b.MustL("s.Out", "out.Roughness");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(Near(e.surface.metallic, 2.0f));
        const float t = 0.25f;
        CHECK(Near(e.surface.roughness, t * t * (3.0f - 2.0f * t)));
    }

    // ベクトル: Dot / Cross / Normalize / Length / Append / ComponentMask / Split
    {
        GB b;
        b.N("Float3", "a", {{"value", Json::array({1.0, 2.0, 3.0})}});
        b.N("Float3", "c", {{"value", Json::array({4.0, 5.0, 6.0})}});
        b.N("Dot", "dot"); b.MustL("a.Out", "dot.A"); b.MustL("c.Out", "dot.B");
        b.N("Cross", "cr"); b.MustL("a.Out", "cr.A"); b.MustL("c.Out", "cr.B");
        b.N("Length", "len"); b.MustL("a.Out", "len.X");
        b.N("Normalize", "nz"); b.MustL("c.Out", "nz.X");
        b.N("MaterialOutput", "out");
        b.MustL("dot.Out", "out.Metallic"); b.MustL("cr.Out", "out.BaseColor"); b.MustL("len.Out", "out.Roughness"); b.MustL("nz.Out", "out.Emissive");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(Near(e.surface.metallic, 32.0f));
        CHECK(Near(e.surface.baseColor[0], -3.0f) && Near(e.surface.baseColor[1], 6.0f) && Near(e.surface.baseColor[2], -3.0f));
        CHECK(Near(e.surface.roughness, std::sqrt(14.0f)));
        const float l = std::sqrt(77.0f);
        CHECK(Near(e.surface.emissive[0], 4.0f / l) && Near(e.surface.emissive[2], 6.0f / l));
    }
    {
        GB b;
        b.N("Float3", "a", {{"value", Json::array({1.0, 2.0, 3.0})}});
        b.N("Float", "s", {{"value", Json(9.0)}});
        b.N("Append", "ap"); b.MustL("s.Out", "ap.A"); b.MustL("a.Out", "ap.B");                 // (9, 1, 2, 3)
        CHECK(b.g.PinType("ap", "Out", true) == ValueType::F4);
        b.N("ComponentMask", "mk", {{"r", Json(false)}, {"g", Json(true)}, {"b", Json(false)}, {"a", Json(true)}});
        b.MustL("ap.Out", "mk.X");                                                                // (1, 3)
        CHECK(b.g.PinType("mk", "Out", true) == ValueType::F2);
        b.N("Split", "sp"); b.MustL("ap.Out", "sp.X");
        b.N("MaterialOutput", "out");
        CHECK(!b.L("mk.Out", "out.Emissive"));   // float2 → float3 は拡張不可（接続の時点で拒否）
        b.N("Append", "ap2"); b.MustL("mk.Out", "ap2.A"); b.Lit("ap2", "B", Value::Float(7.0f));   // (1, 3, 7)
        b.MustL("ap2.Out", "out.Emissive");
        b.MustL("sp.A", "out.Metallic");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(Near(e.surface.emissive[0], 1.0f) && Near(e.surface.emissive[1], 3.0f) && Near(e.surface.emissive[2], 7.0f));
        CHECK(Near(e.surface.metallic, 3.0f));
    }

    // 組み込み入力と Fresnel / CameraVector / Time
    {
        EvalEnv env;
        env.uv[0] = 0.25f; env.uv[1] = 0.75f;
        env.worldPos[0] = 1; env.worldPos[1] = 2; env.worldPos[2] = 3;
        env.cameraPos[0] = 1; env.cameraPos[1] = 2; env.cameraPos[2] = 13;
        env.vertexNormalWS[0] = 0; env.vertexNormalWS[1] = 0; env.vertexNormalWS[2] = 1;
        env.time = 2.5f;
        GB b;
        b.N("TexCoord", "uv", {{"tiling", Json::array({2.0, 4.0})}, {"offset", Json::array({0.5, 0.0})}});
        b.N("Fresnel", "fr");
        b.N("CameraVector", "cv");
        b.N("Time", "t");
        b.N("Split", "sp"); b.MustL("uv.UV", "sp.X");
        b.N("MaterialOutput", "out");
        b.MustL("fr.Out", "out.Metallic"); b.MustL("t.Out", "out.Roughness"); b.MustL("cv.Out", "out.Emissive"); b.MustL("sp.G", "out.Opacity");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        const CpuEvalResult e = EvaluateCpu(r, env);
        CHECK(e.ok);
        CHECK(Near(e.surface.roughness, 2.5f));
        CHECK(Near(e.surface.opacity, 0.75f * 4.0f));
        CHECK(Near(e.surface.emissive[2], 1.0f) && Near(e.surface.emissive[0], 0.0f));
        // 法線 (0,0,1) と視線 (0,0,1) は平行 → dot = 1 → (1-1)^5 = 0 → BaseReflect
        CHECK(Near(e.surface.metallic, 0.04f));
    }

    // Select / Compare / Bool / Int / Desaturation
    {
        GB b;
        b.N("Float", "x", {{"value", Json(0.3)}});
        b.N("Compare", "cmp", {{"mode", Json("Greater")}}); b.MustL("x.Out", "cmp.A"); b.Lit("cmp", "B", Value::Float(0.5f));
        b.N("Select", "sel"); b.Lit("sel", "A", Value::Float3(1, 0, 0)); b.Lit("sel", "B", Value::Float3(0, 1, 0)); b.MustL("cmp.Out", "sel.Cond");
        b.N("Desaturation", "ds"); b.MustL("sel.Out", "ds.Color"); b.Lit("ds", "Fraction", Value::Float(0.5f));
        b.N("Int", "i", {{"value", Json(5)}}); b.N("Bool", "bl", {{"value", Json(true)}});
        b.N("Add", "sum"); b.MustL("i.Out", "sum.A"); b.MustL("bl.Out", "sum.B");
        b.N("MaterialOutput", "out"); b.MustL("ds.Out", "out.Emissive"); b.MustL("sum.Out", "out.Metallic");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(e.ok);
        // 0.3 > 0.5 は false → B = (0,1,0)。輝度 0.7152、Fraction 0.5 → (0.3576, 0.8576, 0.3576)
        CHECK(Near(e.surface.emissive[0], 0.3576f) && Near(e.surface.emissive[1], 0.8576f) && Near(e.surface.emissive[2], 0.3576f));
        CHECK(Near(e.surface.metallic, 6.0f));
        CHECK(Contains(r.hlsl, "bool v"));
    }

    // テクスチャ: EvalEnv::sampleTexture を渡せば評価できる。無ければ診断
    {
        GB b;
        b.N("TextureSample", "ts", {{"texture", Json("x.png")}});
        b.N("MaterialOutput", "out"); b.MustL("ts.RGB", "out.BaseColor"); b.MustL("ts.A", "out.Opacity");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        CHECK(r.ok);
        CHECK(!EvaluateCpu(r, EvalEnv{}).ok);
        EvalEnv env;
        env.uv[0] = 0.5f; env.uv[1] = 0.25f;
        int gotSlot = -1;
        env.sampleTexture = [&](int slot, const float uv[2], float bias, float out[4]) {
            gotSlot = slot;
            out[0] = uv[0]; out[1] = uv[1]; out[2] = bias; out[3] = 0.5f;
        };
        const CpuEvalResult e = EvaluateCpu(r, env);
        CHECK(e.ok && gotSlot == 0);
        CHECK(Near(e.surface.baseColor[0], 0.5f) && Near(e.surface.baseColor[1], 0.25f) && Near(e.surface.opacity, 0.5f));
    }

    // パラメータの上書きは CPU 評価にも効く（sRGB 変換込み）
    {
        GB b;
        b.N("VectorParameter", "p", {{"name", Json("Tint")}, {"default", Json::array({0.0, 0.0, 0.0, 1.0})}, {"srgb", Json(true)}});
        b.N("MaterialOutput", "out"); b.MustL("p.RGB", "out.BaseColor");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        ParamOverrides ov;
        ov["Tint"].v[0] = 0.5f; ov["Tint"].v[1] = 1.0f;
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{}, &ov);
        CHECK(Near(e.surface.baseColor[0], SrgbToLinear(0.5f)) && Near(e.surface.baseColor[1], 1.0f));
    }

    // 未接続の出力ピンは既定のサーフェス値のまま
    {
        GB b;
        b.N("MaterialOutput", "out");
        b.N("Float", "f", {{"value", Json(0.9)}});
        b.MustL("f.Out", "out.Metallic");
        const CompileResult r = CompileGraph(b.g, TestOpt());
        const CpuEvalResult e = EvaluateCpu(r, EvalEnv{});
        CHECK(e.surface.baseColor[0] == 0.5f && e.surface.roughness == 0.5f && e.surface.ao == 1.0f && !e.surface.hasNormal);
    }
}

// ---------------------------------------------------------------------------
// 6. .dxmg 往復
// ---------------------------------------------------------------------------
void TestRoundTrip()
{
    std::printf("[roundtrip]\n");
    GB b;
    b.g.SetName("往復テスト");
    b.N("TexCoord", "uv", {{"tiling", Json::array({2.0, 3.5})}});
    b.N("TextureParameter", "tp", {{"name", Json("Albedo")}, {"default", Json("textures/x.png")}});
    b.N("TextureSample", "ts"); b.MustL("tp.Out", "ts.Tex"); b.MustL("uv.UV", "ts.UV");
    b.N("ScalarParameter", "sp", {{"name", Json("Rough")}, {"default", Json(0.6)}});
    b.N("Multiply", "m"); b.MustL("ts.RGB", "m.A"); b.Lit("m", "B", Value::Float3(1.0f, 0.5f, 0.25f));
    b.N("Custom", "cu", {{"code", Json("return In0 * \"quote\" + 1;\n// 日本語 \\ backslash")}});
    b.N("Compare", "cmp", {{"mode", Json("NotEqual")}}); b.Lit("cmp", "A", Value::Float(1.0f));
    b.N("Int", "iv", {{"value", Json(7)}});
    b.N("Select", "sel"); b.MustL("cmp.Out", "sel.Cond"); b.Lit("sel", "A", Value::Float2(0.1f, 0.2f));
    b.N("MaterialOutput", "out"); b.MustL("m.Out", "out.BaseColor"); b.MustL("sp.Out", "out.Roughness"); b.Lit("out", "Metallic", Value::Float(0.1f));
    b.g.AddNode("FutureNode", -12.5f, 33.25f, "fut");   // 未知の型もそのまま往復する
    b.g.MoveNode("m", -140, 30);
    b.g.MoveNode("out", 160.5f, 40);
    Comment c; c.id = "c_11"; c.text = "岩の色"; c.color = "#3a6"; c.x = -560; c.y = -20; c.w = 380; c.h = 260;
    b.g.AddComment(c);
    GraphSettings st; st.blendMode = "Masked"; st.twoSided = true; st.maskClip = 0.35f;
    b.g.SetSettings(st);

    const std::string t1 = SaveDxmg(b.g);
    MaterialGraph g2;
    std::string err;
    CHECK_MSG(LoadDxmg(t1, g2, &err), "%s", err.c_str());
    const std::string t2 = SaveDxmg(g2);
    CHECK_MSG(t1 == t2, "往復でバイト一致しない\n--- 1 ---\n%s\n--- 2 ---\n%s", t1.c_str(), t2.c_str());
    // 3 回目も同じ
    MaterialGraph g3;
    CHECK(LoadDxmg(t2, g3, &err) && SaveDxmg(g3) == t1);

    // 構造の保存
    CHECK(g2.Nodes().size() == b.g.Nodes().size());
    CHECK(g2.Name() == "往復テスト" && g2.Guid() == "00000001");
    CHECK(g2.Settings() == st);
    CHECK(g2.Comments().size() == 1 && g2.Comments().at("c_11").text == "岩の色" && g2.Comments().at("c_11").w == 380.0f);
    CHECK(g2.FindNode("m")->x == -140.0f && g2.FindNode("out")->x == 160.5f && g2.FindNode("fut")->y == 33.25f);
    CHECK(g2.FindNode("fut")->type == "FutureNode");
    CHECK(g2.GetNodeProp("cu", "code").get<std::string>() == b.g.GetNodeProp("cu", "code").get<std::string>());
    CHECK(g2.Binding("m", "B") && g2.Binding("m", "B")->literal && g2.Binding("m", "B")->literal->type == ValueType::F3);
    CHECK(g2.Binding("ts", "Tex") && g2.Binding("ts", "Tex")->link && g2.Binding("ts", "Tex")->link->pin == "Out");
    CHECK(g2.GetNodeProp("uv", "tiling")[1].get<double>() == 3.5);
    // 型が保たれる（Int ピンではなく F1 / F2 のまま）
    CHECK(g2.Binding("sel", "A")->literal->type == ValueType::F2);

    // 見た目: 先頭のキー順と 1 ノード 1 行
    CHECK(t1.rfind("{\n  \"version\": 1,\n  \"kind\": \"material\",\n  \"guid\": \"00000001\",", 0) == 0);
    CHECK(t1.back() == '\n' && t1.find("\r") == std::string::npos);
    {
        const auto lines = SplitLines(t1);
        int nodeLines = 0;
        for (const std::string& l : lines)
            if (l.rfind("    \"", 0) == 0 && Contains(l, "\"type\": ")) ++nodeLines;
        CHECK_MSG(nodeLines == static_cast<int>(b.g.Nodes().size()), "1 ノード 1 行: %d", nodeLines);
        CHECK(Contains(t1, "\"m\": { \"type\": \"Multiply\", \"in\": { \"A\": \"ts.RGB\", \"B\": [1.0, 0.5, 0.25] } }"));
        CHECK(Contains(t1, "\"m\": \"-140 30\""));
    }

    // git 差分: 移動 = 1 行、接続の付け替え = 1 行、スロット値の編集 = 1 行
    {
        const std::string before = SaveDxmg(g2);
        g2.MoveNode("ts", 999, -1);
        const std::string afterMove = SaveDxmg(g2);
        CHECK_MSG(CountDiffLines(before, afterMove) == 1, "移動の差分行数 %d", CountDiffLines(before, afterMove));
        MaterialGraph g4;
        CHECK(LoadDxmg(afterMove, g4, &err));
        g4.SetNodeProp("sp", "default", Json(0.7));
        const std::string afterVal = SaveDxmg(g4);
        CHECK_MSG(CountDiffLines(afterMove, afterVal) == 1, "値編集の差分行数 %d", CountDiffLines(afterMove, afterVal));
        g4.Disconnect("out", "Roughness");
        const std::string afterLink = SaveDxmg(g4);
        CHECK_MSG(CountDiffLines(afterVal, afterLink) == 1, "切断の差分行数 %d", CountDiffLines(afterVal, afterLink));
    }

    // 未知ノードは診断で E_UNKNOWN_NODE（読めるし書ける）
    {
        MaterialGraph g5;
        CHECK(LoadDxmg(t1, g5, &err));
        CHECK(HasDiag(g5.Analysis().diags, code::kUnknownNode, "fut"));
    }

    // 接続の pin 省略 / 浮動小数の書式
    {
        MaterialGraph g6 = FromText(Dxmg(R"(
            "f": { "type": "Float", "props": { "value": 1 } },
            "t": { "type": "TextureSample", "in": { "MipBias": 2 } },
            "m": { "type": "Multiply", "in": { "A": "f" } })"));
        CHECK(g6.Binding("m", "A")->link->pin == "Out");
        const std::string s = SaveDxmg(g6);
        CHECK(Contains(s, "\"value\": 1.0"));
        CHECK(Contains(s, "\"MipBias\": 2.0"));
        CHECK(Contains(s, "\"A\": \"f.Out\""));
    }

    // 不正な入力
    {
        MaterialGraph g7;
        std::string e;
        CHECK(!LoadDxmg("not json", g7, &e) && !e.empty());
        CHECK(!LoadDxmg("{}", g7, &e));
        CHECK(!LoadDxmg("{ \"version\": 99, \"nodes\": {} }", g7, &e) && Contains(e, "更新"));
        CHECK(!LoadDxmg("{ \"version\": 1, \"kind\": \"function\", \"nodes\": {} }", g7, &e));
        CHECK(!LoadDxmg("{ \"version\": 1, \"nodes\": { \"a.b\": { \"type\": \"Float\" } } }", g7, &e));
        CHECK(!LoadDxmg("{ \"version\": 1, \"nodes\": { \"a\": { } } }", g7, &e));
        CHECK(!LoadDxmg("{ \"version\": 1, \"nodes\": { \"a\": { \"type\": \"Add\", \"in\": { \"A\": {} } } } }", g7, &e));
    }

    // 空のグラフ
    {
        MaterialGraph e0;
        e0.SetGuid("0"); e0.SetName("");
        const std::string s = SaveDxmg(e0);
        MaterialGraph e1;
        CHECK(LoadDxmg(s, e1, &err) && SaveDxmg(e1) == s);
    }

    // ファイル入出力: 内容が同じなら書かない
    {
        const fs::path dir = fs::temp_directory_path() / "matgraph_test_io";
        fs::create_directories(dir);
        const std::string path = (dir / "x.dxmg").string();
        fs::remove(path);
        CHECK(SaveDxmgFileIfChanged(path, b.g, &err));
        const auto mt = fs::last_write_time(path);
        CHECK(!SaveDxmgFileIfChanged(path, b.g, &err));
        CHECK(fs::last_write_time(path) == mt);
        MaterialGraph g8;
        CHECK(LoadDxmgFile(path, g8, &err) && SaveDxmg(g8) == t1);
        CHECK(!LoadDxmgFile((dir / "nothing.dxmg").string(), g8, &err));
        fs::remove_all(dir);
    }
}

// ---------------------------------------------------------------------------
// 7. ノード登録表
// ---------------------------------------------------------------------------
void TestLibrary()
{
    std::printf("[library]\n");
    const NodeLibrary& lib = NodeLibrary::Builtin();
    CHECK_MSG(lib.Size() >= 40, "ノード数 %zu", lib.Size());
    std::printf("  nodes: %zu\n", lib.Size());
    std::set<std::string> cats;
    for (const NodeDef* d : lib.List())
    {
        CHECK_MSG(!d->displayName.empty() && !d->description.empty() && !d->category.empty(), "%s", d->type.c_str());
        CHECK_MSG(!d->outputs.empty() || d->isOutput, "%s", d->type.c_str());
        bool catKnown = false;
        for (int k = 0; k < kCategoryCount; ++k) if (d->category == kCategoryOrder[k]) catKnown = true;
        CHECK_MSG(catKnown, "%s の category が未知", d->type.c_str());
        cats.insert(d->category);
        // ピン名 / 出力名の一意性
        std::set<std::string> names;
        for (const PinDecl& p : d->inputs) CHECK_MSG(names.insert(p.name).second, "%s: 入力名 %s が重複", d->type.c_str(), p.name.c_str());
        names.clear();
        for (const OutPinDecl& o : d->outputs) CHECK_MSG(names.insert(o.name).second, "%s: 出力名 %s が重複", d->type.c_str(), o.name.c_str());
        // HLSL の作り方がある / CPU 評価がある（Custom / TextureParameter / Reroute / 出力ノードを除く）
        if (!d->isOutput && !d->isTransparent)
            CHECK_MSG(!d->hlsl.empty() || static_cast<bool>(d->emit) || d->isCustom, "%s: hlsl も emit も無い", d->type.c_str());
        if (!d->isOutput && !d->isTransparent && !d->isCustom && d->type != "TextureParameter")
            CHECK_MSG(static_cast<bool>(d->eval), "%s: CPU 評価が無い", d->type.c_str());
        // 各プロパティの既定値は自分の型で正規化できる
        for (const PropDecl& pd : d->props)
        {
            Json out;
            CHECK_MSG(NormalizeProp(pd, pd.defaultValue, out), "%s.%s の既定値が不正", d->type.c_str(), pd.name.c_str());
        }
        // 型 ID は英数字のみ
        for (unsigned char ch : d->type) CHECK(std::isalnum(ch));
    }
    CHECK(cats.size() >= 8);
    CHECK(lib.Find("MaterialOutput") && lib.Find("MaterialOutput")->isOutput);
    CHECK(lib.Find("NoSuchNode") == nullptr);
    CHECK(!lib.Search("フレネル").empty() && lib.Search("フレネル")[0]->type == "Fresnel");
    CHECK(!lib.Search("lerp").empty());
    CHECK(lib.Search("").size() == lib.Size());
    // 全ノードを 1 個ずつ置いて既定のまま解析してもクラッシュしない
    MaterialGraph g;
    for (const NodeDef* d : lib.List()) g.AddNode(d->type, 0, 0, d->type);
    (void)g.Analysis();
    (void)CompileGraph(g, TestOpt());
}

// ---------------------------------------------------------------------------
// 8. ゴールデン
// ---------------------------------------------------------------------------
std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}
std::string Lf(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
void WriteFileBytes(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void TestGolden()
{
    std::printf("[golden]\n");
    const fs::path dir = DX12E_MATGRAPH_DATA_DIR;
    const bool update = std::getenv("MATGRAPH_UPDATE_GOLDEN") != nullptr;
    std::vector<fs::path> files;
    if (fs::exists(dir))
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".dxmg") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK_MSG(files.size() >= 10, "ゴールデンの .dxmg が 10 本ない (%zu)", files.size());
    for (const fs::path& f : files)
    {
        const std::string name = f.stem().string();
        MaterialGraph g;
        std::string err;
        const std::string text = Lf(ReadFile(f));
        CHECK_MSG(LoadDxmg(text, g, &err), "%s: %s", name.c_str(), err.c_str());
        const std::string canon = SaveDxmg(g);
        CompileOptions opt = TestOpt();
        const CompileResult r = CompileGraph(g, opt);
        CHECK_MSG(r.ok && !r.HasErrors(), "%s: コンパイル失敗", name.c_str());
        for (const Diagnostic& d : r.diagnostics)
            if (d.severity == Severity::Error && d.reachable) std::printf("  %s: %s [%s.%s] %s\n", name.c_str(), d.code.c_str(), d.nodeId.c_str(), d.pin.c_str(), d.message.c_str());
        const fs::path expected = dir / (name + ".hlsl.expected");
        if (update)
        {
            WriteFileBytes(f, canon);
            WriteFileBytes(expected, r.hlsl);
            std::printf("  updated %s\n", name.c_str());
            continue;
        }
        CHECK_MSG(canon == text, "%s.dxmg が正準形ではない（MATGRAPH_UPDATE_GOLDEN=1 で書き直す）", name.c_str());
        CHECK_MSG(fs::exists(expected), "%s.hlsl.expected が無い", name.c_str());
        const std::string want = Lf(ReadFile(expected));
        if (r.hlsl != want)
        {
            ++g_failures;
            std::printf("FAIL golden %s: 生成 HLSL が期待と違う\n", name.c_str());
            const auto a = SplitLines(want), c = SplitLines(r.hlsl);
            for (size_t i = 0; i < std::max(a.size(), c.size()); ++i)
            {
                const std::string x = i < a.size() ? a[i] : "<EOF>", y = i < c.size() ? c[i] : "<EOF>";
                if (x != y) { std::printf("  line %zu\n    expected: %s\n    got     : %s\n", i + 1, x.c_str(), y.c_str()); break; }
            }
        }
        // 決定論: もう一度読み直して同じ
        MaterialGraph g2;
        CHECK(LoadDxmg(canon, g2, &err));
        CHECK(CompileGraph(g2, opt).hlsl == r.hlsl);
    }
}

int Cli(const char* path)
{
    MaterialGraph g;
    std::string err;
    if (!LoadDxmgFile(path, g, &err)) { std::printf("読み込み失敗: %s\n", err.c_str()); return 2; }
    const CompileResult r = CompileGraph(g);
    for (const Diagnostic& d : r.diagnostics)
        std::printf("%s %s [%s.%s]%s %s\n", d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "info",
                    d.code.c_str(), d.nodeId.c_str(), d.pin.c_str(), d.reachable ? "" : " (dead)", d.message.c_str());
    std::printf("%s", r.hlsl.c_str());
    std::printf("// slots=%d ops=%d cse=%d ok=%d\n", r.slotCount, r.stats.ops, r.stats.cseHits, r.ok ? 1 : 0);
    return r.ok ? 0 : 1;
}

} // namespace

// docs/MATGRAPH_FORMAT.md のノード一覧の元（登録表から Markdown の表を出す）
int ListNodes()
{
    const NodeLibrary& lib = NodeLibrary::Builtin();
    auto pinText = [](const PinDecl& p) {
        std::string t = p.name + ":";
        if (p.mode == PinMode::Fixed) t += TypeName(p.type);
        else if (p.mode == PinMode::Poly) t += "float(1-4)";
        else t += p.allowTexture ? "any" : "float(1-4)";
        if (p.reserved) t += "(予約)";
        else if (p.required) t += "*";
        return t;
    };
    std::printf("| 型 ID | カテゴリ | 入力（* = 必須） | 出力 | 説明 |\n|---|---|---|---|---|\n");
    for (const NodeDef* d : lib.List())
    {
        std::string in, out;
        for (const PinDecl& p : d->inputs) { if (!in.empty()) in += ", "; in += pinText(p); }
        for (const OutPinDecl& o : d->outputs)
        {
            if (!out.empty()) out += ", ";
            out += o.name;
            if (o.mode == PinMode::Fixed) { out += ":"; out += TypeName(o.type); }
            else out += ":float(1-4)";
        }
        std::printf("| %s | %s | %s | %s | %s |\n", d->type.c_str(), d->category.c_str(), in.c_str(), out.c_str(), d->description.c_str());
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--list-nodes") return ListNodes();
    if (argc >= 3 && std::string(argv[1]) == "--compile") return Cli(argv[2]);
    TestTypes();
    TestModel();
    TestDiagnostics();
    TestCodegen();
    TestSlots();
    TestCustom();
    TestCpuEval();
    TestRoundTrip();
    TestLibrary();
    TestGolden();
    std::printf("matgraph: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
