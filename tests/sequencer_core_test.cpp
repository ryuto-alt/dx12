// シーケンサー(.dxseq)の純ロジック(src/sequencer/)の単体テスト。GPU / ECS / ImGui に触れない。
// 補間の数値・クォータニオン・評価の決定論・イベント発火・カット・SeqOp(Undo/Redo ファズ)・シリアライズ往復・
// 旧 sequence_author 台本の変換・バインディング解決・評価性能。
//
// ゴールデンの更新: 環境変数 SEQUENCER_UPDATE_GOLDEN=1 で実行 → tests/data/sequencer/*.dxseq を書き直す(git diff を目視すること)。
// 実行: ctest --output-on-failure -R SequencerCoreTests

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "sequencer/SeqBinding.h"
#include "sequencer/SeqConvert.h"
#include "sequencer/SeqCurve.h"
#include "sequencer/SeqEval.h"
#include "sequencer/SeqModel.h"
#include "sequencer/SeqOps.h"
#include "sequencer/SeqSerialize.h"
#include "sequencer/SeqTypes.h"
#include "ui/UiEase.h"

using namespace dx12e::seq;

namespace
{
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failures; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_MSG(cond, ...)                                                           \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                                  \
            std::printf("\n");                                                         \
        }                                                                              \
    } while (0)

bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol * (1.0 + std::fabs(a) + std::fabs(b)); }

// 決定的な乱数(SplitMix64。処理系に依存しない)
struct Rng
{
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t Next()
    {
        s += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int R(int n) { return static_cast<int>(Next() % static_cast<std::uint64_t>(n)); }   // 0..n-1
    bool P(int percent) { return R(100) < percent; }
    double U() { return static_cast<double>(Next() >> 11) / 9007199254740992.0; }        // [0,1)
};

std::string Ser(const Sequence& s) { return SerializeSequence(s); }
std::uint64_t Hash(const Sequence& s) { return Fnv1a64(Ser(s)); }

// ---------------------------------------------------------------------------
// 組み立てヘルパ
// ---------------------------------------------------------------------------
Key K(Tick t, double v, Interp ip = Interp::Auto)
{
    Key k;
    k.t = t; k.v = v; k.ip = ip;
    return k;
}
Key KE(Tick t, double v, EaseId e)
{
    Key k;
    k.t = t; k.v = v; k.ip = Interp::Ease; k.ease = e;
    return k;
}
Key KB(Tick t, double v, Tick inDt, double inDv, Tick outDt, double outDv)
{
    Key k;
    k.t = t; k.v = v; k.ip = Interp::Bezier;
    k.inDt = inDt; k.inDv = inDv; k.outDt = outDt; k.outDv = outDv;
    return k;
}
Channel Ch(const std::string& name, std::vector<Key> keys, Extrap pre = Extrap::Hold, Extrap post = Extrap::Hold)
{
    Channel c;
    c.name = name; c.keys = std::move(keys); c.pre = pre; c.post = post;
    return c;
}
Track Trk(const std::string& id, TrackType type)
{
    Track t;
    t.id = id; t.type = type; t.params = SeqValue::MakeObject();
    return t;
}
Binding Bnd(const std::string& id, const std::string& name, BindingKind kind = BindingKind::Entity)
{
    Binding b;
    b.id = id; b.name = name; b.kind = kind; b.hint.name = name; b.params = SeqValue::MakeObject();
    return b;
}
Clip Clp(const std::string& id, Tick start, Tick dur)
{
    Clip c;
    c.id = id; c.start = start; c.dur = dur; c.params = SeqValue::MakeObject();
    return c;
}
EventItem Evt(const std::string& id, Tick t, const std::string& kind = "emit", const std::string& name = "ev")
{
    EventItem e;
    e.id = id; e.t = t; e.kind = kind; e.name = name; e.params = SeqValue::MakeObject();
    return e;
}
SeqValue Num(double v) { return SeqValue::MakeNumber(v); }
SeqValue Str(const std::string& s) { return SeqValue::MakeString(s); }

Sequence NewSeq(const std::string& name = "Test")
{
    Sequence s;
    s.id = "q_test0001";
    s.name = name;
    s.meta = SeqValue::MakeObject();
    return s;
}

double Eval1(const Channel& ch, Tick t, bool srgb = false) { return EvalChannel(ch, t, srgb); }

// 度・軸からクォータニオン
Quat QAxis(double x, double y, double z, double deg)
{
    const double n = std::sqrt(x * x + y * y + z * z);
    const double h = deg * 3.14159265358979323846 / 360.0;
    const double s = std::sin(h) / n;
    return { x * s, y * s, z * s, std::cos(h) };
}
Track QuatTrack(const std::string& id, const std::vector<std::pair<Tick, Quat>>& keys, Interp ip = Interp::Linear,
                Extrap post = Extrap::Hold)
{
    Track t = Trk(id, TrackType::Transform);
    t.rotation = RotationMode::Quat;
    static const char* names[4] = { "rotation.x", "rotation.y", "rotation.z", "rotation.w" };
    for (int c = 0; c < 4; ++c)
    {
        Channel ch;
        ch.name = names[c];
        ch.post = post;
        for (const auto& k : keys)
        {
            const double comp[4] = { k.second.x, k.second.y, k.second.z, k.second.w };
            ch.keys.push_back(K(k.first, comp[c], ip));
        }
        t.channels.push_back(std::move(ch));
    }
    return t;
}
// 2 つのクォータニオンの成分距離(q と -q は同一視)。acos は 1 付近で条件が悪いので QuatAngle は近さの判定に使わない。
double QDist(const Quat& a, const Quat& b)
{
    const double d1 = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z) + (a.w - b.w) * (a.w - b.w));
    const double d2 = std::sqrt((a.x + b.x) * (a.x + b.x) + (a.y + b.y) * (a.y + b.y) + (a.z + b.z) * (a.z + b.z) + (a.w + b.w) * (a.w + b.w));
    return std::min(d1, d2);
}
Quat EvalQ(const Track& t, Tick tick)
{
    return EvalQuatChannels(t.channels[0], t.channels[1], t.channels[2], t.channels[3], tick);
}

// ===========================================================================
// 1. 時間・ID
// ===========================================================================
void TestTime()
{
    std::printf("TestTime\n");
    CHECK(SecondsToTicks(0.5, 6000) == 3000);
    CHECK(SecondsToTicks(1.0 / 30.0, 6000) == 200);
    CHECK(SecondsToTicks(-0.5, 6000) == -3000);
    CHECK(SecondsToTicks(0.25, 2) == 1);            // 0.5 tick → 0 から遠い側へ
    CHECK(SecondsToTicks(-0.25, 2) == -1);
    CHECK(SecondsToTicks(std::nan(""), 6000) == 0);
    CHECK(SecondsToTicks(1e30, 6000) == kMaxTick);
    CHECK(Near(TicksToSeconds(3000, 6000), 0.5));

    const int fpsList[] = { 24, 25, 30, 48, 50, 60, 100, 120 };
    for (int fps : fpsList)
    {
        CHECK(FrameRateIsExact(fps, 6000));
        for (std::int64_t f : { -5, 0, 1, 7, 1234 })
        {
            const Tick t = FrameToTick(f, fps, 6000);
            CHECK_MSG(t * fps == f * 6000, "fps=%d f=%lld", fps, static_cast<long long>(f));
            CHECK(TickToFrameFloor(t, fps, 6000) == f);
            CHECK(TickToFrameNearest(t, fps, 6000) == f);
            CHECK(TickToFrameFloor(t - 1, fps, 6000) == f - 1);          // 直前はひとつ前のフレーム
            CHECK(TickToFrameNearest(t + 6000 / fps / 2 - 1, fps, 6000) == f);
        }
    }
    CHECK(!FrameRateIsExact(7, 6000));
    CHECK(FrameToTick(1, 7, 6000) == 857);       // 857.14
    CHECK(FrameToTick(4, 7, 6000) == 3429);      // 3428.57
    CHECK(FrameToTick(1, 2, 3) == 2);            // 1.5 → half-up
    CHECK(FrameToTick(-1, 2, 3) == -1);          // -1.5 → half-up(+∞ 方向)
    CHECK(TickToFrameNearest(1, 2, 3) == 1);
    CHECK(SnapToFrame(3010, 30, 6000) == 3000);
    CHECK(SnapToFrame(3110, 30, 6000) == 3200);  // 15.55 → 16
    CHECK(FloorDiv(-1, 6000) == -1 && FloorMod(-1, 6000) == 5999);
    CHECK(FloorDiv(-6000, 6000) == -1 && FloorMod(-6000, 6000) == 0);
    CHECK(FloorDiv(6001, 6000) == 1 && FloorMod(6001, 6000) == 1);
}

void TestIds()
{
    std::printf("TestIds\n");
    CHECK(FormatId('t', 0x7f3a91c2u) == "t_7f3a91c2");
    CHECK(FormatId('b', 0) == "b_00000000");
    CHECK(IsValidId("c_01") && IsValidId("t_7f3a91c2") && IsValidId("Boss-1.a"));
    CHECK(!IsValidId("") && !IsValidId("a b") && !IsValidId("日本語") && !IsValidId(std::string(41, 'a')));

    IdAllocator a(123), b(123);
    std::vector<std::string> la, lb;
    for (int i = 0; i < 200; ++i) { la.push_back(a.New('t')); lb.push_back(b.New('t')); }
    CHECK(la == lb);                                     // 同じ seed → 同じ列(決定論)
    std::set<std::string> uniq(la.begin(), la.end());
    CHECK(uniq.size() == la.size());                     // 衝突なし
    IdAllocator c(123);
    c.Reserve(la[0]);
    c.Reserve(la[1]);
    const std::string first = c.New('t');
    CHECK(first != la[0] && first != la[1]);             // 予約済みは避ける
    IdAllocator d(7);
    CHECK(d.New('k') != IdAllocator(8).New('k'));
    CHECK(Fnv1a64("") == 0xCBF29CE484222325ull);
}

// ===========================================================================
// 2. イージング統一表
// ===========================================================================
void TestEase()
{
    std::printf("TestEase\n");
    const int n = static_cast<int>(EaseId::Count);
    CHECK(n == 15);
    std::set<std::string> names;
    for (int i = 0; i < n; ++i)
    {
        const EaseId id = static_cast<EaseId>(i);
        names.insert(EaseName(id));
        CHECK(EvalEase(id, 0.0) == 0.0 && EvalEase(id, 1.0) == 1.0);       // 端点は厳密
        CHECK(EvalEase(id, -1.0) == 0.0 && EvalEase(id, 2.0) == 1.0);      // clamp
        EaseId back = EaseId::Linear;
        CHECK(EaseFromName(EaseName(id), back) && back == id);             // 名前の往復
    }
    CHECK(names.size() == static_cast<std::size_t>(n));

    // 単調(オーバーシュートしない)種別
    const EaseId mono[] = { EaseId::Linear, EaseId::InQuad, EaseId::OutQuad, EaseId::InOutQuad, EaseId::InCubic, EaseId::OutCubic,
                            EaseId::InOutCubic, EaseId::InOutSine, EaseId::OutExpo, EaseId::OutQuint };
    for (EaseId id : mono)
    {
        double prev = 0.0;
        bool ok = true;
        for (int i = 1; i <= 1000; ++i)
        {
            const double v = EvalEase(id, i / 1000.0);
            if (v < prev - 1e-15 || v < 0.0 || v > 1.0 + 1e-15) ok = false;
            prev = v;
        }
        CHECK_MSG(ok, "%s は単調でない", EaseName(id));
    }
    // 既知値
    CHECK(Near(EvalEase(EaseId::InQuad, 0.5), 0.25));
    CHECK(Near(EvalEase(EaseId::OutQuad, 0.5), 0.75));
    CHECK(Near(EvalEase(EaseId::InOutQuad, 0.25), 0.125));
    CHECK(Near(EvalEase(EaseId::InCubic, 0.5), 0.125));
    CHECK(Near(EvalEase(EaseId::OutCubic, 0.5), 0.875));
    CHECK(Near(EvalEase(EaseId::InOutSine, 0.5), 0.5));
    CHECK(EvalEase(EaseId::OutBack, 0.8) > 1.0);         // 行き過ぎる
    CHECK(EvalEase(EaseId::InBack, 0.2) < 0.0);          // 溜める
    CHECK(Near(EvalEase(EaseId::OutBounce, 0.5), 0.765625));

    // ★C++ UiEase(src/ui/UiEase.h)の 12 種と全 p で一致(float 実装との差 ≤ 2e-6)
    int uiCount = 0;
    for (int ui = 0; ui < dx12e::kUiEaseCount; ++ui)
    {
        EaseId id = EaseId::Linear;
        CHECK_MSG(EaseFromUiIndex(ui, id), "UiEase %d が統一表に無い", ui);
        if (!EaseFromUiIndex(ui, id)) continue;
        CHECK(UiIndexOf(id) == ui);
        ++uiCount;
        double worst = 0.0;
        for (int i = 0; i <= 1000; ++i)
        {
            const double p = i / 1000.0;
            worst = std::max(worst, std::fabs(static_cast<double>(dx12e::UiEase(ui, static_cast<float>(p))) - EvalEase(id, p)));
        }
        CHECK_MSG(worst < 2e-6, "UiEase %d (%s) の差 %.3g", ui, EaseName(id), worst);
    }
    CHECK(uiCount == 12);

    // ★Lua Ease テーブル(ScriptEngine.cpp prelude)の式を C++ に写して一致を見る(9 種)
    const double pi = 3.14159265358979323846;
    auto lua = [&](const char* name, double t) -> double {
        const std::string n = name;
        if (n == "linear") return t;
        if (n == "inQuad") return t * t;
        if (n == "outQuad") return 1 - (1 - t) * (1 - t);
        if (n == "inOutQuad") return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
        if (n == "inCubic") return t * t * t;
        if (n == "outCubic") return 1 - std::pow(1 - t, 3);
        if (n == "inOutSine") return -(std::cos(pi * t) - 1) / 2;
        if (n == "outBack") { const double c1 = 1.70158; return 1 + (c1 + 1) * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2); }
        if (n == "outBounce")
        {
            const double nn = 7.5625, d = 2.75;
            if (t < 1 / d) return nn * t * t;
            if (t < 2 / d) { t -= 1.5 / d; return nn * t * t + 0.75; }
            if (t < 2.5 / d) { t -= 2.25 / d; return nn * t * t + 0.9375; }
            t -= 2.625 / d; return nn * t * t + 0.984375;
        }
        return -1;
    };
    const char* luaNames[] = { "linear", "inQuad", "outQuad", "inOutQuad", "inCubic", "outCubic", "inOutSine", "outBack", "outBounce" };
    int luaCount = 0;
    for (const char* ln : luaNames)
    {
        EaseId id = EaseId::Linear;
        CHECK(EaseFromName(ln, id));
        CHECK(GetEaseInfo(id).inLua);
        ++luaCount;
        double worst = 0.0;
        for (int i = 1; i < 1000; ++i) worst = std::max(worst, std::fabs(lua(ln, i / 1000.0) - EvalEase(id, i / 1000.0)));
        CHECK_MSG(worst < 1e-12, "Lua %s の差 %.3g", ln, worst);
    }
    CHECK(luaCount == 9);
    int inLuaFlags = 0;
    for (int i = 0; i < n; ++i) if (GetEaseInfo(static_cast<EaseId>(i)).inLua) ++inLuaFlags;
    CHECK(inLuaFlags == 9);

    // ★sequence.ts の 6 種(in/out/inOut は二次)
    const char* ts[] = { "linear", "in", "out", "inOut", "outBack", "outBounce" };
    const char* canon[] = { "linear", "inQuad", "outQuad", "inOutQuad", "outBack", "outBounce" };
    for (int i = 0; i < 6; ++i)
    {
        EaseId a = EaseId::Linear, b = EaseId::Linear;
        CHECK(EaseFromName(ts[i], a) && EaseFromName(canon[i], b) && a == b);
        CHECK(GetEaseInfo(b).legacyTs != nullptr && std::string(GetEaseInfo(b).legacyTs) == ts[i]);
    }
    for (int i = 0; i <= 100; ++i)
    {
        const double k = i / 100.0;
        CHECK(Near(EvalEase(EaseId::InQuad, k), k * k, 1e-15));                                     // TS: in
        CHECK(Near(EvalEase(EaseId::OutQuad, k), 1 - (1 - k) * (1 - k), 1e-15));                    // TS: out
        CHECK(Near(EvalEase(EaseId::InOutQuad, k), k < 0.5 ? 2 * k * k : 1 - std::pow(-2 * k + 2, 2) / 2, 1e-15));
    }
    EaseId dummy = EaseId::Linear;
    CHECK(!EaseFromName("nope", dummy) && !EaseFromUiIndex(99, dummy));
}

// ===========================================================================
// 3. スカラーカーブ
// ===========================================================================
void TestCurveBasics()
{
    std::printf("TestCurveBasics\n");
    // 単一キー・端のホールド
    Channel one = Ch("v", { K(100, 7.5) });
    CHECK(Eval1(one, -1000) == 7.5 && Eval1(one, 100) == 7.5 && Eval1(one, 99999) == 7.5);

    // Step
    Channel st = Ch("v", { K(0, 1, Interp::Step), K(100, 5, Interp::Step), K(200, 9, Interp::Step) });
    CHECK(Eval1(st, -5) == 1 && Eval1(st, 0) == 1 && Eval1(st, 99) == 1 && Eval1(st, 100) == 5 && Eval1(st, 199) == 5 &&
          Eval1(st, 200) == 9 && Eval1(st, 5000) == 9);

    // Linear
    Channel li = Ch("v", { K(0, 0, Interp::Linear), K(100, 10, Interp::Linear), K(300, 30, Interp::Linear) });
    CHECK(Near(Eval1(li, 25), 2.5) && Near(Eval1(li, 100), 10) && Near(Eval1(li, 200), 20) && Near(Eval1(li, 300), 30));
    CHECK(Eval1(li, -50) == 0 && Eval1(li, 400) == 30);          // ホールド

    // Ease: v0 + (v1-v0) * E(p)
    Channel ea = Ch("v", { KE(0, 10, EaseId::InOutQuad), K(400, 20, Interp::Step) });
    CHECK(Near(Eval1(ea, 100), 10 + 10 * EvalEase(EaseId::InOutQuad, 0.25)));
    CHECK(Near(Eval1(ea, 300), 10 + 10 * EvalEase(EaseId::InOutQuad, 0.75)));
    CHECK(Eval1(ea, 400) == 20);

    // 同時刻キーは後ろが勝つ(ちょうどその時刻でも)
    Channel dup = Ch("v", { K(0, 0, Interp::Linear), K(100, 10, Interp::Step), K(100, 50, Interp::Linear), K(200, 60, Interp::Linear) });
    CHECK(Eval1(dup, 99) < 10.0 && Near(Eval1(dup, 99), 9.9));
    CHECK(Eval1(dup, 100) == 50);                                // 後ろのキー
    CHECK(Near(Eval1(dup, 150), 55));

    // 空チャンネルは 0(評価器は空を出力しないが、関数としては安全)
    CHECK(EvalChannel(Channel{}, 5) == 0.0);
}

void TestCurveAuto()
{
    std::printf("TestCurveAuto\n");
    // 通過性: キー時刻ちょうどでキー値
    Channel a = Ch("v", { K(0, 0), K(100, 1), K(250, 1.5), K(400, 4), K(700, 10) });
    for (const Key& k : a.keys) CHECK(Near(Eval1(a, k.t), k.v, 1e-12));

    // 単調データ: 単調増加かつ各区間で [v_i, v_{i+1}] を出ない(オーバーシュート無し)
    bool mono = true, bounded = true;
    double prev = -1e9;
    for (Tick t = 0; t <= 700; ++t)
    {
        const double v = Eval1(a, t);
        if (v < prev - 1e-12) mono = false;
        prev = v;
        for (std::size_t i = 0; i + 1 < a.keys.size(); ++i)
            if (t >= a.keys[i].t && t <= a.keys[i + 1].t && (v < a.keys[i].v - 1e-12 || v > a.keys[i + 1].v + 1e-12)) bounded = false;
    }
    CHECK(mono);
    CHECK(bounded);

    // 極値: 0,10,0 は中央の接線が 0 → 10 を超えない
    Channel pk = Ch("v", { K(0, 0), K(100, 10), K(200, 0) });
    CHECK(AutoTangent(pk.keys, 1) == 0.0);
    double mx = -1e9;
    for (Tick t = 0; t <= 200; ++t) mx = std::max(mx, Eval1(pk, t));
    CHECK(mx <= 10.0 + 1e-12);

    // 平坦(5,5): 両端キーの接線 0 → 完全に平坦
    Channel plateau = Ch("v", { K(0, 0), K(100, 5), K(200, 5), K(300, 10) });
    bool flat = true;
    for (Tick t = 100; t <= 200; ++t) if (!Near(Eval1(plateau, t), 5.0, 1e-12)) flat = false;
    CHECK(flat);

    // 一直線は一直線のまま(Hermite が線形を再現)
    Channel line = Ch("v", { K(0, 0), K(100, 10), K(300, 30) });
    bool lineOk = true;
    for (Tick t = 0; t <= 300; t += 7) if (!Near(Eval1(line, t), 0.1 * static_cast<double>(t), 1e-9)) lineOk = false;
    CHECK(lineOk);
    CHECK(Near(AutoTangent(line.keys, 0), 0.1) && Near(AutoTangent(line.keys, 1), 0.1) && Near(AutoTangent(line.keys, 2), 0.1));

    // C1 連続: キーの前後 1 tick の傾きが近い
    Channel c1 = Ch("v", { K(0, 0), K(1000, 3), K(2000, 4) });
    const double sl = Eval1(c1, 1000) - Eval1(c1, 999), sr = Eval1(c1, 1001) - Eval1(c1, 1000);
    CHECK(std::fabs(sl - sr) < 0.02 * std::fabs(sl) + 1e-9);

    // 色: 制限つき接線でも |m| ≤ 3*min(割線)
    Channel steep = Ch("v", { K(0, 0), K(10, 100), K(1000, 101) });
    const double m1 = AutoTangent(steep.keys, 1);
    CHECK(m1 <= 3.0 * (101.0 - 100.0) / 990.0 + 1e-12 && m1 >= 0.0);
}

void TestCurveBezier()
{
    std::printf("TestCurveBezier\n");
    // ★ハンドルを Auto の Hermite 接線の 1/3 に置いた Bezier は Auto と同一(区間幅は 3 の倍数)
    Channel autoCh = Ch("v", { K(0, 0), K(300, 6), K(900, 2), K(1200, 2.5) });
    Channel bez = autoCh;
    for (std::size_t i = 0; i < bez.keys.size(); ++i)
    {
        Key& k = bez.keys[i];
        const double m = AutoTangent(autoCh.keys, i);
        k.ip = Interp::Bezier;
        k.inDt = (i > 0) ? (autoCh.keys[i].t - autoCh.keys[i - 1].t) / 3 : 0;
        k.outDt = (i + 1 < bez.keys.size()) ? (autoCh.keys[i + 1].t - autoCh.keys[i].t) / 3 : 0;
        k.inDv = m * static_cast<double>(k.inDt);
        k.outDv = m * static_cast<double>(k.outDt);
    }
    double worst = 0.0;
    for (Tick t = 0; t <= 1200; ++t) worst = std::max(worst, std::fabs(Eval1(bez, t) - Eval1(autoCh, t)));
    CHECK_MSG(worst < 1e-9, "Bezier(1/3 ハンドル)と Auto の最大差 %.3g", worst);

    // 端点ちょうど・単調・有限
    Channel b2 = Ch("v", { KB(0, 0, 0, 0, 200, 0), KB(1000, 10, 200, 0, 0, 0) });   // ease-in-out 風(水平ハンドル)
    CHECK(Eval1(b2, 0) == 0 && Eval1(b2, 1000) == 10);
    double prev = -1;
    bool mono = true, finite = true;
    for (Tick t = 0; t <= 1000; ++t)
    {
        const double v = Eval1(b2, t);
        if (v < prev - 1e-12) mono = false;
        if (!std::isfinite(v) || v < -1e-9 || v > 10 + 1e-9) finite = false;
        prev = v;
    }
    CHECK(mono && finite);
    CHECK(Near(Eval1(b2, 500), 5.0, 1e-9));                                            // 対称
    CHECK(Eval1(b2, 100) < 1.0);                                                        // ゆっくり立ち上がる

    // 過大なハンドルは x が単調になるよう制限される(NaN/逆転しない)
    Channel b3 = Ch("v", { KB(0, 0, 0, 0, 50000, 100), KB(1000, 10, 50000, -100, 0, 0) });
    prev = -1e18;
    bool ok = true;
    for (Tick t = 0; t <= 1000; t += 5)
    {
        const double v = Eval1(b3, t);
        if (!std::isfinite(v)) ok = false;
    }
    (void)prev;
    CHECK(ok);
    CHECK(Eval1(b3, 0) == 0 && Eval1(b3, 1000) == 10);

    // Bezier の次のキーが Bezier でないとき、入りハンドルは既定(Auto の 1/3)になる: 2 キーの線形データでは直線
    Channel b4 = Ch("v", { KB(0, 0, 0, 0, 100, 10), K(300, 30, Interp::Linear) });   // 傾き 0.1 のハンドル
    bool line = true;
    for (Tick t = 0; t <= 300; t += 3) if (!Near(Eval1(b4, t), 0.1 * static_cast<double>(t), 1e-9)) line = false;
    CHECK(line);
}

void TestCurveExtrapAndColor()
{
    std::printf("TestCurveExtrapAndColor\n");
    Channel base = Ch("v", { K(100, 0, Interp::Linear), K(200, 10, Interp::Linear) });
    // Hold
    CHECK(Eval1(base, 0) == 0 && Eval1(base, 300) == 10);
    // Linear(端の傾き 0.1 で延長)
    Channel lin = base; lin.pre = Extrap::Linear; lin.post = Extrap::Linear;
    CHECK(Near(Eval1(lin, 0), -10) && Near(Eval1(lin, 300), 20) && Near(Eval1(lin, 150), 5));
    // Loop
    Channel lp = base; lp.pre = Extrap::Loop; lp.post = Extrap::Loop;
    CHECK(Near(Eval1(lp, 250), 5) && Near(Eval1(lp, 350), 5) && Near(Eval1(lp, 200), 10) && Near(Eval1(lp, 300), 0));
    CHECK(Near(Eval1(lp, 50), 5) && Near(Eval1(lp, 0), 0));
    // 周期性: v(t + span) == v(t)
    bool periodic = true;
    for (Tick t = 101; t < 200; t += 7) if (!Near(Eval1(lp, t), Eval1(lp, t + 100)) || !Near(Eval1(lp, t), Eval1(lp, t - 100))) periodic = false;
    CHECK(periodic);
    // PingPong
    Channel pp = base; pp.pre = Extrap::PingPong; pp.post = Extrap::PingPong;
    CHECK(Near(Eval1(pp, 250), 5) && Near(Eval1(pp, 210), 9) && Near(Eval1(pp, 300), 0) && Near(Eval1(pp, 310), 1));
    CHECK(Near(Eval1(pp, 90), 1) && Near(Eval1(pp, 50), 5));
    // pre と post を別々に
    Channel mix = base; mix.pre = Extrap::Linear; mix.post = Extrap::Hold;
    CHECK(Near(Eval1(mix, 0), -10) && Eval1(mix, 1000) == 10);
    // Auto / Step の端の延長
    Channel au = Ch("v", { K(0, 0), K(100, 10), K(200, 30) }, Extrap::Linear, Extrap::Linear);
    CHECK(Near(Eval1(au, -100), -10) && Near(Eval1(au, 300), 50));
    Channel stp = Ch("v", { K(0, 1, Interp::Step), K(100, 5, Interp::Step) }, Extrap::Linear, Extrap::Linear);
    CHECK(Eval1(stp, -50) == 1 && Eval1(stp, 500) == 5);

    // WrapTimeForExtrap
    CHECK(WrapTimeForExtrap(Extrap::Loop, 250, 100, 200) == 150);
    CHECK(WrapTimeForExtrap(Extrap::PingPong, 250, 100, 200) == 150);
    CHECK(WrapTimeForExtrap(Extrap::Hold, 250, 100, 200) == 250);
    CHECK(WrapTimeForExtrap(Extrap::Loop, 250, 100, 100) == 100);          // 幅 0 は先頭

    // sRGB: キー値を sRGB とみなしリニア空間で補間して、リニアで返す
    Channel c = Ch("r", { K(0, 0.2, Interp::Linear), K(100, 0.8, Interp::Linear) });
    CHECK(Near(Eval1(c, 50, false), 0.5));
    CHECK(Near(Eval1(c, 50, true), 0.5 * (SrgbToLinear(0.2) + SrgbToLinear(0.8))));
    CHECK(Eval1(c, 50, true) < 0.5);                                       // リニアの平均は sRGB の中点より暗い
    CHECK(Near(Eval1(c, 0, true), SrgbToLinear(0.2)) && Near(Eval1(c, 100, true), SrgbToLinear(0.8)));
    for (int i = 0; i <= 100; ++i)
    {
        const double v = i / 100.0;
        CHECK(Near(LinearToSrgb(SrgbToLinear(v)), v, 1e-12));
    }
    CHECK(SrgbToLinear(0.0) == 0.0 && Near(SrgbToLinear(1.0), 1.0, 1e-12));
    CHECK(ChannelIsAlpha("a") && ChannelIsAlpha("color.a") && !ChannelIsAlpha("color.r") && !ChannelIsAlpha("alpha"));
    CHECK(Near(UnwrapDegrees(350, 10), 370) && Near(UnwrapDegrees(10, 350), -10) && Near(UnwrapDegrees(0, 90), 90));

    // Evaluate: Property/Color/srgb のトラックで r,g,b は変換され a は変換されない
    Sequence s = NewSeq();
    Binding b = Bnd("b_l", "Lamp");
    Track t = Trk("t_c", TrackType::Property);
    t.path = "PointLight.color";
    t.valueType = ValueType::Color;
    t.colorSpace = ColorSpace::Srgb;
    t.channels.push_back(Ch("r", { K(0, 0.2, Interp::Linear), K(100, 0.8, Interp::Linear) }));
    t.channels.push_back(Ch("a", { K(0, 0.2, Interp::Linear), K(100, 0.8, Interp::Linear) }));
    b.tracks.push_back(t);
    s.bindings.push_back(b);
    EvalResult r = Evaluate(s, 50);
    CHECK(r.channels.size() == 2);
    CHECK(Near(r.channels[0].value, 0.5 * (SrgbToLinear(0.2) + SrgbToLinear(0.8))) && Near(r.channels[1].value, 0.5));
}

// ===========================================================================
// 4. クォータニオン
// ===========================================================================
void TestQuaternion()
{
    std::printf("TestQuaternion\n");
    const Quat id{};
    // slerp の端点・中点・単位長
    const Quat q0 = QAxis(0, 1, 0, 0), q1 = QAxis(0, 1, 0, 90);
    CHECK(QDist(QuatSlerp(q0, q1, 0.0), q0) < 1e-9 && QDist(QuatSlerp(q0, q1, 1.0), q1) < 1e-9);
    CHECK(QDist(QuatSlerp(q0, q1, 0.5), QAxis(0, 1, 0, 45)) < 1e-9);
    const Quat m = QuatSlerp(q0, q1, 0.3);
    CHECK(Near(QuatDot(m, m), 1.0, 1e-12));
    // 最短経路: q と -q は同一回転
    CHECK(QDist(QuatSlerp(q1, QuatNeg(q1), 0.5), q1) < 1e-9);
    // ラップ: 350° と 10°(Y 軸)の中点は 0°(遠回りの 180° ではない)
    const Quat a350 = QAxis(0, 1, 0, 350), a10 = QAxis(0, 1, 0, 10);
    CHECK(QDist(QuatSlerp(a350, a10, 0.5), id) < 1e-9);
    CHECK(QDist(QuatSlerp(a350, a10, 0.25), QAxis(0, 1, 0, -5)) < 1e-9);
    // 平行に近い(sin θ ≈ 0)でも NaN にならない
    const Quat near = QAxis(0, 1, 0, 1e-6);
    const Quat sm = QuatSlerp(id, near, 0.5);
    CHECK(std::isfinite(sm.x) && std::isfinite(sm.w) && Near(QuatDot(sm, sm), 1.0, 1e-12));
    CHECK(Near(QuatAngle(id, QAxis(1, 0, 0, 90)), 3.14159265358979323846 / 2, 1e-12));
    CHECK(QuatNormalize(Quat{ 0, 0, 0, 0 }).w == 1.0);

    // トラック評価
    Track t2 = QuatTrack("t_q", { { 0, q0 }, { 1000, q1 } }, Interp::Linear);
    CHECK(QDist(EvalQ(t2, 500), QAxis(0, 1, 0, 45)) < 1e-9);
    CHECK(QDist(EvalQ(t2, -100), q0) < 1e-12 && QDist(EvalQ(t2, 5000), q1) < 1e-12);
    Track wrap = QuatTrack("t_w", { { 0, a350 }, { 1000, a10 } }, Interp::Linear);
    CHECK(QDist(EvalQ(wrap, 500), id) < 1e-9);
    Track same = QuatTrack("t_s", { { 0, q1 }, { 1000, QuatNeg(q1) } }, Interp::Linear);
    bool constant = true;
    for (Tick t = 0; t <= 1000; t += 50) if (QDist(EvalQ(same, t), q1) > 1e-9) constant = false;
    CHECK(constant);
    Track step = QuatTrack("t_st", { { 0, q0 }, { 1000, q1 } }, Interp::Step);
    CHECK(QDist(EvalQ(step, 999), q0) < 1e-12 && QDist(EvalQ(step, 1000), q1) < 1e-12);

    // 連続性: 符号がバラバラなランダムなキー列でも、隣り合うサンプルの生の成分距離が小さい(q/-q で跳ばない)
    Rng rng(0xC0FFEE);
    for (int trial = 0; trial < 20; ++trial)
    {
        const int n = 3 + rng.R(5);
        std::vector<std::pair<Tick, Quat>> keys;
        Tick t = 0;
        double maxAngle = 0.0;
        Tick minSpan = 1 << 30;
        Quat prevShort{};
        for (int i = 0; i < n; ++i)
        {
            const Quat q = QuatNormalize({ rng.U() - 0.5, rng.U() - 0.5, rng.U() - 0.5, rng.U() - 0.5 });
            const Quat signed_q = rng.P(50) ? QuatNeg(q) : q;      // 符号をランダムに反転
            if (i > 0)
            {
                maxAngle = std::max(maxAngle, QuatAngle(prevShort, signed_q));
                minSpan = std::min<Tick>(minSpan, 200 + 100 * (i % 3));
            }
            prevShort = signed_q;
            keys.push_back({ t, signed_q });
            t += 200 + 100 * ((i + 1) % 3);
        }
        for (Interp ip : { Interp::Linear, Interp::Ease, Interp::Auto })
        {
            Track tr = QuatTrack("t_r", keys, ip);
            if (ip == Interp::Ease)
                for (auto& ch : tr.channels) for (auto& k : ch.keys) { k.ease = EaseId::InOutQuad; }
            double worst = 0.0;
            Quat prev = EvalQ(tr, 0);
            const Tick end = keys.back().first;
            for (Tick tt = 1; tt <= end; ++tt)
            {
                const Quat cur = EvalQ(tr, tt);
                const double dist = std::sqrt((cur.x - prev.x) * (cur.x - prev.x) + (cur.y - prev.y) * (cur.y - prev.y) +
                                              (cur.z - prev.z) * (cur.z - prev.z) + (cur.w - prev.w) * (cur.w - prev.w));
                worst = std::max(worst, dist);
                CHECK(Near(QuatDot(cur, cur), 1.0, 1e-9));
                prev = cur;
            }
            // 1 tick あたりの最大移動は、線形なら maxAngle/minSpan。イージング/スプラインは速度が上がるので余裕を見る(3 倍)。
            // 符号が跳ぶと worst は ~2 になる。
            const double bound = 3.0 * maxAngle / static_cast<double>(minSpan);
            CHECK_MSG(worst < bound + 1e-6 && worst < 0.5, "trial %d ip %d: worst %.4g bound %.4g", trial, static_cast<int>(ip), worst, bound);
            // キー時刻ちょうどでキーの回転(符号は同一視)
            for (const auto& k : keys) CHECK(QDist(EvalQ(tr, k.first), k.second) < 1e-9);
        }
    }
    // ループ
    Track lp = QuatTrack("t_l", { { 0, q0 }, { 1000, q1 } }, Interp::Linear, Extrap::Loop);
    CHECK(QDist(EvalQ(lp, 1500), EvalQ(lp, 500)) < 1e-9);

    // Evaluate: 4 チャンネルは 1 個の QuatSample に束ねられる(スカラーチャンネルは別に残る)
    Sequence s = NewSeq();
    Binding b = Bnd("b_x", "X");
    Track tr = QuatTrack("t_q", { { 0, q0 }, { 1000, q1 } });
    tr.channels.push_back(Ch("position.x", { K(0, 0), K(1000, 10) }));
    b.tracks.push_back(tr);
    s.bindings.push_back(b);
    EvalResult r = Evaluate(s, 500);
    CHECK(r.quats.size() == 1 && r.channels.size() == 1);
    CHECK(r.quats.size() == 1 && QDist(r.quats[0].q, QAxis(0, 1, 0, 45)) < 1e-9);
    // キー数が食い違う(不正/編集途中)ときはスカラー扱いで落ちない
    s.bindings[0].tracks[0].channels[3].keys.pop_back();
    r = Evaluate(s, 500);
    CHECK(r.quats.empty() && r.channels.size() == 5);
    CHECK(!QuatAlignmentProblem(s.bindings[0].tracks[0]).empty());
}

// ===========================================================================
// 5. 評価
// ===========================================================================
Sequence MakeEvalFixture()
{
    Sequence s = NewSeq("EvalFixture");
    Binding cam = Bnd("b_cam", "Cam");
    Track tr = Trk("t_tr", TrackType::Transform);
    tr.channels.push_back(Ch("position.x", { K(0, 0), K(600, 6), K(1200, 2) }));
    tr.channels.push_back(Ch("position.y", { KE(0, 1, EaseId::OutBack), K(1200, 5, Interp::Step) }));
    tr.channels.push_back(Ch("scale.x", { K(0, 1, Interp::Linear), K(1200, 3, Interp::Linear) }, Extrap::Linear, Extrap::PingPong));
    cam.tracks.push_back(tr);
    Track fov = Trk("t_fov", TrackType::Camera);
    fov.channels.push_back(Ch("fov", { K(0, 60, Interp::Linear), K(1200, 30, Interp::Linear) }));
    fov.channels.push_back(Ch("empty", {}));
    cam.tracks.push_back(fov);
    Track shk = Trk("t_shk", TrackType::Shake);
    Clip c1 = Clp("k_s1", 300, 600);
    c1.params.Set("blendIn", Num(100));
    c1.params.Set("blendOut", Num(200));
    shk.clips.push_back(c1);
    shk.clips.push_back(Clp("k_s2", 500, 0));
    cam.tracks.push_back(shk);
    Track muted = Trk("t_mute", TrackType::Property);
    muted.path = "X.y";
    muted.mute = true;
    muted.channels.push_back(Ch("value", { K(0, 1) }));
    cam.tracks.push_back(muted);
    s.bindings.push_back(cam);

    Binding cam2 = Bnd("b_cam2", "Cam2");
    Track t2 = Trk("t_t2", TrackType::Property);
    t2.path = "A.b";
    t2.channels.push_back(Ch("value", { K(0, 0, Interp::Linear), K(1200, 12, Interp::Linear) }));
    cam2.tracks.push_back(t2);
    s.bindings.push_back(cam2);

    Binding sc = Bnd("b_scene", "Scene", BindingKind::Scene);
    Track post = Trk("t_post", TrackType::Post);
    post.channels.push_back(Ch("bloom", { K(0, 0.1, Interp::Linear), K(1200, 0.9, Interp::Linear) }));
    sc.tracks.push_back(post);
    Track ev = Trk("t_ev", TrackType::Event);
    ev.events.push_back(Evt("e_1", 100));
    sc.tracks.push_back(ev);
    s.bindings.push_back(sc);

    Cut c;
    c.id = "c_01"; c.start = 0; c.end = 600; c.camera = "b_cam";
    s.cuts.push_back(c);
    c.id = "c_02"; c.start = 600; c.end = 1200; c.camera = "b_cam2";
    s.cuts.push_back(c);
    return s;
}

void TestEvaluateBasics()
{
    std::printf("TestEvaluateBasics\n");
    Sequence s = MakeEvalFixture();
    CHECK(CheckSequenceStructure(s).empty());
    EvalResult r = Evaluate(s, 300);
    // 並び: バインディング配列順 → トラック → チャンネル。muted と空チャンネルは出ない。
    // cam: position.x, position.y, scale.x, fov / cam2: value / scene: bloom
    CHECK(r.channels.size() == 6);
    if (r.channels.size() == 6)
    {
        CHECK(r.channels[0].binding == 0 && r.channels[0].track == 0 && r.channels[0].channel == 0);
        CHECK(r.channels[3].binding == 0 && r.channels[3].track == 1 && r.channels[3].channel == 0);
        CHECK(r.channels[4].binding == 1 && r.channels[5].binding == 2);
        CHECK(Near(r.channels[3].value, 52.5));                           // fov 60→30 の 1/4(t=300/1200)
        CHECK(Near(r.channels[4].value, 3.0));
        CHECK(Near(r.channels[5].value, 0.1 + 0.8 * 0.25));
    }
    // クリップ: t=300 で k_s1 が始まる(localTick 0、blendIn なので重み 0)
    CHECK(r.clips.size() == 1 && r.clips[0].clip == 0 && r.clips[0].localTick == 0 && r.clips[0].weight == 0.0);
    r = Evaluate(s, 350);
    CHECK(r.clips.size() == 1 && Near(r.clips[0].weight, 0.5));           // blendIn 100 の途中
    r = Evaluate(s, 500);
    CHECK(r.clips.size() == 2 && r.clips[0].weight == 1.0 && r.clips[1].clip == 1 && r.clips[1].localTick == 0);   // 点クリップは t==start のみ
    r = Evaluate(s, 800);
    CHECK(r.clips.size() == 1 && Near(r.clips[0].weight, 0.5));           // 終端 200 の手前 100 = blendOut 200 の半分
    r = Evaluate(s, 900);
    CHECK(r.clips.empty());                                               // [300,900) の外
    r = Evaluate(s, 299);
    CHECK(r.clips.empty());

    // 範囲外(Extrap): scale.x は pre=Linear / post=PingPong
    r = Evaluate(s, -600);
    CHECK(Near(r.channels[2].value, 1.0 - (2.0 / 1200.0) * 600.0));
    r = Evaluate(s, 1500);
    CHECK(Near(r.channels[2].value, 1.0 + 2.0 * (900.0 / 1200.0)));

    // フィルタ: 未解決バインディングを無効化
    EvalFilter f;
    f.bindingEnabled = { 1, 0, 1 };
    r = Evaluate(s, 300, &f);
    CHECK(r.channels.size() == 5);
    for (const auto& c : r.channels) CHECK(c.binding != 1);
    // ミュート: 出力から外れる
    s.bindings[0].tracks[0].mute = true;
    r = Evaluate(s, 300);
    CHECK(r.channels.size() == 3);
    // 範囲
    Tick a, b;
    GetRange(s, a, b);
    CHECK(a == 0 && b == 1200);
    s.hasRange = true; s.rangeStart = 100; s.rangeEnd = 500;
    GetRange(s, a, b);
    CHECK(a == 100 && b == 500);
    CHECK(SequenceExtent(s) == 1200);
}

void TestCameraCut()
{
    std::printf("TestCameraCut\n");
    Sequence s = MakeEvalFixture();
    CHECK(SelectCut(s, -1) == -1);                               // カットの手前
    CHECK(SelectCut(s, 0) == 0 && SelectCut(s, 599) == 0);
    CHECK(SelectCut(s, 600) == 1);                               // [start,end): 境界は後ろのカット
    CHECK(SelectCut(s, 1199) == 1);
    CHECK(SelectCut(s, 1200) == 1);                              // 範囲末尾ちょうどは最後のカットに含める
    CHECK(SelectCut(s, 1201) == -1);
    EvalResult r = Evaluate(s, 300);
    CHECK(r.cutIndex == 0 && r.cutBinding == 0);
    r = Evaluate(s, 700);
    CHECK(r.cutIndex == 1 && r.cutBinding == 1);
    r = Evaluate(s, 5000);
    CHECK(r.cutIndex == -1 && r.cutBinding == -1);

    // 重なり: 配列の後ろが勝つ
    Cut over;
    over.id = "c_03"; over.start = 400; over.end = 700; over.camera = "b_cam";
    s.cuts.push_back(over);
    CHECK(SelectCut(s, 300) == 0 && SelectCut(s, 400) == 2 && SelectCut(s, 650) == 2 && SelectCut(s, 700) == 1);
    // 穴(カットが無い区間)は -1
    s.cuts.erase(s.cuts.begin() + 1);
    s.cuts[1].start = 800;
    s.cuts[1].end = 900;
    CHECK(SelectCut(s, 700) == -1 && SelectCut(s, 850) == 1);
    // 範囲末尾の例外は「範囲末尾ちょうど」のときだけ(途中のカットの終端では働かない)
    s.hasRange = true; s.rangeStart = 0; s.rangeEnd = 1200;
    CHECK(SelectCut(s, 900) == -1);
    s.rangeEnd = 900;
    CHECK(SelectCut(s, 900) == 1);
    // 未知のカメラ: cutBinding = -1(カット自体は選ばれる)
    s.cuts[1].camera = "b_missing";
    r = Evaluate(s, 850);
    CHECK(r.cutIndex == 1 && r.cutBinding == -1);
}

void TestMarkers()
{
    std::printf("TestMarkers\n");
    Sequence s = NewSeq();
    for (Tick t : { 300, 100, 500, 100 })
    {
        Marker m;
        m.id = "m_" + std::to_string(s.markers.size());
        m.t = t;
        s.markers.push_back(m);
    }
    CHECK(NextMarker(s, 0) == 1 && NextMarker(s, 100) == 0 && NextMarker(s, 300) == 2 && NextMarker(s, 500) == -1);
    CHECK(PrevMarker(s, 1000) == 2 && PrevMarker(s, 500) == 0 && PrevMarker(s, 300) == 1 && PrevMarker(s, 100) == -1);
}

// 全要素入りのランダムな評価用シーケンス(決定論テスト用)
Sequence MakeRandomEvalSequence(Rng& rng)
{
    Sequence s = NewSeq("Rand");
    IdAllocator ids(rng.Next());
    Tick maxT = 20000;
    for (int b = 0; b < 4; ++b)
    {
        Binding bd = Bnd(ids.New('b'), "B" + std::to_string(b));
        for (int t = 0; t < 6; ++t)
        {
            Track tr = Trk(ids.New('t'), t == 0 ? TrackType::Transform : (t == 1 ? TrackType::Camera : (t == 2 ? TrackType::Property : TrackType::Post)));
            if (t == 2) { tr.path = "P.q"; tr.valueType = ValueType::Color; tr.colorSpace = rng.P(50) ? ColorSpace::Srgb : ColorSpace::Linear; }
            const int chs = 1 + rng.R(3);
            for (int c = 0; c < chs; ++c)
            {
                Channel ch;
                ch.name = (t == 2) ? std::string("rgb").substr(static_cast<std::size_t>(c), 1) : "ch" + std::to_string(c);
                ch.pre = static_cast<Extrap>(rng.R(4));
                ch.post = static_cast<Extrap>(rng.R(4));
                const int nk = 1 + rng.R(12);
                Tick tt = -1000 + rng.R(2000);
                for (int k = 0; k < nk; ++k)
                {
                    Key key;
                    key.t = tt;
                    key.v = (rng.U() - 0.5) * 200.0;
                    switch (rng.R(5))
                    {
                    case 0: key.ip = Interp::Step; break;
                    case 1: key.ip = Interp::Linear; break;
                    case 2: key.ip = Interp::Auto; break;
                    case 3: key.ip = Interp::Ease; key.ease = static_cast<EaseId>(rng.R(static_cast<int>(EaseId::Count))); break;
                    default: key.ip = Interp::Bezier; key.inDt = rng.R(800); key.outDt = rng.R(800); key.inDv = rng.U() * 50 - 25; key.outDv = rng.U() * 50 - 25; break;
                    }
                    NormalizeKey(key);
                    ch.keys.push_back(key);
                    tt += 1 + rng.R(1500) * (rng.P(10) ? 0 : 1);
                }
                tr.channels.push_back(std::move(ch));
            }
            bd.tracks.push_back(std::move(tr));
        }
        // クォータニオン
        std::vector<std::pair<Tick, Quat>> qk;
        for (int k = 0; k < 5; ++k)
            qk.push_back({ static_cast<Tick>(k * 900), QuatNormalize({ rng.U() - 0.5, rng.U() - 0.5, rng.U() - 0.5, rng.U() - 0.5 }) });
        Track qt = QuatTrack(ids.New('t'), qk, static_cast<Interp>(1 + rng.R(4)), static_cast<Extrap>(rng.R(4)));
        qt.channels[0].keys[0].ip = Interp::Auto;
        bd.tracks.push_back(std::move(qt));
        Track shk = Trk(ids.New('t'), TrackType::Shake);
        for (int k = 0; k < 3; ++k) shk.clips.push_back(Clp(ids.New('k'), rng.R(10000), rng.R(3000)));
        bd.tracks.push_back(std::move(shk));
        s.bindings.push_back(std::move(bd));
    }
    for (int i = 0; i < 3; ++i)
    {
        Cut c;
        c.id = ids.New('c');
        c.start = rng.R(10000);
        c.end = c.start + rng.R(6000);
        c.camera = s.bindings[static_cast<std::size_t>(rng.R(4))].id;
        s.cuts.push_back(c);
    }
    (void)maxT;
    return s;
}

void TestEvaluateDeterminism()
{
    std::printf("TestEvaluateDeterminism\n");
    Rng rng(0xDE7E12);
    for (int trial = 0; trial < 3; ++trial)
    {
        Sequence s = MakeRandomEvalSequence(rng);
        CHECK_MSG(CheckSequenceStructure(s).empty(), "%s", CheckSequenceStructure(s).c_str());
        // 1 万個のランダムな t
        std::vector<Tick> ts(10000);
        for (auto& t : ts) t = static_cast<Tick>(rng.R(40000)) - 10000;
        // 昇順の結果
        std::vector<Tick> sorted = ts;
        std::sort(sorted.begin(), sorted.end());
        std::unordered_map<Tick, EvalResult> asc;
        for (Tick t : sorted) if (!asc.count(t)) asc[t] = Evaluate(s, t);
        // 任意順(シャッフル)+ 使い回しバッファで評価 → 昇順の結果とビット一致
        for (std::size_t i = ts.size(); i > 1; --i) std::swap(ts[i - 1], ts[static_cast<std::size_t>(rng.R(static_cast<int>(i)))]);
        EvalResult buf;
        int mismatches = 0;
        for (Tick t : ts)
        {
            Evaluate(s, t, buf);
            if (!BitEqual(buf, asc[t])) ++mismatches;
        }
        CHECK_MSG(mismatches == 0, "順序依存の結果が %d 件", mismatches);
        // 同じ t を連続で 2 回 → 同一
        EvalResult x = Evaluate(s, 1234), y = Evaluate(s, 1234);
        CHECK(BitEqual(x, y));
        // 別の t を挟んでも変わらない(履歴に依存しない = ヒステリシス無し)
        Evaluate(s, -5000, buf);
        Evaluate(s, 1234, buf);
        CHECK(BitEqual(buf, x));
        // 値が有限
        bool finite = true;
        for (Tick t : { -10000, -1, 0, 1, 777, 5000, 29999, 123456 })
        {
            EvalResult r = Evaluate(s, t);
            for (const auto& c : r.channels) if (!std::isfinite(c.value)) finite = false;
            for (const auto& q : r.quats) if (!std::isfinite(q.q.x) || !std::isfinite(q.q.w)) finite = false;
        }
        CHECK(finite);
    }
    // BitEqual が異なる値を検出できる
    Sequence s = MakeEvalFixture();
    CHECK(!BitEqual(Evaluate(s, 100), Evaluate(s, 101)));
}

// ===========================================================================
// 6. 再生位置・ループ・イベント発火
// ===========================================================================
Sequence MakeEventSeq(const std::vector<Tick>& times, Tick rangeStart, Tick rangeEnd)
{
    Sequence s = NewSeq("Ev");
    s.hasRange = true; s.rangeStart = rangeStart; s.rangeEnd = rangeEnd;
    Binding b = Bnd("b_scene", "Scene", BindingKind::Scene);
    Track t = Trk("t_ev", TrackType::Event);
    for (std::size_t i = 0; i < times.size(); ++i) t.events.push_back(Evt("e_" + std::to_string(i), times[i]));
    b.tracks.push_back(t);
    s.bindings.push_back(b);
    return s;
}

// 独立な参照実装(1 tick ずつ進めて位置を調べる)。展開時刻 u の位置。
Tick PosAt(LoopMode mode, Tick a, Tick b, Tick u)
{
    const Tick P = b - a;
    if (mode == LoopMode::Once || P <= 0) return u;
    if (mode == LoopMode::Loop) return a + FloorMod(u - a, P);
    const Tick m = FloorMod(u - a, 2 * P);
    return a + (m <= P ? m : 2 * P - m);
}
std::multiset<std::pair<Tick, int>> OracleFires(const Sequence& s, LoopMode mode, Tick a, Tick b, Tick u0, Tick u1, bool startInclusive,
                                                 bool fireBackward = true)
{
    std::multiset<std::pair<Tick, int>> out;
    const bool fwd = u1 > u0;
    if (u0 == u1 || (!fwd && !fireBackward)) return out;
    const Tick lo = fwd ? u0 : u1, hi = fwd ? u1 : u0;
    for (Tick u = lo; u <= hi; ++u)
    {
        const bool inside = fwd ? ((u > lo || startInclusive) && u <= hi) : (u >= lo && (u < hi || startInclusive));
        if (!inside) continue;
        const Tick p = PosAt(mode, a, b, u);
        const auto& evs = s.bindings[0].tracks[0].events;
        for (std::size_t i = 0; i < evs.size(); ++i)
        {
            const bool inRange = (mode == LoopMode::Once) || (evs[i].t >= a && evs[i].t <= b && !(mode == LoopMode::Loop && evs[i].t == b));
            if (inRange && evs[i].t == p) out.insert({ u, static_cast<int>(i) });
        }
    }
    return out;
}
std::multiset<std::pair<Tick, int>> ToSet(const std::vector<EventFire>& v)
{
    std::multiset<std::pair<Tick, int>> out;
    for (const auto& f : v) out.insert({ f.time, f.event });
    return out;
}

void TestEventBasics()
{
    std::printf("TestEventBasics\n");
    Sequence s = MakeEventSeq({ 0, 100, 200, 300 }, 0, 300);
    std::vector<EventFire> out;
    EventCollectOptions o;
    o.rangeStart = 0; o.rangeEnd = 300;

    // 前進 (u0, u1]: 左開右閉
    CollectEvents(s, 0, 300, o, out);
    CHECK(out.size() == 3 && out[0].event == 1 && out[1].event == 2 && out[2].event == 3);
    // startInclusive: 開始点のイベント(t=0)を落とさない
    out.clear();
    o.startInclusive = true;
    CollectEvents(s, 0, 300, o, out);
    CHECK(out.size() == 4 && out[0].event == 0 && out[0].time == 0);
    o.startInclusive = false;
    // 境界: (0,100] は 100 を含み、(100,200] は 100 を含まない
    out.clear(); CollectEvents(s, 0, 100, o, out); CHECK(out.size() == 1 && out[0].event == 1);
    out.clear(); CollectEvents(s, 100, 200, o, out); CHECK(out.size() == 1 && out[0].event == 2);
    // u0 == u1 は発火しない(startInclusive でも)
    out.clear(); o.startInclusive = true; CollectEvents(s, 100, 100, o, out); CHECK(out.empty()); o.startInclusive = false;
    // 後退 [u1, u0): 前進の鏡像
    out.clear(); CollectEvents(s, 300, 0, o, out);
    CHECK(out.size() == 3 && out[0].event == 2 && out[1].event == 1 && out[2].event == 0);      // 移動方向の順(300 側から)
    out.clear(); CollectEvents(s, 200, 100, o, out); CHECK(out.size() == 1 && out[0].event == 1);   // [100,200)
    out.clear(); CollectEvents(s, 100, 0, o, out); CHECK(out.size() == 1 && out[0].event == 0);     // [0,100)
    // 逆再生の開始点(startInclusive)は u0 も含む
    out.clear(); o.startInclusive = true; CollectEvents(s, 300, 200, o, out);
    CHECK(out.size() == 2 && out[0].event == 3 && out[1].event == 2); o.startInclusive = false;
    // fireBackward=false では逆再生で発火しない
    out.clear(); o.fireBackward = false; CollectEvents(s, 300, 0, o, out); CHECK(out.empty()); o.fireBackward = true;
    // 前進 + 同じ区間の後退で、境界のイベントは片側でしか発火しない(位置ベースの通過)
    out.clear(); CollectEvents(s, 50, 150, o, out); const std::size_t fwdN = out.size();
    out.clear(); CollectEvents(s, 150, 50, o, out); CHECK(fwdN == 1 && out.size() == 1);
    // ミュートされたイベントトラックは発火しない
    s.bindings[0].tracks[0].mute = true;
    out.clear(); CollectEvents(s, 0, 300, o, out); CHECK(out.empty());
    s.bindings[0].tracks[0].mute = false;
    // 出力の並び: 同時刻は バインディング → トラック → イベントの配列順
    Sequence d = MakeEventSeq({ 100, 100, 100 }, 0, 300);
    Track t2 = Trk("t_ev2", TrackType::Event);
    t2.events.push_back(Evt("e_x", 100));
    d.bindings[0].tracks.push_back(t2);
    out.clear(); CollectEvents(d, 0, 300, o, out);
    CHECK(out.size() == 4 && out[0].track == 0 && out[0].event == 0 && out[1].event == 1 && out[2].event == 2 && out[3].track == 1);
    out.clear(); CollectEvents(d, 300, 0, o, out);
    CHECK(out.size() == 4 && out[0].track == 1 && out[3].track == 0 && out[3].event == 0);                // 後退は逆順
}

void TestEventPartitionInvariance()
{
    std::printf("TestEventPartitionInvariance\n");
    Rng rng(0xE7E27);
    int cases = 0;
    for (int trial = 0; trial < 300; ++trial)
    {
        const LoopMode mode = static_cast<LoopMode>(trial % 3);
        const Tick a = rng.R(50);
        const Tick P = 20 + rng.R(60);
        const Tick b = a + P;
        std::vector<Tick> times;
        const int ne = 1 + rng.R(8);
        for (int i = 0; i < ne; ++i)
        {
            const int kind = rng.R(6);
            times.push_back(kind == 0 ? a : (kind == 1 ? b : (kind == 2 ? a - 5 : (kind == 3 ? b + 3 : a + rng.R(static_cast<int>(P) + 1)))));
        }
        Sequence s = MakeEventSeq(times, a, b);
        EventCollectOptions o;
        o.rangeStart = a; o.rangeEnd = b; o.mode = mode;

        // 前進: 全体を 1 回で / ランダムに分割して → 同じ多重集合(= 1 回ずつ)。オラクルとも一致。
        const Tick total = (mode == LoopMode::Once) ? (b - a + 10) : P * (1 + rng.R(4)) + rng.R(static_cast<int>(P));
        const Tick u0 = a, u1 = a + total;
        std::vector<EventFire> whole;
        o.startInclusive = true;
        CollectEvents(s, u0, u1, o, whole);
        const auto oracle = OracleFires(s, mode, a, b, u0, u1, true);
        CHECK_MSG(ToSet(whole) == oracle, "trial %d mode %d 全体一括がオラクルと不一致", trial, static_cast<int>(mode));

        std::vector<EventFire> split;
        Tick cur = u0;
        bool first = true;
        while (cur < u1)
        {
            const Tick step = std::min<Tick>(u1 - cur, 1 + rng.R(static_cast<int>(P) + 7));
            o.startInclusive = first;
            CollectEvents(s, cur, cur + step, o, split);
            first = false;
            cur += step;
        }
        CHECK_MSG(ToSet(split) == ToSet(whole), "trial %d mode %d 分割で発火の多重集合が変わった", trial, static_cast<int>(mode));
        // 時刻順(前進は非減少)
        bool ordered = true;
        for (std::size_t i = 1; i < split.size(); ++i) if (split[i].time < split[i - 1].time) ordered = false;
        CHECK(ordered);

        // 後退: [u1, u0) をランダム分割(startInclusive は最初の 1 歩だけ)→ 一括と同じ
        std::vector<EventFire> backWhole, backSplit;
        o.startInclusive = true;
        CollectEvents(s, u1, u0, o, backWhole);
        CHECK_MSG(ToSet(backWhole) == OracleFires(s, mode, a, b, u1, u0, true), "trial %d mode %d 逆再生がオラクルと不一致", trial, static_cast<int>(mode));
        cur = u1;
        first = true;
        while (cur > u0)
        {
            const Tick step = std::min<Tick>(cur - u0, 1 + rng.R(static_cast<int>(P) + 7));
            o.startInclusive = first;
            CollectEvents(s, cur, cur - step, o, backSplit);
            first = false;
            cur -= step;
        }
        CHECK_MSG(ToSet(backSplit) == ToSet(backWhole), "trial %d mode %d 逆再生の分割で不一致", trial, static_cast<int>(mode));
        ++cases;
    }
    CHECK(cases == 300);

    // 具体例: Loop で [0,100) 周期。t=0 と t=50 のイベントは周回ごとに 1 回。t=100(=end)は鳴らない。
    Sequence s = MakeEventSeq({ 0, 50, 100 }, 0, 100);
    EventCollectOptions o;
    o.rangeStart = 0; o.rangeEnd = 100; o.mode = LoopMode::Loop; o.startInclusive = true;
    std::vector<EventFire> out;
    CollectEvents(s, 0, 350, o, out);
    int c0 = 0, c1 = 0, c2 = 0;
    for (const auto& f : out) { if (f.event == 0) ++c0; if (f.event == 1) ++c1; if (f.event == 2) ++c2; }
    CHECK(c0 == 4 && c1 == 4 && c2 == 0);          // t=0 は u=0,100,200,300、t=50 は u=50,150,250,350
    // ちょうど周回境界をまたぐ 1 歩(99 → 101)は「t=0 の 1 回」だけ
    out.clear(); o.startInclusive = false; CollectEvents(s, 99, 101, o, out);
    CHECK(out.size() == 1 && out[0].event == 0 && out[0].time == 100);
    // PingPong: 折り返しの端は 1 回
    Sequence pp = MakeEventSeq({ 0, 100, 40 }, 0, 100);
    o.mode = LoopMode::PingPong; o.startInclusive = true;
    out.clear(); CollectEvents(pp, 0, 400, o, out);
    int e0 = 0, e1 = 0, e2 = 0;
    for (const auto& f : out) { if (f.event == 0) ++e0; if (f.event == 1) ++e1; if (f.event == 2) ++e2; }
    CHECK(e0 == 3 && e1 == 2 && e2 == 4);           // u∈[0,400]: t=0 は 0,200,400 / t=100 は 100,300 / t=40 は 40,160,240,360
    // 64 周を超える巨大な移動は打ち切る(暴走防止)
    out.clear(); o.mode = LoopMode::Loop; o.startInclusive = false;
    CollectEvents(s, 0, 100000, o, out);
    CHECK(out.size() <= 2 * 65 && !out.empty());
}

void TestPlayback()
{
    std::printf("TestPlayback\n");
    Sequence s = MakeEventSeq({ 0, 1500, 3000, 6000 }, 0, 6000);     // 秒: 0, 0.25, 0.5, 1.0
    // --- Once ---
    SeqPlayback pb;
    pb.Reset(s, LoopMode::Once);
    std::vector<EventFire> out;
    CHECK(pb.Position() == 0 && !pb.IsPlaying());
    pb.Advance(s, 1.0, out);
    CHECK(out.empty() && pb.Position() == 0);                        // 再生していない
    pb.Play();
    pb.Advance(s, 0.25, out);                                        // 0 → 1500: t=0(開始)と t=1500
    CHECK(out.size() == 2 && out[0].event == 0 && out[1].event == 1 && pb.Position() == 1500);
    pb.Advance(s, 0.25, out);
    CHECK(out.size() == 3 && out[2].event == 2);
    pb.Advance(s, 5.0, out);                                         // 端で止まる
    CHECK(out.size() == 4 && out[3].event == 3 && pb.Position() == 6000 && pb.IsFinished() && !pb.IsPlaying());
    pb.Advance(s, 1.0, out);
    CHECK(out.size() == 4);                                          // 止まったあとは何も起きない

    // ★スクラブ(Seek)では発火しない
    out.clear();
    pb.Seek(4500);
    CHECK(pb.Position() == 4500 && out.empty() && !pb.IsFinished());
    pb.Seek(0);
    CHECK(out.empty());
    pb.Seek(-50);
    CHECK(pb.Position() == 0);                                       // 範囲に clamp
    // Seek(0) のあと Play すると t=0 は再び発火(「頭から再生」)。途中位置からの再生では発火しない
    pb.Play();
    pb.Advance(s, 0.0001, out);
    CHECK(out.empty());                                              // 0.6 tick は進まない(整数 tick に満たない)。武装は維持
    pb.Advance(s, 0.0999, out);
    CHECK(out.size() == 1 && out[0].event == 0);
    out.clear();
    pb.Seek(3000);
    pb.Play();
    pb.Advance(s, 0.25, out);                                        // 3000 → 4500: 3000 は既に通過済み(発火しない)
    CHECK(out.empty());

    // --- 逆再生 ---
    out.clear();
    pb.Reset(s, LoopMode::Once);
    pb.SetRate(-1.0);
    pb.Seek(6000);
    pb.Play();
    pb.Advance(s, 0.5, out);                                         // 6000 → 3000: 逆再生の起点(6000)を含め [3000,6000]
    CHECK(out.size() == 2 && out[0].event == 3 && out[1].event == 2);
    pb.Advance(s, 10.0, out);
    CHECK(pb.IsFinished() && pb.Position() == 0 && out.size() == 4 && out[3].event == 0);   // [0,3000) の t=1500 と t=0
    pb.SetRate(1.0);

    // --- Loop ---
    pb.Reset(s, LoopMode::Loop);
    pb.Seek(0);
    out.clear();
    pb.Play();
    pb.Advance(s, 1.5, out);                                         // 0 → 9000(1.5 周)
    CHECK(pb.Position() == 3000 && pb.IsPlaying() && !pb.IsFinished());
    // Loop の範囲は [0,6000): t=6000 は鳴らず、t=0,1500,3000 が周回ごとに
    int counts[4] = { 0, 0, 0, 0 };
    for (const auto& f : out) ++counts[f.event];
    CHECK(counts[0] == 2 && counts[1] == 2 && counts[2] == 2 && counts[3] == 0);          // 0 と 6000 周期 → u=0,6000 / 1500,7500 / 3000,9000
    // --- PingPong ---
    pb.Reset(s, LoopMode::PingPong);
    pb.Seek(0);
    pb.Play();
    out.clear();
    pb.Advance(s, 0.75, out);                                        // 4500
    CHECK(pb.Position() == 4500);
    pb.Advance(s, 0.5, out);                                         // u=7500 → 折り返して 4500 (6000 - 1500)
    CHECK(pb.Position() == 4500);
    pb.Advance(s, 0.5, out);                                         // u=10500 → 1500
    CHECK(pb.Position() == 1500);
    // 位置: Position() の各モード
    CHECK(pb.Unwrapped() == 10500);
    // 半端な時間(端数の繰り越し): 0.1s × 10 回 = 1.0s ちょうど(6000 tick)
    pb.Reset(s, LoopMode::Once);
    pb.Play();
    out.clear();
    for (int i = 0; i < 10; ++i) pb.Advance(s, 0.1, out);
    CHECK(pb.Position() == 6000 && out.size() == 4);
    // 3 分割の時間(1/3 秒 ×3)でも 1 tick も失わない
    pb.Reset(s, LoopMode::Once);
    pb.Play();
    for (int i = 0; i < 3; ++i) pb.Advance(s, 1.0 / 3.0, out);
    CHECK(pb.Position() == 6000);
    // 異常な dt(NaN / 巨大)でも落ちない・暴走しない
    pb.Reset(s, LoopMode::Loop);
    pb.Play();
    out.clear();
    pb.Advance(s, std::nan(""), out);
    CHECK(pb.Position() == 0 && out.empty());
    pb.Advance(s, 1e300, out);
    CHECK(!out.empty() && out.size() <= 3 * 65 + 3);
}

// ===========================================================================
// 7. SeqOp
// ===========================================================================
Sequence MakeFixture()
{
    Sequence s = NewSeq("Fixture");
    s.id = "q_fixture";
    Binding cam = Bnd("b_cam", "Cam");
    Track tr = Trk("t_tr", TrackType::Transform);
    tr.channels.push_back(Ch("position.x", { K(0, 0), K(600, 6), K(1200, 2) }));
    tr.channels.push_back(Ch("position.y", { K(0, 1), K(1200, 5) }));
    cam.tracks.push_back(tr);
    Track fov = Trk("t_fov", TrackType::Camera);
    fov.channels.push_back(Ch("fov", { K(0, 60, Interp::Linear), KB(1200, 30, 100, 1, 0, 0) }));
    cam.tracks.push_back(fov);
    Track shk = Trk("t_shk", TrackType::Shake);
    Clip c = Clp("k_s1", 300, 600);
    c.params.Set("amp", Num(0.35));
    shk.clips.push_back(c);
    cam.tracks.push_back(shk);
    s.bindings.push_back(cam);
    Binding sc = Bnd("b_scene", "Scene", BindingKind::Scene);
    Track ev = Trk("t_ev", TrackType::Event);
    ev.events.push_back(Evt("e_1", 100));
    ev.events.push_back(Evt("e_2", 700, "lua", "OnBoss"));
    sc.tracks.push_back(ev);
    Track flag = Trk("t_flag", TrackType::Property);
    flag.path = "X.on";
    flag.valueType = ValueType::Bool;
    flag.channels.push_back(Ch("value", { K(0, 1, Interp::Step), K(500, 0, Interp::Step) }));
    sc.tracks.push_back(flag);
    s.bindings.push_back(sc);
    Cut cu;
    cu.id = "c_01"; cu.start = 0; cu.end = 1200; cu.camera = "b_cam";
    s.cuts.push_back(cu);
    Marker m;
    m.id = "m_01"; m.t = 600; m.name = "mid";
    s.markers.push_back(m);
    return s;
}

// op を適用 → 逆 op で元へ(バイト一致)→ もう一度適用で同じ結果(決定的)
void ExpectOpRoundTrip(const Sequence& base, const SeqOp& op, const char* label)
{
    Sequence s = base;
    const std::string before = Ser(s);
    SeqOp inv;
    ApplyResult r = ApplyOp(s, op, &inv);
    CHECK_MSG(r.ok, "%s: 適用失敗: %s", label, r.error.c_str());
    if (!r.ok) return;
    const std::string after = Ser(s);
    CHECK_MSG(CheckSequenceStructure(s).empty(), "%s: 適用後が不正: %s", label, CheckSequenceStructure(s).c_str());
    SeqOp inv2;
    ApplyResult r2 = ApplyOp(s, inv, &inv2);
    CHECK_MSG(r2.ok, "%s: 逆 op の適用失敗: %s", label, r2.error.c_str());
    CHECK_MSG(Ser(s) == before, "%s: 逆 op で元に戻らない", label);
    ApplyResult r3 = ApplyOp(s, inv2);
    CHECK_MSG(r3.ok && Ser(s) == after, "%s: 逆の逆で再現しない", label);
}

void ExpectOpFails(const Sequence& base, const SeqOp& op, const char* label)
{
    Sequence s = base;
    const std::string before = Ser(s);
    SeqOp inv;
    ApplyResult r = ApplyOp(s, op, &inv);
    CHECK_MSG(!r.ok, "%s: 失敗すべきなのに成功した", label);
    CHECK_MSG(Ser(s) == before, "%s: 失敗したのにシーケンスが変わった", label);
    CHECK_MSG(r.ok || !r.error.empty(), "%s: エラー文が空", label);
}

void TestOps()
{
    std::printf("TestOps\n");
    const Sequence fx = MakeFixture();
    CHECK(CheckSequenceStructure(fx).empty());
    CHECK(std::string(OpName(SeqOp(OpAddKey{}))) == "AddKey" && std::string(OpName(SeqOp(OpSetCut{}))) == "SetCut");

    // ---- シーケンス自体
    ExpectOpRoundTrip(fx, OpSetName{ "Renamed \"x\"\n日本語" }, "SetName");
    ExpectOpRoundTrip(fx, OpSetFrameRate{ 24 }, "SetFrameRate");
    ExpectOpRoundTrip(fx, OpSetRange{ true, 100, 5000 }, "SetRange");
    { Sequence r = fx; r.hasRange = true; r.rangeStart = 1; r.rangeEnd = 2; ExpectOpRoundTrip(r, OpSetRange{ false, 0, 0 }, "SetRange(解除)"); }
    { RenderSettings rs; rs.width = 1280; rs.height = 720; rs.shutter = 0.25; rs.warmup = 30; ExpectOpRoundTrip(fx, OpSetRender{ rs }, "SetRender"); }
    { SeqValue m = SeqValue::MakeObject(); m.Set("k", Num(1)); m.Set("a", SeqValue::MakeArray()); ExpectOpRoundTrip(fx, OpSetMeta{ m }, "SetMeta"); }

    // ---- バインディング
    { Binding b = Bnd("b_new", "New"); ExpectOpRoundTrip(fx, OpAddBinding{ b, -1 }, "AddBinding(末尾)"); ExpectOpRoundTrip(fx, OpAddBinding{ b, 0 }, "AddBinding(先頭)"); }
    { Binding b = Bnd("b_new", "New"); Track t = Trk("t_n", TrackType::Property); t.path = "A.b"; t.channels.push_back(Ch("value", { K(0, 1) })); b.tracks.push_back(t);
      ExpectOpRoundTrip(fx, OpAddBinding{ b, 1 }, "AddBinding(トラックつき)"); }
    { Sequence r = fx; r.cuts.clear(); ExpectOpRoundTrip(r, OpDeleteBinding{ "b_cam" }, "DeleteBinding"); }
    ExpectOpRoundTrip(fx, OpMoveBinding{ "b_cam", 1 }, "MoveBinding");
    { BindingHeader h; h.name = "CamRenamed"; h.kind = BindingKind::Entity; h.hint.guid = "00a1b2c3d4e5f607"; h.hint.path = "Rig/Cam"; h.params.Set("note", Str("x"));
      ExpectOpRoundTrip(fx, OpSetBindingHeader{ "b_cam", h }, "SetBindingHeader"); }

    // ---- トラック
    { Track t = Trk("t_new", TrackType::Property); t.path = "L.i"; t.channels.push_back(Ch("value", { K(0, 1), K(10, 2) })); ExpectOpRoundTrip(fx, OpAddTrack{ "b_cam", t, 0 }, "AddTrack"); }
    ExpectOpRoundTrip(fx, OpDeleteTrack{ "t_fov" }, "DeleteTrack");
    ExpectOpRoundTrip(fx, OpMoveTrack{ "t_fov", "b_cam", 0 }, "MoveTrack(同一バインディング内)");
    ExpectOpRoundTrip(fx, OpMoveTrack{ "t_fov", "b_scene", 1 }, "MoveTrack(別バインディングへ)");
    { TrackHeader h; h.name = "FOV"; h.mute = true; h.lock = true; h.params.Set("focus", Str("b_scene")); ExpectOpRoundTrip(fx, OpSetTrackHeader{ "t_fov", h }, "SetTrackHeader"); }

    // ---- チャンネル
    ExpectOpRoundTrip(fx, OpAddChannel{ "t_tr", Ch("position.z", { K(0, 3), K(100, 4) }), -1 }, "AddChannel");
    ExpectOpRoundTrip(fx, OpAddChannel{ "t_tr", Ch("scale.x", {}), 0 }, "AddChannel(空)");
    ExpectOpRoundTrip(fx, OpDeleteChannel{ "t_tr", "position.y" }, "DeleteChannel");
    ExpectOpRoundTrip(fx, OpSetChannelExtrap{ "t_tr", "position.x", Extrap::Loop, Extrap::PingPong }, "SetChannelExtrap");

    // ---- キー
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", K(300, 9), -1 }, "AddKey(時刻順)");
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", K(600, 9), -1 }, "AddKey(同時刻は後ろ)");
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", K(600, 9), 1 }, "AddKey(同時刻を前に = 添字指定)");
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", K(9999, 9), -1 }, "AddKey(末尾)");
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", K(-50, 9), -1 }, "AddKey(先頭より前)");
    ExpectOpRoundTrip(fx, OpAddKey{ "t_tr", "position.x", KE(300, 1, EaseId::Linear), -1 }, "AddKey(Ease+Linear は Linear に正規化)");
    ExpectOpRoundTrip(fx, OpDeleteKey{ "t_tr", "position.x", 1 }, "DeleteKey");
    ExpectOpRoundTrip(fx, OpMoveKey{ "t_tr", "position.x", 0, 900, -1 }, "MoveKey(順序が入れ替わる)");
    ExpectOpRoundTrip(fx, OpMoveKey{ "t_tr", "position.x", 1, 1200, -1 }, "MoveKey(同時刻の後ろへ)");
    ExpectOpRoundTrip(fx, OpMoveKey{ "t_tr", "position.x", 1, 1200, 1 }, "MoveKey(同時刻の前へ = 添字指定)");
    ExpectOpRoundTrip(fx, OpSetKeyValue{ "t_tr", "position.x", 1, -3.5 }, "SetKeyValue");
    ExpectOpRoundTrip(fx, OpSetInterp{ "t_tr", "position.x", 0, Interp::Ease, EaseId::OutBounce }, "SetInterp(Ease)");
    ExpectOpRoundTrip(fx, OpSetInterp{ "t_tr", "position.x", 0, Interp::Bezier, EaseId::Linear }, "SetInterp(Bezier: 既定ハンドルが入る)");
    ExpectOpRoundTrip(fx, OpSetInterp{ "t_fov", "fov", 1, Interp::Linear, EaseId::Linear }, "SetInterp(Bezier → Linear: ハンドルが消える)");
    ExpectOpRoundTrip(fx, OpSetTangent{ "t_fov", "fov", 1, 300, 2.5, 0, 0 }, "SetTangent");
    { Key k = KB(0, 60, 0, 0, 200, -3); ExpectOpRoundTrip(fx, OpSetKey{ "t_fov", "fov", 0, k }, "SetKey"); }

    // ---- クリップ / イベント
    { Clip c = Clp("k_new", 1000, 50); c.params.Set("p", Str("q")); ExpectOpRoundTrip(fx, OpAddClip{ "t_shk", c, 0 }, "AddClip"); }
    ExpectOpRoundTrip(fx, OpDeleteClip{ "t_shk", "k_s1" }, "DeleteClip");
    { Clip c = Clp("k_s1", 400, 100); c.params.Set("amp", Num(9)); ExpectOpRoundTrip(fx, OpSetClip{ "t_shk", c }, "SetClip"); }
    ExpectOpRoundTrip(fx, OpAddEvent{ "t_ev", Evt("e_9", 50), 0 }, "AddEvent");
    ExpectOpRoundTrip(fx, OpDeleteEvent{ "t_ev", "e_1" }, "DeleteEvent");
    { EventItem e = Evt("e_2", 800, "log", "hello"); ExpectOpRoundTrip(fx, OpSetEvent{ "t_ev", e }, "SetEvent"); }

    // ---- マーカー / カット
    { Marker m; m.id = "m_02"; m.t = 10; m.name = "a"; ExpectOpRoundTrip(fx, OpAddMarker{ m, 0 }, "AddMarker"); }
    ExpectOpRoundTrip(fx, OpDeleteMarker{ "m_01" }, "DeleteMarker");
    { Marker m; m.id = "m_01"; m.t = 700; m.name = "moved"; ExpectOpRoundTrip(fx, OpSetMarker{ m }, "SetMarker"); }
    { Cut c; c.id = "c_02"; c.start = 100; c.end = 200; c.camera = "b_cam"; c.blend = 30; ExpectOpRoundTrip(fx, OpAddCut{ c, -1 }, "AddCut"); }
    ExpectOpRoundTrip(fx, OpDeleteCut{ "c_01" }, "DeleteCut");
    { Cut c; c.id = "c_01"; c.start = 5; c.end = 6; c.camera = "b_scene"; ExpectOpRoundTrip(fx, OpSetCut{ c }, "SetCut"); }

    // ---- 失敗する op は何も変えない
    ExpectOpFails(fx, OpSetFrameRate{ 0 }, "fps=0");
    ExpectOpFails(fx, OpSetRange{ true, 10, 5 }, "range 逆転");
    ExpectOpFails(fx, OpAddBinding{ Bnd("b_cam", "Dup"), -1 }, "バインディング ID 重複");
    ExpectOpFails(fx, OpAddBinding{ Bnd("b_x", "X"), 9 }, "バインディング挿入位置が範囲外");
    ExpectOpFails(fx, OpAddBinding{ Bnd("bad id", "X"), -1 }, "ID 形式");
    ExpectOpFails(fx, OpDeleteBinding{ "b_cam" }, "カットが参照するバインディングは消せない");
    ExpectOpFails(fx, OpDeleteBinding{ "b_nope" }, "存在しないバインディング");
    ExpectOpFails(fx, OpMoveBinding{ "b_cam", 5 }, "MoveBinding 範囲外");
    { Track t = Trk("t_fov", TrackType::Property); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "トラック ID 重複"); }
    { Track t = Trk("t_x", TrackType::Property); t.clips.push_back(Clp("k_x", 0, 1)); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "カーブ族にクリップ"); }
    { Track t = Trk("t_x", TrackType::Shake); t.clips.push_back(Clp("k_s1", 0, 1)); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "クリップ ID がシーケンス内で重複"); }
    { Track t = Trk("t_x", TrackType::Property); t.channels.push_back(Ch("v", { K(10, 1), K(5, 2) })); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "キーが昇順でない"); }
    { Track t = Trk("t_x", TrackType::Property); t.channels.push_back(Ch("v", {})); t.channels.push_back(Ch("v", {})); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "チャンネル名重複"); }
    { Track t = Trk("t_x", TrackType::Property); t.params.Set("path", Str("x")); ExpectOpFails(fx, OpAddTrack{ "b_cam", t, -1 }, "params に予約語"); }
    ExpectOpFails(fx, OpDeleteTrack{ "t_nope" }, "存在しないトラック");
    ExpectOpFails(fx, OpMoveTrack{ "t_fov", "b_nope", 0 }, "移動先バインディングが無い");
    ExpectOpFails(fx, OpMoveTrack{ "t_fov", "b_scene", 9 }, "移動先の添字が範囲外");
    {
        Sequence r = fx;
        Track lin = Trk("t_lin", TrackType::Property);
        lin.path = "X.y";
        lin.channels.push_back(Ch("value", { K(0, 1, Interp::Linear), K(100, 2, Interp::Linear) }));
        CHECK(ApplyOp(r, OpAddTrack{ "b_scene", lin, -1 }).ok);
        TrackHeader h;
        h.path = "X.y";
        h.valueType = ValueType::Bool;
        ExpectOpFails(r, OpSetTrackHeader{ "t_lin", h }, "SetTrackHeader: Bool なのに Step でないキーがある");
    }
    ExpectOpFails(fx, OpAddChannel{ "t_shk", Ch("x", {}), -1 }, "クリップ族にチャンネル");
    ExpectOpFails(fx, OpAddChannel{ "t_tr", Ch("position.x", {}), -1 }, "チャンネル名重複(op)");
    ExpectOpFails(fx, OpAddChannel{ "t_tr", Ch("", {}), -1 }, "空のチャンネル名");
    ExpectOpFails(fx, OpDeleteChannel{ "t_tr", "nope" }, "存在しないチャンネル");
    ExpectOpFails(fx, OpAddKey{ "t_tr", "position.x", K(0, std::nan("")), -1 }, "NaN の値");
    ExpectOpFails(fx, OpAddKey{ "t_tr", "position.x", K(0, std::numeric_limits<double>::infinity()), -1 }, "inf の値");
    ExpectOpFails(fx, OpAddKey{ "t_tr", "position.x", KB(50, 1, -1, 0, 0, 0), -1 }, "負のハンドル dt");
    ExpectOpFails(fx, OpAddKey{ "t_tr", "position.x", K(50, 1), 3 }, "時刻順を壊す挿入位置");
    ExpectOpFails(fx, OpAddKey{ "t_tr", "position.x", K(50, 1), 99 }, "挿入位置が範囲外");
    ExpectOpFails(fx, OpAddKey{ "t_flag", "value", K(100, 1, Interp::Linear), -1 }, "Bool プロパティに Step 以外のキー");
    ExpectOpFails(fx, OpDeleteKey{ "t_tr", "position.x", 7 }, "DeleteKey 範囲外");
    ExpectOpFails(fx, OpDeleteKey{ "t_tr", "position.x", -1 }, "DeleteKey 負の添字");
    ExpectOpFails(fx, OpMoveKey{ "t_tr", "position.x", 0, 5, 2 }, "MoveKey 時刻順を壊す添字");
    ExpectOpFails(fx, OpSetKeyValue{ "t_tr", "position.x", 0, std::nan("") }, "SetKeyValue NaN");
    ExpectOpFails(fx, OpSetInterp{ "t_flag", "value", 0, Interp::Linear, EaseId::Linear }, "SetInterp: Bool を Linear に");
    ExpectOpFails(fx, OpSetTangent{ "t_tr", "position.x", 0, 1, 1, 1, 1 }, "SetTangent: Bezier でないキー");
    ExpectOpFails(fx, OpSetTangent{ "t_fov", "fov", 1, -5, 0, 0, 0 }, "SetTangent 負の dt");
    { Key k = K(1, 5); ExpectOpFails(fx, OpSetKey{ "t_tr", "position.x", 0, k }, "SetKey: 時刻は変えられない"); }
    ExpectOpFails(fx, OpAddClip{ "t_shk", Clp("k_s1", 0, 1), -1 }, "クリップ ID 重複");
    ExpectOpFails(fx, OpAddClip{ "t_shk", Clp("k_z", 0, -5), -1 }, "負の dur");
    ExpectOpFails(fx, OpAddClip{ "t_tr", Clp("k_z", 0, 5), -1 }, "カーブ族にクリップを足す");
    { Clip c = Clp("k_z", 0, 5); c.params.Set("start", Num(1)); ExpectOpFails(fx, OpAddClip{ "t_shk", c, -1 }, "クリップ params の予約語"); }
    ExpectOpFails(fx, OpSetClip{ "t_shk", Clp("k_nope", 0, 1) }, "存在しないクリップの SetClip");
    ExpectOpFails(fx, OpAddEvent{ "t_ev", Evt("e_1", 5), -1 }, "イベント ID 重複");
    { EventItem e = Evt("e_z", 5); e.kind = ""; ExpectOpFails(fx, OpAddEvent{ "t_ev", e, -1 }, "kind が空"); }
    ExpectOpFails(fx, OpAddEvent{ "t_shk", Evt("e_z", 5), -1 }, "クリップ族にイベント");
    { Marker m; m.id = "c_01"; ExpectOpFails(fx, OpAddMarker{ m, -1 }, "マーカー ID が他の種別と重複"); }
    { Cut c; c.id = "c_02"; c.start = 5; c.end = 1; c.camera = "b_cam"; ExpectOpFails(fx, OpAddCut{ c, -1 }, "カット end < start"); }
    { Cut c; c.id = "c_02"; c.start = 1; c.end = 5; c.camera = "b_nope"; ExpectOpFails(fx, OpAddCut{ c, -1 }, "カットの参照先が無い"); }
    ExpectOpFails(fx, OpDeleteCut{ "c_nope" }, "存在しないカット");
    { Sequence r = fx; r.frameRate = 30; SeqValue bad = SeqValue::MakeNumber(1); ExpectOpFails(r, OpSetMeta{ bad }, "meta がオブジェクトでない"); }
    { RenderSettings rs; rs.width = 0; ExpectOpFails(fx, OpSetRender{ rs }, "解像度 0"); }
    { RenderSettings rs; rs.shutter = 2.0; ExpectOpFails(fx, OpSetRender{ rs }, "shutter > 1"); }

    // ---- Bezier への切替は Auto と同じ見た目になる(既定ハンドル)
    {
        Sequence s = fx;
        // position.x を 3 の倍数の区間にして Auto と Bezier を比べる
        for (int i = 0; i < 3; ++i) CHECK(ApplyOp(s, OpSetInterp{ "t_tr", "position.x", i, Interp::Bezier, EaseId::Linear }).ok);
        const Channel& bz = GetTrack(s, "t_tr")->channels[0];
        const Channel& au = GetTrack(fx, "t_tr")->channels[0];
        double worst = 0;
        for (Tick t = 0; t <= 1200; t += 3) worst = std::max(worst, std::fabs(EvalChannel(bz, t) - EvalChannel(au, t)));
        CHECK_MSG(worst < 1e-9, "既定ハンドルの Bezier と Auto の差 %.3g", worst);
    }
    // ---- 移動後の添字・並び
    {
        Sequence s = fx;
        CHECK(ApplyOp(s, OpMoveKey{ "t_tr", "position.x", 0, 900, -1 }).ok);
        const auto& k = GetTrack(s, "t_tr")->channels[0].keys;
        CHECK(k.size() == 3 && k[0].t == 600 && k[1].t == 900 && k[2].t == 1200 && k[1].v == 0.0);
    }
}

void TestTxnAndHistory()
{
    std::printf("TestTxnAndHistory\n");
    Sequence s = MakeFixture();
    const std::string base = Ser(s);
    Marker m1, m2;
    m1.id = "m_10"; m1.t = 1; m2.id = "m_10"; m2.t = 2;      // 2 個目は ID 重複で失敗する
    SeqTxn bad;
    bad.label = "bad";
    bad.ops = { OpAddMarker{ m1, -1 }, OpSetName{ "changed" }, OpAddMarker{ m2, -1 } };
    SeqTxn inv;
    ApplyResult r = ApplyTxn(s, bad, &inv);
    CHECK(!r.ok && Ser(s) == base);                             // 全部巻き戻る
    CHECK(r.error.find("op[2]") != std::string::npos);          // どの op で失敗したか
    // 成功する txn の逆 txn
    m2.id = "m_11";
    SeqTxn good;
    good.label = "good";
    good.ops = { OpAddMarker{ m1, -1 }, OpSetName{ "changed" }, OpAddMarker{ m2, 0 } };
    CHECK(ApplyTxn(s, good, &inv).ok);
    CHECK(s.name == "changed" && s.markers.size() == 3 && s.markers[0].id == "m_11");
    CHECK(inv.ops.size() == 3 && inv.label == "good");
    CHECK(ApplyTxn(s, inv).ok && Ser(s) == base);

    // 履歴
    SeqHistory h;
    CHECK(!h.CanUndo() && !h.CanRedo() && !h.Undo(s).ok && !h.Redo(s).ok);
    SeqTxn a; a.label = "A"; a.ops = { OpSetName{ "A" } };
    SeqTxn b; b.label = "B"; b.ops = { OpSetName{ "B" } };
    SeqTxn c; c.label = "C"; c.ops = { OpSetName{ "C" } };
    CHECK(h.Execute(s, a).ok && h.Execute(s, b).ok);
    CHECK(s.name == "B" && h.UndoCount() == 2 && *h.UndoLabel() == "B");
    CHECK(h.Undo(s).ok && s.name == "A" && h.CanRedo() && *h.RedoLabel() == "B");
    CHECK(h.Redo(s).ok && s.name == "B" && !h.CanRedo());
    CHECK(h.Undo(s).ok && h.Execute(s, c).ok);                  // 新しい操作で Redo は消える
    CHECK(!h.CanRedo() && s.name == "C");
    CHECK(h.Undo(s).ok && h.Undo(s).ok);
    CHECK(Ser(s) == base && !h.CanUndo());
    // 失敗する Execute は履歴に積まない
    SeqTxn failing; failing.ops = { OpDeleteTrack{ "nope" } };
    const std::size_t before = h.UndoCount();
    CHECK(!h.Execute(s, failing).ok && h.UndoCount() == before && Ser(s) == base);
    // ドラッグ(適用済みの変更を 1 ステップにまとめて Record): 途中経過を ApplyTxn で直接動かし、離したときに履歴へ
    {
        Sequence d = MakeFixture();
        const std::string dBase = Ser(d);
        SeqHistory dh;
        SeqTxn drag1, drag2, inv1, inv2;
        drag1.ops = { OpMoveKey{ "t_tr", "position.x", 1, 700, -1 } };
        CHECK(ApplyTxn(d, drag1, &inv1).ok);
        drag2.ops = { OpMoveKey{ "t_tr", "position.x", 1, 800, -1 } };
        CHECK(ApplyTxn(d, drag2, &inv2).ok);
        SeqTxn whole, wholeInv;
        whole.label = "ドラッグ";
        whole.ops = { OpMoveKey{ "t_tr", "position.x", 1, 800, -1 } };            // 開始状態から見た最終結果(= 1 ステップ)
        wholeInv.ops = inv2.ops;
        wholeInv.ops.insert(wholeInv.ops.end(), inv1.ops.begin(), inv1.ops.end());   // 逆順に戻す
        dh.Record(whole, wholeInv);
        CHECK(dh.CanUndo() && *dh.UndoLabel() == "ドラッグ");
        const std::string dAfter = Ser(d);
        CHECK(dh.Undo(d).ok && Ser(d) == dBase);                                     // 1 回の Undo でドラッグ前へ
        CHECK(dh.Redo(d).ok && Ser(d) == dAfter);
    }
    // 上限
    SeqHistory small(3);
    for (int i = 0; i < 10; ++i) { SeqTxn t; t.ops = { OpSetName{ "n" + std::to_string(i) } }; CHECK(small.Execute(s, t).ok); }
    CHECK(small.UndoCount() == 3);
    // 履歴の外で壊された場合は安全に破棄
    SeqHistory h2;
    SeqTxn add; add.ops = { OpAddMarker{ m1, -1 } };
    CHECK(h2.Execute(s, add).ok);
    s.markers.clear();                                          // 外部編集
    CHECK(!h2.Undo(s).ok && !h2.CanUndo() && !h2.CanRedo());
}

// ---------------------------------------------------------------------------
// ランダム操作列ファズ
// ---------------------------------------------------------------------------
struct Gen
{
    Rng rng;
    IdAllocator alloc;
    explicit Gen(std::uint64_t seed) : rng(seed), alloc(seed ^ 0x5A5A5A5Aull) {}

    int R(int n) { return rng.R(n); }
    bool P(int pct) { return rng.P(pct); }

    double Val()
    {
        switch (R(8))
        {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return static_cast<double>(R(2001) - 1000);
        case 3: return (R(200001) - 100000) / 1000.0;
        case 4: return (R(2001) - 1000) / 8.0;
        case 5: return 1e-7 * (R(100) + 1);
        case 6: return 123456789.125 + R(1000);
        default: return R(1000) * 0.001 - 0.5;
        }
    }
    Tick T() { return static_cast<Tick>(R(4001) - 200) * 30 + (P(20) ? R(30) : 0); }
    std::string Word()
    {
        static const char* w[] = { "alpha", "Boss", "quote\"d", "back\\slash", "日本語", "line\nbreak", "tab\t", "a b", "", "x.y", "ctrl\x01z", "/path/" };
        return w[R(12)];
    }
    SeqValue Value(int depth)
    {
        switch (R(depth > 1 ? 4 : 6))
        {
        case 0: return SeqValue::MakeNumber(Val());
        case 1: return SeqValue::MakeBool(P(50));
        case 2: return SeqValue::MakeString(Word());
        case 3: return SeqValue();
        case 4: { SeqValue a = SeqValue::MakeArray(); const int n = R(4); for (int i = 0; i < n; ++i) a.arr.push_back(Value(depth + 1)); return a; }
        default: { SeqValue o = SeqValue::MakeObject(); const int n = R(4); for (int i = 0; i < n; ++i) o.Set("k" + std::to_string(R(6)), Value(depth + 1)); return o; }
        }
    }
    SeqValue Params(const char* const* reserved, int nReserved)
    {
        SeqValue o = SeqValue::MakeObject();
        const int n = R(4);
        for (int i = 0; i < n; ++i) o.Set("p" + std::to_string(R(8)), Value(0));
        if (P(3) && nReserved > 0) o.Set(reserved[R(nReserved)], Value(2));      // たまに予約語(拒否されるべき)
        return o;
    }
    Key RKey(Tick t)
    {
        Key k;
        k.t = t; k.v = Val();
        switch (R(6))
        {
        case 0: k.ip = Interp::Step; break;
        case 1: k.ip = Interp::Linear; break;
        case 2: case 3: k.ip = Interp::Auto; break;
        case 4: k.ip = Interp::Ease; k.ease = static_cast<EaseId>(R(static_cast<int>(EaseId::Count))); break;
        default: k.ip = Interp::Bezier; k.inDt = R(600); k.outDt = R(600); k.inDv = Val(); k.outDv = Val();
                 if (P(3)) k.inDt = -1; break;          // たまに不正(拒否されるべき)
        }
        return k;
    }
    Channel RChannel(bool stepOnly)
    {
        static const char* names[] = { "position.x", "position.y", "position.z", "value", "r", "g", "b", "a", "fov", "scale.x" };
        Channel c;
        c.name = names[R(10)];
        c.pre = static_cast<Extrap>(R(4));
        c.post = static_cast<Extrap>(R(4));
        const int n = R(6);
        Tick t = T();
        for (int i = 0; i < n; ++i)
        {
            Key k = RKey(t);
            if (stepOnly) { k.ip = Interp::Step; }
            c.keys.push_back(k);
            t += R(500) * (P(15) ? 0 : 1);
        }
        return c;
    }
    Track RTrack()
    {
        Track t = Trk(alloc.New('t'), static_cast<TrackType>(R(static_cast<int>(TrackType::Count))));
        if (P(30)) t.name = Word();
        t.mute = P(10); t.lock = P(10);
        static const char* res[] = { "id", "type", "channels", "clips" };
        t.params = Params(res, 4);
        const TrackFamily f = FamilyOf(t.type);
        if (t.type == TrackType::Property)
        {
            t.path = "Comp.field" + std::to_string(R(5));
            t.valueType = static_cast<ValueType>(R(4));
            t.colorSpace = static_cast<ColorSpace>(R(2));
        }
        if (t.type == TrackType::Transform) t.rotation = static_cast<RotationMode>(R(2));
        const bool stepOnly = t.type == TrackType::Property && (t.valueType == ValueType::Bool || t.valueType == ValueType::Int) && !P(5);
        if (f == TrackFamily::Curve)
        {
            const int n = R(4);
            for (int i = 0; i < n; ++i)
            {
                Channel c = RChannel(stepOnly);
                bool dup = false;
                for (const auto& e : t.channels) if (e.name == c.name) dup = true;
                if (!dup) t.channels.push_back(std::move(c));
            }
        }
        else if (f == TrackFamily::Clip)
        {
            const int n = R(3);
            for (int i = 0; i < n; ++i) t.clips.push_back(RClip());
        }
        else if (f == TrackFamily::Event)
        {
            const int n = R(4);
            for (int i = 0; i < n; ++i) t.events.push_back(REvent());
        }
        return t;
    }
    Clip RClip()
    {
        Clip c = Clp(alloc.New('k'), T(), R(2000) - (P(3) ? 2000 : 0));
        static const char* res[] = { "id", "start", "dur" };
        c.params = Params(res, 3);
        return c;
    }
    EventItem REvent()
    {
        EventItem e = Evt(alloc.New('e'), T(), P(50) ? "emit" : "lua", Word());
        if (P(2)) e.kind = "";
        static const char* res[] = { "id", "t", "kind", "name" };
        e.params = Params(res, 4);
        return e;
    }

    // ---- 現在のシーケンスから対象を選ぶ ----
    struct TLoc { int b = -1, t = -1; };
    TLoc PickTrack(const Sequence& s, int familyOrMinus1 = -1)
    {
        std::vector<TLoc> all;
        for (std::size_t b = 0; b < s.bindings.size(); ++b)
            for (std::size_t t = 0; t < s.bindings[b].tracks.size(); ++t)
                if (familyOrMinus1 < 0 || static_cast<int>(FamilyOf(s.bindings[b].tracks[t].type)) == familyOrMinus1)
                    all.push_back({ static_cast<int>(b), static_cast<int>(t) });
        if (all.empty()) return {};
        return all[static_cast<std::size_t>(R(static_cast<int>(all.size())))];
    }
    std::string BindingId(const Sequence& s)
    {
        if (s.bindings.empty() || P(4)) return "b_missing";
        return s.bindings[static_cast<std::size_t>(R(static_cast<int>(s.bindings.size())))].id;
    }
    int Idx(int size, int extra = 0)   // ほぼ有効な添字(たまに範囲外)
    {
        if (P(4)) return R(size + 4) - 2;
        return size > 0 ? R(size + extra) : 0;
    }
    int InsIdx(int size)               // 挿入位置: -1(既定)が多め
    {
        if (P(60)) return -1;
        if (P(5)) return R(size + 6) - 3;
        return R(size + 1);
    }

    int Weight(int kind, int keyCount)
    {
        static const int w[] = { 1, 1, 1, 1, 2, 2, 1, 1, 2, 4, 2, 2, 2, 3, 1, 2, 14, 6, 8, 4, 5, 3, 3, 3, 1, 3, 3, 1, 3, 2, 1, 2, 2, 1, 2 };
        int x = w[kind];
        if (keyCount > 450 && (kind == 17 || kind == 10 || kind == 14 || kind == 6)) x *= 12;
        if (keyCount > 450 && (kind == 16 || kind == 9 || kind == 5 || kind == 13)) x = 0;
        return x;
    }

    SeqOp RandomOp(const Sequence& s, int keyCount)
    {
        int total = 0;
        for (int k = 0; k < 35; ++k) total += Weight(k, keyCount);
        int pick = R(total), kind = 0;
        for (int k = 0; k < 35; ++k) { pick -= Weight(k, keyCount); if (pick < 0) { kind = k; break; } }
        kindPicked = kind;

        switch (kind)
        {
        case 0: return OpSetName{ Word() };
        case 1: return OpSetFrameRate{ P(5) ? 0 : (P(50) ? 24 : 30 + R(31)) };
        case 2: { const Tick a = T(); return OpSetRange{ P(70), a, a + R(20000) - (P(5) ? 30000 : 0) }; }
        case 3: { RenderSettings r; if (P(20)) return OpSetRender{ std::nullopt }; r.width = 1 + R(4000); r.height = P(3) ? 0 : 1 + R(3000); r.shutter = R(11) / 10.0; r.warmup = R(100); return OpSetRender{ r }; }
        case 4: return OpSetMeta{ Value(0) };
        case 5: { Binding b = Bnd(alloc.New('b'), Word(), static_cast<BindingKind>(R(3))); b.hint.guid = P(50) ? "00a1b2c3d4e5f607" : ""; b.hint.path = P(30) ? Word() : "";
                  static const char* res[] = { "id", "tracks", "hint" }; b.params = Params(res, 3);
                  const int n = R(3); for (int i = 0; i < n; ++i) b.tracks.push_back(RTrack());
                  return OpAddBinding{ b, InsIdx(static_cast<int>(s.bindings.size())) }; }
        case 6: return OpDeleteBinding{ BindingId(s) };
        case 7: return OpMoveBinding{ BindingId(s), Idx(static_cast<int>(s.bindings.size())) };
        case 8: { BindingHeader h; h.name = Word(); h.kind = static_cast<BindingKind>(R(3)); h.hint.guid = P(50) ? "abcdef0123456789" : ""; h.hint.path = P(30) ? Word() : ""; h.hint.name = Word();
                  static const char* res[] = { "id", "tracks", "name" }; h.params = Params(res, 3);
                  return OpSetBindingHeader{ BindingId(s), h }; }
        case 9: { const std::string bid = BindingId(s); int n = 0; const int bi = FindBinding(s, bid); if (bi >= 0) n = static_cast<int>(s.bindings[static_cast<std::size_t>(bi)].tracks.size());
                  return OpAddTrack{ bid, RTrack(), InsIdx(n) }; }
        case 10: { const TLoc l = PickTrack(s); return OpDeleteTrack{ l.b < 0 || P(3) ? "t_missing" : s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)].id }; }
        case 11: { const TLoc l = PickTrack(s); const std::string dst = BindingId(s); int n = 0; const int di = FindBinding(s, dst); if (di >= 0) n = static_cast<int>(s.bindings[static_cast<std::size_t>(di)].tracks.size());
                   return OpMoveTrack{ l.b < 0 ? "t_missing" : s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)].id, dst, Idx(n, 1) }; }
        case 12: { const TLoc l = PickTrack(s); TrackHeader h; h.name = Word(); h.mute = P(30); h.lock = P(30); h.path = P(60) ? "Comp.f" : ""; h.valueType = static_cast<ValueType>(P(90) ? 0 : R(4)); h.colorSpace = static_cast<ColorSpace>(R(2)); h.rotation = static_cast<RotationMode>(R(2));
                   static const char* res[] = { "id", "channels", "path" }; h.params = Params(res, 3);
                   return OpSetTrackHeader{ l.b < 0 ? "t_missing" : s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)].id, h }; }
        case 13: { const TLoc l = PickTrack(s, 0); if (l.b < 0) return OpAddChannel{ "t_missing", RChannel(false), -1 };
                   const Track& t = s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)];
                   return OpAddChannel{ t.id, RChannel(t.type == TrackType::Property && (t.valueType == ValueType::Bool || t.valueType == ValueType::Int)), InsIdx(static_cast<int>(t.channels.size())) }; }
        case 14: { const TLoc l = PickTrack(s, 0); if (l.b < 0 || s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)].channels.empty()) return OpDeleteChannel{ "t_missing", "x" };
                   const Track& t = s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)];
                   return OpDeleteChannel{ t.id, t.channels[static_cast<std::size_t>(R(static_cast<int>(t.channels.size())))].name }; }
        case 15: { const TLoc l = PickTrack(s, 0); if (l.b < 0 || s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)].channels.empty()) return OpSetChannelExtrap{ "t_missing", "x", Extrap::Hold, Extrap::Hold };
                   const Track& t = s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)];
                   return OpSetChannelExtrap{ t.id, t.channels[static_cast<std::size_t>(R(static_cast<int>(t.channels.size())))].name, static_cast<Extrap>(R(4)), static_cast<Extrap>(R(4)) }; }
        default: break;
        }
        if (kind >= 16 && kind <= 22)
        {
            // キー系: 実在するチャンネルを選ぶ(無ければ空振り = 失敗するはずの op)
            std::vector<std::pair<const Track*, const Channel*>> chs;
            for (const auto& b : s.bindings) for (const auto& t : b.tracks) for (const auto& c : t.channels) chs.push_back({ &t, &c });
            if (chs.empty()) return OpDeleteKey{ "t_missing", "x", 0 };
            const auto pc = chs[static_cast<std::size_t>(R(static_cast<int>(chs.size())))];
            const Track& t = *pc.first;
            const Channel& c = *pc.second;
            const int n = static_cast<int>(c.keys.size());
            const bool stepOnly = t.type == TrackType::Property && (t.valueType == ValueType::Bool || t.valueType == ValueType::Int);
            switch (kind)
            {
            case 16: { Key k = RKey(P(60) ? T() : (n > 0 ? c.keys[static_cast<std::size_t>(R(n))].t : T())); if (stepOnly && !P(3)) k.ip = Interp::Step; return OpAddKey{ t.id, c.name, k, InsIdx(n) }; }
            case 17: return OpDeleteKey{ t.id, c.name, Idx(n) };
            case 18: return OpMoveKey{ t.id, c.name, Idx(n), P(30) && n > 0 ? c.keys[static_cast<std::size_t>(R(n))].t : T(), P(70) ? -1 : Idx(n) };
            case 19: return OpSetKeyValue{ t.id, c.name, Idx(n), P(2) ? std::nan("") : Val() };
            case 20: { Interp ip = static_cast<Interp>(R(5)); if (stepOnly && !P(3)) ip = Interp::Step; return OpSetInterp{ t.id, c.name, Idx(n), ip, static_cast<EaseId>(R(static_cast<int>(EaseId::Count))) }; }
            case 21: return OpSetTangent{ t.id, c.name, Idx(n), R(500) - (P(3) ? 500 : 0), Val(), R(500), Val() };
            default: { const int i = Idx(n); Key k = RKey(i >= 0 && i < n && !P(4) ? c.keys[static_cast<std::size_t>(i)].t : T()); if (stepOnly && !P(3)) k.ip = Interp::Step; return OpSetKey{ t.id, c.name, i, k }; }
            }
        }
        if (kind >= 23 && kind <= 25)
        {
            const TLoc l = PickTrack(s, static_cast<int>(TrackFamily::Clip));
            if (l.b < 0) return OpAddClip{ "t_missing", RClip(), -1 };
            const Track& t = s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)];
            if (kind == 23) return OpAddClip{ t.id, RClip(), InsIdx(static_cast<int>(t.clips.size())) };
            if (t.clips.empty()) return OpDeleteClip{ t.id, "k_missing" };
            const Clip& pc = t.clips[static_cast<std::size_t>(R(static_cast<int>(t.clips.size())))];
            if (kind == 24) return OpDeleteClip{ t.id, pc.id };
            Clip c = RClip();
            c.id = P(95) ? pc.id : c.id;
            return OpSetClip{ t.id, c };
        }
        if (kind >= 26 && kind <= 28)
        {
            const TLoc l = PickTrack(s, static_cast<int>(TrackFamily::Event));
            if (l.b < 0) return OpAddEvent{ "t_missing", REvent(), -1 };
            const Track& t = s.bindings[static_cast<std::size_t>(l.b)].tracks[static_cast<std::size_t>(l.t)];
            if (kind == 26) return OpAddEvent{ t.id, REvent(), InsIdx(static_cast<int>(t.events.size())) };
            if (t.events.empty()) return OpDeleteEvent{ t.id, "e_missing" };
            const EventItem& pe = t.events[static_cast<std::size_t>(R(static_cast<int>(t.events.size())))];
            if (kind == 27) return OpDeleteEvent{ t.id, pe.id };
            EventItem e = REvent();
            e.id = P(95) ? pe.id : e.id;
            return OpSetEvent{ t.id, e };
        }
        if (kind == 29) { Marker m; m.id = alloc.New('m'); m.t = T(); m.name = Word(); static const char* res[] = { "id", "t", "name" }; m.params = Params(res, 3); return OpAddMarker{ m, InsIdx(static_cast<int>(s.markers.size())) }; }
        if (kind == 30) return OpDeleteMarker{ s.markers.empty() || P(4) ? "m_missing" : s.markers[static_cast<std::size_t>(R(static_cast<int>(s.markers.size())))].id };
        if (kind == 31) { Marker m; m.id = s.markers.empty() || P(4) ? "m_missing" : s.markers[static_cast<std::size_t>(R(static_cast<int>(s.markers.size())))].id; m.t = T(); m.name = Word(); m.params = Params(nullptr, 0); return OpSetMarker{ m }; }
        if (kind == 32) { Cut c; c.id = alloc.New('c'); c.start = T(); c.end = c.start + R(5000) - (P(3) ? 5000 : 0); c.camera = BindingId(s); c.blend = R(100); return OpAddCut{ c, InsIdx(static_cast<int>(s.cuts.size())) }; }
        if (kind == 33) return OpDeleteCut{ s.cuts.empty() || P(4) ? "c_missing" : s.cuts[static_cast<std::size_t>(R(static_cast<int>(s.cuts.size())))].id };
        Cut c; c.id = s.cuts.empty() || P(4) ? "c_missing" : s.cuts[static_cast<std::size_t>(R(static_cast<int>(s.cuts.size())))].id; c.start = T(); c.end = c.start + R(5000); c.camera = BindingId(s); c.blend = R(100);
        return OpSetCut{ c };
    }
    int kindPicked = 0;
};

int CountKeys(const Sequence& s)
{
    int n = 0;
    for (const auto& b : s.bindings) for (const auto& t : b.tracks) for (const auto& c : t.channels) n += static_cast<int>(c.keys.size());
    return n;
}

struct FuzzResult
{
    std::uint64_t finalHash = 0;
    std::string finalText;
    int ok = 0, failed = 0;
};

FuzzResult RunFuzz(std::uint64_t seed, int iterations, bool verifyUndoRedo)
{
    FuzzResult fr;
    Gen g(seed);
    Sequence seq = MakeFixture();
    ReserveAllIds(seq, g.alloc);
    SeqHistory hist(0);
    const std::string initialText = Ser(seq);
    std::vector<std::uint64_t> hashes{ Fnv1a64(initialText) };   // hashes[j] = j 回成功した後の状態のハッシュ(0 = 採っていない)
    int opOk[35] = {};
    int roundTrips = 0;

    for (int i = 0; i < iterations; ++i)
    {
        SeqTxn txn;
        txn.label = "fuzz" + std::to_string(i);
        std::vector<int> kinds;
        const int n = 1 + g.R(3);
        const int keyCount = CountKeys(seq);
        for (int j = 0; j < n; ++j)
        {
            txn.ops.push_back(g.RandomOp(seq, keyCount));
            kinds.push_back(g.kindPicked);
        }
        // 重い検査(シリアライズ)は間引く: 拒否された txn の不変性は 1/3 の反復で(直前のコピーと比べる)、
        // 各段の状態ハッシュは 4 段に 1 回と最後の 100 反復で採る。最終状態のバイト一致は全段階の後で必ず検査する。
        Sequence pre;
        const bool checkReject = verifyUndoRedo && (i % 3 == 0);
        if (checkReject) pre = seq;
        const ApplyResult r = hist.Execute(seq, txn);
        if (r.ok)
        {
            ++fr.ok;
            for (int k : kinds) ++opOk[k];
            const bool sample = verifyUndoRedo && (hashes.size() % 4 == 0 || i >= iterations - 100);
            hashes.push_back(sample ? Hash(seq) : 0);
        }
        else
        {
            ++fr.failed;
            if (checkReject) CHECK_MSG(Ser(seq) == Ser(pre), "iter %d: 失敗した txn がシーケンスを変えた: %s", i, r.error.c_str());
        }
        // 途中で「シリアライズ → パース」に差し替える(履歴の逆 op は添字と ID で動くので有効なまま)
        if (verifyUndoRedo && i % 97 == 96)
        {
            const std::string text = Ser(seq);
            Sequence parsed;
            const ParseResult pr = ParseSequence(text, parsed);
            CHECK_MSG(pr.ok, "iter %d: 往復のパースに失敗: %s", i, pr.error.c_str());
            if (pr.ok)
            {
                CHECK_MSG(Ser(parsed) == text, "iter %d: 往復でバイト不一致", i);
                seq = std::move(parsed);
                ++roundTrips;
            }
        }
        if (verifyUndoRedo && i % 250 == 0) CHECK_MSG(CheckSequenceStructure(seq).empty(), "iter %d: %s", i, CheckSequenceStructure(seq).c_str());
    }
    fr.finalText = Ser(seq);
    fr.finalHash = Fnv1a64(fr.finalText);
    if (!verifyUndoRedo) return fr;

    CHECK(CheckSequenceStructure(seq).empty());
    CHECK(fr.ok > iterations / 3);                                   // 十分な割合で成功している
    CHECK(hist.UndoCount() == static_cast<std::size_t>(fr.ok));
    // 全 op 種別が最低 1 回は成功している(ファズが偏っていない)
    if (iterations >= 5000)
        for (int k = 0; k < 35; ++k) CHECK_MSG(opOk[k] > 0, "op 種別 %d が一度も成功していない", k);
    std::printf("  fuzz: %d 成功 / %d 失敗(拒否) / 往復 %d 回 / 最終キー数 %d / 最終サイズ %zu bytes\n", fr.ok, fr.failed, roundTrips,
                CountKeys(seq), fr.finalText.size());

    // 全 Undo: 1 段ごとに直前の状態と一致
    for (std::size_t k = hist.UndoCount(); k > 0; --k)
    {
        const ApplyResult r = hist.Undo(seq);
        if (!r.ok) { CHECK_MSG(false, "Undo 失敗 (k=%zu): %s", k, r.error.c_str()); break; }
        if (hashes[k - 1] != 0 && Hash(seq) != hashes[k - 1]) { CHECK_MSG(false, "Undo %zu 段目で状態が一致しない", k); break; }
    }
    CHECK(Ser(seq) == initialText);                                  // ★初期状態に厳密一致(バイト)
    CHECK(!hist.CanUndo() && hist.RedoCount() == static_cast<std::size_t>(fr.ok));
    // 全 Redo: 1 段ごとに再現
    for (std::size_t k = 1; hist.CanRedo(); ++k)
    {
        if (!hist.CanRedo()) break;
        const ApplyResult r = hist.Redo(seq);
        if (!r.ok) { CHECK_MSG(false, "Redo 失敗 (k=%zu): %s", k, r.error.c_str()); break; }
        if (hashes[k] != 0 && Hash(seq) != hashes[k]) { CHECK_MSG(false, "Redo %zu 段目で状態が一致しない", k); break; }
    }
    CHECK(Ser(seq) == fr.finalText);                                 // ★Redo で最終状態を厳密に再現
    // さらに、途中まで Undo → 新規操作 → Redo が消える → もう一度全 Undo でも初期に戻る
    for (int k = 0; k < 300 && hist.CanUndo(); ++k) CHECK(hist.Undo(seq).ok);
    SeqTxn extra;
    extra.ops = { OpSetName{ "after-undo" } };
    CHECK(hist.Execute(seq, extra).ok && !hist.CanRedo());
    while (hist.CanUndo()) { if (!hist.Undo(seq).ok) { CHECK(false); break; } }
    CHECK(Ser(seq) == initialText);
    return fr;
}

void TestUndoFuzz()
{
    // 既定 10,000 回。SEQUENCER_FUZZ_ITERATIONS で減らせる(ASAN + Debug など遅い環境向け。既定の合否基準は 10,000)
    int iterations = 10000;
    if (const char* env = std::getenv("SEQUENCER_FUZZ_ITERATIONS")) iterations = std::max(100, std::atoi(env));
    std::printf("TestUndoFuzz (%d ops)\n", iterations);
    const FuzzResult a = RunFuzz(0xF00DFACEull, iterations, true);
    (void)a;
    // 決定論: 同じ seed ならバイト単位で同じ最終状態(ID の発行も含めて再現する)
    const FuzzResult b1 = RunFuzz(0x1234ABCDull, 1500, false);
    const FuzzResult b2 = RunFuzz(0x1234ABCDull, 1500, false);
    CHECK(b1.finalText == b2.finalText && b1.ok == b2.ok);
    const FuzzResult c = RunFuzz(0x1234ABCEull, 1500, false);
    CHECK(c.finalText != b1.finalText);
}

// ===========================================================================
// 8. シリアライズ
// ===========================================================================
Sequence MakeGoldenSequence()
{
    Sequence s;
    s.id = "q_00000001";
    s.name = "GoldenShot";
    s.frameRate = 30;
    s.hasRange = true; s.rangeStart = 0; s.rangeEnd = 27000;
    RenderSettings rs;
    s.render = rs;
    s.meta = SeqValue::MakeObject();
    s.meta.Set("author", Str("tests"));
    {
        SeqValue notes = SeqValue::MakeArray();
        notes.arr = { Str("a"), Num(1.5), SeqValue::MakeBool(true), SeqValue() };
        s.meta.Set("notes", notes);
    }

    Binding cam = Bnd("b_cam", "CutsceneCam");
    cam.hint.guid = "00a1b2c3d4e5f607";
    cam.hint.path = "Rig/CutsceneCam";
    {
        Track tr = Trk("t_tr", TrackType::Transform);
        tr.channels.push_back(Ch("position.x", { K(0, 0), K(19200, 0) }));
        tr.channels.push_back(Ch("position.y", { K(0, 6), KE(19200, 2.2, EaseId::InOutQuad) }, Extrap::Hold, Extrap::PingPong));
        tr.channels.push_back(Ch("position.z", { K(0, 14), KB(19200, 6, 400, 0, 400, 0) }));
        tr.channels.push_back(Ch("rotation.y", { K(0, 0, Interp::Linear), K(6000, 90, Interp::Step), K(12000, 45, Interp::Linear) }, Extrap::Loop, Extrap::Linear));
        cam.tracks.push_back(tr);
        std::vector<std::pair<Tick, Quat>> qk = { { 0, Quat{} }, { 9000, QAxis(0, 1, 0, 90) } };
        Track q = QuatTrack("t_q", qk, Interp::Auto);
        q.name = "QuatRot";
        cam.tracks.push_back(q);
        Track fov = Trk("t_fov", TrackType::Camera);
        fov.channels.push_back(Ch("fov", { K(0, 60, Interp::Linear), K(19200, 42) }));
        fov.channels.push_back(Ch("dofAperture", { K(0, 2.8, Interp::Step) }));
        SeqValue focus = SeqValue::MakeObject();
        focus.Set("binding", Str("b_boss"));
        fov.params.Set("focus", focus);
        cam.tracks.push_back(fov);
        Track aim = Trk("t_aim", TrackType::Aim);
        SeqValue tgt = SeqValue::MakeObject();
        tgt.Set("binding", Str("b_boss"));
        aim.params.Set("target", tgt);
        SeqValue off = SeqValue::MakeArray();
        off.arr = { Num(0), Num(1.2), Num(0) };
        aim.params.Set("offset", off);
        aim.params.Set("roll", Num(0));
        cam.tracks.push_back(aim);
        Track shk = Trk("t_shk", TrackType::Shake);
        shk.mute = true; shk.lock = true;
        Clip c = Clp("k_s1", 15600, 4200);
        c.params.Set("amp", Num(0.35)); c.params.Set("freq", Num(26)); c.params.Set("seed", Num(7));
        shk.clips.push_back(c);
        cam.tracks.push_back(shk);
        Track col = Trk("t_col", TrackType::Property);
        col.path = "PointLight.color";
        col.valueType = ValueType::Color;
        col.colorSpace = ColorSpace::Srgb;
        col.channels.push_back(Ch("r", { K(0, 1, Interp::Linear), K(3000, 0.25, Interp::Linear) }));
        col.channels.push_back(Ch("g", { K(0, 0.5, Interp::Linear) }));
        col.channels.push_back(Ch("b", { K(0, 0, Interp::Linear) }));
        col.channels.push_back(Ch("a", { K(0, 1, Interp::Step) }));
        cam.tracks.push_back(col);
        Track vis = Trk("t_vis", TrackType::Property);
        vis.path = "MeshRenderer.visible";
        vis.valueType = ValueType::Bool;
        vis.channels.push_back(Ch("value", { K(0, 1, Interp::Step), K(6000, 0, Interp::Step) }));
        cam.tracks.push_back(vis);
    }
    s.bindings.push_back(cam);
    Binding cam2 = Bnd("b_cam2", "Cam \"Two\"\n日本語");
    s.bindings.push_back(cam2);
    Binding boss = Bnd("b_boss", "Boss");
    boss.hint.guid = "0123456789abcdef";
    {
        Track an = Trk("t_anim", TrackType::AnimationClip);
        Clip c = Clp("k_a1", 0, 12000);
        c.params.Set("clip", Str("Idle")); c.params.Set("offset", Num(0)); c.params.Set("rate", Num(1)); c.params.Set("loop", SeqValue::MakeBool(true));
        c.params.Set("blendIn", Num(0)); c.params.Set("blendOut", Num(600));
        an.clips.push_back(c);
        boss.tracks.push_back(an);
        Track au = Trk("t_aud", TrackType::Audio);
        Clip a = Clp("k_au", 2400, 0);
        a.params.Set("path", Str("audio/boss_theme.wav")); a.params.Set("bus", Str("music")); a.params.Set("volume", Num(0.8));
        au.clips.push_back(a);
        boss.tracks.push_back(au);
        Track vf = Trk("t_vfx", TrackType::VfxSpawn);
        Clip v = Clp("k_vf", 15600, 0);
        v.params.Set("mode", Str("burst")); v.params.Set("preset", Str("explosion")); v.params.Set("scale", Num(1.4));
        vf.clips.push_back(v);
        boss.tracks.push_back(vf);
        Track sub = Trk("t_sub", TrackType::Subsequence);
        Clip sc = Clp("k_sub", 3000, 6000);
        sc.params.Set("sequence", Str("sequences/inner.dxseq")); sc.params.Set("rate", Num(0.5));
        sub.clips.push_back(sc);
        boss.tracks.push_back(sub);
    }
    s.bindings.push_back(boss);
    Binding sc = Bnd("b_scene", "Scene", BindingKind::Scene);
    sc.hint = BindingHint{};
    {
        Track post = Trk("t_post", TrackType::Post);
        post.channels.push_back(Ch("bloom", { K(15600, 0.3, Interp::Linear), K(18000, 0.9, Interp::Linear) }));
        sc.tracks.push_back(post);
        Track ts = Trk("t_ts", TrackType::TimeScale);
        ts.channels.push_back(Ch("value", { K(15600, 0.25, Interp::Step), KE(18600, 1, EaseId::OutQuad) }));
        sc.tracks.push_back(ts);
        Track light = Trk("t_light", TrackType::Light);
        light.channels.push_back(Ch("intensity", { K(0, 3, Interp::Linear), K(6000, 12, Interp::Linear) }));
        sc.tracks.push_back(light);
        Track ev = Trk("t_ev", TrackType::Event);
        EventItem e1 = Evt("e_1", 26400, "emit", "bossFightStart");
        SeqValue data = SeqValue::MakeObject();
        data.Set("value", Num(1));
        e1.params.Set("data", data);
        ev.events.push_back(e1);
        EventItem e2 = Evt("e_2", 1200, "lua", "OnBossReveal");
        SeqValue args = SeqValue::MakeArray();
        args.arr = { Num(1), Str("x") };
        e2.params.Set("args", args);
        ev.events.push_back(e2);
        sc.tracks.push_back(ev);
    }
    s.bindings.push_back(sc);
    Cut c1;
    c1.id = "c_01"; c1.start = 0; c1.end = 12000; c1.camera = "b_cam";
    Cut c2;
    c2.id = "c_02"; c2.start = 12000; c2.end = 27000; c2.camera = "b_cam2"; c2.blend = 300;
    s.cuts = { c1, c2 };
    Marker m;
    m.id = "m_01"; m.t = 15600; m.name = "impact";
    m.params.Set("color", Str("red"));
    s.markers.push_back(m);
    Marker m2;
    m2.id = "m_02"; m2.t = 0; m2.name = "";
    s.markers.push_back(m2);
    return s;
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
void WriteFile(const std::filesystem::path& p, const std::string& s)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

// ゴールデン: 正準形のテキストが tests/data/sequencer に固定されている。Serialize(Parse(golden)) == golden(バイト一致)。
void CheckGolden(const std::string& file, const Sequence& built)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path(DX12E_SEQUENCER_DATA_DIR) / file;
    const std::string canon = Ser(built);
    if (std::getenv("SEQUENCER_UPDATE_GOLDEN") != nullptr)
    {
        WriteFile(path, canon);
        std::printf("  [golden] wrote %s\n", path.string().c_str());
    }
    const std::string golden = ReadFile(path);
    CHECK_MSG(!golden.empty(), "%s が読めない(SEQUENCER_UPDATE_GOLDEN=1 で生成)", file.c_str());
    CHECK_MSG(golden == canon, "%s が組み立て結果と一致しない(SEQUENCER_UPDATE_GOLDEN=1 で書き直す)", file.c_str());
    Sequence parsed;
    const ParseResult pr = ParseSequence(golden, parsed);
    CHECK_MSG(pr.ok, "%s: %s", file.c_str(), pr.error.c_str());
    CHECK_MSG(pr.warnings.empty(), "%s: 警告が出た", file.c_str());
    if (pr.ok) CHECK_MSG(Ser(parsed) == golden, "%s: Serialize(Parse(golden)) != golden", file.c_str());   // ★バイト一致
}

void TestSerialize()
{
    std::printf("TestSerialize\n");
    const Sequence g = MakeGoldenSequence();
    CHECK_MSG(CheckSequenceStructure(g).empty(), "%s", CheckSequenceStructure(g).c_str());
    CheckGolden("golden_camera_shot.dxseq", g);

    const std::string text = Ser(g);
    CHECK(!text.empty() && text.back() == '\n' && text.find('\r') == std::string::npos);
    CHECK(text.find("\"format\": \"dxseq\"") != std::string::npos);
    CHECK(text.find("[0, 6, \"a\"]") != std::string::npos);                      // 1 キー 1 行のタプル
    CHECK(text.find("[19200, 6, \"b\", 400, 0, 400, 0]") != std::string::npos);
    CHECK(text.find("\"e:inOutQuad\"") != std::string::npos);
    // 往復
    Sequence p;
    ParseResult pr = ParseSequence(text, p);
    CHECK(pr.ok && pr.warnings.empty());
    CHECK(Ser(p) == text);
    CHECK(Hash(p) == Hash(g));
    // 評価も一致(モデルが同じ)
    for (Tick t : { -10, 0, 1000, 5000, 19200, 30000 }) CHECK(BitEqual(Evaluate(p, t), Evaluate(g, t)));

    // 数値の書式: 最短の往復表現。0.1 → "0.1"、-0 → "0"
    Sequence n = NewSeq();
    Binding b = Bnd("b_n", "N");
    Track t = Trk("t_n", TrackType::Property);
    t.path = "A.b";
    t.channels.push_back(Ch("value", { K(0, 0.1), K(1, -0.0), K(2, 1e-7), K(3, 123456789.125), K(4, 1e21), K(5, 0.30000000000000004), K(6, -2.5e-300) }));
    b.tracks.push_back(t);
    n.bindings.push_back(b);
    const std::string nt = Ser(n);
    CHECK(nt.find("[0, 0.1, \"a\"]") != std::string::npos);
    CHECK(nt.find("[1, 0, \"a\"]") != std::string::npos);
    Sequence np;
    CHECK(ParseSequence(nt, np).ok && Ser(np) == nt);
    const auto& keys = np.bindings[0].tracks[0].channels[0].keys;
    CHECK(keys[0].v == 0.1 && keys[2].v == 1e-7 && keys[3].v == 123456789.125 && keys[4].v == 1e21 && keys[5].v == 0.30000000000000004 && keys[6].v == -2.5e-300);
    CHECK(std::signbit(keys[1].v) == false);

    // 正準でない入力は正準形へ直る(冪等): "e:linear" → "l"、キー順、未知フィールド
    const std::string messy = R"({
      "format":"dxseq","version":1,"name":"Messy","ticksPerSecond":6000,"frameRate":30,"unknownRoot":1,
      "cuts":[],"markers":[],
      "bindings":[{"id":"b_a","name":"A","kind":"entity","zzz":5,
        "tracks":[{"id":"t_a","type":"property","path":"X.y","custom":{"b":2,"a":[1,2]},
          "channels":{"v":{"keys":[[200,2,"e:linear"],[100,1],[100,1.5,"s"]],"extra":1}}}]}]})";
    Sequence mp;
    pr = ParseSequence(messy, mp);
    CHECK_MSG(pr.ok, "%s", pr.error.c_str());
    CHECK(pr.warnings.size() == 3);                                  // unknownRoot / channel.extra / 並べ替え(binding の未知フィールドは params に保持)
    const Channel& mc = mp.bindings[0].tracks[0].channels[0];
    CHECK(mc.keys.size() == 3 && mc.keys[0].t == 100 && mc.keys[0].v == 1 && mc.keys[1].t == 100 && mc.keys[1].v == 1.5 && mc.keys[2].t == 200);   // 安定ソート
    CHECK(mc.keys[0].ip == Interp::Auto && mc.keys[2].ip == Interp::Linear);                                          // 既定 / e:linear → Linear
    const std::string canon = Ser(mp);
    Sequence mp2;
    CHECK(ParseSequence(canon, mp2).ok && Ser(mp2) == canon && ParseSequence(canon, mp2).warnings.empty());          // 冪等
    CHECK(canon.find("\"custom\": {\"a\": [1, 2], \"b\": 2}") != std::string::npos);                                  // 未知の track フィールドは params に保持・辞書順

    // エラー(位置つきの日本語)
    auto expectErr = [](const std::string& json, const char* needle, const char* label) {
        Sequence x;
        const ParseResult r = ParseSequence(json, x);
        CHECK_MSG(!r.ok && r.error.find(needle) != std::string::npos, "%s: error=\"%s\"", label, r.error.c_str());
    };
    const std::string head = R"({"format":"dxseq","version":1,"name":"E","ticksPerSecond":6000,"frameRate":30,"cuts":[],"markers":[],"bindings":[)";
    expectErr("not json", "JSON", "JSON でない");
    expectErr("[]", "オブジェクト", "配列ルート");
    expectErr(R"({"format":"other","version":1})", "dxseq", "format");
    expectErr(R"({"format":"dxseq","version":2,"bindings":[]})", "バージョン", "未対応バージョン");
    expectErr(R"({"format":"dxseq"})", "version", "version 欠落");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"nope"}]}]})", "tracks[0].type", "未知のトラック種別");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"property","channels":{"v":{"keys":[[0,1,"zz"]]}}}]}]})", "channels.v.keys[0][2]", "未知の補間");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"property","channels":{"v":{"keys":[[0,1,"b",1,2]]}}}]}]})", "ベジェ", "ベジェの要素数");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"property","channels":{"v":{"keys":[[0.5,1]]}}}]}]})", "整数", "小数の時刻");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"property","channels":{"v":{"keys":[[0,1,"e:nope"]]}}}]}]})", "イージング", "未知のイージング名");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"transform"}]},{"id":"b_a","tracks":[]}]})", "重複", "ID 重複");
    expectErr(head + R"({"id":"b_a","tracks":[{"id":"t_a","type":"shake","channels":{"v":{"keys":[]}}}]}]})", "チャンネル", "族の不整合");
    expectErr(head + R"({"id":"bad id"}]})", "ID", "ID 形式");
    expectErr(R"({"format":"dxseq","version":1,"name":"E","ticksPerSecond":6000,"frameRate":30,"cuts":[{"id":"c_1","start":0,"end":5,"camera":"b_nope"}],"markers":[],"bindings":[]})",
              "b_nope", "カットの参照先");
    expectErr(R"({"format":"dxseq","version":1,"name":"E","ticksPerSecond":0,"frameRate":30,"cuts":[],"markers":[],"bindings":[]})", "ticksPerSecond", "tps=0");
    expectErr(R"({"format":"dxseq","version":1,"name":"E","ticksPerSecond":6000,"frameRate":0,"cuts":[],"markers":[],"bindings":[]})", "frameRate", "fps=0");
    expectErr(R"({"format":"dxseq","version":1,"name":"E","ticksPerSecond":6000,"frameRate":30,"range":[5,1],"cuts":[],"markers":[],"bindings":[]})", "range", "range 逆転");
}

void TestSerializeFile()
{
    std::printf("TestSerializeFile\n");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dx12e_seq_test";
    fs::create_directories(dir);
    const std::string path = (dir / "a.dxseq").string();
    const Sequence g = MakeGoldenSequence();
    std::string err;
    CHECK(SaveSequenceFile(path, g, &err));
    CHECK(SaveSequenceFile(path, g, &err));                          // 上書き(リネームで置換)
    CHECK(!fs::exists(path + ".tmp"));
    Sequence l;
    CHECK(LoadSequenceFile(path, l).ok && Ser(l) == Ser(g));
    CHECK(!LoadSequenceFile((dir / "nope.dxseq").string(), l).ok);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ===========================================================================
// 9. 旧 sequence_author 台本の変換
// ===========================================================================
const char* kLegacyBossReveal = R"({
  "name": "BossReveal",
  "camera": "CutsceneCam",
  "tracks": [
    { "t": 0.0, "type": "fade", "to": "clear", "dur": 0.8 },
    { "t": 0.0, "type": "camera", "from": [0, 6, 14], "to": [0, 2.2, 6], "lookAtName": "Boss", "dur": 3.2, "ease": "inOut" },
    { "t": 0.4, "type": "sound", "path": "audio/boss_theme.wav", "bgm": true },
    { "t": 2.6, "type": "vfx", "preset": "explosion", "atName": "Boss", "scale": 1.4 },
    { "t": 2.8, "type": "vfxPlay", "target": "FX_BossAura" },
    { "t": 2.6, "type": "shake", "amp": 0.35, "freq": 26, "dur": 0.7 },
    { "t": 2.6, "type": "timeScale", "value": 0.25 },
    { "t": 3.1, "type": "timeScale", "value": 1.0, "dur": 0.4 },
    { "t": 3.2, "type": "post", "set": { "saturation": 1.35, "contrast": 1.2 }, "dur": 1.0 },
    { "t": 4.4, "type": "event", "name": "bossFightStart" }
  ]
})";

const char* kLegacyProps = R"({
  "name": "PropsShow", "camera": "MainCam", "loop": true, "doneEvent": "PropsShow:end",
  "tracks": [
    { "t": 0, "type": "camera", "to": [0, 3, 10], "from": [0, 3, 20], "lookAt": [0, 1, 0], "dur": 2, "ease": "out" },
    { "t": 2.5, "type": "camera", "to": [5, 3, 10], "lookAtName": "Statue", "lookAt": [0, 0, 0], "dur": 1.5 },
    { "t": 0.5, "type": "move", "target": "Statue", "to": [0, 1, 0], "from": [0, 0, 0], "dur": 1, "ease": "outBack" },
    { "t": 1.0, "type": "rotate", "target": "Statue", "to": [0, 180, 0], "dur": 2 },
    { "t": 0, "type": "shaderParam", "target": "Statue", "param": "p1", "to": 1, "from": 0, "dur": 3, "ease": "linear" },
    { "t": 1, "type": "light", "target": "Lamp", "intensity": 8, "color": [1, 0.5, 0.2], "dur": 1 },
    { "t": 1.5, "type": "vfxPlay", "target": "Sparks", "layer": "core" },
    { "t": 3.5, "type": "vfxStop", "target": "Sparks", "layer": "core" },
    { "t": 1.2, "type": "sound", "path": "audio/hit.wav", "volume": 0.7 },
    { "t": 0.2, "type": "sound", "path": "audio/bgm.ogg", "bgm": true, "loop": false },
    { "t": 4, "type": "vfx", "preset": "sparks", "at": [1, 2, 3] },
    { "t": 4.2, "type": "log", "text": "done \"quoted\"" },
    { "t": 4.5, "type": "scene", "path": "scenes/next.json", "fade": 1.2 },
    { "t": 0, "type": "fade", "to": "white", "dur": 0.3 },
    { "t": 0.3, "type": "fade", "to": "clear", "dur": 0.5 }
  ]
})";

// 旧 Lua の EASE と同じ式(sequence.ts の EASE_LUA を C++ に写したもの)
double LegacyEase(const std::string& name, double k)
{
    if (name == "linear") return k;
    if (name == "in") return k * k;
    if (name == "out") return 1 - (1 - k) * (1 - k);
    if (name == "inOut") return k < 0.5 ? 2 * k * k : 1 - std::pow(-2 * k + 2, 2) / 2;
    if (name == "outBack") { const double c = 1.70158, f = k - 1; return 1 + (c + 1) * f * f * f + c * f * f; }
    return k;
}

double SampleOf(const Sequence& s, Tick t, const std::string& binding, TrackType type, const std::string& path, const std::string& channel)
{
    const EvalResult r = Evaluate(s, t);
    for (const auto& c : r.channels)
    {
        const Binding& b = s.bindings[static_cast<std::size_t>(c.binding)];
        const Track& tr = b.tracks[static_cast<std::size_t>(c.track)];
        if (b.name == binding && tr.type == type && tr.path == path && tr.channels[static_cast<std::size_t>(c.channel)].name == channel) return c.value;
    }
    return std::nan("");
}

int CountLevel(const ConvertResult& r, ConvertNote::Level l) { return r.Count(l); }

void TestConvert()
{
    std::printf("TestConvert\n");
    const std::int64_t tps = kDefaultTicksPerSecond;
    // ---- BossReveal(sequence.ts の SEQUENCE_EXAMPLE)
    ConvertOptions opt;
    opt.initialPost["saturation"] = 1.0;
    opt.initialPost["contrast"] = 1.0;
    Sequence s;
    const ConvertResult r = ConvertLegacySpec(kLegacyBossReveal, opt, s);
    CHECK_MSG(r.ok, "%s", r.error.c_str());
    if (!r.ok) return;
    CHECK(CheckSequenceStructure(s).empty());
    CHECK(s.name == "BossReveal" && s.ticksPerSecond == tps && s.frameRate == 30);
    CHECK(s.hasRange && s.rangeStart == 0 && s.rangeEnd == SecondsToTicks(4.4, tps));
    CHECK(s.cuts.size() == 1 && s.cuts[0].start == 0 && s.cuts[0].end == s.rangeEnd);
    CHECK(FindBinding(s, "b_scene") >= 0 && s.bindings[static_cast<std::size_t>(FindBinding(s, "b_scene"))].kind == BindingKind::Scene);
    CHECK(CountLevel(r, ConvertNote::Level::Error) == 0);
    CHECK(CountLevel(r, ConvertNote::Level::Lossy) >= 1 && !r.Lossless());                 // shake の波形などロスレスでない点が明記される
    std::printf("  BossReveal: bindings=%zu notes=%zu (lossy=%d)\n", s.bindings.size(), r.notes.size(), CountLevel(r, ConvertNote::Level::Lossy));

    // カメラ位置は旧 Lua と同じ式(from + (to-from) * EASE.inOut(k))
    const std::array<double, 3> from{ 0, 6, 14 }, to{ 0, 2.2, 6 };
    for (double sec : { 0.0, 0.4, 0.8, 1.6, 2.4, 3.0, 3.2, 3.5 })
    {
        const double k = LegacyEase("inOut", std::min(1.0, sec / 3.2));
        const Tick t = SecondsToTicks(sec, tps);
        for (int a = 0; a < 3; ++a)
        {
            const double want = from[static_cast<std::size_t>(a)] + (to[static_cast<std::size_t>(a)] - from[static_cast<std::size_t>(a)]) * k;
            const double got = SampleOf(s, t, "CutsceneCam", TrackType::Transform, "", std::string("position.") + "xyz"[a]);
            CHECK_MSG(Near(got, want, 1e-9), "camera %c @%.1fs: got %.9g want %.9g", "xyz"[a], sec, got, want);
        }
    }
    // フェード(clear が最初 = 黒から明ける): 0 → 1 を線形で 0.8 秒
    for (double sec : { 0.0, 0.2, 0.4, 0.8, 2.0 })
        CHECK(Near(SampleOf(s, SecondsToTicks(sec, tps), "Scene", TrackType::Post, "", "exposure"), std::min(1.0, sec / 0.8), 1e-9));
    // timeScale: 2.6 で 0.25(dur 0 = ステップ)、3.1〜3.5 で 0.25 → 1.0 の線形
    CHECK(std::isnan(SampleOf(s, 0, "Scene", TrackType::TimeScale, "", "value")) == false);           // 先頭ホールド
    CHECK(Near(SampleOf(s, SecondsToTicks(2.7, tps), "Scene", TrackType::TimeScale, "", "value"), 0.25));
    CHECK(Near(SampleOf(s, SecondsToTicks(3.3, tps), "Scene", TrackType::TimeScale, "", "value"), 0.25 + 0.75 * 0.5, 1e-9));
    CHECK(Near(SampleOf(s, SecondsToTicks(3.6, tps), "Scene", TrackType::TimeScale, "", "value"), 1.0));
    // post: 開始値を initialPost で焼くと、inOut で 1.0 → 1.35
    CHECK(Near(SampleOf(s, SecondsToTicks(3.7, tps), "Scene", TrackType::Post, "", "saturation"), 1.0 + 0.35 * LegacyEase("inOut", 0.5), 1e-9));
    CHECK(Near(SampleOf(s, SecondsToTicks(4.2, tps), "Scene", TrackType::Post, "", "contrast"), 1.2, 1e-9));
    // イベント: bossFightStart は 4.4 秒に 1 回(値 0)。CollectEvents で拾える
    {
        std::vector<EventFire> fires;
        EventCollectOptions o;
        o.rangeStart = s.rangeStart; o.rangeEnd = s.rangeEnd;
        o.startInclusive = true;
        CollectEvents(s, s.rangeStart, s.rangeEnd, o, fires);
        CHECK(fires.size() == 1);
        if (!fires.empty())
        {
            const EventItem& e = s.bindings[static_cast<std::size_t>(fires[0].binding)].tracks[static_cast<std::size_t>(fires[0].track)].events[static_cast<std::size_t>(fires[0].event)];
            CHECK(e.name == "bossFightStart" && e.kind == "emit" && e.t == SecondsToTicks(4.4, tps) && e.params.Find("data") && e.params.Find("data")->GetNumber("value", -1) == 0.0);
        }
    }
    // クリップ: shake(2.6〜3.3)、vfx burst、vfxPlay(emitter は終端まで)、sound(bgm)
    {
        const Binding& cam = s.bindings[static_cast<std::size_t>(FindBinding(s, s.cuts[0].camera))];
        const Track* shake = nullptr;
        for (const auto& t : cam.tracks) if (t.type == TrackType::Shake) shake = &t;
        CHECK(shake && shake->clips.size() == 1 && shake->clips[0].start == SecondsToTicks(2.6, tps) && shake->clips[0].dur == SecondsToTicks(0.7, tps));
        if (shake) CHECK(shake->clips[0].params.GetNumber("amp", 0) == 0.35 && shake->clips[0].params.GetNumber("freq", 0) == 26);
        int aura = -1;
        for (std::size_t i = 0; i < s.bindings.size(); ++i) if (s.bindings[i].name == "FX_BossAura") aura = static_cast<int>(i);
        CHECK(aura >= 0);
        if (aura >= 0)
        {
            const Clip& em = s.bindings[static_cast<std::size_t>(aura)].tracks[0].clips[0];
            CHECK(em.params.GetString("mode") == "emitter" && em.start == SecondsToTicks(2.8, tps) && em.start + em.dur == s.rangeEnd);
        }
        const EvalResult ev = Evaluate(s, SecondsToTicks(3.0, tps));
        CHECK(ev.clips.size() >= 2);                                                                  // shake と emitter が有効
        const EvalResult atStart = Evaluate(s, SecondsToTicks(0.4, tps));
        bool sound = false;
        for (const auto& c : atStart.clips)
        {
            const Clip& cl = s.bindings[static_cast<std::size_t>(c.binding)].tracks[static_cast<std::size_t>(c.track)].clips[static_cast<std::size_t>(c.clip)];
            if (cl.params.GetString("path") == "audio/boss_theme.wav") sound = cl.params.GetString("bus") == "music" && cl.params.GetBool("loop", false);
        }
        CHECK(sound);
        // aim(注視): lookAtName="Boss" → Boss バインディングの ID を指す
        bool aim = false;
        int bossIdx = -1;
        for (std::size_t i = 0; i < s.bindings.size(); ++i) if (s.bindings[i].name == "Boss") bossIdx = static_cast<int>(i);
        CHECK(bossIdx >= 0);
        for (const auto& t : cam.tracks)
            if (t.type == TrackType::Aim && bossIdx >= 0)
            {
                const SeqValue* tg = t.params.Find("target");
                aim = tg && tg->GetString("binding") == s.bindings[static_cast<std::size_t>(bossIdx)].id;
            }
        CHECK(aim);
    }
    // 開始値が不明なら Lossy(post の initialPost を渡さないと出る)
    {
        Sequence s2;
        const ConvertResult r2 = ConvertLegacySpec(kLegacyBossReveal, ConvertOptions{}, s2);
        CHECK(r2.ok && r2.Count(ConvertNote::Level::Lossy) > r.Count(ConvertNote::Level::Lossy));
    }
    // 決定論: 同じ入力 → 同じバイト列(ID も同じ)
    {
        Sequence s3;
        CHECK(ConvertLegacySpec(kLegacyBossReveal, opt, s3).ok && Ser(s3) == Ser(s));
        CheckGolden("legacy_boss_reveal.dxseq", s);
    }
    // 往復・評価の一致
    {
        Sequence p;
        CHECK(ParseSequence(Ser(s), p).ok && Ser(p) == Ser(s));
        for (Tick t : { 0, 5000, 12345, 20000, 26400 }) CHECK(BitEqual(Evaluate(p, t), Evaluate(s, t)));
    }

    // ---- PropsShow: move/rotate/shaderParam/light/vfxPlay+Stop/sound/log/scene/fade/lookAt(点)/camera 2 本目の from 省略
    Sequence ps;
    ConvertOptions o2;
    o2.initialRotation["Statue"] = { 0, 0, 0 };
    const ConvertResult r2 = ConvertLegacySpec(kLegacyProps, o2, ps);
    CHECK_MSG(r2.ok, "%s", r2.error.c_str());
    if (r2.ok)
    {
        CHECK(CheckSequenceStructure(ps).empty());
        CHECK(CountLevel(r2, ConvertNote::Level::Error) == 0);
        CHECK(ps.meta.Find("legacy") && ps.meta.Find("legacy")->GetBool("loop", false) && ps.meta.Find("legacy")->GetString("doneEvent") == "PropsShow:end");
        CHECK(ps.rangeEnd == SecondsToTicks(4.5, tps));
        // 2 本目のカメラは from 省略 → 直前トラックの終点 [0,3,10] から [5,3,10]
        CHECK(Near(SampleOf(ps, SecondsToTicks(2.5, tps), "MainCam", TrackType::Transform, "", "position.x"), 0.0));
        CHECK(Near(SampleOf(ps, SecondsToTicks(3.25, tps), "MainCam", TrackType::Transform, "", "position.x"), 5.0 * LegacyEase("inOut", 0.5), 1e-9));
        CHECK(Near(SampleOf(ps, SecondsToTicks(1.0, tps), "MainCam", TrackType::Transform, "", "position.z"), 20 + (10 - 20) * LegacyEase("out", 0.5), 1e-9));
        // move: outBack、rotate(初期値を焼いた)、shaderParam(linear)
        CHECK(Near(SampleOf(ps, SecondsToTicks(1.0, tps), "Statue", TrackType::Transform, "", "position.y"), LegacyEase("outBack", 0.5), 1e-9));
        CHECK(Near(SampleOf(ps, SecondsToTicks(2.0, tps), "Statue", TrackType::Transform, "", "rotation.y"), 180 * LegacyEase("inOut", 0.5), 1e-9));
        CHECK(Near(SampleOf(ps, SecondsToTicks(1.5, tps), "Statue", TrackType::Property, "MeshRenderer.p1", "value"), 0.5, 1e-9));
        // light は outQuad(旧 Lighting.tween の既定)
        CHECK(Near(SampleOf(ps, SecondsToTicks(1.5, tps), "Lamp", TrackType::Property, "Light.color", "g"), 0.0, 1.0));   // 開始値不明 → 終点ホールドなので値があるだけ確認
        // vfxPlay/vfxStop は 1.5〜3.5 のエミッタ区間
        int sp = -1;
        for (std::size_t i = 0; i < ps.bindings.size(); ++i) if (ps.bindings[i].name == "Sparks") sp = static_cast<int>(i);
        CHECK(sp >= 0);
        if (sp >= 0)
        {
            const Clip& c = ps.bindings[static_cast<std::size_t>(sp)].tracks[0].clips[0];
            CHECK(c.start == SecondsToTicks(1.5, tps) && c.dur == SecondsToTicks(2.0, tps) && c.params.GetString("layer") == "core");
        }
        // sfx: volume 0.7 / loop false。bgm: loop=false を尊重
        int sfx = 0, bgm = 0;
        for (const auto& b : ps.bindings) for (const auto& t : b.tracks) if (t.type == TrackType::Audio) for (const auto& c : t.clips)
        {
            if (c.params.GetString("path") == "audio/hit.wav") sfx = (c.params.GetNumber("volume", 0) == 0.7 && !c.params.GetBool("loop", true)) ? 1 : -1;
            if (c.params.GetString("path") == "audio/bgm.ogg") bgm = (c.params.GetString("bus") == "music" && !c.params.GetBool("loop", true)) ? 1 : -1;
        }
        CHECK(sfx == 1 && bgm == 1);
        // log / scene イベント
        std::vector<EventFire> fires;
        EventCollectOptions eo;
        eo.rangeStart = 0; eo.rangeEnd = ps.rangeEnd; eo.startInclusive = true;
        CollectEvents(ps, 0, ps.rangeEnd, eo, fires);
        CHECK(fires.size() == 2);
        if (fires.size() == 2)
        {
            const Track& et = ps.bindings[static_cast<std::size_t>(fires[0].binding)].tracks[static_cast<std::size_t>(fires[0].track)];
            CHECK(et.events[static_cast<std::size_t>(fires[0].event)].kind == "log" && et.events[static_cast<std::size_t>(fires[0].event)].name == "done \"quoted\"");
            CHECK(et.events[static_cast<std::size_t>(fires[1].event)].kind == "loadScene" && et.events[static_cast<std::size_t>(fires[1].event)].params.GetNumber("fade", 0) == 1.2);
        }
        // fade: white(dur .3)の後の clear(.5)は、white(8.0)から 1.0 へ
        CHECK(Near(SampleOf(ps, SecondsToTicks(0.3, tps), "Scene", TrackType::Post, "", "exposure"), 8.0));
        CHECK(Near(SampleOf(ps, SecondsToTicks(0.55, tps), "Scene", TrackType::Post, "", "exposure"), 8.0 + (1.0 - 8.0) * 0.5, 1e-9));
        // 注視: 固定点 aim と、lookAtName(優先)の aim。lookAt+lookAtName 併記は警告。
        int aims = 0, aimsPoint = 0;
        for (const auto& t : ps.bindings[0].tracks) if (t.type == TrackType::Aim) { ++aims; if (t.params.Find("target") && t.params.Find("target")->Find("point")) ++aimsPoint; }
        CHECK(aims == 2 && aimsPoint == 1);
        CHECK(CountLevel(r2, ConvertNote::Level::Warning) >= 1);
        CheckGolden("legacy_props_show.dxseq", ps);
        Sequence p;
        CHECK(ParseSequence(Ser(ps), p).ok && Ser(p) == Ser(ps));
    }

    // ---- 不正な台本
    {
        Sequence x;
        CHECK(!ConvertLegacySpec("not json", ConvertOptions{}, x).ok);
        CHECK(!ConvertLegacySpec(R"({"name":"1bad","tracks":[{"t":0,"type":"log","text":"x"}]})", ConvertOptions{}, x).ok);
        CHECK(!ConvertLegacySpec(R"({"name":"N","tracks":[]})", ConvertOptions{}, x).ok);
        const ConvertResult e = ConvertLegacySpec(R"({"name":"Bad","tracks":[
            {"t":0,"type":"nope"},{"t":-1,"type":"log","text":"neg"},{"t":0,"type":"camera","to":[1,2,3]},
            {"t":0,"type":"move","target":"A","to":[1]},{"t":1,"type":"vfxStop","target":"Z"},{"t":2,"type":"log","text":"ok"}]})", ConvertOptions{}, x);
        CHECK(e.ok && e.Count(ConvertNote::Level::Error) == 4 && e.Count(ConvertNote::Level::Warning) == 1);
        CHECK(CheckSequenceStructure(x).empty() && x.bindings.size() == 1);                           // 有効な log だけが残る
    }
}

// ===========================================================================
// 10. バインディング解決
// ===========================================================================
void TestBinding()
{
    std::printf("TestBinding\n");
    MapBindingResolver res;
    res.Add({ 1, "aaaa000000000001", "Rig/Cam", "Cam" });
    res.Add({ 2, "aaaa000000000002", "Rig/Boss", "Boss" });
    res.Add({ 3, "", "Other/Boss", "Boss" });              // 同名(後から作られた)
    res.Add({ 4, "aaaa000000000004", "Rig/Cam", "CamB" }); // 同パス

    Sequence s = NewSeq();
    auto add = [&](const std::string& id, const std::string& name, const std::string& guid, const std::string& path, BindingKind k = BindingKind::Entity) {
        Binding b = Bnd(id, name, k);
        b.hint.guid = guid; b.hint.path = path; b.hint.name = name;
        Track t = Trk("t_" + id, TrackType::Property);
        t.path = "A.b";
        t.channels.push_back(Ch("value", { K(0, 1) }));
        b.tracks.push_back(t);
        s.bindings.push_back(b);
    };
    add("b_guid", "Boss", "aaaa000000000002", "");            // guid で一意
    add("b_stale", "Cam", "ffff000000000000", "Rig/Boss");    // guid 消失 → パスで解決(FellBack)
    add("b_name", "Boss", "", "");                            // 名前が 2 個 → 最後に作られた方(id=3)
    add("b_gone", "Nobody", "", "");                          // 未解決
    add("b_scene", "Scene", "", "", BindingKind::Scene);
    add("b_spawn", "Spawn", "", "", BindingKind::Spawnable);
    add("b_pathdup", "X", "", "Rig/Cam");                     // パス候補 2 個(id=4 が新しい)
    add("b_ovr", "Nobody", "", "");                           // 上書きで解決

    BindingOverrides ov;
    ov["b_ovr"] = "aaaa000000000001";
    ov["b_guid"] = "ffff000000000000";                        // 上書き先が無いときは通常の解決へ落ちる
    const BindingSet set = ResolveBindings(s, res, &ov);
    CHECK(set.byBinding.size() == s.bindings.size());
    CHECK(set.byBinding[0].via == ResolveVia::Guid && set.byBinding[0].target == 2);
    CHECK(set.byBinding[1].via == ResolveVia::Path && set.byBinding[1].target == 2);
    CHECK(set.byBinding[2].via == ResolveVia::Name && set.byBinding[2].target == 3 && set.byBinding[2].candidates == 2);
    CHECK(!set.byBinding[3].Resolved() && set.byBinding[3].target == kNoTarget);
    CHECK(set.byBinding[4].via == ResolveVia::Scene && set.byBinding[4].target == kSceneTarget);
    CHECK(!set.byBinding[5].Resolved());
    CHECK(set.byBinding[6].via == ResolveVia::Path && set.byBinding[6].target == 4 && set.byBinding[6].candidates == 2);
    CHECK(set.byBinding[7].via == ResolveVia::Override && set.byBinding[7].target == 1);
    CHECK(!set.AllResolved());
    auto has = [&](BindingIssue::Code c, const std::string& bid) {
        for (const auto& i : set.issues) if (i.code == c && i.bindingId == bid) return true;
        return false;
    };
    CHECK(has(BindingIssue::Code::Unresolved, "b_gone"));
    CHECK(has(BindingIssue::Code::FellBack, "b_stale"));
    CHECK(has(BindingIssue::Code::AmbiguousName, "b_name"));
    CHECK(has(BindingIssue::Code::AmbiguousPath, "b_pathdup"));
    CHECK(has(BindingIssue::Code::SpawnableUnsupported, "b_spawn"));
    CHECK(!has(BindingIssue::Code::Unresolved, "b_guid") && !has(BindingIssue::Code::AmbiguousName, "b_guid"));
    // 未解決のトラックは警告として保持される(消えない)
    for (const auto& i : set.issues)
        if (i.code == BindingIssue::Code::Unresolved)
        {
            CHECK(i.trackIds.size() == 1 && i.trackIds[0] == "t_b_gone" && !i.message.empty());
        }
    CHECK(s.bindings[3].tracks.size() == 1);
    // 同じ対象に解決された別バインディングの警告(b_ovr と b_stale? — b_ovr=1 と b_pathdup=4 は別。b_guid=2 と b_stale=2 が共有)
    CHECK(has(BindingIssue::Code::SharedTarget, "b_stale"));

    // EvalFilter との連携: 未解決バインディングのサンプルは出ない
    EvalFilter f;
    f.bindingEnabled = set.EnabledMask();
    CHECK(f.bindingEnabled.size() == s.bindings.size() && f.bindingEnabled[0] == 1 && f.bindingEnabled[3] == 0 && f.bindingEnabled[4] == 1 && f.bindingEnabled[5] == 0);
    const EvalResult all = Evaluate(s, 0), filtered = Evaluate(s, 0, &f);
    CHECK(all.channels.size() == 8 && filtered.channels.size() == 6);
    for (const auto& c : filtered.channels) CHECK(c.binding != 3 && c.binding != 5);

    // 全部解決できるとき
    Sequence ok = NewSeq();
    Binding b = Bnd("b_a", "Cam");
    b.hint.guid = "aaaa000000000001";
    ok.bindings.push_back(b);
    const BindingSet s2 = ResolveBindings(ok, res);
    CHECK(s2.AllResolved() && s2.issues.empty());
    CHECK(std::string(ResolveViaName(ResolveVia::Guid)) == "guid" && std::string(ResolveViaName(ResolveVia::None)) == "none");
}

// ===========================================================================
// 11. 純ロジックであること(依存の検査)
// ===========================================================================
void TestPurity()
{
    std::printf("TestPurity\n");
    namespace fs = std::filesystem;
    const fs::path dir = DX12E_SEQUENCER_SRC_DIR;
    static const std::set<std::string> allowed = { "algorithm", "array", "charconv", "cmath", "cstdint", "cstdio", "cstring", "filesystem",
                                                   "fstream", "initializer_list", "iterator", "map", "optional", "random", "sstream", "string",
                                                   "string_view", "unordered_map", "unordered_set", "utility", "variant", "vector",
                                                   "nlohmann/json.hpp" };
    int files = 0, includes = 0;
    bool clean = true;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const std::string ext = e.path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        ++files;
        std::ifstream f(e.path());
        std::string line;
        while (std::getline(f, line))
        {
            std::size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || line.compare(p, 8, "#include") != 0) continue;
            ++includes;
            const std::size_t a = line.find_first_of("<\"", p + 8);
            if (a == std::string::npos) continue;
            const char close = line[a] == '<' ? '>' : '"';
            const std::size_t b = line.find(close, a + 1);
            const std::string name = line.substr(a + 1, b - a - 1);
            const bool ok = (line[a] == '"') ? name.rfind("sequencer/", 0) == 0 : allowed.count(name) != 0;
            if (!ok) { clean = false; std::printf("  禁止/未許可の include: %s (%s)\n", name.c_str(), e.path().filename().string().c_str()); }
        }
    }
    CHECK(files >= 14);
    CHECK(includes > 30);
    CHECK_MSG(clean, "src/sequencer が標準ライブラリ + nlohmann 以外を include している");
}

// ===========================================================================
// 12. 性能
// ===========================================================================
Sequence MakePerfSequence(int tracks, int channelsPerTrack, int keysPerChannel, Rng& rng)
{
    Sequence s = NewSeq("Perf");
    IdAllocator ids(99);
    Binding b = Bnd("b_perf", "Perf");
    for (int t = 0; t < tracks; ++t)
    {
        Track tr = Trk(ids.New('t'), TrackType::Property);
        tr.path = "C.f" + std::to_string(t);
        for (int c = 0; c < channelsPerTrack; ++c)
        {
            Channel ch;
            ch.name = "c" + std::to_string(c);
            Tick tt = 0;
            for (int k = 0; k < keysPerChannel; ++k)
            {
                Key key;
                key.t = tt;
                key.v = rng.U() * 100;
                switch (k % 4)
                {
                case 0: key.ip = Interp::Auto; break;
                case 1: key.ip = Interp::Bezier; key.inDt = 20; key.outDt = 20; key.inDv = 1; key.outDv = -1; break;
                case 2: key.ip = Interp::Ease; key.ease = EaseId::InOutCubic; break;
                default: key.ip = Interp::Linear; break;
                }
                ch.keys.push_back(key);
                tt += 50 + rng.R(100);
            }
            tr.channels.push_back(std::move(ch));
        }
        b.tracks.push_back(std::move(tr));
    }
    s.bindings.push_back(std::move(b));
    return s;
}

double MeasureEvalMicros(const Sequence& s, Tick maxT, int iterations, Rng& rng)
{
    EvalResult buf;
    volatile double sink = 0;
    // 3 バッチの最小(スケジューラの揺らぎを避ける)
    double best = 1e18;
    for (int batch = 0; batch < 3; ++batch)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i)
        {
            Evaluate(s, static_cast<Tick>(rng.R(static_cast<int>(maxT))), buf);
            sink = sink + (buf.channels.empty() ? 0.0 : buf.channels[0].value);
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::micro>(t1 - t0).count() / iterations);
    }
    (void)sink;
    return best;
}

void TestPerformance()
{
    std::printf("TestPerformance\n");
    Rng rng(0xBEEF);
    // 目標: 50 トラック・各 100 キーで 1 回 ≤ 0.3ms
    const Sequence a = MakePerfSequence(50, 1, 100, rng);
    const double us1 = MeasureEvalMicros(a, 10000, 20000, rng);
    // 参考: 各トラック 3 チャンネル(= 15000 キー)
    const Sequence b = MakePerfSequence(50, 3, 100, rng);
    const double us3 = MeasureEvalMicros(b, 10000, 20000, rng);
    // 参考: 100 トラック x 3 チャンネル x 300 キー(設計 §4.7 の想定)
    const Sequence c = MakePerfSequence(100, 3, 300, rng);
    const double us300 = MeasureEvalMicros(c, 30000, 5000, rng);
    std::printf("  Evaluate 平均: 50トラック x 1ch x 100キー = %.2f us / 50 x 3ch x 100 = %.2f us / 100 x 3ch x 300 = %.2f us\n", us1, us3, us300);
#ifdef NDEBUG
    // 合否は最適化ビルド(Release)だけ。Debug / ASAN では数値を出すだけ(未最適化 + 検査で 20 倍以上遅くなる)
    CHECK_MSG(us1 <= 300.0, "目標 0.3ms を超過: %.2f us", us1);
    CHECK_MSG(us3 <= 300.0, "3ch でも 0.3ms 以内: %.2f us", us3);
    CHECK_MSG(us300 <= 300.0, "100x3x300 でも 0.3ms 以内: %.2f us", us300);
#else
    std::printf("  (最適化なしのビルドなので性能は合否に使わない)\n");
    CHECK(us1 > 0.0 && us3 > 0.0 && us300 > 0.0);
#endif
}

} // namespace

int main()
{
    TestTime();
    TestIds();
    TestEase();
    TestCurveBasics();
    TestCurveAuto();
    TestCurveBezier();
    TestCurveExtrapAndColor();
    TestQuaternion();
    TestEvaluateBasics();
    TestCameraCut();
    TestMarkers();
    TestEvaluateDeterminism();
    TestEventBasics();
    TestEventPartitionInvariance();
    TestPlayback();
    TestOps();
    TestTxnAndHistory();
    TestUndoFuzz();
    TestSerialize();
    TestSerializeFile();
    TestConvert();
    TestBinding();
    TestPurity();
    TestPerformance();

    std::printf("SequencerCoreTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
